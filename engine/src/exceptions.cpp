#include "exceptions.hpp"
#include <iostream>
#include <iomanip>
#include <ctime>
#include "thread_safe_time.hpp"

void EngineException::logException() const {
    std::time_t now = std::time(nullptr);
    std::tm timeinfo{};
    char buffer[80] = "unavailable";
    if (pacificdb::time_format::local(now, timeinfo))
        std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &timeinfo);

    const char* severityStr[] = {"INFO", "WARNING", "ERROR", "CRITICAL"};

    std::cerr << "[" << buffer << "] "
              << "[" << severityStr[static_cast<int>(severity_)] << "] "
              << "[" << component_ << "] "
              << "Code: " << errorCode_ << " - "
              << message_ << std::endl;

    // Could also log to file, send to monitoring service, etc.
}
