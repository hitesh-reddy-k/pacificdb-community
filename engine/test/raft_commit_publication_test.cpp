#include "database_engine.hpp"
#include "lsm.hpp"
#include "owned_test_root.hpp"
#include "raft_core.hpp"
#include "wal.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <atomic>
#include <thread>
#include <chrono>
#ifdef __linux__
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
namespace {
std::atomic<bool> failLogSync{false}, failDirectorySync{false};
std::atomic<int> injectedFailures{0};
bool endsWith(int fd, const char* suffix) {
    char name[64], target[4096];
    std::snprintf(name, sizeof(name), "/proc/self/fd/%d", fd);
    const auto size = ::readlink(name, target, sizeof(target));
    const auto length = std::strlen(suffix);
    return size >= static_cast<ssize_t>(length) &&
        std::memcmp(target + size - length, suffix, length) == 0;
}
}
// Test executable interposition exercises the real syscall-error handling.
// No fault configuration or interposition is linked into the shipped engine.
extern "C" int fdatasync(int fd) {
    if (failLogSync.load() && injectedFailures.load() == 0 && endsWith(fd, "/raft/log.bin")) {
        ++injectedFailures; errno = EIO; return -1;
    }
    return static_cast<int>(::syscall(SYS_fdatasync, fd));
}
extern "C" int fsync(int fd) {
    if (failDirectorySync.load() && endsWith(fd, "/raft")) {
        ++injectedFailures; errno = EIO; return -1;
    }
    return static_cast<int>(::syscall(SYS_fsync, fd));
}
#endif

