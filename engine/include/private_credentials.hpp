#pragma once
#include <filesystem>
#include <string>

namespace pacificdb {
// Read the same bounded, regular file whose ownership and access are checked.
// Never changes permissions or includes credential contents in diagnostics.
std::string readPrivateCredentials(const std::filesystem::path& path);
}
