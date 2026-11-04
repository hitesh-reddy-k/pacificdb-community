#pragma once
#include <filesystem>
#include <system_error>

namespace pacificdb::durable_file {
// Flush an existing regular file; never create a missing artifact.
bool syncFile(const std::filesystem::path& path);
// POSIX: fsync directory entries. Windows: validate the directory; publication
// must use rename() below, whose native operation requests WRITE_THROUGH.
// This does not claim a POSIX directory-fsync equivalent on Windows.
bool syncDirectory(const std::filesystem::path& path);
bool rename(const std::filesystem::path& source, const std::filesystem::path& target,
            std::error_code& error);
void rename(const std::filesystem::path& source, const std::filesystem::path& target);
}
