// http_status.cpp — the testable core of the HTTP status server.
//
// All status-page logic lives here: LED-server protocol response parsing,
// asset layout JSON parsing and discovery, HTML rendering, request routing,
// the socket server, and the LED-server client. There is no process-global
// state, so unit tests can drive every piece directly and end-to-end tests
// can run a real listener.

#include "http_status.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <limits>
#include <map>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#include "led_state.h"  // parse_op_mode / parse_rgb / to_string

namespace ugreen {

// ---------------------------------------------------------------------------
// Minimal JSON parser (recursive descent)
//
// Just enough for the layout files: objects, arrays, strings (full escape
// handling including \uXXXX with surrogate pairs), numbers, booleans, null.
// Strict: any syntax error rejects the whole document.
// ---------------------------------------------------------------------------

namespace {

struct JsonValue {
    enum class Type { Null, Bool, Number, String, Array, Object };
    Type type = Type::Null;
    bool boolean = false;
    double number = 0;
    std::string str;
    std::vector<JsonValue> items;                            // Type::Array
    std::vector<std::pair<std::string, JsonValue>> members;  // Type::Object

    const JsonValue* find(const std::string& key) const {
        if (type != Type::Object) return nullptr;
        for (const auto& m : members)
            if (m.first == key) return &m.second;
        return nullptr;
    }
    bool is_number() const { return type == Type::Number; }
    bool is_string() const { return type == Type::String; }
};

struct JsonParser {
    const std::string& text;
    size_t pos = 0;

    explicit JsonParser(const std::string& t) : text(t) {}

    bool parse(JsonValue& out) {
        skip_ws();
        if (!parse_value(out)) return false;
        skip_ws();
        if (pos != text.size()) return false;  // trailing garbage
        return true;
    }

    void skip_ws() {
        while (pos < text.size() &&
               (text[pos] == ' ' || text[pos] == '\t' ||
                text[pos] == '\n' || text[pos] == '\r'))
            ++pos;
    }

    bool parse_value(JsonValue& out) {
        if (pos >= text.size()) return false;
        char c = text[pos];
        if (c == '{') return parse_object(out);
        if (c == '[') return parse_array(out);
        if (c == '"') {
            out.type = JsonValue::Type::String;
            return parse_string(out.str);
        }
        if (c == 't' || c == 'f') return parse_bool(out);
        if (c == 'n') return parse_null(out);
        return parse_number(out);
    }

    bool parse_object(JsonValue& out) {
        out.type = JsonValue::Type::Object;
        ++pos;  // '{'
        skip_ws();
        if (pos < text.size() && text[pos] == '}') {
            ++pos;
            return true;
        }
        while (true) {
            skip_ws();
            if (pos >= text.size() || text[pos] != '"') return false;
            std::string key;
            if (!parse_string(key)) return false;
            skip_ws();
            if (pos >= text.size() || text[pos] != ':') return false;
            ++pos;
            skip_ws();
            JsonValue v;
            if (!parse_value(v)) return false;
            out.members.emplace_back(std::move(key), std::move(v));
            skip_ws();
            if (pos >= text.size()) return false;
            if (text[pos] == ',') {
                ++pos;
                continue;
            }
            if (text[pos] == '}') {
                ++pos;
                return true;
            }
            return false;
        }
    }

    bool parse_array(JsonValue& out) {
        out.type = JsonValue::Type::Array;
        ++pos;  // '['
        skip_ws();
        if (pos < text.size() && text[pos] == ']') {
            ++pos;
            return true;
        }
        while (true) {
            skip_ws();
            JsonValue v;
            if (!parse_value(v)) return false;
            out.items.push_back(std::move(v));
            skip_ws();
            if (pos >= text.size()) return false;
            if (text[pos] == ',') {
                ++pos;
                continue;
            }
            if (text[pos] == ']') {
                ++pos;
                return true;
            }
            return false;
        }
    }

    bool parse_string(std::string& out) {
        // text[pos] == '"'
        ++pos;
        out.clear();
        while (true) {
            if (pos >= text.size()) return false;  // unterminated
            char c = text[pos++];
            if (c == '"') return true;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (pos >= text.size()) return false;
            char e = text[pos++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    unsigned int cp = 0;
                    if (!parse_hex4(cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF &&
                        pos + 1 < text.size() && text[pos] == '\\' &&
                        text[pos + 1] == 'u') {
                        pos += 2;
                        unsigned int lo = 0;
                        if (!parse_hex4(lo)) return false;
                        if (lo < 0xDC00 || lo > 0xDFFF) return false;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    } else if (cp >= 0xD800 && cp <= 0xDFFF) {
                        return false;  // lone surrogate
                    }
                    append_utf8(out, cp);
                    break;
                }
                default:
                    return false;
            }
        }
    }

