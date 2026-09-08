#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace pacificdb::storage_v2 {

struct IndexMutation {
    enum class Op : std::uint8_t { Clear = 1, Put = 2 };
    Op op{Op::Put};
    std::string value;
    std::string id;
};

struct IndexSegment {
    bool snapshot{false};
    std::vector<IndexMutation> mutations;
};

std::vector<std::filesystem::path> listIndexSegments(
    const std::filesystem::path& directory);

bool writeIndexSegment(const std::filesystem::path& directory,
                       std::vector<IndexMutation> mutations,
                       bool snapshot,
                       std::filesystem::path* writtenPath = nullptr,
                       std::string* error = nullptr);

bool readIndexSegment(const std::filesystem::path& path,
                      IndexSegment& segment,
                      std::string* error = nullptr);

struct SstWriteStats {
    std::uint64_t rows{0};
    std::uint64_t blocks{0};
    std::uint64_t uncompressedBytes{0};
    std::uint64_t storedBytes{0};
};

bool isBinarySst(const std::filesystem::path& path);

bool writeSst(const std::filesystem::path& path,
              const std::vector<std::reference_wrapper<const nlohmann::json>>& rows,
              std::size_t targetBlockBytes,
              SstWriteStats* stats = nullptr,
              std::string* error = nullptr);

bool scanSst(const std::filesystem::path& path,
             const std::function<bool(nlohmann::json&&)>& visitor,
             std::string* error = nullptr);

class MsgpackRowView {
public:
    MsgpackRowView(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}

    std::optional<nlohmann::json> field(std::string_view name) const;
    nlohmann::json materialize() const;
    const std::uint8_t* data() const { return data_; }
    std::size_t size() const { return size_; }

private:
    const std::uint8_t* data_{nullptr};
    std::size_t size_{0};
};

class SstCursor {
public:
    static std::optional<SstCursor> open(const std::filesystem::path& path,
                                         std::string* error = nullptr);
    SstCursor(SstCursor&&) noexcept;
    SstCursor& operator=(SstCursor&&) noexcept;
    ~SstCursor();
    SstCursor(const SstCursor&) = delete;
    SstCursor& operator=(const SstCursor&) = delete;

    bool valid() const;
    std::string_view id() const;
    const MsgpackRowView& row() const;
    bool next(std::string* error = nullptr);

private:
    struct Impl;
    explicit SstCursor(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

bool visitLatestSstRows(
    const std::vector<std::filesystem::path>& oldestToNewest,
    const std::function<bool(const MsgpackRowView&)>& visitor,
    std::string* error = nullptr);

bool visitIndexMutations(
    const std::filesystem::path& directory,
    const std::function<bool(const IndexMutation&)>& visitor,
    std::string* error = nullptr);

bool scanSstViews(const std::filesystem::path& path,
                  const std::function<bool(const MsgpackRowView&)>& visitor,
                  std::string* error = nullptr);

std::optional<nlohmann::json> findInSst(const std::filesystem::path& path,
                                        const std::string& id,
                                        std::string* error = nullptr);

bool verifySst(const std::filesystem::path& path,
               std::string* error = nullptr);

bool buildBloom(const std::filesystem::path& sstPath,
                const std::vector<std::string>& keys,
                std::string* error = nullptr);

// nullopt means the sidecar is absent or invalid and the caller must conservatively read.
std::optional<bool> bloomMayContain(const std::filesystem::path& sstPath,
                                    const std::string& key);

void invalidateCaches(const std::filesystem::path& pathPrefix = {});

}  // namespace pacificdb::storage_v2
