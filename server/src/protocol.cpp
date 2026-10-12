#include "protocol.h"

#include <charconv>
#include <set>
#include <string>

namespace ugreen {

StreamBuffer::AppendResult StreamBuffer::append(const char* data, size_t len) {
    if (buffer_.size() + len > kMaxRecvBuffer) {
        return AppendResult::BufferOverflow;
    }
    buffer_.append(data, len);
    return AppendResult::Ok;
}

bool StreamBuffer::extract_line(std::string& line, bool& line_overflow) {
    line_overflow = false;
    size_t nl_pos = buffer_.find('\n');
    if (nl_pos != std::string::npos) {
        if (nl_pos > kMaxLineLength) {
            line_overflow = true;
            return true;
        }
        line = buffer_.substr(0, nl_pos);
        buffer_.erase(0, nl_pos + 1);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        return true;
    }

    if (buffer_.size() > kMaxLineLength) {
        line_overflow = true;
        return true;
    }

    return false;
}

std::string Response::serialize() const {
    std::string out;
    if (status == ResponseStatus::Ok) {
        out = "OK " + std::to_string(request_id) + " " + action;
    } else {
        out = "ERR " + std::to_string(request_id) + " " + action;
    }

    for (const auto& [k, v] : fields) {
        out += " ";
        out += k;
        out += "=";
        out += v;
    }
    out += "\n";
    return out;
}

std::vector<std::string_view> ProtocolParser::tokenize(std::string_view line) {
    std::vector<std::string_view> tokens;
    size_t start = 0;
    while (start < line.size()) {
        while (start < line.size() && (line[start] == ' ' || line[start] == '\t')) {
            ++start;
        }
        if (start >= line.size()) break;
        size_t end = start;
        while (end < line.size() && line[end] != ' ' && line[end] != '\t') {
            ++end;
        }
        tokens.push_back(line.substr(start, end - start));
        start = end;
    }
    return tokens;
}

static bool parse_u64(std::string_view s, uint64_t& out) {
    if (s.empty()) return false;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
    }
    auto res = std::from_chars(s.data(), s.data() + s.size(), out);
    return res.ec == std::errc() && res.ptr == s.data() + s.size();
}

static bool parse_u32(std::string_view s, uint32_t& out) {
    if (s.empty()) return false;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
    }
    auto res = std::from_chars(s.data(), s.data() + s.size(), out);
    return res.ec == std::errc() && res.ptr == s.data() + s.size();
}

static bool parse_u16(std::string_view s, uint16_t& out) {
    uint32_t tmp = 0;
    if (!parse_u32(s, tmp) || tmp > 65535) return false;
    out = static_cast<uint16_t>(tmp);
    return true;
}

static bool parse_u8(std::string_view s, uint8_t& out) {
    uint32_t tmp = 0;
    if (!parse_u32(s, tmp) || tmp > 255) return false;
    out = static_cast<uint8_t>(tmp);
    return true;
}

static bool parse_int(std::string_view s, int& out) {
    if (s.empty()) return false;
    size_t start = 0;
    if (s[0] == '-' || s[0] == '+') {
        if (s.size() == 1) return false;
        start = 1;
    }
    for (size_t i = start; i < s.size(); ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
    }
    auto res = std::from_chars(s.data(), s.data() + s.size(), out);
    return res.ec == std::errc() && res.ptr == s.data() + s.size();
}

