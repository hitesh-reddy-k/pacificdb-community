#include "database_engine.hpp"
#include "lsm.hpp"
#include "owned_test_root.hpp"
#include "raft_core.hpp"
#include "wal.hpp"
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <thread>

int main() {
    using namespace std::chrono_literals;
    OwnedTestRoot owned("raft-health-recovery");
    const auto root = owned.dataRoot();
    setenv("DATA_ROOT", root.c_str(), 1);
    setenv("DATA_DIR", root.c_str(), 1);
    setenv("RAFT_MAX_WORKERS", "2", 1);
    setenv("WAL_FSYNC_ENABLED", "true", 1);
    setenv("RAFT_ASYNC_LOG_FSYNC", "0", 1);
    std::filesystem::create_directories(root / "raft");
    std::ofstream(root / "raft/current_term.txt") << "1\n";
    DatabaseEngine::init(root.string());
    LSM::init(root.string());
    WAL::init();
    auto& raft = RaftCore::instance();
    raft.init({}, "health-test", 0, true);
    raft.start();
    int result = 0;
    try {
        const json stale = {{"type", "raft_noop"}, {"leader_term", 0}};
        for (int i = 0; i < 100; ++i) {
            if (raft.replicateAndApply(stale)) throw std::runtime_error("stale write accepted");
        }
        auto deadline = std::chrono::steady_clock::now() + 8s;
        while (raft.isSystemHealthy() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(20ms);
        if (raft.isSystemHealthy()) throw std::runtime_error("health gate did not trip");
        // Requests continue while the original failure burst ages out. Rejections
        // must not renew the unhealthy state indefinitely.
        bool recovered = false;
        deadline = std::chrono::steady_clock::now() + 12s;
        while (std::chrono::steady_clock::now() < deadline) {
            if (raft.replicateAndApply({{"type", "raft_noop"}}, OperationPriority::NORMAL, 1000)) {
                recovered = true;
                break;
            }
            std::this_thread::sleep_for(20ms);
        }
        if (!recovered) throw std::runtime_error("health rejections perpetuated outage");
        if (raft.getWriteReplicationMetrics().value("healthRejectedRequests", 0ULL) == 0)
            throw std::runtime_error("rejection accounting was lost");
        if (raft.getLastApplied() == 0 || raft.getLastApplied() > raft.getCommitIndex())
            throw std::runtime_error("recovered operation was not committed and applied");
        std::cout << "RAFT_HEALTH_RECOVERY_PASS\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        result = 1;
    }
    raft.stop();
    WAL::shutdown();
    return result;
}
