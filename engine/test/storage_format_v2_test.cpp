#include "storage_format_v2.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <chrono>
#include <string>
#include <vector>
#include <thread>
#include <random>
#include <cmath>

namespace fs = std::filesystem;
using json = nlohmann::json;
using namespace pacificdb::storage_v2;

static void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

static void testRawDenseScoring() {
    std::mt19937 random(1923);
    std::uniform_real_distribution<double> value(-10, 10);
    for (int example = 0; example < 200; ++example) {
        json vector = json::array({0, -1, -33, -129, -32769, int64_t(-2147483649LL),
            std::numeric_limits<int64_t>::min(), uint64_t(255), uint64_t(65535),
            uint64_t(4294967295ULL), std::numeric_limits<uint64_t>::max(), 1.25, -0.0});
        for (int i = 0; i < example % 100; ++i) vector.push_back(value(random));
        std::vector<double> query(vector.size()); double qnorm = 0;
        for (auto& number : query) { number = value(random); qnorm += number * number; }
        qnorm = std::sqrt(qnorm);
        auto bytes = json::to_msgpack(json{{"id", "numeric"}, {"vector", vector}});
        const MsgpackRowView indexed(bytes.data(), bytes.size(), true);
        for (const std::string metric : {"cosine", "l2", "euclidean", "dot", "dot_product"}) {
            double expected = 0, norm = 0;
            const bool l2 = metric == "l2" || metric == "euclidean";
            const bool dot = metric == "dot" || metric == "dot_product";
            for (size_t i = 0; i < query.size(); ++i) {
                const double number = vector[i].get<double>();
                if (l2) { const double difference = query[i] - number; expected += difference * difference; }
                else { expected += query[i] * number; norm += number * number; }
            }
            if (l2) expected = -std::sqrt(expected);
            else if (!dot) expected = qnorm * std::sqrt(norm) < 1e-12 ? 0.0 : expected / (qnorm * std::sqrt(norm));
            const auto score = indexed.denseScore("vector", query, metric, qnorm);
            require(score.has_value(), "numeric array did not use raw decoder");
            require(std::abs(*score - expected) <= 1e-12 * std::max(1.0, std::abs(expected)),
                    "raw numeric score differs from independent JSON reference");
        }
        require(!indexed.denseScore("missing", query, "cosine", qnorm), "raw missing vector accepted");
        require(!indexed.denseScore("vector", {1.0}, "cosine", 1.0), "raw dimension mismatch accepted");
    }
    for (const json vector : {json::array({"1", "2"}), json::array({true, 1}), json::object()}) {
        const auto bytes = json::to_msgpack(json{{"vector", vector}});
        require(!MsgpackRowView(bytes.data(), bytes.size(), true).denseScore("vector", {1., 2.}, "dot", 1.),
                "non-numeric array bypassed legacy decoder");
    }
}

static void testIndexedRows() {
    for (int example = 0; example < 100; ++example) {
        json document = {{"id", std::to_string(example)}, {"null", nullptr},
            {"binary", json::binary({0, 1, 2, 255})}, {"vector", {1., -2., 3.}},
            {"signed", std::numeric_limits<int64_t>::min()},
            {"unsigned", std::numeric_limits<uint64_t>::max()}};
        for (int field = 0; field < 50; ++field)
            document["f" + std::to_string(field)] = {{"nested", {example, field, true, "text"}}};
        auto bytes = json::to_msgpack(document);
        const MsgpackRowView plain(bytes.data(), bytes.size());
        const MsgpackRowView indexed(bytes.data(), bytes.size(), true);
        std::vector<std::string> names{"absent", "id", "id", "vector", "null"};
        for (auto it = document.begin(); it != document.end(); ++it) {
            names.push_back(it.key());
            require(indexed.field(it.key()) == plain.field(it.key()), "indexed field mismatch");
        }
        require(!indexed.field("absent"), "indexed missing field mismatch");
        require(indexed.project(names) == plain.project(names), "indexed wide projection mismatch");
        require(indexed.materialize() == document, "indexed materialization mismatch");
        auto copied = indexed;
        require(copied.project(names) == indexed.project(names), "copied field spans mismatch");
        // Immutable spans can be read by multiple workers without shared mutations.
        if (example == 0) {
            std::vector<std::thread> readers;
            for (int worker = 0; worker < 4; ++worker) readers.emplace_back([&] {
                for (int run = 0; run < 100; ++run)
                    require(indexed.project(names) == plain.project(names), "concurrent indexed lookup mismatch");
            });
            for (auto& reader : readers) reader.join();
        }
    }
    const std::vector<std::vector<uint8_t>> malformed = {{}, {0x81},
        {0x81, 0xa2, 'i', 'd', 0xd9, 0xff}, {0x80, 0xc0}, {0x91, 0x01}};
    for (const auto& bytes : malformed) {
        const MsgpackRowView indexed(bytes.data(), bytes.size(), true);
        bool failed = false;
        try { (void)indexed.project({"id"}); } catch (const std::exception&) { failed = true; }
        require(failed, "malformed indexed map accepted");
    }
    const std::vector<uint8_t> duplicate = {0x82, 0xa2, 'i', 'd', 0xa1, 'a', 0xa2, 'i', 'd', 0xa1, 'b'};
    const MsgpackRowView plain(duplicate.data(), duplicate.size());
    const MsgpackRowView indexed(duplicate.data(), duplicate.size(), true);
    require(indexed.field("id") == plain.field("id"), "duplicate field lookup changed");
    require(indexed.project({"id"}) == plain.project({"id"}), "duplicate field projection changed");
}

int main() {
    const fs::path root = fs::temp_directory_path() /
        ("pacificdb-storage-v2-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);
    try {
        testIndexedRows();
        testRawDenseScoring();
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
            const auto projected = row.project({"id", "score", "missing", "id"});
            require(projected == json{{"id", documents.at(viewed).at("id")},
                                      {"score", documents.at(viewed).at("score")}},
                    "selective row projection mismatch");
            ++viewed;
            return true;
        }, &error), error);
        require(viewed == documents.size(), "streaming SST row count mismatch");
        json projectionFixture{{"id", "projection"}, {"null", nullptr},
            {"vector", {1., 0., -2.}}, {"nested", {{"tags", {"a", "b"}}}},
            {"unicode", "నమస్తే"}, {"payload", std::string(5000, 'x')}};
        for (int i = 0; i < 30; ++i) projectionFixture["field" + std::to_string(i)] = i;
        const auto packedFixture = json::to_msgpack(projectionFixture);
        const MsgpackRowView projectionView(packedFixture.data(), packedFixture.size());
        const std::vector<std::string> selectedFields{"id", "null", "vector", "nested", "unicode", "absent"};
        auto projected = projectionView.project(selectedFields);
        auto expectedProjection = projectionFixture;
        for (auto it = expectedProjection.begin(); it != expectedProjection.end();) {
            if (std::find(selectedFields.begin(), selectedFields.end(), it.key()) == selectedFields.end()) it = expectedProjection.erase(it);
            else ++it;
        }
        require(projected == expectedProjection, "map16/nested/null/unicode projection mismatch");
        for (const auto& corrupt : std::vector<std::vector<uint8_t>>{{}, {0x81}, {0x90}, {0x80, 0x00}, {0x81, 0xa1, 'x', 0xdd}}) {
            bool rejected = false;
            try { MsgpackRowView(corrupt.data(), corrupt.size()).project(selectedFields); }
            catch (const std::exception&) { rejected = true; }
            require(rejected, "malformed projected row was not rejected");
        }
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