    bool parse_hex4(unsigned int& v) {
        if (pos + 4 > text.size()) return false;
        v = 0;
        for (int i = 0; i < 4; ++i) {
            char c = text[pos++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<unsigned>(c - 'A' + 10);
            else return false;
        }
        return true;
    }

    static void append_utf8(std::string& out, unsigned int cp) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    bool parse_number(JsonValue& out) {
        size_t start = pos;
        if (pos < text.size() && text[pos] == '-') ++pos;
        if (pos >= text.size() ||
            !std::isdigit(static_cast<unsigned char>(text[pos])))
            return false;
        while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos])))
            ++pos;
        if (pos < text.size() && text[pos] == '.') {
            ++pos;
            if (pos >= text.size() ||
                !std::isdigit(static_cast<unsigned char>(text[pos])))
                return false;
            while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos])))
                ++pos;
        }
        if (pos < text.size() && (text[pos] == 'e' || text[pos] == 'E')) {
            ++pos;
            if (pos < text.size() && (text[pos] == '+' || text[pos] == '-')) ++pos;
            if (pos >= text.size() ||
                !std::isdigit(static_cast<unsigned char>(text[pos])))
                return false;
            while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos])))
                ++pos;
        }
        std::string num = text.substr(start, pos - start);
        char* endp = nullptr;
        out.number = std::strtod(num.c_str(), &endp);
        if (endp != num.c_str() + num.size()) return false;
        out.type = JsonValue::Type::Number;
        return true;
    }

    bool parse_bool(JsonValue& out) {
        if (text.compare(pos, 4, "true") == 0) {
            out.type = JsonValue::Type::Bool;
            out.boolean = true;
            pos += 4;
            return true;
        }
        if (text.compare(pos, 5, "false") == 0) {
            out.type = JsonValue::Type::Bool;
            out.boolean = false;
            pos += 5;
            return true;
        }
        return false;
    }

    bool parse_null(JsonValue& out) {
        if (text.compare(pos, 4, "null") == 0) {
            out.type = JsonValue::Type::Null;
            pos += 4;
            return true;
        }
        return false;
    }
};

}  // namespace

// ---------------------------------------------------------------------------
// Asset layout parsing and discovery
// ---------------------------------------------------------------------------

std::optional<AssetConfig> parse_asset_json(const std::string& path) {
    std::ifstream f(path);
    if (!f) return std::nullopt;
    std::string text((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());

    JsonValue root;
    JsonParser parser(text);
    if (!parser.parse(root) || root.type != JsonValue::Type::Object)
        return std::nullopt;

    const JsonValue* image_v = root.find("image");
    if (!image_v || !image_v->is_string() || image_v->str.empty())
        return std::nullopt;

    const JsonValue* w = root.find("image_width");
    const JsonValue* h = root.find("image_height");
    if (!w || !h || !w->is_number() || !h->is_number()) return std::nullopt;
    int width = static_cast<int>(w->number);
    int height = static_cast<int>(h->number);
    if (width <= 0 || height <= 0) return std::nullopt;

    const JsonValue* leds_v = root.find("leds");
    if (!leds_v || leds_v->type != JsonValue::Type::Object || leds_v->members.empty())
        return std::nullopt;

    AssetConfig cfg;
    cfg.image = image_v->str;
    cfg.image_width = width;
    cfg.image_height = height;

    for (const auto& m : leds_v->members) {
        const std::string& name = m.first;
        const JsonValue& v = m.second;
        if (name.empty() || v.type != JsonValue::Type::Object) return std::nullopt;

        JsonLed jl;
        jl.name = name;
        if (const JsonValue* x = v.find("x")) {
            if (!x->is_number()) return std::nullopt;
            jl.x = static_cast<int>(x->number);
        }
        if (const JsonValue* y = v.find("y")) {
            if (!y->is_number()) return std::nullopt;
            jl.y = static_cast<int>(y->number);
        }
        if (const JsonValue* r = v.find("radius")) {
            if (!r->is_number()) return std::nullopt;
            jl.radius = static_cast<int>(r->number);
            if (jl.radius <= 0) return std::nullopt;
        }
        if (const JsonValue* label = v.find("label")) {
            if (!label->is_string()) return std::nullopt;
            jl.label = label->str;
        }
        cfg.leds.push_back(std::move(jl));
    }
    return cfg;
}

std::optional<AssetConfig> find_asset(const std::string& assets_dir,
                                      const std::vector<std::string>& led_names) {
    if (assets_dir.empty() || led_names.empty()) return std::nullopt;

    // Preferred: the exact configuration name "<led1>-<led2>-....json"
    // (LED names in the CAPS order the server reported).
    std::string joined;
    for (size_t i = 0; i < led_names.size(); ++i) {
        if (i) joined += '-';
        joined += led_names[i];
    }
    if (auto exact = parse_asset_json(assets_dir + "/" + joined + ".json"))
        return exact;

    // Fallback: any *.json whose name mentions every LED; the shortest name
    // is the most specific match (a superset layout like ...-disk5-....json
    // loses to an exact-shape one).
    DIR* dir = opendir(assets_dir.c_str());
    if (!dir) return std::nullopt;
    std::vector<std::string> candidates;
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string n = entry->d_name;
        if (n.size() <= 5 || n.compare(n.size() - 5, 5, ".json") != 0) continue;
        bool all = true;
        for (const auto& name : led_names) {
            if (n.find(name) == std::string::npos) {
                all = false;
                break;
            }
        }
        if (all) candidates.push_back(std::move(n));
    }
    closedir(dir);

    if (candidates.empty()) return std::nullopt;
    std::sort(candidates.begin(), candidates.end(),
              [](const std::string& a, const std::string& b) {
                  if (a.size() != b.size()) return a.size() < b.size();
                  return a < b;
              });
    for (const auto& c : candidates)
        if (auto cfg = parse_asset_json(assets_dir + "/" + c)) return cfg;
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// LED-server protocol response parsing
// ---------------------------------------------------------------------------

namespace {

// Split a protocol line into whitespace-separated tokens.
std::vector<std::string> split_tokens(const std::string& line) {
    std::vector<std::string> tokens;
    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
        size_t j = i;
        while (j < line.size() && line[j] != ' ' && line[j] != '\t') ++j;
        if (j > i) tokens.push_back(line.substr(i, j - i));
        i = j;
    }
    return tokens;
}

// Decimal non-negative integer in [0, max].
bool parse_uint(const std::string& s, unsigned int max, unsigned int& out) {
    if (s.empty()) return false;
    unsigned int v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + static_cast<unsigned int>(c - '0');
        if (v > max) return false;
    }
    out = v;
    return true;
}

}  // namespace

