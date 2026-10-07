#include "owned_test_root.hpp"
#include "security_manager.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>
#ifndef _WIN32
#include <csignal>
#include <sys/resource.h>
#include <unistd.h>
#endif

using namespace pacificdb::security;

int main() {
    const OwnedTestRoot root("security-audit");
    auto& security = SecurityManager::instance();
    security.initialize(root.dataRoot().string());
    security.enableAuditLogging(true);
    assert(security.createUser("reader", "fixture-password", Role::READ_ONLY, "test"));
    const auto login = security.authenticate("reader", "fixture-password", "127.0.0.1");
    assert(login);
    const auto before = security.getAuditLog(100).size();
    assert(!security.hasPermission(login->token, Permission::WRITE));
    const auto denied = security.getAuditLog(100);
    assert(denied.size() == before + 1);
    assert(denied.front().username == "reader");
    assert(denied.front().action == AuditAction::PERMISSION_DENIED);
    assert(!denied.front().success);

    const auto audit = root.dataRoot() / "security" / "audit.log";
    std::filesystem::rename(audit, audit.string() + ".saved");
    std::filesystem::create_directory(audit);
    std::ostringstream alerts;
    auto* previous = std::cerr.rdbuf(alerts.rdbuf());
    for (int i = 0; i < 2; ++i)
        security.logAudit("test", "", AuditAction::PERMISSION_DENIED, "db", "sink test", false);
    auto metrics = security.getSecurityMetrics();
    assert(!metrics.at("auditHealthy").get<bool>());
    assert(metrics.at("auditWriteFailures") == 2);
    assert(alerts.str().find("audit_write_failed") != std::string::npos);
    assert(alerts.str().find("audit_write_failed") == alerts.str().rfind("audit_write_failed"));
    std::filesystem::remove(audit); // Empty directory in this owned fixture only.
    std::filesystem::rename(audit.string() + ".saved", audit);
    security.logAudit("test", "", AuditAction::PERMISSION_DENIED, "db", "recovered", false);
    assert(security.getSecurityMetrics().at("auditHealthy").get<bool>());
    assert(alerts.str().find("audit_write_recovered") != std::string::npos);
#ifndef _WIN32
    // An existing writable file must also report failures during write/flush.
    rlimit originalLimit{};
    assert(getrlimit(RLIMIT_FSIZE, &originalLimit) == 0);
    auto writeLimit = originalLimit;
    writeLimit.rlim_cur = std::filesystem::file_size(audit);
    auto previousSignal = std::signal(SIGXFSZ, SIG_IGN);
    assert(previousSignal != SIG_ERR);
    assert(setrlimit(RLIMIT_FSIZE, &writeLimit) == 0);
    security.logAudit("test", "", AuditAction::QUERY, "db", "write failure", true);
    assert(!security.getSecurityMetrics().at("auditHealthy").get<bool>());
    assert(setrlimit(RLIMIT_FSIZE, &originalLimit) == 0);
    std::signal(SIGXFSZ, previousSignal);
    security.logAudit("test", "", AuditAction::QUERY, "db", "write recovered", true);
    assert(security.getSecurityMetrics().at("auditHealthy").get<bool>());
#endif
    std::cerr.rdbuf(previous);

    // Concurrent appenders must leave complete JSONL records, not mixed fragments.
    std::thread a([&] { for (int i = 0; i < 100; ++i)
        security.logAudit("worker-a", "", AuditAction::QUERY, "db", "safe", true); });
    std::thread b([&] { for (int i = 0; i < 100; ++i)
        security.logAudit("worker-b", "", AuditAction::QUERY, "db", "safe", true); });
    a.join(); b.join();
    std::ifstream input(audit);
    std::string line;
    int workers = 0;
    while (std::getline(input, line)) {
        const auto event = json::parse(line);
        if (event.at("username") == "worker-a" || event.at("username") == "worker-b") ++workers;
        assert(line.find(login->token) == std::string::npos);
        assert(line.find("fixture-password") == std::string::npos);
    }
    assert(workers == 200);
#ifndef _WIN32
    if (geteuid() != 0) {
        for (int i = 0; i < 5000; ++i)
            security.logAudit("rotation", "", AuditAction::QUERY, "db", "safe", true);
        const auto directory = audit.parent_path();
        const auto originalPermissions = std::filesystem::status(directory).permissions();
        std::filesystem::permissions(directory, std::filesystem::perms::owner_read |
            std::filesystem::perms::owner_exec);
        security.rotateAuditLog();
        assert(!security.getSecurityMetrics().at("auditHealthy").get<bool>());
        security.logAudit("rotation", "", AuditAction::QUERY, "db", "retry", true);
        assert(!security.getSecurityMetrics().at("auditHealthy").get<bool>());
        for (int i = 0; i < 5000; ++i)
            security.logAudit("rotation", "", AuditAction::QUERY, "db", "bounded", true);
        assert(security.getAuditLog(20000).size() == 10000);
        assert(security.getSecurityMetrics().at("auditBufferEvictions").get<uint64_t>() > 0);
        std::filesystem::permissions(directory, originalPermissions);
        security.rotateAuditLog();
        assert(security.getSecurityMetrics().at("auditHealthy").get<bool>());
        assert(security.getAuditLog(10000).size() == 5000);
        security.logAudit("rotation", "", AuditAction::QUERY, "db", "append failure", true);
        std::filesystem::rename(audit, audit.string() + ".saved");
        std::filesystem::create_directory(audit);
        security.rotateAuditLog();
        assert(!security.getSecurityMetrics().at("auditHealthy").get<bool>());
        std::filesystem::remove(audit);
        std::filesystem::rename(audit.string() + ".saved", audit);
        security.logAudit("rotation", "", AuditAction::QUERY, "db", "recovered", true);
        assert(security.getSecurityMetrics().at("auditHealthy").get<bool>());
    }
#endif
    std::cout << "SECURITY_AUDIT_PASS\n";
}
