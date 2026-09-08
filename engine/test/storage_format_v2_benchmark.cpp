#include "storage_format_v2.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <nlohmann/json.hpp>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;
using clock_type = std::chrono::steady_clock;

int main() {
    const fs::path root = fs::temp_directory_path() /
        ("pacificdb-storage-v2-bench-" + std::to_string(
            clock_type::now().time_since_epoch().count()));
    fs::create_directories(root);
    try {
        std::vector<json> documents;
        json legacyIndex = json::object();
        for (int i = 0; i < 2000; ++i) {
            const std::string id = "user" + std::to_string(1000000 + i);
            const std::string field = "value" + std::to_string(i % 100);
            documents.push_back({
                {"id", id}, {"field0", field}, {"field1", std::string(100, 'a' + (i % 20))},
                {"field2", std::string(100, 'z' - (i % 20))}, {"field3", i},
                {"field4", i * 7}, {"field5", "ycsb-pacificdb-v2"},
            });
            legacyIndex[field].push_back(id);
        }

        const fs::path legacySst = root / "legacy.sst";
        {
            std::ofstream out(legacySst, std::ios::binary);
            for (const auto& document : documents) out << document.dump() << '\n';
        }
        std::vector<std::reference_wrapper<const json>> refs;
        for (const auto& document : documents) refs.emplace_back(document);
        const fs::path binarySst = root / "binary.sst";
        pacificdb::storage_v2::SstWriteStats stats;
        std::string error;
        if (!pacificdb::storage_v2::writeSst(binarySst, refs, 8192, &stats, &error)) {
            throw std::runtime_error(error);
        }

        const std::string legacyIndexRewrite = legacyIndex.dump();
        fs::path segment;
        if (!pacificdb::storage_v2::writeIndexSegment(root / "field0.cidx", {
                {pacificdb::storage_v2::IndexMutation::Op::Clear, "", "user1000123"},
                {pacificdb::storage_v2::IndexMutation::Op::Put, "value24", "user1000123"},
            }, false, &segment, &error)) {
            throw std::runtime_error(error);
        }

        const auto legacyStart = clock_type::now();
        for (int probe = 0; probe < 100; ++probe) {
            std::ifstream in(legacySst);
            std::string line;
            const std::string wanted = "user" + std::to_string(1000000 + ((probe * 19) % 2000));
            bool found = false;
            while (std::getline(in, line)) {
                const json row = json::parse(line);
                if (row.value("id", "") == wanted) { found = true; break; }
            }
            if (!found) throw std::runtime_error("legacy lookup failed");
        }
        const auto legacyUs = std::chrono::duration_cast<std::chrono::microseconds>(
            clock_type::now() - legacyStart).count();
        const auto binaryStart = clock_type::now();
        for (int probe = 0; probe < 100; ++probe) {
            const std::string wanted = "user" + std::to_string(1000000 + ((probe * 19) % 2000));
            if (!pacificdb::storage_v2::findInSst(binarySst, wanted, &error)) {
                throw std::runtime_error("binary lookup failed: " + error);
            }
        }
        const auto binaryUs = std::chrono::duration_cast<std::chrono::microseconds>(
            clock_type::now() - binaryStart).count();

        const double sstRatio = static_cast<double>(fs::file_size(legacySst)) /
            static_cast<double>(fs::file_size(binarySst));
        const double indexRatio = static_cast<double>(legacyIndexRewrite.size()) /
            static_cast<double>(fs::file_size(segment));
        const double lookupRatio = static_cast<double>(legacyUs) /
            static_cast<double>(std::max<long long>(1, binaryUs));
        std::cout << std::fixed << std::setprecision(2)
                  << "{\"documents\":2000,\"legacy_sst_bytes\":" << fs::file_size(legacySst)
                  << ",\"binary_sst_bytes\":" << fs::file_size(binarySst)
                  << ",\"sst_size_reduction_x\":" << sstRatio
                  << ",\"legacy_index_rewrite_bytes\":" << legacyIndexRewrite.size()
                  << ",\"index_segment_bytes\":" << fs::file_size(segment)
                  << ",\"index_write_reduction_x\":" << indexRatio
                  << ",\"legacy_point_lookup_us\":" << legacyUs
                  << ",\"binary_point_lookup_us\":" << binaryUs
                  << ",\"point_lookup_speedup_x\":" << lookupRatio << "}\n";
        fs::remove_all(root);
        return indexRatio >= 3.0 && lookupRatio > 1.0 ? 0 : 1;
    } catch (const std::exception& error) {
        fs::remove_all(root);
        std::cerr << "STORAGE_FORMAT_V2_BENCHMARK_FAIL: " << error.what() << '\n';
        return 1;
    }
}
