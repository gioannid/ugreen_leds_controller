#ifndef UGREEN_SERVER_PROTOCOL_H
#define UGREEN_SERVER_PROTOCOL_H

#include "led_state.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ugreen {

constexpr size_t kMaxLineLength = 1024;
constexpr size_t kMaxRecvBuffer = 8192;
constexpr size_t kMaxQueuedOutput = 32768;

enum class Verb {
    Hello,
    Caps,
    Set,
    Get,
    Reset,
    Flush,
    Unknown,
};

struct Target {
    bool is_all = false;
    LedId id = LedId::Power;
};

struct Request {
    Verb verb = Verb::Unknown;
    uint64_t request_id = 0;

    // HELLO
    uint32_t version = 0;

    // SET, GET, RESET
    Target target;

    // SET
    LedPatch patch;
    bool reply_none = false;

    // GET
    std::string source = "desired"; // "desired", "applied", "hardware"
};

enum class ResponseStatus {
    Ok,
    Error,
};

struct Response {
    ResponseStatus status = ResponseStatus::Ok;
    uint64_t request_id = 0;
    std::string action; // "HELLO", "CAPS", "ACCEPTED", "RESET", "FLUSH", "GET", or error code
    std::vector<std::pair<std::string, std::string>> fields;

    std::string serialize() const;

    static Response ok(uint64_t req_id, std::string action, std::vector<std::pair<std::string, std::string>> fields = {}) {
        return Response{ResponseStatus::Ok, req_id, std::move(action), std::move(fields)};
    }

    static Response error(uint64_t req_id, std::string code, std::vector<std::pair<std::string, std::string>> fields = {}) {
        return Response{ResponseStatus::Error, req_id, std::move(code), std::move(fields)};
    }
};

struct ParseResult {
    enum class Status {
        Success,
        Error,          // Bad syntax or semantic validation error
        BufferOverflow, // Framing limit exceeded (connection should be closed)
    };

    Status status = Status::Success;
    std::optional<Request> request;
    std::optional<Response> error_response;
    bool should_close = false;
};

// StreamBuffer handles TCP stream fragmentation, buffering, and message boundary detection.
class StreamBuffer {
public:
    enum class AppendResult {
        Ok,
        BufferOverflow,
    };

    AppendResult append(const char* data, size_t len);
    bool extract_line(std::string& line, bool& line_overflow);
    size_t size() const { return buffer_.size(); }
    void clear() { buffer_.clear(); }

private:
    std::string buffer_;
};

// Protocol parser with strict validation.
class ProtocolParser {
public:
    static ParseResult parse_line(std::string_view line);

private:
    static std::vector<std::string_view> tokenize(std::string_view line);
};

} // namespace ugreen

#endif // UGREEN_SERVER_PROTOCOL_H
