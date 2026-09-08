#pragma once

#include <string>
#include <nlohmann/json.hpp>
#include <memory>
#include <iostream>
#include <thread>
#include <chrono>

using json = nlohmann::json;

/**
 * @brief Exception hierarchy for the database engine
 *
 * Provides structured error handling with:
 * - Specific exception types for different errors
 * - Recoverable vs fatal classification
 * - Automatic logging and metrics
 * - User-friendly error messages
 */

class EngineException : public std::exception {
public:
    enum class Severity {
        INFO,      // Recoverable, informational
        WARNING,   // Recoverable, but concerning
        ERROR,     // Recoverable, but problematic
        CRITICAL   // Fatal, requires intervention
    };

    EngineException(
        const std::string& message,
        const std::string& component,
        Severity severity = Severity::ERROR,
        int errorCode = -1
    )
        : message_(message), component_(component), severity_(severity), errorCode_(errorCode) {
        logException();
    }

    const char* what() const noexcept override {
        return message_.c_str();
    }

    std::string component() const { return component_; }
    Severity severity() const { return severity_; }
    int errorCode() const { return errorCode_; }

    bool isRecoverable() const {
        return severity_ != Severity::CRITICAL;
    }

    json toJson() const {
        return {
            {"error", message_},
            {"component", component_},
            {"severity", static_cast<int>(severity_)},
            {"code", errorCode_}
        };
    }

protected:
    virtual void logException() const;

private:
    std::string message_;
    std::string component_;
    Severity severity_;
    int errorCode_;
};

// ==================== SPECIFIC EXCEPTION TYPES ====================

class StorageException : public EngineException {
public:
    StorageException(const std::string& msg, int code = 1001)
        : EngineException(msg, "STORAGE", Severity::ERROR, code) {}
};

class IndexException : public EngineException {
public:
    IndexException(const std::string& msg, int code = 2001)
        : EngineException(msg, "INDEX", Severity::ERROR, code) {}
};

class QueryException : public EngineException {
public:
    QueryException(const std::string& msg, int code = 3001)
        : EngineException(msg, "QUERY", Severity::ERROR, code) {}
};

class TransactionException : public EngineException {
public:
    TransactionException(const std::string& msg, int code = 4001)
        : EngineException(msg, "TRANSACTION", Severity::ERROR, code) {}
};

class MemoryException : public EngineException {
public:
    MemoryException(const std::string& msg, int code = 5001)
        : EngineException(msg, "MEMORY", Severity::CRITICAL, code) {}
};

class ConfigException : public EngineException {
public:
    ConfigException(const std::string& msg, int code = 6001)
        : EngineException(msg, "CONFIG", Severity::CRITICAL, code) {}
};

class TimeoutException : public EngineException {
public:
    TimeoutException(const std::string& msg, int code = 7001)
        : EngineException(msg, "TIMEOUT", Severity::WARNING, code) {}
};

/**
 * @brief Error handler helper for common operations
 */
class ErrorHandler {
public:
    /**
     * @brief Wrap an operation with automatic error handling
     * Usage: ErrorHandler::tryOp([](){  }, "OPERATION_NAME");
     */
    template<typename F>
    static bool tryOp(F&& operation, const std::string& opName) {
        try {
            operation();
            return true;
        } catch (const EngineException& e) {
            if (!e.isRecoverable()) throw;
            std::cerr << "[ERROR][" << opName << "] " << e.what() << std::endl;
            return false;
        } catch (const std::exception& e) {
            std::cerr << "[FATAL][" << opName << "] " << e.what() << std::endl;
            throw EngineException(
                "Unhandled exception: " + std::string(e.what()),
                opName,
                EngineException::Severity::CRITICAL
            );
        }
    }

    /**
     * @brief Retry operation with exponential backoff
     */
    template<typename F>
    static bool retryOp(
        F&& operation,
        const std::string& opName,
        int maxRetries = 3,
        int initialDelayMs = 100
    ) {
        int delay = initialDelayMs;
        for (int i = 0; i < maxRetries; ++i) {
            try {
                operation();
                return true;
            } catch (const TimeoutException&) {
                if (i < maxRetries - 1) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(delay));
                    delay *= 2;  // exponential backoff
                    continue;
                }
                throw;
            }
        }
        return false;
    }
};
