#pragma once
#include <string>

class Logger {
public:
    static void write(const std::string& logPath,
                      const std::string& msg);
};
