#include "exceptions.hpp"
#include <iostream>
#include <iomanip>
#include <ctime>

void EngineException::logException() const {
    std::time_t now = std::time(nullptr);
    std::tm* timeinfo = std::localtime(&now);
    char buffer[80];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", timeinfo);

    const char* severityStr[] = {"INFO", "WARNING", "ERROR", "CRITICAL"};

    std::cerr << "[" << buffer << "] "
              << "[" << severityStr[static_cast<int>(severity_)] << "] "
              << "[" << component_ << "] "
              << "Code: " << errorCode_ << " - "
              << message_ << std::endl;

    // Could also log to file, send to monitoring service, etc.
}