LedInfo parse_get_response(const std::string& led_name, const std::string& line) {
    LedInfo li;
    li.name = led_name;

    auto tokens = split_tokens(line);
    if (tokens.empty() || tokens[0] != "OK") return li;

    for (size_t i = 1; i < tokens.size(); ++i) {
        const std::string& t = tokens[i];
        size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        std::string key = t.substr(0, eq);
        std::string val = t.substr(eq + 1);

        if (key == "led") {
            if (val != led_name) return li;  // response for another LED
        } else if (key == "mode") {
            auto m = parse_op_mode(val);
            if (!m) return li;
            li.mode = to_string(*m);
        } else if (key == "rgb") {
            auto c = parse_rgb(val);
            if (!c) return li;
            li.r = c->r;
            li.g = c->g;
            li.b = c->b;
        } else if (key == "brightness") {
            unsigned int v;
            if (!parse_uint(val, 255, v)) return li;
            li.brightness = static_cast<int>(v);
        } else if (key == "on_ms" || key == "off_ms") {
            unsigned int v;
            if (!parse_uint(val, 65535, v)) return li;
            if (key == "on_ms") li.on_ms = static_cast<int>(v);
            else li.off_ms = static_cast<int>(v);
        } else if (key == "status") {
            if (val != "valid") return li;
        }
        // Unknown fields are ignored (forward compatibility, PROTOCOL.md §3).
    }

    li.ok = !li.mode.empty();
    return li;
}

std::vector<std::string> parse_caps_leds(const std::string& line) {
    auto tokens = split_tokens(line);
    if (tokens.empty() || tokens[0] != "OK") return {};
    for (const auto& t : tokens) {
        if (t.rfind("leds=", 0) != 0) continue;
        std::vector<std::string> names;
        std::string cur;
        for (char c : t.substr(5)) {
            if (c == ',') {
                if (!cur.empty()) names.push_back(cur);
                cur.clear();
            } else {
                cur += c;
            }
        }
        if (!cur.empty()) names.push_back(cur);
        return names;
    }
    return {};
}

// ---------------------------------------------------------------------------
// HTTP helpers
// ---------------------------------------------------------------------------

std::string url_path(const std::string& request_line) {
    size_t s = request_line.find(' ');
    if (s == std::string::npos) return "/";
    size_t e = request_line.find(' ', s + 1);
    std::string path = (e == std::string::npos)
                           ? request_line.substr(s + 1)
                           : request_line.substr(s + 1, e - s - 1);
    size_t q = path.find('?');
    if (q != std::string::npos) path = path.substr(0, q);
    return path.empty() ? "/" : path;
}

std::string mime_type(const std::string& filename) {
    size_t dot = filename.rfind('.');
    if (dot == std::string::npos) return "application/octet-stream";
    std::string ext = filename.substr(dot + 1);
    if (ext == "html" || ext == "htm") return "text/html; charset=utf-8";
    if (ext == "css") return "text/css";
    if (ext == "js") return "application/javascript";
    if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
    if (ext == "png") return "image/png";
    if (ext == "gif") return "image/gif";
    if (ext == "svg") return "image/svg+xml";
    if (ext == "json") return "application/json";
    if (ext == "ico") return "image/x-icon";
    return "application/octet-stream";
}

std::string HttpResponse::to_wire() const {
    std::ostringstream o;
    o << "HTTP/1.1 " << status_code << ' ' << status_text << "\r\n"
      << "Content-Type: " << content_type << "\r\n"
      << "Content-Length: " << body.size() << "\r\n"
      << "Connection: close\r\n"
      << "\r\n"
      << body;
    return o.str();
}

namespace {

std::string html_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default: out += c;
        }
    }
    return out;
}

