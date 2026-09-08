#include "snapshot_bundle.hpp"

#include "data_durability.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace pacificdb::snapshot_bundle {
namespace {

constexpr std::array<char, 8> kMagic{'P', 'D', 'B', 'S', 'N', 'A', 'P', '3'};
constexpr uint64_t kHeaderBytes = 16;
constexpr uint64_t kMaxManifestBytes = 64U * 1024U * 1024U;
constexpr size_t kCopyBytes = 1024U * 1024U;

bool isSha256(const std::string& value) {
    if (value.size() != 64) return false;
    for (const unsigned char c : value) {
        if (!std::isxdigit(c)) return false;
    }
    return true;
}

void writeU64(std::ostream& output, uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8) {
        output.put(static_cast<char>((value >> shift) & 0xffU));
    }
}

uint64_t readU64(std::istream& input) {
    uint64_t value = 0;
    for (unsigned shift = 0; shift < 64; shift += 8) {
        const int byte = input.get();
        if (byte == std::char_traits<char>::eof()) {
            throw std::runtime_error("snapshot bundle header is truncated");
        }
        value |= static_cast<uint64_t>(static_cast<unsigned char>(byte)) << shift;
    }
    return value;
}

nlohmann::json& files(nlohmann::json& manifest) {
    if (!manifest.is_object() ||
        manifest.value("snapshot_format", std::string()) != "pdb-snapshot-bundle-v3" ||
        !manifest.contains("lsm_payload") || !manifest["lsm_payload"].is_object() ||
        !manifest["lsm_payload"].contains("files") ||
        !manifest["lsm_payload"]["files"].is_array()) {
        throw std::runtime_error("snapshot bundle manifest is malformed");
    }
    return manifest["lsm_payload"]["files"];
}

const nlohmann::json& files(const nlohmann::json& manifest) {
    return files(const_cast<nlohmann::json&>(manifest));
}

void copyBytes(std::istream& input, std::ostream& output, uint64_t bytes) {
    std::array<char, kCopyBytes> buffer{};
    while (bytes > 0) {
        const size_t wanted = static_cast<size_t>(std::min<uint64_t>(bytes, buffer.size()));
        if (!input.read(buffer.data(), static_cast<std::streamsize>(wanted))) {
            throw std::runtime_error("snapshot artifact is truncated");
        }
        output.write(buffer.data(), static_cast<std::streamsize>(wanted));
        if (!output) throw std::runtime_error("cannot write snapshot artifact");
        bytes -= wanted;
    }
}

}  // namespace

bool isBundle(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::array<char, kMagic.size()> magic{};
    return input.read(magic.data(), static_cast<std::streamsize>(magic.size())) &&
        magic == kMagic;
}

Bundle read(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::array<char, kMagic.size()> magic{};
    if (!input.read(magic.data(), static_cast<std::streamsize>(magic.size())) ||
        magic != kMagic) {
        throw std::runtime_error("snapshot bundle magic is invalid");
    }
    const uint64_t manifestBytes = readU64(input);
    if (manifestBytes == 0 || manifestBytes > kMaxManifestBytes) {
        throw std::runtime_error("snapshot bundle manifest size is invalid");
    }
    const uint64_t fileBytes = std::filesystem::file_size(path);
    if (manifestBytes > fileBytes - std::min(fileBytes, kHeaderBytes)) {
        throw std::runtime_error("snapshot bundle manifest is truncated");
    }
    std::string encoded(static_cast<size_t>(manifestBytes), '\0');
    if (!input.read(encoded.data(), static_cast<std::streamsize>(encoded.size()))) {
        throw std::runtime_error("snapshot bundle manifest is truncated");
    }
    Bundle bundle{nlohmann::json::parse(encoded), kHeaderBytes + manifestBytes};
    const auto& artifacts = files(bundle.manifest);
    uint64_t expectedOffset = 0;
    for (const auto& artifact : artifacts) {
        if (!artifact.is_object() ||
            !artifact.contains("artifactOffset") ||
            !artifact["artifactOffset"].is_number_unsigned() ||
            !artifact.contains("artifactBytes") ||
            !artifact["artifactBytes"].is_number_unsigned() ||
            !artifact.contains("artifactSha256") ||
            !artifact["artifactSha256"].is_string() ||
            artifact.value("contentEncoding", std::string()) != "raw" ||
            artifact.contains("content")) {
            throw std::runtime_error("snapshot bundle artifact metadata is malformed");
        }
        const uint64_t offset = artifact["artifactOffset"].get<uint64_t>();
        const uint64_t bytes = artifact["artifactBytes"].get<uint64_t>();
        if (offset != expectedOffset || bytes > std::numeric_limits<uint64_t>::max() - offset ||
            !isSha256(artifact["artifactSha256"].get<std::string>())) {
            throw std::runtime_error("snapshot bundle artifact range is invalid");
        }
        expectedOffset += bytes;
    }
    if (bundle.dataOffset > fileBytes || expectedOffset != fileBytes - bundle.dataOffset) {
        throw std::runtime_error("snapshot bundle data length is invalid");
    }
    return bundle;
}

