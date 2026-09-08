#include "data_durability.hpp"
#include "lsm.hpp"
#include "owned_test_root.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#ifndef _WIN32
#include <sys/wait.h>
#endif

namespace fs = std::filesystem;
using json = nlohmann::json;

static constexpr int kHalf = 10000;
static constexpr uint64_t kCrashLsn = 20000;
static const std::string kUser = "checkpoint_user";
static const std::string kDatabase = "checkpoint_db";
static const std::string kCollection = "documents";

static std::string payloadFor(int index) {
    std::string payload(128, static_cast<char>('a' + index % 26));
    const std::string prefix = std::to_string(index) + ":";
    payload.replace(0, prefix.size(), prefix);
    return payload;
}

static std::string expectedDigest() {
    std::string bytes;
    for (int index = 0; index < kHalf * 2; ++index) {
        bytes += "doc-" + std::to_string(index) + '\0' + payloadFor(index) + '\n';
    }
    return pacificdb::durability::ChecksumCalculator::sha256(bytes.data(), bytes.size());
}

static void appendRange(int begin, int end) {
    for (int start = begin; start < end; start += 1000) {
        std::vector<json> docs;
        for (int index = start; index < std::min(start + 1000, end); ++index) {
            docs.push_back({{"id", "doc-" + std::to_string(index)},
                            {"payload", payloadFor(index)}});
        }
        const auto result = LSM::putMany(kUser, kDatabase, kCollection, docs);
        if (result.value("status", std::string()) != "ok") {
            throw std::runtime_error("fixture write failed");
        }
    }
}

static int initialize(const fs::path& root) {
    setenv("WAL_GROUP_COMMIT", "false", 1);
    setenv("WAL_FSYNC_ENABLED", "true", 1);
    setenv("WAL_SEGMENT_MAX_BYTES", "524288", 1);
    LSM::init(root.string());
    appendRange(0, kHalf);
    LSM::forceFlush();
    std::ofstream(root / "expected.sha256") << expectedDigest();
    return 0;
}

static int crashAtCheckpoint(const fs::path& root) {
    setenv("WAL_GROUP_COMMIT", "false", 1);
    setenv("WAL_FSYNC_ENABLED", "true", 1);
    setenv("WAL_SEGMENT_MAX_BYTES", "524288", 1);
    LSM::init(root.string());
    LSM::restoreFromWal();
    appendRange(kHalf, kHalf * 2);
    LSM::forceFlush();
    return 90;  // selected failpoint must terminate before this line
}

static uint64_t walBytes(const fs::path& root) {
    uint64_t bytes = 0;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file()) {
            const auto path = it->path().string();
            if (path.find(".wal") != std::string::npos) bytes += it->file_size();
        }
    }
    if (ec) throw std::runtime_error("cannot inspect WAL bytes: " + ec.message());
    return bytes;
}

static size_t sstFiles(const fs::path& root) {
    size_t count = 0;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file() && it->path().extension() == ".sst") ++count;
    }
    if (ec) throw std::runtime_error("cannot inspect SST files: " + ec.message());
    return count;
}

static int verify(const fs::path& root, const fs::path& resultPath) {
    setenv("WAL_GROUP_COMMIT", "false", 1);
    setenv("WAL_FSYNC_ENABLED", "true", 1);
    setenv("WAL_SEGMENT_MAX_BYTES", "524288", 1);
    LSM::init(root.string());
    LSM::restoreFromWal();
    auto docs = LSM::getAll(kUser, kDatabase, kCollection);
    std::sort(docs.begin(), docs.end(), [](const json& left, const json& right) {
        const int leftId = std::stoi(left.at("id").get<std::string>().substr(4));
        const int rightId = std::stoi(right.at("id").get<std::string>().substr(4));
        return leftId < rightId;
    });
    std::string bytes;
    for (const auto& doc : docs) {
        bytes += doc.at("id").get<std::string>() + '\0' +
                 doc.at("payload").get<std::string>() + '\n';
    }
    const auto digest = pacificdb::durability::ChecksumCalculator::sha256(
        bytes.data(), bytes.size());
    std::ofstream(resultPath) << json{{"count", docs.size()}, {"digest", digest},
                                     {"sst_files", sstFiles(root)},
                                     {"wal_bytes", walBytes(root)}}.dump();
    return docs.size() == kHalf * 2 ? 0 : 4;
}

