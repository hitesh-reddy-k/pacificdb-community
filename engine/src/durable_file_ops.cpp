#include "durable_file_ops.hpp"
#include <cerrno>
#include <cstdio>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace pacificdb::durable_file {
bool syncFile(const std::filesystem::path& path) {
#ifdef _WIN32
    const HANDLE handle=CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,
        FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,nullptr);
    if(handle==INVALID_HANDLE_VALUE)return false;
    const bool synced=FlushFileBuffers(handle)!=0;
    CloseHandle(handle);return synced;
#else
    const int fd=::open(path.c_str(),O_RDONLY|O_CLOEXEC);
    if(fd<0)return false;
    const bool synced=::fsync(fd)==0;
    ::close(fd);return synced;
#endif
}
bool syncDirectory(const std::filesystem::path& path) {
#ifdef _WIN32
    // Win32 has no documented POSIX directory-fsync equivalent. Callers publish
    // with MOVEFILE_WRITE_THROUGH below, as the existing manifest writer did.
    const DWORD attributes=GetFileAttributesW(path.c_str());
    return attributes!=INVALID_FILE_ATTRIBUTES && (attributes&FILE_ATTRIBUTE_DIRECTORY)!=0;
#else
    const int fd=::open(path.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    if(fd<0)return false;
    const bool synced=::fsync(fd)==0;
    ::close(fd);return synced;
#endif
}
bool rename(const std::filesystem::path& source,const std::filesystem::path& target,
            std::error_code& error) {
#ifdef _WIN32
    const DWORD attributes=GetFileAttributesW(source.c_str());
    if(attributes==INVALID_FILE_ATTRIBUTES){error={static_cast<int>(GetLastError()),std::system_category()};return false;}
    DWORD flags=MOVEFILE_WRITE_THROUGH;
    if((attributes&FILE_ATTRIBUTE_DIRECTORY)==0)flags|=MOVEFILE_REPLACE_EXISTING;
    if(!MoveFileExW(source.c_str(),target.c_str(),flags)){
        error={static_cast<int>(GetLastError()),std::system_category()};return false;
    }
#else
    if(::rename(source.c_str(),target.c_str())!=0){error={errno,std::generic_category()};return false;}
#endif
    error.clear();return true;
}
void rename(const std::filesystem::path& source,const std::filesystem::path& target) {
    std::error_code error;
    if(!pacificdb::durable_file::rename(source,target,error))throw std::filesystem::filesystem_error("cannot publish storage artifact",source,target,error);
}
}