std::string human_mode(const LedInfo& li) {
    if (!li.ok) return "unavailable";
    if (li.mode == "off") return "Off";
    if (li.mode == "on") return "On";
    char buf[64];
    if (li.mode == "blink") {
        snprintf(buf, sizeof(buf), "Blink (%d/%d ms)", li.on_ms, li.off_ms);
        return buf;
    }
    if (li.mode == "breath") {
        snprintf(buf, sizeof(buf), "Breath (%d/%d ms)", li.on_ms, li.off_ms);
        return buf;
    }
    return li.mode;
}

}  // namespace

std::string render_html(const std::vector<LedInfo>& leds,
                        const std::optional<AssetConfig>& asset,
                        const std::string& assets_url_prefix) {
    std::ostringstream h;

    std::map<std::string, const LedInfo*> led_map;
    for (const auto& l : leds) led_map[l.name] = &l;

    h << "<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n"
      << "<meta charset=\"utf-8\">\n"
      << "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
      << "<meta http-equiv=\"refresh\" content=\"5\">\n"
      << "<title>UGREEN NAS — LED Status</title>\n"
      << "<style>\n"
      << "  :root {\n"
      << "    --bg: #0f1117;\n"
      << "    --surface: #1a1d27;\n"
      << "    --surface2: #22263a;\n"
      << "    --border: #2e3250;\n"
      << "    --accent: #5b8af5;\n"
      << "    --text: #e2e8f0;\n"
      << "    --text-muted: #8892a4;\n"
      << "    --radius: 12px;\n"
      << "    --font: 'Inter', 'Segoe UI', system-ui, -apple-system, sans-serif;\n"
      << "  }\n"
      << "  * { box-sizing: border-box; margin: 0; padding: 0; }\n"
      << "  body {\n"
      << "    background: var(--bg);\n"
      << "    color: var(--text);\n"
      << "    font-family: var(--font);\n"
      << "    min-height: 100vh;\n"
      << "    padding: 2rem 1rem;\n"
      << "  }\n"
      << "  header {\n"
      << "    text-align: center;\n"
      << "    margin-bottom: 2rem;\n"
      << "  }\n"
      << "  header h1 {\n"
      << "    font-size: 1.6rem;\n"
      << "    font-weight: 700;\n"
      << "    letter-spacing: -0.02em;\n"
      << "    background: linear-gradient(135deg, #5b8af5 0%, #9b72f5 100%);\n"
      << "    -webkit-background-clip: text;\n"
      << "    -webkit-text-fill-color: transparent;\n"
      << "    background-clip: text;\n"
      << "  }\n"
      << "  header p {\n"
      << "    margin-top: 0.4rem;\n"
      << "    color: var(--text-muted);\n"
      << "    font-size: 0.85rem;\n"
      << "  }\n"
      << "  .card {\n"
      << "    background: var(--surface);\n"
      << "    border: 1px solid var(--border);\n"
      << "    border-radius: var(--radius);\n"
      << "    padding: 1.5rem;\n"
      << "    max-width: 860px;\n"
      << "    margin: 0 auto 1.5rem auto;\n"
      << "  }\n"
      << "  /* NAS image + SVG overlay */\n"
      << "  .nas-wrap {\n"
      << "    position: relative;\n"
      << "    display: block;\n"
      << "    line-height: 0;\n"
      << "    border-radius: 8px;\n"
      << "    overflow: hidden;\n"
      << "  }\n"
      << "  .nas-wrap img {\n"
      << "    width: 100%;\n"
      << "    height: auto;\n"
      << "    display: block;\n"
      << "    border-radius: 8px;\n"
      << "  }\n"
      << "  .nas-wrap svg {\n"
      << "    position: absolute;\n"
      << "    top: 0; left: 0;\n"
      << "    width: 100%; height: 100%;\n"
      << "    pointer-events: none;\n"
      << "  }\n"
      << "  /* LED table */\n"
      << "  table {\n"
      << "    width: 100%;\n"
      << "    border-collapse: collapse;\n"
      << "    font-size: 0.9rem;\n"
      << "  }\n"
      << "  thead th {\n"
      << "    text-align: left;\n"
      << "    padding: 0.5rem 0.75rem;\n"
      << "    color: var(--text-muted);\n"
      << "    font-weight: 600;\n"
      << "    font-size: 0.75rem;\n"
      << "    text-transform: uppercase;\n"
      << "    letter-spacing: 0.06em;\n"
      << "    border-bottom: 1px solid var(--border);\n"
      << "  }\n"
      << "  tbody tr {\n"
      << "    border-bottom: 1px solid var(--border);\n"
      << "    transition: background 0.15s;\n"
      << "  }\n"
      << "  tbody tr:last-child { border-bottom: none; }\n"
      << "  tbody tr:hover { background: var(--surface2); }\n"
      << "  td {\n"
      << "    padding: 0.6rem 0.75rem;\n"
      << "    vertical-align: middle;\n"
      << "  }\n"
      << "  .led-dot {\n"
      << "    display: inline-block;\n"
      << "    width: 14px; height: 14px;\n"
      << "    border-radius: 50%;\n"
      << "    vertical-align: middle;\n"
      << "    margin-right: 0.5rem;\n"
      << "    box-shadow: 0 0 6px 2px currentColor;\n"
      << "  }\n"
      << "  .led-label {\n"
      << "    color: var(--text-muted);\n"
      << "    font-size: 0.8em;\n"
      << "    margin-left: 0.5rem;\n"
      << "  }\n"
      << "  .badge {\n"
      << "    display: inline-block;\n"
      << "    padding: 0.15rem 0.55rem;\n"
      << "    border-radius: 999px;\n"
      << "    font-size: 0.75rem;\n"
      << "    font-weight: 600;\n"
      << "    background: var(--surface2);\n"
      << "    color: var(--text-muted);\n"
      << "    border: 1px solid var(--border);\n"
      << "  }\n"
      << "  .badge.on  { background: rgba(91,138,245,0.15); color: #5b8af5; border-color: #5b8af5; }\n"
      << "  .badge.off { background: rgba(80,80,100,0.15);  color: #555870; border-color: #34374d; }\n"
      << "  .badge.blink  { background: rgba(245,185,50,0.15); color: #f5b932; border-color: #f5b932; }\n"
      << "  .badge.breath { background: rgba(130,245,140,0.15); color: #4cd964; border-color: #4cd964; }\n"
      << "  .rgb-swatch {\n"
      << "    display: inline-block;\n"
      << "    width: 18px; height: 18px;\n"
      << "    border-radius: 4px;\n"
      << "    vertical-align: middle;\n"
      << "    border: 1px solid rgba(255,255,255,0.15);\n"
      << "  }\n"
      << "  footer {\n"
      << "    text-align: center;\n"
      << "    color: var(--text-muted);\n"
      << "    font-size: 0.75rem;\n"
      << "    margin-top: 2rem;\n"
      << "  }\n"
      << "  /* Pulse / blink animations for the SVG overlay circles */\n"
      << "  @keyframes led-blink {\n"
      << "    0%,100% { opacity: 1; } 50% { opacity: 0.15; }\n"
      << "  }\n"
      << "  @keyframes led-breath {\n"
      << "    0%,100% { opacity: 0.15; } 50% { opacity: 1; }\n"
      << "  }\n"
      << "  .anim-blink  { animation: led-blink  1s ease-in-out infinite; }\n"
      << "  .anim-breath { animation: led-breath 2s ease-in-out infinite; }\n"
      << "  /* Position from the layout file but not reported by the server */\n"
      << "  .led-missing {\n"
      << "    stroke: rgba(160,165,180,0.7);\n"
      << "    stroke-width: 2;\n"
      << "    stroke-dasharray: 6 6;\n"
      << "  }\n"
      << "</style>\n"
      << "</head>\n<body>\n"
      << "<header>\n"
      << "  <h1>UGREEN NAS — LED Status</h1>\n"
      << "  <p>Live view &middot; auto-refreshes every 5 s</p>\n"
      << "</header>\n";

    // -----------------------------------------------------------------------
    // NAS image card (only when a layout asset is available)
    // -----------------------------------------------------------------------
    if (asset) {
        h << "<div class=\"card\">\n"
          << "  <div class=\"nas-wrap\">\n"
          << "    <img src=\"" << assets_url_prefix << "/" << html_escape(asset->image)
          << "\" alt=\"NAS front panel\" loading=\"lazy\">\n"
          << "    <svg viewBox=\"0 0 " << asset->image_width << " " << asset->image_height
          << "\"\n"
          << "         xmlns=\"http://www.w3.org/2000/svg\">\n"
          << "      <defs>\n"
          << "        <filter id=\"glow\" x=\"-50%\" y=\"-50%\" width=\"200%\" height=\"200%\">\n"
          << "          <feGaussianBlur stdDeviation=\"8\" result=\"blur\"/>\n"
          << "          <feMerge><feMergeNode in=\"blur\"/><feMergeNode in=\"SourceGraphic\"/></feMerge>\n"
          << "        </filter>\n"
          << "      </defs>\n";

        for (const auto& jl : asset->leds) {
            auto it = led_map.find(jl.name);
            if (it == led_map.end()) {
                // Position the layout knows but the server does not report:
                // draw it dim with a dashed outline so the photo still shows
                // every panel position.
                h << "      <circle cx=\"" << jl.x << "\" cy=\"" << jl.y
                  << "\" r=\"" << jl.radius << "\"\n"
                  << "              fill=\"rgba(70,70,80,0.35)\"\n"
                  << "              class=\"led-missing\"/>\n";
                continue;
            }
            const LedInfo& li = *it->second;

            char fill[64];
            if (!li.ok || li.mode == "off") {
                snprintf(fill, sizeof(fill), "rgba(70,70,80,0.55)");
            } else {
                // Mostly opaque so the live color reads over the photo.
                snprintf(fill, sizeof(fill), "rgba(%d,%d,%d,0.82)", li.r, li.g, li.b);
            }

            std::string anim_class;
            if (li.ok && li.mode == "blink") anim_class = " class=\"anim-blink\"";
            if (li.ok && li.mode == "breath") anim_class = " class=\"anim-breath\"";

            h << "      <circle cx=\"" << jl.x << "\" cy=\"" << jl.y
              << "\" r=\"" << jl.radius << "\"\n"
              << "              fill=\"" << fill << "\"\n"
              << "              filter=\"url(#glow)\""
              << anim_class << "/>\n";
        }

        h << "    </svg>\n"
          << "  </div>\n"
          << "</div>\n";
    }

    // -----------------------------------------------------------------------
    // Status table card
    // -----------------------------------------------------------------------
    h << "<div class=\"card\">\n"
      << "  <table>\n"
      << "    <thead><tr>\n"
      << "      <th>LED</th>\n"
      << "      <th>Mode</th>\n"
      << "      <th>Color</th>\n"
      << "      <th>Brightness</th>\n"
      << "      <th>Timing</th>\n"
      << "    </tr></thead>\n"
      << "    <tbody>\n";

    // Label lookup from the layout file (optional).
    auto label_for = [&](const std::string& name) -> const std::string* {
        if (!asset) return nullptr;
        for (const auto& jl : asset->leds)
            if (jl.name == name && !jl.label.empty()) return &jl.label;
        return nullptr;
    };

    for (const auto& li : leds) {
        std::string mode_class = li.ok ? li.mode : "off";

        char dot_style[128];
        if (!li.ok || li.mode == "off") {
            snprintf(dot_style, sizeof(dot_style),
                     "background:rgb(60,60,70); box-shadow:none; color:rgb(60,60,70)");
        } else {
            snprintf(dot_style, sizeof(dot_style),
                     "background:rgb(%d,%d,%d); color:rgb(%d,%d,%d)",
                     li.r, li.g, li.b, li.r, li.g, li.b);
        }

        char swatch_style[64];
        snprintf(swatch_style, sizeof(swatch_style),
                 "background:rgb(%d,%d,%d)", li.r, li.g, li.b);

        std::string timing = "—";
        if (li.ok && (li.mode == "blink" || li.mode == "breath")) {
            char buf[64];
            snprintf(buf, sizeof(buf), "%d / %d ms", li.on_ms, li.off_ms);
            timing = buf;
        }

        h << "    <tr>\n"
          << "      <td><span class=\"led-dot\" style=\"" << dot_style << "\"></span>"
          << html_escape(li.name);
        if (const std::string* label = label_for(li.name))
            h << "<span class=\"led-label\">" << html_escape(*label) << "</span>";
        h << "</td>\n"
          << "      <td><span class=\"badge " << mode_class << "\">"
          << human_mode(li) << "</span></td>\n"
          << "      <td>";
        if (li.ok && li.mode != "off") {
            h << "<span class=\"rgb-swatch\" style=\"" << swatch_style << "\"></span> "
              << "rgb(" << li.r << "," << li.g << "," << li.b << ")";
        } else {
            h << "—";
        }
        h << "</td>\n"
          << "      <td>" << (li.ok ? std::to_string(li.brightness) : "—") << "</td>\n"
          << "      <td>" << timing << "</td>\n"
          << "    </tr>\n";
    }

    h << "    </tbody>\n  </table>\n</div>\n";

    h << "<footer>UGREEN LED Controller &mdash; HTTP Status &mdash; "
      << "<a href=\"/\" style=\"color:var(--accent)\">Refresh now</a></footer>\n"
      << "</body>\n</html>\n";

    return h.str();
}

