#include <iostream>
#include <filesystem>
#include <fstream>
#include <string>
#include <nlohmann/json.hpp>
#include "../include/raft_core.hpp"
#include "../include/owned_test_root.hpp"

using json = nlohmann::json;
namespace fs = std::filesystem;

int main() {
    const OwnedTestRoot ownedRoot("snapshot-install");
    const std::string testRoot = ownedRoot.dataRoot().string();
    fs::create_directories(testRoot + "/raft");

    // set DATA_ROOT env for RaftCore
#ifdef _WIN32
    _putenv_s("DATA_ROOT", testRoot.c_str());
#else
    setenv("DATA_ROOT", testRoot.c_str(), 1);
#endif

    // create a simple binary raft log with 5 entries
    std::string logPath = testRoot + "/raft/log.bin";
    std::ofstream out(logPath, std::ios::binary | std::ios::trunc);
    for (uint64_t i = 1; i <= 5; ++i) {
        uint64_t idx = i;
        uint64_t term = 1;
        std::string payload = json{{"id", i}, {"action","test"}}.dump();
        uint32_t sz = static_cast<uint32_t>(payload.size());
        out.write(reinterpret_cast<char*>(&idx), sizeof(idx));
        out.write(reinterpret_cast<char*>(&term), sizeof(term));
        out.write(reinterpret_cast<char*>(&sz), sizeof(sz));
        out.write(payload.data(), sz);
    }
    out.close();

    auto &r = RaftCore::instance();
    // create snapshot including index 3
    bool ok = r.createSnapshot(3);
    if (!ok) { std::cerr << "createSnapshot failed" << std::endl; return 2; }
    // compact log up to 3
    bool c = r.compactRaftLog(3);
    if (!c) { std::cerr << "compactRaftLog failed" << std::endl; return 3; }

    // verify log now contains only indices >3 (i.e., 4 and 5)
    std::ifstream in(logPath, std::ios::binary);
    int count = 0; std::vector<uint64_t> idxs;
    while (!in.eof()) {
        uint64_t idx=0; uint64_t term=0; uint32_t sz=0;
        in.read(reinterpret_cast<char*>(&idx), sizeof(idx));
        if (!in) break;
        in.read(reinterpret_cast<char*>(&term), sizeof(term));
        in.read(reinterpret_cast<char*>(&sz), sizeof(sz));
        if (!in) break;
        in.seekg(sz, std::ios::cur);
        idxs.push_back(idx);
    }
    in.close();
    if (idxs.size() == 2 && idxs[0] == 4 && idxs[1] == 5) {
        std::cout << "Test passed: compact kept indices 4 and 5" << std::endl;
        return 0;
    }
    std::cerr << "Test failed: remaining indices:";
    for (auto v: idxs) std::cerr << " " << v;
    std::cerr << std::endl;
    return 4;
}
