#include "env_config.hpp"

#include <cassert>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace {

void setPathEnvironment(const char* name, const fs::path& value) {
#ifdef _WIN32
    std::wstring wideName;
    while (*name) wideName.push_back(static_cast<wchar_t>(*name++));
    assert(_wputenv_s(wideName.c_str(), value.c_str()) == 0);
#else
    assert(::setenv(name, value.c_str(), 1) == 0);
#endif
}

void clearEnvironment(const char* name) {
#ifdef _WIN32
    std::wstring wideName;
    while (*name) wideName.push_back(static_cast<wchar_t>(*name++));
    assert(_wputenv_s(wideName.c_str(), L"") == 0);
#else
    assert(::unsetenv(name) == 0);
#endif
}

}  // namespace

int main() {
    const auto suffix = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const fs::path root = fs::temp_directory_path() /
        fs::u8path(std::string(u8"PacificDB config తెలుగు space ") + suffix);
    const fs::path data = root / "data";
    const fs::path backup = root / "backup";
    const fs::path restore = root / "restore";

    setPathEnvironment("DATA_ROOT", data);
    setPathEnvironment("BACKUP_ROOT", backup);
    setPathEnvironment("RESTORE_DIR", restore);
    try {
        EnvConfig::reload();
        const auto config = EnvConfig::getStorageConfig();
        assert(fs::u8path(config.dataRoot) == data.lexically_normal());
        assert(fs::u8path(config.backupDir) == backup.lexically_normal());
        assert(fs::u8path(config.restoreDir) == restore.lexically_normal());
        fs::create_directories(fs::u8path(config.dataRoot));
        fs::create_directories(fs::u8path(config.backupDir));
        fs::create_directories(fs::u8path(config.restoreDir));
        assert(fs::is_directory(data));
        assert(fs::is_directory(backup));
        assert(fs::is_directory(restore));
    } catch (...) {
        fs::remove_all(root);
        clearEnvironment("DATA_ROOT");
        clearEnvironment("BACKUP_ROOT");
        clearEnvironment("RESTORE_DIR");
        throw;
    }
    fs::remove_all(root);
    clearEnvironment("DATA_ROOT");
    clearEnvironment("BACKUP_ROOT");
    clearEnvironment("RESTORE_DIR");
    return 0;
}