// ---------------------------------------------------------------------------
// Request routing
// ---------------------------------------------------------------------------

namespace {

HttpResponse make_error(int code, const std::string& text, const std::string& body) {
    HttpResponse r;
    r.status_code = code;
    r.status_text = text;
    r.body = body;
    return r;
}

const char kUnavailablePage[] =
    "<!DOCTYPE html>\n"
    "<html lang=\"en\">\n"
    "<head>\n"
    "<meta charset=\"utf-8\">\n"
    "<meta http-equiv=\"refresh\" content=\"5\">\n"
    "<title>UGREEN NAS — LED Status</title>\n"
    "</head>\n"
    "<body style=\"background:#0f1117;color:#e2e8f0;font-family:system-ui,-apple-system,sans-serif;"
    "text-align:center;padding-top:4rem;\">\n"
    "<h2>Cannot reach LED server</h2>\n"
    "<p>Check that ugreen_leds_server is running and the socket/port is correct.</p>\n"
    "</body>\n"
    "</html>\n";

}  // namespace

HttpResponse handle_request(const std::string& path,
                            const HttpStatusConfig& cfg,
                            const LedQuery& query) {
    if (path == "/" || path == "/index.html") {
        std::vector<LedInfo> leds = query();
        if (leds.empty()) {
            HttpResponse r;
            r.status_code = 503;
            r.status_text = "Service Unavailable";
            r.content_type = "text/html; charset=utf-8";
            r.body = kUnavailablePage;
            return r;
        }

        std::optional<AssetConfig> asset;
        if (!cfg.assets_dir.empty()) {
            std::vector<std::string> names;
            names.reserve(leds.size());
            for (const auto& l : leds) names.push_back(l.name);
            asset = find_asset(cfg.assets_dir, names);
        }

        HttpResponse r;
        r.status_code = 200;
        r.status_text = "OK";
        r.content_type = "text/html; charset=utf-8";
        r.body = render_html(leds, asset, cfg.assets_url_prefix);
        return r;
    }

    const std::string prefix = cfg.assets_url_prefix;
    if (path.size() > prefix.size() + 1 &&
        path.compare(0, prefix.size(), prefix) == 0 &&
        path[prefix.size()] == '/') {
        if (cfg.assets_dir.empty())
            return make_error(404, "Not Found", "Not found\n");
        std::string filename = path.substr(prefix.size() + 1);
        if (filename.empty())
            return make_error(404, "Not Found", "Not found\n");
        // The layout is a flat directory: no subpaths, and no traversal.
        if (filename.find("..") != std::string::npos)
            return make_error(403, "Forbidden", "Forbidden\n");
        if (filename.find('/') != std::string::npos)
            return make_error(404, "Not Found", "Not found\n");
        std::ifstream f(cfg.assets_dir + "/" + filename, std::ios::binary);
        if (!f)
            return make_error(404, "Not Found", "Not found\n");
        std::string body((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
        HttpResponse r;
        r.status_code = 200;
        r.status_text = "OK";
        r.content_type = mime_type(filename);
        r.body = std::move(body);
        return r;
    }

    return make_error(404, "Not Found", "Not found\n");
}

// ---------------------------------------------------------------------------
// Socket plumbing
// ---------------------------------------------------------------------------

namespace {

bool set_socket_timeout_ms(int fd, int ms) {
    timeval tv{};
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0 &&
           setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) == 0;
}

bool send_all(int fd, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        ssize_t n = write(fd, data.data() + sent, data.size() - sent);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

// Read one LF-terminated line (a trailing CR is stripped). Returns false on
// EOF, read error, timeout, or a pathologically long line.
bool read_line(int fd, std::string& out) {
    out.clear();
    char chunk[4096];
    size_t scanned = 0;
    while (true) {
        for (size_t i = scanned; i < out.size(); ++i) {
            if (out[i] == '\n') {
                out.resize(i);
                if (!out.empty() && out.back() == '\r') out.pop_back();
                return true;
            }
        }
        scanned = out.size();
        if (scanned > 65536) return false;
        ssize_t n = read(fd, chunk, sizeof(chunk));
        if (n <= 0) return false;
        out.append(chunk, static_cast<size_t>(n));
    }
}

// Read the request line: the header block is consumed until "\r\n\r\n"
// (or "\n\n"), a bare first line, or EOF; only the first line is kept.
// Returns false when no usable request line was received.
bool read_request_line(int fd, std::string& request_line) {
    std::string buf;
    char chunk[4096];
    while (true) {
        if (buf.find("\r\n\r\n") != std::string::npos) break;
        if (buf.find("\n\n") != std::string::npos) break;
        if (buf.size() > 8192) break;
        ssize_t n = read(fd, chunk, sizeof(chunk));
        if (n <= 0) break;
        buf.append(chunk, static_cast<size_t>(n));
    }
    size_t end = buf.find("\r\n");
    if (end == std::string::npos) end = buf.find('\n');
    request_line = (end == std::string::npos) ? buf : buf.substr(0, end);
    size_t cr = request_line.find('\r');
    if (cr != std::string::npos) request_line.erase(cr);
    return !request_line.empty();
}

// Open a connected socket to the LED server (Unix or TCP) with the
// configured I/O timeout. The protocol session (HELLO + CAPS) is established
// separately by LedStatusClient::connect() so a persistent connection can be
// kept open across queries.
int open_led_socket(const HttpStatusConfig& cfg) {
    int fd;
    if (!cfg.use_tcp) {
        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) return -1;
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        if (cfg.led_sock.size() >= sizeof(addr.sun_path)) {
            close(fd);
            return -1;
        }
        std::strncpy(addr.sun_path, cfg.led_sock.c_str(), sizeof(addr.sun_path) - 1);
        if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            close(fd);
            return -1;
        }
    } else {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return -1;
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(cfg.led_port));
        if (inet_pton(AF_INET, cfg.led_host.c_str(), &addr.sin_addr) != 1) {
            close(fd);
            return -1;
        }
        if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            close(fd);
            return -1;
        }
    }
    // The connection is now kept open across queries, so ask the kernel to
    // notice a dead peer (a no-op on Unix sockets, useful on TCP) — the same
    // as the Python LED agents.
    int keepalive = 1;
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));
    set_socket_timeout_ms(fd, cfg.led_timeout_ms);
    return fd;
}

}  // namespace

