#include "lsm_manifest.hpp"
#include "owned_test_root.hpp"
#include "storage_format_v2.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;

static void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

static void writeOneRowSst(const fs::path& path, const std::string& id) {
    const json row = {{"id", id}, {"value", id}};
    const std::vector<std::reference_wrapper<const json>> rows{row};
    std::string error;
    require(pacificdb::storage_v2::writeSst(path, rows, 4096, nullptr, &error), error);
    require(pacificdb::storage_v2::verifySst(path, &error), error);
}

int main() {
    const OwnedTestRoot ownedRoot("lsm-manifest");
    const fs::path lsm = ownedRoot.dataRoot() / "users" / "db" / "items.lsm";
    fs::create_directories(lsm);
    try {
        const fs::path first = lsm / "001.sst";
        const fs::path second = lsm / "002.sst";
        const fs::path orphan = lsm / "orphan.sst";
        writeOneRowSst(first, "a");
        writeOneRowSst(second, "b");
        writeOneRowSst(orphan, "orphan");

        std::vector<fs::path> published;
        std::string error;
        require(LsmManifestStore::publishedSsts(lsm, published, &error), error);
        require(published == std::vector<fs::path>({first, second, orphan}),
                "manifest-free collections must expose sorted legacy SSTs");

        LsmManifest generation1;
        generation1.generation = 1;
        generation1.coveredWalLsn = 10;
        generation1.sstFiles = {first.filename().string()};
        require(LsmManifestStore::publish(lsm, generation1, {first}, &error), error);
        require(LsmManifestStore::publishedSsts(lsm, published, &error), error);
        require(published == std::vector<fs::path>({first}),
                "published manifest hides unlisted orphan SSTs");

        LsmManifest generation2;
        generation2.generation = 2;
        generation2.coveredWalLsn = 20;
        generation2.sstFiles = {second.filename().string(), first.filename().string()};
        require(LsmManifestStore::publish(lsm, generation2, {second}, &error), error);
        const auto loaded = LsmManifestStore::load(lsm);
        require(loaded.status == LsmManifestLoadStatus::OK &&
                    loaded.manifest.generation == 2 && loaded.manifest.coveredWalLsn == 20,
                "latest complete manifest loads");
        require(LsmManifestStore::publishedSsts(lsm, published, &error), error);
        require(published == std::vector<fs::path>({first, second}),
                "manifest SST order is stable by filename");

        LsmManifest smuggled = generation2;
        smuggled.generation = 3;
        smuggled.sstFiles.push_back(orphan.filename().string());
        require(!LsmManifestStore::publish(lsm, smuggled, {}, &error),
                "manifest cannot add an SST that was neither previously published nor new");
        require(!LsmManifestStore::publish(lsm, generation2, {}, &error),
                "manifest generation cannot repeat or regress");

        LsmManifest unsafe = generation2;
        unsafe.generation = 3;
        unsafe.sstFiles = {"../escape.sst"};
        require(!LsmManifestStore::publish(lsm, unsafe, {}, &error),
                "manifest rejects paths outside the LSM directory");

        {
            std::ofstream corrupt(lsm / "_manifest.json", std::ios::trunc);
            corrupt << "{not-json";
        }
        const auto corrupt = LsmManifestStore::load(lsm);
        require(corrupt.status == LsmManifestLoadStatus::ERROR,
                "corrupt manifest fails closed");
        published.clear();
        require(!LsmManifestStore::publishedSsts(lsm, published, &error) && published.empty(),
                "corrupt manifest never falls back to orphan enumeration");

        std::cout << "lsm_manifest_test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "lsm_manifest_test failed: " << error.what() << '\n';
        return 1;
    }
}