namespace fs = std::filesystem;
static void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
static void set(const char* key, const std::string& value) {
#ifdef _WIN32
    _putenv_s(key, value.c_str());
#else
    setenv(key, value.c_str(), 1);
#endif
}
#ifdef __linux__
static int freePort() {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    require(fd >= 0, "test socket");
    sockaddr_in address{}; address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    require(::bind(fd,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0,"test bind");
    socklen_t length=sizeof(address);
    require(::getsockname(fd,reinterpret_cast<sockaddr*>(&address),&length)==0,"test port");
    const int port=ntohs(address.sin_port); ::close(fd); return port;
}
static json heartbeat(int port, RaftCore& raft) {
    const int fd=::socket(AF_INET,SOCK_STREAM,0); require(fd>=0,"heartbeat socket");
    timeval timeout{2,0}; ::setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
    sockaddr_in address{}; address.sin_family=AF_INET; address.sin_port=htons(port);
    address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    require(::connect(fd,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0,"heartbeat connect");
    const std::string request=json({{"type","heartbeat"},{"rpcFormat","json"},
        {"clusterId","commit-publication"},{"term",raft.getCurrentTerm()},
        {"leader","probe"},{"leaderLastLogIndex",raft.getLastIndex()},
        {"leaderLastLogTerm",raft.getLastLogTerm()}}).dump();
    const uint32_t size=request.size();
    require(::send(fd,&size,sizeof(size),MSG_NOSIGNAL)==sizeof(size),"heartbeat header");
    require(::send(fd,request.data(),request.size(),MSG_NOSIGNAL)==static_cast<ssize_t>(request.size()),"heartbeat send");
    std::string response; char bytes[4096]; ssize_t count;
    while ((count=::recv(fd,bytes,sizeof(bytes),0))>0) response.append(bytes,count);
    ::close(fd); return json::parse(response);
}
#endif

int main(int argc, char** argv) {
    const bool syncFailure = argc > 1 && std::string(argv[1]) == "--log-sync-failure";
    const bool directoryFailure = argc > 1 && std::string(argv[1]) == "--directory-sync-failure";
    OwnedTestRoot owned("raft-commit-publication");
    const auto root = owned.dataRoot();
    set("DATA_ROOT", root.string()); set("DATA_DIR", root.string());
    set("RAFT_ASYNC_LOG_FSYNC", "0"); set("RAFT_MAX_WORKERS", "2");
    set("RAFT_CLUSTER_ID", "commit-publication");
    fs::create_directories(root / "raft");
    DatabaseEngine::init(root.string()); LSM::init(root.string()); WAL::init();
    int listenerPort=0;
#ifdef __linux__
    listenerPort=freePort();
#endif
    auto& raft = RaftCore::instance(); raft.init({}, "commit-publication", listenerPort, true); raft.start();
    int result = 0;
    try {
        require(raft.replicateAndApply({{"op","CREATE_DB"},{"userId","alice"},{"db","app"}}), "baseline database");
        require(raft.replicateAndApply({{"op","CREATE_COLLECTION"},{"userId","alice"},{"db","app"},{"collection","docs"}}), "baseline collection");
        require(raft.replicateAndApply({{"op","INSERT"},{"userId","alice"},{"db","app"},{"collection","docs"},{"data",{{"id","one"},{"value",1}}}}), "baseline record");
        const auto committed = raft.getCommitIndex();
        const auto applied = raft.getLastApplied();
        const auto blocked = root / "raft/consensus_state.json.tmp";
        if (!syncFailure && !directoryFailure) fs::create_directory(blocked);
#ifdef __linux__
        failLogSync = syncFailure; failDirectorySync = directoryFailure;
#endif
        bool accepted = false;
        std::string writeError;
        try {
            accepted = raft.replicateAndApply({{"op","UPDATE"},{"userId","alice"},{"db","app"},{"collection","docs"},{"data",{{"id","one"},{"value",2}}}}, OperationPriority::NORMAL, 1000);
        } catch (const std::exception& error) { writeError = error.what(); }
        require(!accepted, "write acknowledged without durable commit publication");
        if (directoryFailure) require(writeError == "write_outcome_unknown",
            "uncertain published commit was reported as definitely uncommitted");
#ifdef __linux__
        if (syncFailure || directoryFailure) require(injectedFailures > 0, "syscall failure was not injected");
        failLogSync = false; failDirectorySync = false;
#endif
        require(raft.getCommitIndex() == committed, "failed publication advanced visible commit");
        require(raft.getLastApplied() == applied, "failed publication applied a mutation");
        const auto rows = DatabaseEngine::find("alice","app","docs",{{"id","one"}},1);
        require(rows.size() == 1 && rows.front().at("value") == 1, "failed commit publication changed a record");
        fs::remove(blocked);
        if (directoryFailure || syncFailure) {
            require(!raft.isLeader(), "uncertain durability did not fence leadership");
            require(!raft.isRecoveryComplete(), "uncertain durability left recovery complete");
            require(!raft.replicateAndApply({{"type","raft_noop"}}), "uncertain publication accepted a later write");
#ifdef __linux__
            for (int i=0;i<100&&!raft.isRaftListenerRunning();++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            const auto response=heartbeat(listenerPort,raft);
            require(response.value("success",true)==false &&
                    response.value("matchIndex",uint64_t{0})==0 &&
                    response.value("logMatch",true)==false &&
                    response.value("reason",std::string())=="recovery_incomplete",
                    "fenced node acknowledged heartbeat/tail");
#endif
        } else {
            std::ifstream beforeRetry(root / "raft/consensus_state.json");
            json original; beforeRetry >> original;
            require(original.at("commitIndex").get<uint64_t>() == committed, "failed publication poisoned durable checkpoint");
            require(raft.replicateAndApply({{"type","raft_noop"}}, OperationPriority::NORMAL, 1000), "publication did not recover after storage repair");
            const auto unchanged = DatabaseEngine::find("alice","app","docs",{{"id","one"}},1);
            require(unchanged.size() == 1 && unchanged.front().at("value") == 1, "later append committed a rejected tail");
        }
        std::ifstream state(root / "raft/consensus_state.json");
        json persisted; state >> persisted;
        require(persisted.at("commitIndex").get<uint64_t>() == (directoryFailure ? committed + 1 : raft.getCommitIndex()), "repaired commit was not durable");
        std::cout << "RAFT_COMMIT_PUBLICATION_PASS\n";
    } catch (const std::exception& error) {
        std::cerr << "RAFT_COMMIT_PUBLICATION_FAIL: " << error.what() << '\n'; result = 1;
    }
    fs::remove_all(root / "raft/consensus_state.json.tmp");
    raft.stop(); WAL::shutdown();
    return result;
}