// ---------------------------------------------------------------------------
// LED-server client (persistent connection, PROTOCOL.md §10.7)
// ---------------------------------------------------------------------------

namespace {

double now_sec() {
    return std::chrono::duration_cast<std::chrono::duration<double>>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace

LedStatusClient::LedStatusClient(const HttpStatusConfig& cfg) : cfg_(cfg) {}

LedStatusClient::~LedStatusClient() { close(); }

void LedStatusClient::close() {
    connected_ = false;
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    next_rid_ = 0;
    leds_.clear();
}

// One request/response exchange on the persistent connection. The response
// is validated against the request id and verb (PROTOCOL.md §3): an
// unexpected line would desynchronize the stream, so it counts as a dead
// connection.
bool LedStatusClient::transact(const char* verb, const std::string& args,
                               std::string& response) {
    std::string rid = std::to_string(next_rid());
    std::string request = std::string(verb) + " " + rid +
                          (args.empty() ? "" : " " + args);
    if (!send_all(fd_, request + "\n")) return false;
    if (!read_line(fd_, response)) return false;
    auto tokens = split_tokens(response);
    return tokens.size() >= 3 && tokens[0] == "OK" && tokens[1] == rid &&
           tokens[2] == verb;
}

// Request ids wrap like the Python agents do: they stay in 1 .. 2^64-1
// (the server reserves id 0 for its own AUTH_FAILED lines).
uint64_t LedStatusClient::next_rid() {
    next_rid_ = next_rid_ % std::numeric_limits<uint64_t>::max() + 1;
    return next_rid_;
}

bool LedStatusClient::connect() {
    close();
    int fd = open_led_socket(cfg_);
    if (fd < 0) return false;
    fd_ = fd;

    std::string resp;
    if (!transact("HELLO", "1", resp)) {
        close();
        return false;
    }
    if (!transact("CAPS", "", resp)) {
        close();
        return false;
    }
    leds_ = parse_caps_leds(resp);
    if (leds_.empty()) {
        close();  // a server with no LEDs is useless for the status page
        return false;
    }
    connected_ = true;
    return true;
}

bool LedStatusClient::fetch_all(std::vector<LedInfo>& out) {
    out.clear();
    out.reserve(leds_.size());
    for (const auto& name : leds_) {
        std::string resp;
        if (!transact("GET", name + " source=applied", resp)) return false;
        out.push_back(parse_get_response(name, resp));
    }
    return true;
}

// One failed attempt doubles the backoff (capped) and schedules the next
// retry, mirroring the Python agents' reconnect loop.
void LedStatusClient::pace_retry() {
    retry_at_ = now_sec() + backoff_sec_;
    backoff_sec_ = std::min(kBackoffMaxSec, backoff_sec_ * 2.0);
}

std::vector<LedInfo> LedStatusClient::query() {
    // While the backoff after a failed connection attempt is in effect, do
    // not even try (a stopped LED server must not be probed on every page
    // refresh).
    if (!connected_ && now_sec() < retry_at_) return {};

    for (int attempt = 0; attempt < 2; ++attempt) {
        if (!connected_ && !connect()) {
            pace_retry();  // server unreachable
            return {};
        }
        std::vector<LedInfo> result;
        if (fetch_all(result)) {
            backoff_sec_ = kBackoffBaseSec;  // success: reset the backoff
            retry_at_ = 0.0;
            return result;
        }
        close();  // the connection died mid-query (e.g. the LED server
                  // restarted); the next loop iteration retries on a fresh
                  // connection
    }

    // The server accepted the connection but two consecutive queries failed
    // anyway; pace these too rather than retrying on every page refresh.
    pace_retry();
    return {};
}

// ---------------------------------------------------------------------------
// Server
// ---------------------------------------------------------------------------

HttpStatusServer::HttpStatusServer(HttpStatusConfig cfg)
    : cfg_(std::move(cfg)), led_client_(cfg_) {}

HttpStatusServer::~HttpStatusServer() { stop(); }

bool HttpStatusServer::start() {
    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) return false;
    int yes = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(cfg_.http_port));
    if (inet_pton(AF_INET, cfg_.http_bind.c_str(), &addr.sin_addr) != 1) {
        close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }
    if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }
    if (listen(listen_fd_, 16) != 0) {
        close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }
    socklen_t len = sizeof(addr);
    if (getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len) == 0)
        bound_port_ = ntohs(addr.sin_port);

    running_ = true;
    thread_ = std::thread(&HttpStatusServer::run_loop, this);
    return true;
}

