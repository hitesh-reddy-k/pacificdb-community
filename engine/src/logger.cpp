#include "logger.hpp"
#include <fstream>
#include <iostream>
#include <chrono>
#include <ctime>

void Logger::write(const std::string& logPath,
                   const std::string& msg) {

    auto now = std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now());

    std::cout << "[LOGGER] Writing log entry" << std::endl;

    std::ofstream out(logPath, std::ios::app);

    if (!out.is_open()) {
        std::cout << "[ERROR] Logger open failed" << std::endl;
        return;
    }

    out << "[" << std::ctime(&now) << "] " << msg << "\n";

    std::cout << "[LOGGER] Log written" << std::endl;
}
