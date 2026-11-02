#include "logger.hpp"
#include <fstream>
#include <iostream>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include "thread_safe_time.hpp"

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

    std::tm local{};
    std::ostringstream stamp;
    if (pacificdb::time_format::local(now, local)) {
        // Preserve ctime's human-readable layout, including space-padded day
        // and the newline, without borrowing its shared static buffer.
        stamp << std::put_time(&local, "%a %b ") << std::setw(2) << local.tm_mday
              << std::put_time(&local, " %H:%M:%S %Y") << '\n';
    } else stamp << "unavailable";
    out << "[" << stamp.str() << "] " << msg << "\n";

    std::cout << "[LOGGER] Log written" << std::endl;
}