void HttpStatusServer::stop() {
    if (!running_.exchange(false)) return;
    if (listen_fd_ >= 0) shutdown(listen_fd_, SHUT_RDWR);  // wake accept()
    if (thread_.joinable()) thread_.join();
    if (listen_fd_ >= 0) {
        close(listen_fd_);
        listen_fd_ = -1;
    }
    // Release the persistent LED-server connection too: once the accept
    // loop is gone nothing can drive the client, and a peer blocked reading
    // our end (e.g. a restarting LED server, or the tests' fake) should see
    // the disconnect.
    led_client_.close();
}

void HttpStatusServer::run_loop() {
    while (running_) {
        sockaddr_in peer{};
        socklen_t peer_len = sizeof(peer);
        int client_fd =
            accept(listen_fd_, reinterpret_cast<sockaddr*>(&peer), &peer_len);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            break;  // stop() shut the listener down
        }
        // Bound how long one client may stall the single accept thread.
        set_socket_timeout_ms(client_fd, 5000);
        std::string req_line;
        if (read_request_line(client_fd, req_line)) {
            // The LED query runs on the persistent client connection (see
            // LedStatusClient); it reuses the open socket and only
            // reconnects when it has failed.
            HttpResponse resp =
                handle_request(url_path(req_line), cfg_,
                               [this]() { return led_client_.query(); });
            send_all(client_fd, resp.to_wire());
        }
        close(client_fd);
    }
}

}  // namespace ugreen
