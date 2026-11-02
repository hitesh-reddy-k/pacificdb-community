#include "security_manager.hpp"
#include "thread_safe_time.hpp"
#include <array>
#include <cassert>
#include <iostream>
#include <thread>

int main() {
    const std::array<std::time_t, 3> times{{0, 946684800, 1609459200}};
    const std::array<const char*, 3> expected{{"1970-01-01T00:00:00Z",
        "2000-01-01T00:00:00Z", "2021-01-01T00:00:00Z"}};
    std::array<std::thread, 8> threads;
    for (size_t worker = 0; worker < threads.size(); ++worker) {
        threads[worker] = std::thread([&, worker] {
            for (size_t round = 0; round < 2000; ++round) {
                const size_t index = (round + worker) % times.size();
                std::tm utc{}, local{};
                assert(pacificdb::time_format::utc(times[index], utc));
                const auto retained = utc;
                assert(pacificdb::time_format::local(times[(index+1)%times.size()], local));
                assert(utc.tm_year == retained.tm_year && utc.tm_mday == retained.tm_mday);
                pacificdb::security::AuditLogEntry entry{};
                entry.timestamp = std::chrono::system_clock::from_time_t(times[index]);
                assert(entry.toJson().at("timestamp") == expected[index]);
            }
        });
    }
    for (auto& thread : threads) thread.join();
    std::cout << "THREAD_SAFE_TIME_AND_CONCURRENT_AUDIT_FORMAT_PASS\n";
}
