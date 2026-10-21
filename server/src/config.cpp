#include "config.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cctype>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>

namespace ugreen {

namespace {

std::string trim(const std::string& s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

bool parse_uint(const std::string& s, uint64_t& out, uint64_t max) {
    if (s.empty()) return false;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
    }
    unsigned long long v = 0;
    for (char c : s) {
        v = v * 10 + static_cast<unsigned long long>(c - '0');
        if (v > max) return false;
    }
    out = static_cast<uint64_t>(v);
    return true;
}

bool parse_u8_field(const std::string& s, uint8_t& out) {
    uint64_t v = 0;
    if (!parse_uint(s, v, 255)) return false;
    out = static_cast<uint8_t>(v);
    return true;
}

bool parse_u16_field(const std::string& s, uint16_t& out) {
    uint64_t v = 0;
    if (!parse_uint(s, v, 65535)) return false;
    out = static_cast<uint16_t>(v);
    return true;
}

bool parse_bool_field(const std::string& s, bool& out) {
    if (s == "true" || s == "1" || s == "yes" || s == "on") { out = true; return true; }
    if (s == "false" || s == "0" || s == "no" || s == "off") { out = false; return true; }
    return false;
}

bool parse_ipv4_addr(const std::string& s, uint32_t& out) {
    int parts[4] = {0, 0, 0, 0};
    int idx = 0;
    int val = 0;
    bool any_digit = false;
    for (size_t i = 0; i <= s.size(); ++i) {
        char c = (i < s.size()) ? s[i] : '.';
        if (c >= '0' && c <= '9') {
            val = val * 10 + (c - '0');
            if (val > 255) return false;
            any_digit = true;
        } else if (c == '.') {
            if (!any_digit) return false;
            if (i == s.size()) {
                // Virtual trailing dot: finalize the fourth octet.
                if (idx != 3) return false;
                parts[3] = val;
            } else {
                if (idx >= 3) return false;
                parts[idx++] = val;
                val = 0;
                any_digit = false;
            }
        } else {
            return false;
        }
    }
    out = (static_cast<uint32_t>(parts[0]) << 24) |
          (static_cast<uint32_t>(parts[1]) << 16) |
          (static_cast<uint32_t>(parts[2]) << 8) |
          static_cast<uint32_t>(parts[3]);
    return true;
}

bool parse_source(const std::string& s, AccessRule& rule, std::string& err) {
    if (s == "local") {
        rule.family = 0;
        rule.prefix_len = 0;
        return true;
    }

    std::string addr_part = s;
    int prefix = -1;
    size_t slash = s.find_last_of('/');
    if (slash != std::string::npos) {
        addr_part = s.substr(0, slash);
        std::string pfx = s.substr(slash + 1);
        uint64_t p = 0;
        if (!parse_uint(pfx, p, 128)) {
            err = "invalid prefix length in '" + s + "'";
            return false;
        }
        prefix = static_cast<int>(p);
    }

    if (addr_part.find(':') != std::string::npos) {
        struct in6_addr a6;
        if (inet_pton(AF_INET6, addr_part.c_str(), &a6) != 1) {
            err = "invalid IPv6 address '" + addr_part + "'";
            return false;
        }
        if (prefix < 0) prefix = 128;
        std::memcpy(rule.addr8, &a6, 16);
        rule.family = 6;
        rule.prefix_len = prefix;
        return true;
    }

    uint32_t a4 = 0;
    if (!parse_ipv4_addr(addr_part, a4)) {
        err = "invalid IPv4 address '" + addr_part + "'";
        return false;
    }
    if (prefix < 0) prefix = 32;
    if (prefix > 32) {
        err = "IPv4 prefix length out of range in '" + s + "'";
        return false;
    }
    rule.family = 4;
    rule.prefix_len = prefix;
    rule.addr4 = a4;
    return true;
}

bool parse_verb_field(const std::string& s, Verb& out) {
    if (s == "CAPS") { out = Verb::Caps; return true; }
    if (s == "GET") { out = Verb::Get; return true; }
    if (s == "SET") { out = Verb::Set; return true; }
    if (s == "RESET") { out = Verb::Reset; return true; }
    if (s == "FLUSH") { out = Verb::Flush; return true; }
    return false;
}

bool parse_leds_field(const std::string& s, AccessRule& rule, std::string& err) {
    if (s == "all") {
        rule.all_leds = true;
        return true;
    }
    rule.all_leds = false;
    std::istringstream iss(s);
    std::string tok;
    while (std::getline(iss, tok, ',')) {
        tok = trim(tok);
        if (tok.empty()) {
            err = "empty LED name in '" + s + "'";
            return false;
        }
        auto led = parse_led_id(tok);
        if (!led) {
            err = "unknown LED name '" + tok + "'";
            return false;
        }
        for (auto existing : rule.leds) {
            if (existing == *led) {
                err = "duplicate LED name '" + tok + "'";
                return false;
            }
        }
        rule.leds.push_back(*led);
    }
    if (rule.leds.empty()) {
        err = "empty LED list";
        return false;
    }
    return true;
}

bool parse_verbs_field(const std::string& s, AccessRule& rule, std::string& err) {
    std::istringstream iss(s);
    std::string tok;
    bool any = false;
    while (std::getline(iss, tok, ',')) {
        tok = trim(tok);
        if (tok.empty()) {
            err = "empty verb name in '" + s + "'";
            return false;
        }
        Verb v;
        if (!parse_verb_field(tok, v)) {
            err = "unknown verb '" + tok + "'";
            return false;
        }
        if (rule.verbs.count(v)) {
            err = "duplicate verb '" + tok + "'";
            return false;
        }
        rule.verbs.insert(v);
        any = true;
    }
    if (!any) {
        err = "empty verb list";
        return false;
    }
    return true;
}

bool parse_fallback_state(const std::string& s, LedState& out, std::string& err) {
    std::set<std::string> seen;
    std::istringstream iss(s);
    std::string tok;
    while (iss >> tok) {
        auto eq = tok.find('=');
        if (eq == std::string::npos || eq == 0) {
            err = "invalid field '" + tok + "' (expected key=value)";
            return false;
        }
        std::string key = tok.substr(0, eq);
        std::string val = tok.substr(eq + 1);
        if (seen.count(key)) {
            err = "duplicate field '" + key + "'";
            return false;
        }
        seen.insert(key);

        if (key == "mode") {
            auto m = parse_op_mode(val);
            if (!m) {
                err = "invalid mode '" + val + "'";
                return false;
            }
            out.mode = *m;
        } else if (key == "brightness") {
            if (!parse_u8_field(val, out.brightness)) {
                err = "invalid brightness '" + val + "'";
                return false;
            }
        } else if (key == "rgb") {
            auto rgb = parse_rgb(val);
            if (!rgb) {
                err = "invalid rgb '" + val + "'";
                return false;
            }
            out.rgb = *rgb;
        } else if (key == "on_ms") {
            if (!parse_u16_field(val, out.on_ms)) {
                err = "invalid on_ms '" + val + "'";
                return false;
            }
        } else if (key == "off_ms") {
            if (!parse_u16_field(val, out.off_ms)) {
                err = "invalid off_ms '" + val + "'";
                return false;
            }
        } else {
            err = "unknown fallback field '" + key + "'";
            return false;
        }
    }

    if (seen.empty()) {
        err = "empty fallback state";
        return false;
    }

    // The chip never stores a zero brightness or a black color (the write is
    // acknowledged but ignored), so an explicitly zero field can never
    // converge. Unspecified fields keep the chip-representable defaults.
    if (seen.count("brightness") && out.brightness == 0) {
        err = "brightness 0 is not supported by the hardware";
        return false;
    }
    if (seen.count("rgb") && out.rgb.r == 0 && out.rgb.g == 0 && out.rgb.b == 0) {
        err = "rgb 0,0,0 is not supported by the hardware";
        return false;
    }

    if (seen.count("on_ms") && seen.count("off_ms")) {
        std::string err_field;
        if (!validate_cycle_timing(out.on_ms, out.off_ms, out.mode, &err_field)) {
            err = "invalid cycle timing (on_ms + off_ms out of range)";
            return false;
        }
    }
    return true;
}

bool same_network(const AccessRule& a, const AccessRule& b) {
    if (a.family != b.family || a.prefix_len != b.prefix_len) return false;
    if (a.family == 0) return true;
    if (a.family == 4) return a.addr4 == b.addr4;
    return std::memcmp(a.addr8, b.addr8, 16) == 0;
}

} // namespace

void apply_dev_defaults(ServerConfig& config) {
    if (!config.access_rules.empty()) {
        return;
    }

    AccessRule local;
    local.name = "local-dev";
    local.family = 0;
    local.prefix_len = 0;
    local.all_leds = true;
    local.priority = 0;
    local.max_connections = 0;
    config.access_rules.push_back(local);

    AccessRule loopback;
    loopback.name = "loopback-dev";
    loopback.family = 4;
    loopback.prefix_len = 8;
    loopback.addr4 = 0x7F000000;
    loopback.all_leds = true;
    loopback.priority = 0;
    loopback.max_connections = 0;
    config.access_rules.push_back(loopback);
}

bool load_server_config(const std::string& path, ServerConfig& out, std::string& error) {
    std::ifstream f(path);
    if (!f.is_open()) {
        error = "cannot open config file: " + path;
        return false;
    }

    ServerConfig cfg;
    enum class Section { None, Server, Access, Fallback };
    Section section = Section::None;
    std::string access_name;
    size_t current_rule = std::string::npos;

    std::string line;
    int line_no = 0;
    while (std::getline(f, line)) {
        ++line_no;
        std::string t = trim(line);
        if (t.empty() || t[0] == '#' || t[0] == ';') continue;

        if (t.front() == '[') {
            if (t.back() != ']') {
                error = "line " + std::to_string(line_no) + ": malformed section header";
                return false;
            }
            std::string name = trim(t.substr(1, t.size() - 2));
            if (name.empty()) {
                error = "line " + std::to_string(line_no) + ": empty section name";
                return false;
            }
            if (name == "server") {
                section = Section::Server;
                access_name.clear();
                current_rule = std::string::npos;
                continue;
            }
            if (name == "fallback") {
                section = Section::Fallback;
                access_name.clear();
                current_rule = std::string::npos;
                continue;
            }
            if (name.rfind("access.", 0) == 0) {
                access_name = name.substr(7);
                if (access_name.empty()) {
                    error = "line " + std::to_string(line_no) + ": empty access rule name";
                    return false;
                }
                section = Section::Access;
                current_rule = std::string::npos;
                continue;
            }
            error = "line " + std::to_string(line_no) + ": unknown section [" + name + "]";
            return false;
        }

        auto eq = t.find('=');
        if (eq == std::string::npos) {
            error = "line " + std::to_string(line_no) + ": expected key=value";
            return false;
        }
        std::string key = trim(t.substr(0, eq));
        std::string val = trim(t.substr(eq + 1));
        if (key.empty()) {
            error = "line " + std::to_string(line_no) + ": empty key";
            return false;
        }

        if (section == Section::None) {
            error = "line " + std::to_string(line_no) + ": key outside of any section";
            return false;
        }

        if (section == Section::Server) {
            if (key == "unix_socket") {
                cfg.transport.unix_socket_path = val;
            } else if (key == "tcp_bind") {
                cfg.transport.tcp_bind_ip = val;
            } else if (key == "tcp_port") {
                uint64_t v = 0;
                if (!parse_uint(val, v, 65535) || v < 1) {
                    error = "line " + std::to_string(line_no) + ": invalid tcp_port '" + val + "'";
                    return false;
                }
                cfg.transport.tcp_port = static_cast<int>(v);
            } else if (key == "scheduler_interval_ms") {
                uint64_t v = 0;
                if (!parse_uint(val, v, 60000) || v < 1) {
                    error = "line " + std::to_string(line_no) + ": invalid scheduler_interval_ms '" + val + "'";
                    return false;
                }
                cfg.transport.scheduler_interval_ms = static_cast<int>(v);
            } else if (key == "enable_unix_socket") {
                if (!parse_bool_field(val, cfg.transport.enable_unix_socket)) {
                    error = "line " + std::to_string(line_no) + ": invalid boolean '" + val + "'";
                    return false;
                }
            } else if (key == "enable_tcp") {
                if (!parse_bool_field(val, cfg.transport.enable_tcp)) {
                    error = "line " + std::to_string(line_no) + ": invalid boolean '" + val + "'";
                    return false;
                }
            } else if (key == "max_connections") {
                uint64_t v = 0;
                if (!parse_uint(val, v, 1000000)) {
                    error = "line " + std::to_string(line_no) + ": invalid max_connections '" + val + "'";
                    return false;
                }
                cfg.max_total_connections = static_cast<int>(v);
            } else if (key == "command_rate_limit") {
                uint64_t v = 0;
                if (!parse_uint(val, v, 1000000)) {
                    error = "line " + std::to_string(line_no) + ": invalid command_rate_limit '" + val + "'";
                    return false;
                }
                cfg.command_rate_limit = static_cast<int>(v);
            } else if (key == "hardware_read_rate_limit") {
                uint64_t v = 0;
                if (!parse_uint(val, v, 1000000)) {
                    error = "line " + std::to_string(line_no) + ": invalid hardware_read_rate_limit '" + val + "'";
                    return false;
                }
                cfg.hardware_read_rate_limit = static_cast<int>(v);
            } else if (key == "lock_file") {
                cfg.lock_file = val;
            } else if (key == "log_file") {
                cfg.log_file = val;
            } else if (key == "verbose") {
                if (!parse_bool_field(val, cfg.verbose)) {
                    error = "line " + std::to_string(line_no) + ": invalid verbose '" + val + "'";
                    return false;
                }
            } else if (key == "backend") {
                if (val != "fake" && val != "i2c") {
                    error = "line " + std::to_string(line_no) + ": invalid backend '" + val +
                            "' (expected fake or i2c)";
                    return false;
                }
                cfg.backend = val;
            } else if (key == "init_to_fallback") {
                if (!parse_bool_field(val, cfg.init_to_fallback)) {
                    error = "line " + std::to_string(line_no) + ": invalid init_to_fallback '" + val + "'";
                    return false;
                }
            } else {
                error = "line " + std::to_string(line_no) + ": unknown server key '" + key + "'";
                return false;
            }
        } else if (section == Section::Access) {
            if (current_rule == std::string::npos) {
                AccessRule rule;
                rule.name = access_name;
                cfg.access_rules.push_back(rule);
                current_rule = cfg.access_rules.size() - 1;
            }
            AccessRule& rule = cfg.access_rules[current_rule];
            if (key == "source") {
                if (rule.source_set) {
                    error = "line " + std::to_string(line_no) + ": duplicate source in rule '" +
                            access_name + "'";
                    return false;
                }
                rule.source_set = true;
                std::string serr;
                if (!parse_source(val, rule, serr)) {
                    error = "line " + std::to_string(line_no) + " in rule '" + access_name +
                            "': " + serr;
                    return false;
                }
            } else if (key == "leds") {
                std::string serr;
                if (!parse_leds_field(val, rule, serr)) {
                    error = "line " + std::to_string(line_no) + " in rule '" + access_name +
                            "': " + serr;
                    return false;
                }
            } else if (key == "verbs") {
                std::string serr;
                if (!parse_verbs_field(val, rule, serr)) {
                    error = "line " + std::to_string(line_no) + " in rule '" + access_name +
                            "': " + serr;
                    return false;
                }
            } else if (key == "priority") {
                uint64_t v = 0;
                if (!parse_uint(val, v, 2000000000)) {
                    error = "line " + std::to_string(line_no) + ": invalid priority '" + val + "'";
                    return false;
                }
                rule.priority = static_cast<int>(v);
            } else if (key == "max_connections") {
                uint64_t v = 0;
                if (!parse_uint(val, v, 1000000)) {
                    error = "line " + std::to_string(line_no) + ": invalid max_connections '" +
                            val + "'";
                    return false;
                }
                rule.max_connections = static_cast<size_t>(v);
            } else {
                error = "line " + std::to_string(line_no) + ": unknown key '" + key + "' in rule '" +
                        access_name + "'";
                return false;
            }
        } else {
            auto led = parse_led_id(key);
            if (!led) {
                error = "line " + std::to_string(line_no) + ": unknown LED '" + key + "' in [fallback]";
                return false;
            }
            if (cfg.fallbacks.count(*led)) {
                error = "line " + std::to_string(line_no) + ": duplicate fallback for LED '" +
                        key + "'";
                return false;
            }
            LedState st;
            std::string serr;
            if (!parse_fallback_state(val, st, serr)) {
                error = "line " + std::to_string(line_no) + " for LED '" + key + "': " + serr;
                return false;
            }
            cfg.fallbacks[*led] = st;
        }
    }

    for (const auto& rule : cfg.access_rules) {
        if (!rule.source_set) {
            error = "access rule '" + rule.name + "' has no source";
            return false;
        }
    }
    for (size_t i = 0; i < cfg.access_rules.size(); ++i) {
        for (size_t j = i + 1; j < cfg.access_rules.size(); ++j) {
            if (cfg.access_rules[i].name == cfg.access_rules[j].name) {
                error = "duplicate access rule name '" + cfg.access_rules[i].name + "'";
                return false;
            }
            if (same_network(cfg.access_rules[i], cfg.access_rules[j])) {
                error = "duplicate rules for network " + describe_rule_source(cfg.access_rules[i]);
                return false;
            }
        }
    }

    struct in_addr bind_addr;
    if (inet_pton(AF_INET, cfg.transport.tcp_bind_ip.c_str(), &bind_addr) != 1) {
        error = "invalid tcp_bind address '" + cfg.transport.tcp_bind_ip + "'";
        return false;
    }

    out = std::move(cfg);
    return true;
}

const AccessRule* match_access_rule(const ServerConfig& config, const sockaddr_storage& peer,
                                    socklen_t peer_len, bool is_unix) {
    (void)peer_len;
    const AccessRule* best = nullptr;
    int best_prefix = -1;

    for (const auto& rule : config.access_rules) {
        bool matches = false;
        if (is_unix) {
            matches = (rule.family == 0);
        } else if (peer.ss_family == AF_INET && rule.family == 4) {
            const auto* sin = reinterpret_cast<const sockaddr_in*>(&peer);
            uint32_t p = ntohl(sin->sin_addr.s_addr);
            if (rule.prefix_len == 0) {
                matches = true;
            } else {
                uint32_t mask = 0xFFFFFFFFu << (32 - rule.prefix_len);
                matches = ((p & mask) == (rule.addr4 & mask));
            }
        } else if (peer.ss_family == AF_INET6 && rule.family == 6) {
            const auto* sin6 = reinterpret_cast<const sockaddr_in6*>(&peer);
            const uint8_t* p = sin6->sin6_addr.s6_addr;
            int full_bytes = rule.prefix_len / 8;
            int rem_bits = rule.prefix_len % 8;
            matches = true;
            for (int i = 0; i < full_bytes; ++i) {
                if (p[i] != rule.addr8[i]) {
                    matches = false;
                    break;
                }
            }
            if (matches && rem_bits > 0) {
                uint8_t mask = static_cast<uint8_t>(0xFFu << (8 - rem_bits));
                if ((p[full_bytes] & mask) != (rule.addr8[full_bytes] & mask)) {
                    matches = false;
                }
            }
        }

        if (matches) {
            int pref = (rule.family == 0) ? 128 : rule.prefix_len;
            if (pref > best_prefix) {
                best_prefix = pref;
                best = &rule;
            }
        }
    }
    return best;
}

bool rule_allows_led(const AccessRule* rule, LedId id) {
    if (!rule || rule->all_leds) return true;
    for (auto l : rule->leds) {
        if (l == id) return true;
    }
    return false;
}

bool rule_allows_all_leds(const AccessRule* rule, const std::vector<LedId>& supported) {
    if (!rule || rule->all_leds) return true;
    for (auto l : supported) {
        if (!rule_allows_led(rule, l)) return false;
    }
    return true;
}

bool rule_allows_verb(const AccessRule* rule, Verb verb) {
    if (!rule || rule->verbs.empty()) return true;
    return rule->verbs.count(verb) > 0;
}

std::string describe_rule_source(const AccessRule& rule) {
    if (rule.family == 0) return "local";
    char buf[64] = {0};
    if (rule.family == 4) {
        struct in_addr a;
        a.s_addr = htonl(rule.addr4);
        inet_ntop(AF_INET, &a, buf, sizeof(buf));
        if (rule.prefix_len == 32) return buf;
        return std::string(buf) + "/" + std::to_string(rule.prefix_len);
    }
    struct in6_addr a6;
    std::memcpy(&a6, rule.addr8, 16);
    inet_ntop(AF_INET6, &a6, buf, sizeof(buf));
    if (rule.prefix_len == 128) return buf;
    return std::string(buf) + "/" + std::to_string(rule.prefix_len);
}

} // namespace ugreen
