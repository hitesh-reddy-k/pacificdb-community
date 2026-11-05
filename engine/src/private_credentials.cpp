#include "private_credentials.hpp"
#include <array>
#include <stdexcept>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#include <aclapi.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace pacificdb {
std::string readPrivateCredentials(const std::filesystem::path& path) {
    constexpr size_t limit = 1024 * 1024;
    std::string result;
    std::array<char, 4096> buffer{};
#ifdef _WIN32
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ | READ_CONTROL,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("accounts file is unreadable");
    struct Close { HANDLE value; ~Close() { CloseHandle(value); } } close{file};
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file, &info) || GetFileType(file) != FILE_TYPE_DISK ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
        throw std::runtime_error("accounts file must be a regular non-reparse file");
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        throw std::runtime_error("accounts file identity unavailable");
    Close closeToken{token};
    DWORD bytes = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
    std::vector<unsigned char> storage(bytes);
    if (!bytes || !GetTokenInformation(token, TokenUser, storage.data(), bytes, &bytes))
        throw std::runtime_error("accounts file identity unavailable");
    const auto user = reinterpret_cast<TOKEN_USER*>(storage.data())->User.Sid;
    PSID owner = nullptr; PACL dacl = nullptr; PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD error = GetSecurityInfo(file, SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
        &owner, nullptr, &dacl, nullptr, &descriptor);
    struct Free { void* value; ~Free() { if (value) LocalFree(value); } } free{descriptor};
    if (error != ERROR_SUCCESS || !owner || !EqualSid(owner, user) || !dacl || !IsValidAcl(dacl))
        throw std::runtime_error("accounts file requires current-user ownership and a private ACL");
    for (DWORD i = 0; i < dacl->AceCount; ++i) {
        void* ace = nullptr;
        if (!GetAce(dacl, i, &ace)) throw std::runtime_error("accounts file ACL is unreadable");
        const auto header = static_cast<ACE_HEADER*>(ace);
        if (header->AceFlags & INHERIT_ONLY_ACE) continue;
        if (header->AceType == ACCESS_DENIED_ACE_TYPE) continue;
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE)
            throw std::runtime_error("accounts file ACL contains unsupported access rules");
        const auto allowed = static_cast<ACCESS_ALLOWED_ACE*>(ace);
        const auto sid = const_cast<DWORD*>(&allowed->SidStart);
        // SYSTEM and local administrators already control the host and may
        // retain explicit management access; ordinary users/groups may not.
        if (allowed->Mask && !EqualSid(sid, user) &&
            !IsWellKnownSid(sid, WinLocalSystemSid) &&
            !IsWellKnownSid(sid, WinBuiltinAdministratorsSid))
            throw std::runtime_error("accounts file must not grant access to other users or groups");
    }
    for (;;) {
        DWORD count = 0;
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr))
            throw std::runtime_error("accounts file read failed");
        if (!count) break;
        if (count > limit - result.size()) throw std::runtime_error("accounts file exceeds 1 MiB");
        result.append(buffer.data(), count);
    }
#else
    const int file = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (file < 0) throw std::runtime_error("accounts file is unreadable");
    struct Close { int value; ~Close() { ::close(value); } } close{file};
    struct stat info{};
    if (::fstat(file, &info) != 0 || !S_ISREG(info.st_mode) || info.st_uid != ::geteuid() ||
        (info.st_mode & 0077) != 0)
        throw std::runtime_error("accounts file requires current-user ownership and no group/other access");
    for (;;) {
        const auto count = ::read(file, buffer.data(), buffer.size());
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) throw std::runtime_error("accounts file read failed");
        if (!count) break;
        if (static_cast<size_t>(count) > limit - result.size())
            throw std::runtime_error("accounts file exceeds 1 MiB");
        result.append(buffer.data(), static_cast<size_t>(count));
    }
#endif
    return result;
}
}
