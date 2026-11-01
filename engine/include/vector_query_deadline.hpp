#pragma once
#include <algorithm>
#include <chrono>
#include <optional>

namespace pacificdb::vector_query {
using Clock = std::chrono::steady_clock;
using Deadline = std::optional<Clock::time_point>;
// Pass this value through synchronous child rankers/fallbacks. No borrowed
// request state or process-global timer can be reused by a different request.
inline Clock::time_point bound(Clock::time_point start,std::chrono::seconds limit,Deadline parent = {}) {
    auto local = start;
    if (limit.count() > 0) {
        const auto available = std::chrono::duration_cast<std::chrono::seconds>(Clock::time_point::max()-start);
        local = limit >= available ? Clock::time_point::max() : start + limit;
    }
    return parent ? std::min(local,*parent) : local;
}
}