static std::string quote(const fs::path& path) { return "'" + path.string() + "'"; }

static int exitStatus(int status) {
#ifndef _WIN32
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#else
    return status;
#endif
}

static int runOne(const fs::path& executable, const std::string& failpoint) {
    const OwnedTestRoot ownedRoot("lsm-checkpoint-crash");
    const fs::path root = ownedRoot.dataRoot();
    if (std::system((quote(executable) + " --init " + quote(root)).c_str()) != 0) {
        throw std::runtime_error("generation-1 setup failed");
    }
    setenv("PACIFICDB_TEST_MODE", "lsm_checkpoint_crash", 1);
    setenv("PACIFICDB_TEST_FAILPOINT_CONFIRM",
           "I_UNDERSTAND_THIS_PROCESS_WILL_TERMINATE", 1);
    setenv("PACIFICDB_TEST_FAILPOINT", failpoint.c_str(), 1);
    setenv("PACIFICDB_TEST_FAILPOINT_INDEX", std::to_string(kCrashLsn).c_str(), 1);
    setenv("PACIFICDB_TEST_FAILPOINT_ACTION", "crash", 1);
    const int crashed = exitStatus(std::system(
        (quote(executable) + " --crash " + quote(root)).c_str()));
    unsetenv("PACIFICDB_TEST_MODE");
    unsetenv("PACIFICDB_TEST_FAILPOINT_CONFIRM");
    unsetenv("PACIFICDB_TEST_FAILPOINT");
    unsetenv("PACIFICDB_TEST_FAILPOINT_INDEX");
    unsetenv("PACIFICDB_TEST_FAILPOINT_ACTION");
    if (crashed != 86) throw std::runtime_error("failpoint was not reached");

    const fs::path first = root / "verify-first.json";
    const fs::path second = root / "verify-second.json";
    if (std::system((quote(executable) + " --verify " + quote(root) + " " + quote(first)).c_str()) != 0 ||
        std::system((quote(executable) + " --verify " + quote(root) + " " + quote(second)).c_str()) != 0) {
        throw std::runtime_error("post-crash verification failed");
    }
    json firstResult, secondResult;
    std::ifstream(first) >> firstResult;
    std::ifstream(second) >> secondResult;
    std::string expected;
    std::ifstream(root / "expected.sha256") >> expected;
    if (firstResult.value("count", 0) != kHalf * 2 ||
        firstResult.value("digest", std::string()) != expected || firstResult != secondResult) {
        throw std::runtime_error("post-crash count, digest, SST, or WAL stability mismatch");
    }
    std::cout << "CHECKPOINT_CRASH_PASS " << failpoint << " " << firstResult.dump() << '\n';
    return 0;
}

int main(int argc, char** argv) {
    try {
        if (argc == 3 && std::string(argv[1]) == "--init") return initialize(argv[2]);
        if (argc == 3 && std::string(argv[1]) == "--crash") return crashAtCheckpoint(argv[2]);
        if (argc == 4 && std::string(argv[1]) == "--verify") return verify(argv[2], argv[3]);
        if (argc == 3 && std::string(argv[1]) == "--run-one") {
            return runOne(fs::canonical(argv[0]), argv[2]);
        }
        throw std::runtime_error("usage: --run-one FAILPOINT");
    } catch (const std::exception& error) {
        std::cerr << "lsm_checkpoint_crash_driver failed: " << error.what() << '\n';
        return 1;
    }
}
