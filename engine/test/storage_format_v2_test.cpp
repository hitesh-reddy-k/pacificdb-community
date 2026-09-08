#include "storage_format_v2.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <chrono>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;
using namespace pacificdb::storage_v2;

static void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

int main() {
    const fs::path root = fs::temp_directory_path() /
        ("pacificdb-storage-v2-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);
    try {
        std::string error;
        require(writeIndexSegment(root / "name.cidx", {
            {IndexMutation::Op::Clear, "", "a"},
            {IndexMutation::Op::Put, "alice", "a"},
            {IndexMutation::Op::Put, "bob", "b"},
        }, false, nullptr, &error), error);
        const auto segments = listIndexSegments(root / "name.cidx");
        require(segments.size() == 1, "index segment not published");
        IndexSegment segment;
        require(readIndexSegment(segments.front(), segment, &error), error);
        require(!segment.snapshot && segment.mutations.size() == 3,
                "index segment round-trip mismatch");
        const fs::path corruptIndex = root / "corrupt-index.seg";
        fs::copy_file(segments.front(), corruptIndex);
        {
            std::fstream corrupt(corruptIndex, std::ios::binary | std::ios::in | std::ios::out);
            corrupt.seekg(42);
            char byte = 0;
            corrupt.read(&byte, 1);
            corrupt.seekp(42);
            byte ^= 0x55;
            corrupt.write(&byte, 1);
        }
        require(!readIndexSegment(corruptIndex, segment, &error),
                "corrupted column-index segment passed verification");

        const fs::path orderedDirectory = root / "ordered.cidx";
        fs::create_directories(orderedDirectory);
        const fs::path futureSegment = orderedDirectory / "09999999999999999999.seg";
        std::ofstream(futureSegment).close();
        fs::path orderedSegment;
        require(writeIndexSegment(orderedDirectory, {
            {IndexMutation::Op::Put, "alice", "a"},
        }, false, &orderedSegment, &error), error);
        require(orderedSegment.filename() > futureSegment.filename(),
                "index segment order regressed after clock rollback");

        std::vector<json> documents = {
            {{"array", json::array({1, "two", false})}, {"boolean", true},
             {"id", "a"}, {"name", "alice"}, {"nested", {{"x", 1}}},
             {"nil", nullptr}, {"score", 1}},
            {{"id", "b"}, {"name", "bob"}, {"score", 2}},
            {{"id", "c"}, {"name", "carol"}, {"score", 3}},
        };
        std::vector<std::reference_wrapper<const json>> rows;
        for (const auto& document : documents) rows.emplace_back(document);
        SstWriteStats stats;
        const fs::path sst = root / "data.sst";
        require(writeSst(sst, rows, 4096, &stats, &error), error);
        require(stats.rows == documents.size() && stats.blocks == 1,
                "unexpected SST write statistics");
        require(verifySst(sst, &error), error);
        std::vector<json> readBack;
        require(scanSst(sst, [&](json&& row) {
            readBack.push_back(std::move(row));
            return true;
        }, &error), error);
        require(readBack == documents, "SST scan round-trip mismatch");
        std::size_t viewed = 0;
        require(scanSstViews(sst, [&](const MsgpackRowView& row) {
            const auto id = row.field("id");
            require(id && id->is_string(), "streaming SST id decode failed");
            require(!row.field("missing"), "streaming SST returned a missing field");
            require(row.materialize() == documents.at(viewed),
                    "streaming SST materialization mismatch");
            ++viewed;
            return true;
        }, &error), error);
        require(viewed == documents.size(), "streaming SST row count mismatch");
        auto found = findInSst(sst, "b", &error);
        require(found && found->value("name", "") == "bob", "SST point lookup failed");
        require(!findInSst(sst, "missing", &error), "missing SST key was returned");

        const std::vector<std::vector<json>> generations = {
            {{{"id", "a"}, {"value", 1}}, {{"id", "b"}, {"value", 1}},
             {{"id", "c"}, {"value", 1}},
             {{"id", "f"}, {"value", 1}, {"_mvcc_version", 10}}},
            {{{"id", "b"}, {"value", 2}}, {{"id", "d"}, {"value", 1}}},
            {{{"id", "a"}, {"_deleted", true}}, {{"id", "c"}, {"value", 2}}},
            {{{"id", "e"}, {"value", 1}},
             {{"id", "f"}, {"value", 2}, {"_mvcc_version", 5}}},
        };
        std::vector<fs::path> generationPaths;
        for (std::size_t i = 0; i < generations.size(); ++i) {
            std::vector<std::reference_wrapper<const json>> generationRows;
            for (const auto& row : generations[i]) generationRows.emplace_back(row);
            const auto path = root / ("generation-" + std::to_string(i) + ".sst");
            require(writeSst(path, generationRows, 4096, nullptr, &error), error);
            generationPaths.push_back(path);
        }
        std::vector<std::pair<std::string, int>> latestRows;
        require(visitLatestSstRows(generationPaths, [&](const MsgpackRowView& row) {
            latestRows.push_back({row.field("id")->get<std::string>(),
                                  row.field("value")->get<int>()});
            return true;
        }, &error), error);
        require(latestRows == std::vector<std::pair<std::string, int>>{
                    {"b", 2}, {"c", 2}, {"d", 1}, {"e", 1}, {"f", 1}},
                "k-way SST visitor did not emit one newest live row per id");

        require(buildBloom(sst, {"a", "b", "c"}, &error), error);
        require(bloomMayContain(sst, "a").value_or(false), "bloom false negative");
        require(!bloomMayContain(sst, "definitely-not-present").value_or(true),
                "bloom failed deterministic negative probe");

        // An interrupted writer leaves only an ignored .writing artifact; the
        // published SST remains readable.
        {
            std::ofstream interrupted(sst.string() + ".writing", std::ios::binary);
            interrupted << "partial";
        }
        invalidateCaches(sst);
        require(verifySst(sst, &error), "orphan SST temporary affected published data");

        {
            std::fstream corrupt(sst, std::ios::binary | std::ios::in | std::ios::out);
            corrupt.seekp(64 + 24 + 2);
            char byte = 0;
            corrupt.read(&byte, 1);
            corrupt.seekp(64 + 24 + 2);
            byte ^= 0x55;
            corrupt.write(&byte, 1);
        }
        invalidateCaches(sst);
        require(!verifySst(sst, &error), "corrupted SST passed verification");
        fs::remove_all(root);
        std::cout << "STORAGE_FORMAT_V2_PASS\n";
        return 0;
    } catch (const std::exception& e) {
        fs::remove_all(root);
        std::cerr << "STORAGE_FORMAT_V2_FAIL: " << e.what() << '\n';
        return 1;
    }
}
