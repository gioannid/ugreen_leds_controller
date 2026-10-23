#include "logger.h"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <ctime>

#include <sys/stat.h>
#include <unistd.h>

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
    file_path_ = config.file;
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
    file_path_.clear();
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

bool Logger::ensure_file_locked(std::string& error) {
    if (file_path_.empty()) {
        return true;  // stdout mode
    }

    if (file_) {
        struct stat opened {};
        struct stat named {};

        if (::fstat(::fileno(file_), &opened) != 0) {
            const int saved_errno = errno;
            error = "cannot inspect open log file: " +
                    std::string(std::strerror(saved_errno));
            return false;
        }

        if (::stat(file_path_.c_str(), &named) == 0) {
            if (opened.st_dev == named.st_dev &&
                opened.st_ino == named.st_ino) {
                return true;
            }
            // The pathname now points to a different file.
        } else {
            const int saved_errno = errno;
            if (saved_errno != ENOENT) {
                error = "cannot stat log file '" + file_path_ + "': " +
                        std::strerror(saved_errno);
                return false;
            }
            // The pathname disappeared: recreate it below.
        }
    }

    std::FILE* replacement = std::fopen(file_path_.c_str(), "a");
    if (!replacement) {
        const int saved_errno = errno;
        error = "cannot reopen log file '" + file_path_ + "': " +
                std::strerror(saved_errno);
        return false;
    }

    std::FILE* previous = file_;
    file_ = replacement;

    if (previous) {
        std::fclose(previous);
    }

    return true;
}

void Logger::log(LogLevel level, const std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_ || level < min_level_) {
        return;
    }

    const std::string line =
        utc_timestamp() + " [" + to_string(level) + "] " +
        message + "\n";

    const auto fallback = [&](const std::string& reason) {
        // Never call log::error() here: mutex_ is already held.
        std::fprintf(stderr, "ugreen-leds logger: %s\n", reason.c_str());
        std::fputs(line.c_str(), stderr);
        std::fflush(stderr);
    };

    if (to_screen_) {
        const bool write_failed =
            std::fputs(line.c_str(), stdout) == EOF;
        const bool flush_failed =
            std::fflush(stdout) == EOF;

        if (write_failed || flush_failed) {
            fallback("stdout write/flush failed");
            std::clearerr(stdout);
        }
        return;
    }

    std::string error;
    if (!ensure_file_locked(error)) {
        fallback(error);
        return;
    }

    const bool write_failed =
        std::fputs(line.c_str(), file_) == EOF;
    const bool flush_failed =
        std::fflush(file_) == EOF;

    if (write_failed || flush_failed) {
        // Keep file_path_ and file-target mode so the next record retries.
        std::fclose(file_);
        file_ = nullptr;
        fallback("log file write/flush failed");
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
