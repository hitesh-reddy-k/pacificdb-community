#pragma once

#include <nlohmann/json.hpp>

#include <stdexcept>
#include <string>

namespace pacificdb::cli {

struct ShellContext {
    std::string database;
    std::string projectId;
};

class MediaUploadInterrupted final : public std::runtime_error {
public:
    MediaUploadInterrupted(std::string uploadId, long long nextChunk,
                           long long receivedChunks, long long receivedBytes,
                           std::string cause);
    nlohmann::json publicResponse() const;

private:
    std::string uploadId_;
    long long nextChunk_;
    long long receivedChunks_;
    long long receivedBytes_;
};

nlohmann::json parseShellCommand(const std::string& line,
                                 const ShellContext& context);
const char* shellHelp();

}  // namespace pacificdb::cli