void write(const std::filesystem::path& path,
           nlohmann::json manifest,
           const std::vector<ArtifactSource>& sources) {
    auto& artifacts = files(manifest);
    if (artifacts.size() != sources.size()) {
        throw std::runtime_error("snapshot artifact source count does not match manifest");
    }
    uint64_t offset = 0;
    for (size_t i = 0; i < sources.size(); ++i) {
        const auto& source = sources[i].source;
        if (!std::filesystem::is_regular_file(source)) {
            throw std::runtime_error("snapshot artifact source is not a regular file");
        }
        const uint64_t bytes = std::filesystem::file_size(source);
        if (bytes > std::numeric_limits<uint64_t>::max() - offset) {
            throw std::runtime_error("snapshot artifact size overflows bundle");
        }
        const std::string checksum =
            pacificdb::durability::ChecksumCalculator::sha256File(source.string());
        if (!isSha256(checksum)) throw std::runtime_error("cannot checksum snapshot artifact");
        artifacts[i].erase("content");
        artifacts[i]["contentEncoding"] = "raw";
        artifacts[i]["artifactOffset"] = offset;
        artifacts[i]["artifactBytes"] = bytes;
        artifacts[i]["artifactSha256"] = checksum;
        offset += bytes;
    }

    manifest.erase("snapshot_checksum");
    const std::string checksumInput = manifest.dump();
    manifest["snapshot_checksum"] =
        pacificdb::durability::ChecksumCalculator::sha256(
            checksumInput.data(), checksumInput.size());
    const std::string encoded = manifest.dump();
    if (encoded.empty() || encoded.size() > kMaxManifestBytes) {
        throw std::runtime_error("snapshot bundle manifest is too large");
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("cannot create snapshot bundle");
    output.write(kMagic.data(), static_cast<std::streamsize>(kMagic.size()));
    writeU64(output, encoded.size());
    output.write(encoded.data(), static_cast<std::streamsize>(encoded.size()));
    for (const auto& source : sources) {
        std::ifstream input(source.source, std::ios::binary);
        if (!input) throw std::runtime_error("cannot read snapshot artifact");
        copyBytes(input, output, std::filesystem::file_size(source.source));
    }
    output.flush();
    if (!output) throw std::runtime_error("cannot finish snapshot bundle");
}

void extract(const std::filesystem::path& bundlePath,
             const Bundle& bundle,
             size_t artifactIndex,
             const std::filesystem::path& target) {
    const auto& artifacts = files(bundle.manifest);
    if (artifactIndex >= artifacts.size()) {
        throw std::runtime_error("snapshot artifact index is invalid");
    }
    const auto& artifact = artifacts[artifactIndex];
    const uint64_t offset = artifact["artifactOffset"].get<uint64_t>();
    const uint64_t bytes = artifact["artifactBytes"].get<uint64_t>();
    const uint64_t maxOffset =
        static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max());
    if (offset > maxOffset || bundle.dataOffset > maxOffset - offset) {
        throw std::runtime_error("snapshot artifact offset exceeds stream limits");
    }
    std::ifstream input(bundlePath, std::ios::binary);
    input.seekg(static_cast<std::streamoff>(bundle.dataOffset + offset));
    std::ofstream output(target, std::ios::binary | std::ios::trunc);
    if (!input || !output) throw std::runtime_error("cannot open snapshot artifact");
    copyBytes(input, output, bytes);
    output.flush();
    output.close();
    if (!output || std::filesystem::file_size(target) != bytes ||
        pacificdb::durability::ChecksumCalculator::sha256File(target.string()) !=
            artifact["artifactSha256"].get<std::string>()) {
        std::error_code ec;
        std::filesystem::remove(target, ec);
        throw std::runtime_error("snapshot artifact checksum mismatch");
    }
}

}  // namespace pacificdb::snapshot_bundle
