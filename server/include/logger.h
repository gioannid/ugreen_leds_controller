#ifndef UGREEN_SERVER_LOGGER_H
#define UGREEN_SERVER_LOGGER_H

#include <cstdio>
#include <mutex>
#include <string>

namespace ugreen {

// Log levels in increasing order of severity.
// Info and above are always emitted (production observability);
// Debug is emitted only when verbose logging is enabled (troubleshooting).
enum class LogLevel : int {
    Debug = 0,
    Info = 1,
    Warn = 2,
    Error = 3,
};

const char* to_string(LogLevel level);

struct LogConfig {
    std::string file;      // empty: log to the screen (stdout)
    bool verbose = false;  // true: include debug-level messages
};

// Process-wide, thread-safe logger.
//
// Until init() succeeds the logger is silent, which keeps unit tests and
// embedded Reactor usage quiet. The server's main() always initializes it
// (default: screen, info level), so a running service always logs.
//
// Lines have the form:
//   2026-10-17T10:15:30.123Z [INFO] message
// with an ISO-8601 UTC timestamp, a level tag, and the message.
// Every line is flushed immediately so it survives crashes and is picked
// up by the service manager without delay.
class Logger {
public:
    static Logger& instance();

    // Opens the target (screen or file, append mode). Returns false with a
    // descriptive error if the log file cannot be opened; the caller must
    // refuse to start. May be called again to retarget an active logger.
    bool init(const LogConfig& config, std::string& error);

    // Closes the target and disables all logging.
    void shutdown();

    bool initialized() const;
    bool enabled(LogLevel level) const;
    void log(LogLevel level, const std::string& message);

private:
    Logger() = default;

    mutable std::mutex mutex_;
    bool initialized_ = false;
    LogLevel min_level_ = LogLevel::Info;
    bool to_screen_ = true;
    std::FILE* file_ = nullptr;
};

namespace log {

bool enabled(LogLevel level);
void debug(const std::string& message);
void info(const std::string& message);
void warn(const std::string& message);
void error(const std::string& message);

} // namespace log

} // namespace ugreen

#endif // UGREEN_SERVER_LOGGER_H
