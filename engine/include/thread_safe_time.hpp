#pragma once
#include <ctime>

namespace pacificdb::time_format {
// The C time functions return shared process storage. Every caller supplies its
// own result so audit/log formatting cannot race another request's conversion.
inline bool utc(std::time_t value, std::tm& result) noexcept {
#ifdef _WIN32
    return ::gmtime_s(&result, &value) == 0;
#else
    return ::gmtime_r(&value, &result) != nullptr;
#endif
}
inline bool local(std::time_t value, std::tm& result) noexcept {
#ifdef _WIN32
    return ::localtime_s(&result, &value) == 0;
#else
    return ::localtime_r(&value, &result) != nullptr;
#endif
}
}
