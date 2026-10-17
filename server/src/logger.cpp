#include "logger.h"

#include <chrono>
#include <ctime>

namespace ugreen {

namespace {

// ISO-8601 UTC timestamp with millisecond precision: 2026-10-17T10:15:30.123Z
std::string utc_timestamp() {
    auto now = std::chrono::system_clock::now();
    auto micros = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch());
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&t, &tm);

    char base[32];
    std::strftime(base, sizeof(base), "%Y-%m-%dT%H:%M:%S", &tm);
    int ms = static_cast<int>((micros.count() % 1000000) / 1000);

    char out[48];
    std::snprintf(out, sizeof(out), "%s.%03dZ", base, ms);
    return out;
}

} // namespace

const char* to_string(LogLevel level) {
    switch (level) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO";
        case LogLevel::Warn:  return "WARN";
        case LogLevel::Error: return "ERROR";
    }
    return "?";
}

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

bool Logger::init(const LogConfig& config, std::string& error) {
    std::FILE* file = nullptr;
    if (!config.file.empty()) {
        file = std::fopen(config.file.c_str(), "a");
        if (!file) {
            error = "cannot open log file '" + config.file + "'";
            return false;
        }
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (file_) {
        std::fclose(file_);
    }
    file_ = file;
    to_screen_ = (file_ == nullptr);
    min_level_ = config.verbose ? LogLevel::Debug : LogLevel::Info;
    initialized_ = true;
    return true;
}

void Logger::shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_) {
        std::fclose(file_);
        file_ = nullptr;
    }
    to_screen_ = true;
    min_level_ = LogLevel::Info;
    initialized_ = false;
}

bool Logger::initialized() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return initialized_;
}

bool Logger::enabled(LogLevel level) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return initialized_ && level >= min_level_;
}

void Logger::log(LogLevel level, const std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_ || level < min_level_) {
        return;
    }

    std::string line = utc_timestamp() + " [" + to_string(level) + "] " + message + "\n";
    if (to_screen_ || !file_) {
        std::fputs(line.c_str(), stdout);
        std::fflush(stdout);
    } else {
        std::fputs(line.c_str(), file_);
        std::fflush(file_);
    }
}

namespace log {

bool enabled(LogLevel level) {
    return Logger::instance().enabled(level);
}

void debug(const std::string& message) {
    Logger::instance().log(LogLevel::Debug, message);
}

void info(const std::string& message) {
    Logger::instance().log(LogLevel::Info, message);
}

void warn(const std::string& message) {
    Logger::instance().log(LogLevel::Warn, message);
}

void error(const std::string& message) {
    Logger::instance().log(LogLevel::Error, message);
}

} // namespace log

} // namespace ugreen