ParseResult ProtocolParser::parse_line(std::string_view line) {
    ParseResult result;
    auto tokens = tokenize(line);
    if (tokens.empty()) {
        result.status = ParseResult::Status::Error;
        result.error_response = Response::error(0, "INVALID_COMMAND", {{"reason", "empty_line"}});
        return result;
    }

    std::string_view verb_str = tokens[0];
    uint64_t req_id = 0;

    if (tokens.size() > 1) {
        if (!parse_u64(tokens[1], req_id)) {
            result.status = ParseResult::Status::Error;
            result.error_response = Response::error(0, "INVALID_ARGUMENT", {{"field", "request_id"}});
            return result;
        }
    } else {
        result.status = ParseResult::Status::Error;
        result.error_response = Response::error(0, "INVALID_ARGUMENT", {{"field", "request_id"}});
        return result;
    }

    Request req;
    req.request_id = req_id;

    if (verb_str == "HELLO") {
        req.verb = Verb::Hello;
        if (tokens.size() != 3) {
            result.status = ParseResult::Status::Error;
            result.error_response = Response::error(req_id, "INVALID_ARGUMENT", {{"reason", "syntax"}});
            return result;
        }
        uint32_t ver = 0;
        if (!parse_u32(tokens[2], ver)) {
            result.status = ParseResult::Status::Error;
            result.error_response = Response::error(req_id, "INVALID_ARGUMENT", {{"field", "version"}});
            return result;
        }
        if (ver != 1) {
            result.status = ParseResult::Status::Error;
            result.error_response = Response::error(req_id, "UNSUPPORTED_VERSION", {{"version", std::string(tokens[2])}});
            result.should_close = true;
            return result;
        }
        req.version = ver;
        result.request = req;
        return result;
    }

    if (verb_str == "CAPS") {
        req.verb = Verb::Caps;
        if (tokens.size() != 2) {
            result.status = ParseResult::Status::Error;
            result.error_response = Response::error(req_id, "INVALID_ARGUMENT", {{"reason", "syntax"}});
            return result;
        }
        result.request = req;
        return result;
    }

    if (verb_str == "FLUSH") {
        req.verb = Verb::Flush;
        if (tokens.size() != 2) {
            result.status = ParseResult::Status::Error;
            result.error_response = Response::error(req_id, "INVALID_ARGUMENT", {{"reason", "syntax"}});
            return result;
        }
        result.request = req;
        return result;
    }

    if (verb_str == "RESET") {
        req.verb = Verb::Reset;
        if (tokens.size() != 3) {
            result.status = ParseResult::Status::Error;
            result.error_response = Response::error(req_id, "INVALID_ARGUMENT", {{"reason", "syntax"}});
            return result;
        }
        std::string_view target_str = tokens[2];
        if (target_str == "all") {
            req.target.is_all = true;
        } else {
            auto led = parse_led_id(target_str);
            if (!led) {
                result.status = ParseResult::Status::Error;
                result.error_response = Response::error(req_id, "UNSUPPORTED_LED", {{"led", std::string(target_str)}});
                return result;
            }
            req.target.is_all = false;
            req.target.id = *led;
        }
        result.request = req;
        return result;
    }

    if (verb_str == "GET") {
        req.verb = Verb::Get;
        if (tokens.size() < 3) {
            result.status = ParseResult::Status::Error;
            result.error_response = Response::error(req_id, "INVALID_ARGUMENT", {{"reason", "syntax"}});
            return result;
        }
        std::string_view target_str = tokens[2];
        auto led = parse_led_id(target_str);
        if (!led) {
            result.status = ParseResult::Status::Error;
            result.error_response = Response::error(req_id, "UNSUPPORTED_LED", {{"led", std::string(target_str)}});
            return result;
        }
        req.target.is_all = false;
        req.target.id = *led;

        std::set<std::string> seen_fields;
        for (size_t i = 3; i < tokens.size(); ++i) {
            auto eq = tokens[i].find('=');
            if (eq == std::string_view::npos) {
                result.status = ParseResult::Status::Error;
                result.error_response = Response::error(req_id, "INVALID_ARGUMENT", {{"reason", "syntax"}});
                return result;
            }
            std::string key(tokens[i].substr(0, eq));
            std::string_view val = tokens[i].substr(eq + 1);
            if (seen_fields.count(key)) {
                result.status = ParseResult::Status::Error;
                result.error_response = Response::error(req_id, "INVALID_ARGUMENT", {{"field", key}});
                return result;
            }
            seen_fields.insert(key);

            if (key == "source") {
                if (val != "desired" && val != "applied" && val != "hardware") {
                    result.status = ParseResult::Status::Error;
                    result.error_response = Response::error(req_id, "INVALID_ARGUMENT", {{"field", "source"}});
                    return result;
                }
                req.source = std::string(val);
            } else {
                result.status = ParseResult::Status::Error;
                result.error_response = Response::error(req_id, "INVALID_ARGUMENT", {{"field", key}});
                return result;
            }
        }

        result.request = req;
        return result;
    }

    if (verb_str == "SET") {
        req.verb = Verb::Set;
        if (tokens.size() < 4) {
            result.status = ParseResult::Status::Error;
            result.error_response = Response::error(req_id, "INVALID_ARGUMENT", {{"reason", "syntax"}});
            return result;
        }
        std::string_view target_str = tokens[2];
        if (target_str == "all") {
            req.target.is_all = true;
        } else {
            auto led = parse_led_id(target_str);
            if (!led) {
                result.status = ParseResult::Status::Error;
                result.error_response = Response::error(req_id, "UNSUPPORTED_LED", {{"led", std::string(target_str)}});
                return result;
            }
            req.target.is_all = false;
            req.target.id = *led;
        }

        std::set<std::string> seen_fields;
        for (size_t i = 3; i < tokens.size(); ++i) {
            auto eq = tokens[i].find('=');
            if (eq == std::string_view::npos) {
                result.status = ParseResult::Status::Error;
                result.error_response = Response::error(req_id, "INVALID_ARGUMENT", {{"reason", "syntax"}});
                return result;
            }
            std::string key(tokens[i].substr(0, eq));
            std::string_view val = tokens[i].substr(eq + 1);

            if (seen_fields.count(key)) {
                result.status = ParseResult::Status::Error;
                result.error_response = Response::error(req_id, "INVALID_ARGUMENT", {{"field", key}});
                return result;
            }
            seen_fields.insert(key);

            if (key == "mode") {
                auto mode = parse_op_mode(val);
                if (!mode) {
                    result.status = ParseResult::Status::Error;
                    result.error_response = Response::error(req_id, "INVALID_ARGUMENT", {{"field", "mode"}});
                    return result;
                }
                req.patch.mode = *mode;
            } else if (key == "brightness") {
                uint8_t b = 0;
                if (!parse_u8(val, b)) {
                    result.status = ParseResult::Status::Error;
                    result.error_response = Response::error(req_id, "INVALID_ARGUMENT", {{"field", "brightness"}});
                    return result;
                }
                req.patch.brightness = b;
            } else if (key == "rgb") {
                auto rgb = parse_rgb(val);
                if (!rgb) {
                    result.status = ParseResult::Status::Error;
                    result.error_response = Response::error(req_id, "INVALID_ARGUMENT", {{"field", "rgb"}});
                    return result;
                }
                req.patch.rgb = *rgb;
            } else if (key == "on_ms") {
                uint16_t on = 0;
                if (!parse_u16(val, on)) {
                    result.status = ParseResult::Status::Error;
                    result.error_response = Response::error(req_id, "INVALID_ARGUMENT", {{"field", "on_ms"}});
                    return result;
                }
                req.patch.on_ms = on;
            } else if (key == "off_ms") {
                uint16_t off = 0;
                if (!parse_u16(val, off)) {
                    result.status = ParseResult::Status::Error;
                    result.error_response = Response::error(req_id, "INVALID_ARGUMENT", {{"field", "off_ms"}});
                    return result;
                }
                req.patch.off_ms = off;
            } else if (key == "priority") {
                int p = 0;
                if (!parse_int(val, p) || p < 0) {
                    result.status = ParseResult::Status::Error;
                    result.error_response = Response::error(req_id, "INVALID_ARGUMENT", {{"field", "priority"}});
                    return result;
                }
                req.patch.priority = p;
            } else if (key == "reply") {
                if (val != "none") {
                    result.status = ParseResult::Status::Error;
                    result.error_response = Response::error(req_id, "INVALID_ARGUMENT", {{"field", "reply"}});
                    return result;
                }
                req.reply_none = true;
            } else {
                result.status = ParseResult::Status::Error;
                result.error_response = Response::error(req_id, "INVALID_ARGUMENT", {{"field", key}});
                return result;
            }
        }

        // Validate timing constraints if both on_ms and off_ms are present
        if (req.patch.on_ms && req.patch.off_ms) {
            std::string err_field;
            OpMode m = req.patch.mode.value_or(OpMode::Blink);
            if (!validate_cycle_timing(*req.patch.on_ms, *req.patch.off_ms, m, &err_field)) {
                result.status = ParseResult::Status::Error;
                result.error_response = Response::error(req_id, "INVALID_ARGUMENT", {{"field", err_field}});
                return result;
            }
        }

        result.request = req;
        return result;
    }

    result.status = ParseResult::Status::Error;
    result.error_response = Response::error(req_id, "UNKNOWN_COMMAND", {{"verb", std::string(verb_str)}});
    return result;
}

} // namespace ugreen
