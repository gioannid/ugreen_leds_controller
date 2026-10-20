// Hardware monkey test for the real UGREEN LED controller (Stage 4).
//
// This binary is built separately (statically linked, single file) so it can
// be copied to the production machine that runs ugreen_leds_server with
// backend=i2c and run there against the LIVE server:
//
//   sudo ./hw_monkey_test --socket /tmp/ugreen_leds.sock --confirm
//
// It drives one physical LED through a long sequence of RANDOM partial
// updates (every combination of mode / rgb / brightness / on_ms / off_ms,
// including edge cycles and deliberately invalid patches), waits for a
// FLUSH barrier after each, and verifies the outcome the way a real client
// can: GET source=hardware must match the expected state, and the
// server's own GET source=applied / source=desired must agree with it.
//
// The expected-state model below is an INDEPENDENT implementation of the
// documented transition contract (PROTOCOL.md section 5.2 plus the server's
// mode/timing resolution rules); it is intentionally not shared code, so a
// disagreement between the model and the server is a real finding.
//
// Run it when no other client is driving the tested LED (ownership makes
// equal-priority competitors harmless, but a higher-priority one would
// distort the verification).
//
// Exit codes:
//   0  all checks passed
//   1  one or more checks failed
//   2  usage or environment error (cannot connect, no suitable LED, ...)
//   3  cancelled by the user

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <optional>
#include <random>
#include <string>
#include <vector>

namespace {

int g_checks_passed = 0;
int g_checks_failed = 0;

struct Options {
    std::string socket_path = "/tmp/ugreen_leds.sock";
    bool use_tcp = false;
    std::string tcp_ip = "127.0.0.1";
    int tcp_port = 39001;
    std::string led;              // empty: auto-select
    bool include_power = false;
    int iterations = 200;
    uint64_t seed = 0;            // 0: time-based
    int settle_ms = 50;
    bool confirm = false;
    bool help = false;
};

void show_help() {
    std::cout
        << "Usage: hw_monkey_test [options]\n"
        << "\n"
        << "Drives one physical NAS front-panel LED through a long sequence\n"
        << "of random partial updates against a RUNNING ugreen_leds_server\n"
        << "(backend=i2c), verifies every step with GET source=hardware and\n"
        << "cross-checks the server's applied/desired state. A FLUSH barrier\n"
        << "is waited for after every accepted update.\n"
        << "\n"
        << "The LED's original state is restored at the end (note: when this\n"
        << "test disconnects, the server applies its configured fallback to\n"
        << "the LED it owned, per the ownership rules).\n"
        << "\n"
        << "Options:\n"
        << "  --socket <path>     server Unix socket (default /tmp/ugreen_leds.sock)\n"
        << "  --tcp <ip> <port>   use TCP instead of the Unix socket\n"
        << "  --led <name>        LED to test (default: first available disk LED;\n"
        << "                      the power LED requires --include-power)\n"
        << "  --include-power     allow testing the power LED\n"
        << "  --iterations <n>    number of random transitions (default 200)\n"
        << "  --seed <n>          RNG seed for reproducibility (default: time-based)\n"
        << "  --settle-ms <n>     extra settle time after FLUSH, before readback\n"
        << "  --confirm           ask the user to confirm before starting\n"
        << "  --help              display this help\n"
        << "\n"
        << "Exit codes: 0 = all checks passed, 1 = checks failed,\n"
        << "            2 = usage/environment error, 3 = cancelled by user\n";
}

void check(bool ok, const std::string& name, const std::string& detail = "") {
    if (ok) {
        ++g_checks_passed;
        std::cout << "  [PASS] " << name;
    } else {
        ++g_checks_failed;
        std::cout << "  [FAIL] " << name;
    }
    if (!detail.empty()) {
        std::cout << " (" << detail << ")";
    }
    std::cout << "\n";
}

bool ask_yes(const std::string& prompt) {
    std::cout << prompt << " [y/N] ";
    std::cout.flush();
    std::string line;
    if (!std::getline(std::cin, line)) {
        return false;
    }
    return line == "y" || line == "Y";
}

// ---------------------------------------------------------------------------
// Expected-state model (independent implementation of the server contract).
// ---------------------------------------------------------------------------

struct State {
    std::string mode = "off";  // off | on | blink | breath
    int brightness = 0;
    std::string rgb = "0,0,0";
    int on_ms = 0;
    int off_ms = 0;

    bool is_animation() const { return mode == "blink" || mode == "breath"; }

    std::string describe() const {
        std::string out = "mode=" + mode + " rgb=" + rgb +
                          " brightness=" + std::to_string(brightness);
        if (is_animation()) {
            out += " on_ms=" + std::to_string(on_ms) + " off_ms=" + std::to_string(off_ms);
        }
        return out;
    }
};

struct Patch {
    std::optional<std::string> mode;
    std::optional<int> brightness;
    std::optional<std::string> rgb;
    std::optional<int> on_ms;
    std::optional<int> off_ms;

    bool empty() const {
        return !mode && !brightness && !rgb && !on_ms && !off_ms;
    }

    std::string describe() const {
        std::string out;
        if (mode) out += "mode=" + *mode + " ";
        if (rgb) out += "rgb=" + *rgb + " ";
        if (brightness) out += "brightness=" + std::to_string(*brightness) + " ";
        if (on_ms) out += "on_ms=" + std::to_string(*on_ms) + " ";
        if (off_ms) out += "off_ms=" + std::to_string(*off_ms) + " ";
        while (!out.empty() && out.back() == ' ') out.pop_back();
        return out;
    }
};

void default_cycle_for(const std::string& mode, int& on, int& off) {
    if (mode == "breath") {
        on = 1000;
        off = 1000;
    } else {
        on = 500;
        off = 500;
    }
}

// Resolves a patch against the current expected state, mirroring the server
// contract:
//   * parser: when both on_ms and off_ms are present, the sum must be <=
//     65535, and > 0 when the effective mode is blink/breath (the effective
//     mode is the given mode, or blink when mode is omitted);
//   * server: a single-field update must not push the resolved cycle over
//     65535 against the retained field;
//   * resolution: timing without a mode implies blink; entering blink/breath
//     from a different mode without explicit timing uses the mode's default
//     cycle; a zero cycle in blink/breath falls back to the default cycle;
//     off/on states carry no active cycle.
// Returns false (and sets *rejected) when the server must reject the SET.
bool resolve_patch(const State& cur, const Patch& p, State& out, bool* rejected) {
    *rejected = false;
    out = cur;

    const bool timing_given = p.on_ms.has_value() || p.off_ms.has_value();
    std::string m = p.mode ? *p.mode : cur.mode;
    int on = p.on_ms.value_or(cur.on_ms);
    int off = p.off_ms.value_or(cur.off_ms);

    // Timing without an explicit mode implies entering blink.
    if (!p.mode && timing_given && m != "blink" && m != "breath") {
        m = "blink";
    }

    // Parser-level rejections (both fields present).
    if (p.on_ms && p.off_ms) {
        int sum = *p.on_ms + *p.off_ms;
        std::string eff = p.mode ? *p.mode : "blink";
        if (sum > 65535 || ((eff == "blink" || eff == "breath") && sum == 0)) {
            *rejected = true;
            return false;
        }
    }

    // Entering an animation mode from a different mode without explicit
    // timing: the mode's default cycle, not a stale inherited one.
    if (p.mode && (m == "blink" || m == "breath") && !p.on_ms && !p.off_ms &&
        cur.mode != m) {
        default_cycle_for(m, on, off);
    }
    // A zero cycle in an animation mode falls back to the default cycle.
    if ((m == "blink" || m == "breath") && on + off == 0) {
        default_cycle_for(m, on, off);
    }
    // Off/on states carry no active cycle.
    if (m == "off" || m == "on") {
        on = 0;
        off = 0;
    }

    // Server-level rejection: the resolved cycle must fit 16 bits.
    if ((m == "blink" || m == "breath") && on + off > 65535) {
        *rejected = true;
        return false;
    }

    out.mode = m;
    out.on_ms = on;
    out.off_ms = off;
    if (p.brightness) out.brightness = *p.brightness;
    if (p.rgb) out.rgb = *p.rgb;
    return true;
}

// ---------------------------------------------------------------------------
// Random patch generation.
// ---------------------------------------------------------------------------

const std::vector<std::string>& mode_names() {
    static const std::vector<std::string> m = {"off", "on", "blink", "breath"};
    return m;
}

// Vivid palette so every transition is visible by eye, plus one random color.
const std::vector<std::string>& rgb_palette() {
    static const std::vector<std::string> c = {
        "255,0,0", "0,255,0", "0,0,255", "255,255,0",
        "255,0,255", "0,255,255", "255,255,255", "128,64,0",
    };
    return c;
}

// Includes deliberate edges: very fast (5 ms) and very slow (60 s) halves.
const std::vector<int>& timing_pool() {
    static const std::vector<int> t = {5, 10, 25, 50, 100, 200, 500, 1000,
                                       2000, 5000, 15000, 30000, 60000};
    return t;
}

template <typename T>
T pick(std::mt19937& rng, const std::vector<T>& v) {
    std::uniform_int_distribution<size_t> d(0, v.size() - 1);
    return v[d(rng)];
}

Patch make_random_patch(const State& /*exp*/, std::mt19937& rng) {
    std::uniform_int_distribution<int> d01(0, 99);
    Patch p;

    // ~5% deliberately invalid (zero cycle or 16-bit overflow): the server
    // must reject them and leave the state untouched.
    if (d01(rng) < 5) {
        if (d01(rng) < 50) {
            p.mode = "blink";
            p.on_ms = 0;
            p.off_ms = 0;
        } else {
            p.mode = "breath";
            p.on_ms = 60000;
            p.off_ms = 10000;
        }
        return p;
    }

    if (d01(rng) < 55) {
        p.mode = pick(rng, mode_names());
    }
    if (d01(rng) < 45) {
        p.rgb = pick(rng, rgb_palette());
    } else if (d01(rng) < 50) {
        std::uniform_int_distribution<int> d255(0, 255);
        p.rgb = std::to_string(d255(rng)) + "," + std::to_string(d255(rng)) + "," +
                std::to_string(d255(rng));
    }
    if (d01(rng) < 45) {
        std::uniform_int_distribution<int> d255(0, 255);
        p.brightness = d255(rng);
    }

    int t = d01(rng);
    if (t < 35) {
        p.on_ms = pick(rng, timing_pool());
        p.off_ms = pick(rng, timing_pool());
        // Keep both-field patches inside the 16-bit cycle (the invalid
        // overflow cases come from the 5% bucket above).
        while (*p.on_ms + *p.off_ms > 65535) {
            *p.off_ms = std::max(1, 65535 - *p.on_ms);
        }
    } else if (t < 45) {
        p.on_ms = pick(rng, timing_pool());
    } else if (t < 55) {
        p.off_ms = pick(rng, timing_pool());
    }

    // A SET without any field is a syntax error, not a transition.
    if (p.empty()) {
        p.mode = pick(rng, mode_names());
    }
    return p;
}

// ---------------------------------------------------------------------------
// Protocol client.
// ---------------------------------------------------------------------------

class Client {
public:
    bool connect_unix(const std::string& path) {
        fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd_ < 0) return false;
        setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv_, sizeof(tv_));
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
        return ::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    }

    bool connect_tcp(const std::string& ip, int port) {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) return false;
        setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv_, sizeof(tv_));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);
        return ::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    }

    ~Client() {
        if (fd_ >= 0) ::close(fd_);
    }

    bool send_line(const std::string& line) {
        std::string msg = line + "\n";
        size_t total = 0;
        while (total < msg.size()) {
            ssize_t n = ::write(fd_, msg.data() + total, msg.size() - total);
            if (n <= 0) return false;
            total += static_cast<size_t>(n);
        }
        return true;
    }

    // Returns the response line, or empty string on timeout/EOF.
    std::string read_line() {
        std::string line;
        char c = 0;
        while (::read(fd_, &c, 1) == 1) {
            if (c == '\n') break;
            line += c;
        }
        if (!line.empty() && line.back() == '\r') line.pop_back();
        return line;
    }

private:
    int fd_ = -1;
    timeval tv_{5, 0};
};

struct Response {
    bool ok = false;
    uint64_t id = 0;
    std::string action;
    std::string field(const std::string& key) const {
        for (const auto& kv : fields) {
            if (kv.first == key) return kv.second;
        }
        return "";
    }
    std::vector<std::pair<std::string, std::string>> fields;
};

Response parse_response(const std::string& line) {
    Response r;
    std::vector<std::string> tokens;
    std::string cur;
    for (char c : line) {
        if (c == ' ') {
            if (!cur.empty()) {
                tokens.push_back(cur);
                cur.clear();
            }
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) tokens.push_back(cur);
    if (tokens.size() < 3) return r;
    r.ok = (tokens[0] == "OK");
    r.id = std::stoull(tokens[1]);
    r.action = tokens[2];
    for (size_t i = 3; i < tokens.size(); ++i) {
        auto eq = tokens[i].find('=');
        if (eq != std::string::npos) {
            r.fields.emplace_back(tokens[i].substr(0, eq), tokens[i].substr(eq + 1));
        }
    }
    return r;
}

State state_from_get(const Response& r) {
    State s;
    s.mode = r.field("mode");
    s.brightness = r.field("brightness").empty() ? 0 : std::stoi(r.field("brightness"));
    s.rgb = r.field("rgb");
    s.on_ms = r.field("on_ms").empty() ? 0 : std::stoi(r.field("on_ms"));
    s.off_ms = r.field("off_ms").empty() ? 0 : std::stoi(r.field("off_ms"));
    return s;
}

// Compares a hardware readback with the expected state. In off/on modes the
// chip retains its cycle registers, so timing is only compared for active
// blink/breath states.
bool hardware_matches(const State& hw, const State& exp) {
    if (hw.mode != exp.mode) return false;
    if (hw.brightness != exp.brightness || hw.rgb != exp.rgb) return false;
    if (exp.is_animation()) {
        return hw.on_ms == exp.on_ms && hw.off_ms == exp.off_ms;
    }
    return true;
}

// Full (normalized) state comparison for the server's logical states.
bool logical_matches(const State& got, const State& exp) {
    return got.mode == exp.mode && got.brightness == exp.brightness &&
           got.rgb == exp.rgb && got.on_ms == exp.on_ms && got.off_ms == exp.off_ms;
}

// GET source=hardware with retries for the server-wide hardware-read rate
// limit. Returns the parsed response; r.ok is false when no valid GET
// response could be obtained.
Response get_hardware(Client& client, const std::string& led,
                      std::function<uint64_t()> next_id) {
    Response last;
    for (int attempt = 0; attempt < 8; ++attempt) {
        if (!client.send_line("GET " + std::to_string(next_id()) + " " + led +
                              " source=hardware")) {
            return Response{};
        }
        Response r = parse_response(client.read_line());
        last = r;
        if (r.ok && r.action == "GET") {
            return r;
        }
        if (r.action == "RATE_LIMITED") {
            ::usleep(150 * 1000);
            continue;
        }
        break;
    }
    return last;
}

} // namespace

int main(int argc, char* argv[]) {
    Options opts;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--socket" && i + 1 < argc) {
            opts.socket_path = argv[++i];
        } else if (arg == "--tcp" && i + 2 < argc) {
            opts.use_tcp = true;
            opts.tcp_ip = argv[++i];
            opts.tcp_port = std::stoi(argv[++i]);
        } else if (arg == "--led" && i + 1 < argc) {
            opts.led = argv[++i];
        } else if (arg == "--include-power") {
            opts.include_power = true;
        } else if (arg == "--iterations" && i + 1 < argc) {
            opts.iterations = std::stoi(argv[++i]);
        } else if (arg == "--seed" && i + 1 < argc) {
            opts.seed = std::stoull(argv[++i]);
        } else if (arg == "--settle-ms" && i + 1 < argc) {
            opts.settle_ms = std::stoi(argv[++i]);
        } else if (arg == "--confirm") {
            opts.confirm = true;
        } else if (arg == "--help" || arg == "-h") {
            opts.help = true;
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            show_help();
            return 2;
        }
    }

    if (opts.help) {
        show_help();
        return 0;
    }
    if (opts.iterations < 1) opts.iterations = 1;
    if (opts.iterations > 100000) opts.iterations = 100000;
    if (opts.settle_ms < 0) opts.settle_ms = 0;

    std::cout << "UGREEN LED hardware monkey test\n";
    std::cout << "================================\n";
    std::cout << "This test changes the state of a physical LED on the NAS front\n";
    std::cout << "panel while the LED server is running, and restores the original\n";
    std::cout << "state at the end.\n";

    Client client;
    if (opts.use_tcp) {
        if (!client.connect_tcp(opts.tcp_ip, opts.tcp_port)) {
            std::cerr << "ERROR: cannot connect to TCP " << opts.tcp_ip << ":"
                      << opts.tcp_port << " (is the server running?)\n";
            return 2;
        }
    } else {
        if (!client.connect_unix(opts.socket_path)) {
            std::cerr << "ERROR: cannot connect to " << opts.socket_path
                      << " (is the server running with that socket?)\n";
            return 2;
        }
    }

    // --- HELLO + CAPS -----------------------------------------------------
    if (!client.send_line("HELLO 1 1")) return 2;
    std::string hello_line = client.read_line();
    Response hello = parse_response(hello_line);
    if (!hello.ok || hello.action != "HELLO") {
        std::cerr << "ERROR: HELLO rejected: "
                  << (hello_line.empty() ? std::string("no response (auth denied?)") : hello_line)
                  << "\n";
        return 2;
    }

    if (!client.send_line("CAPS 2")) return 2;
    std::string caps_line = client.read_line();
    Response caps = parse_response(caps_line);
    if (!caps.ok || caps.action != "CAPS") {
        std::cerr << "ERROR: CAPS rejected: "
                  << (caps_line.empty() ? std::string("no response") : caps_line) << "\n";
        return 2;
    }
    std::vector<std::string> supported;
    {
        std::string leds = caps.field("leds");
        size_t start = 0;
        while (start <= leds.size()) {
            size_t comma = leds.find(',', start);
            std::string name = (comma == std::string::npos)
                                   ? leds.substr(start)
                                   : leds.substr(start, comma - start);
            if (!name.empty()) supported.push_back(name);
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
    }
    if (supported.empty()) {
        std::cerr << "ERROR: server reports no LEDs\n";
        return 2;
    }

    // --- LED selection (same preference as the smoke test) -----------------
    static const std::vector<std::string> kOrder = {
        "power", "netdev", "disk1", "disk2", "disk3", "disk4",
        "disk5", "disk6", "disk7", "disk8",
    };
    auto is_supported = [&supported](const std::string& n) {
        for (const auto& s : supported) {
            if (s == n) return true;
        }
        return false;
    };

    std::string led;
    if (!opts.led.empty()) {
        led = opts.led;
        if (led == "power" && !opts.include_power) {
            std::cerr << "ERROR: refusing to test the power LED without --include-power\n";
            return 2;
        }
        if (!is_supported(led)) {
            std::cerr << "ERROR: LED " << led << " is not supported by this server\n";
            return 2;
        }
    } else {
        for (const auto& n : kOrder) {
            if (n == "power" || n == "netdev") continue;
            if (is_supported(n)) {
                led = n;
                break;
            }
        }
        if (led.empty() && is_supported("netdev")) led = "netdev";
        if (led.empty()) {
            std::cerr << "ERROR: no suitable LED found; pass --led <name> explicitly\n";
            return 2;
        }
    }

    std::cout << "Server LEDs:  ";
    for (const auto& s : supported) std::cout << s << " ";
    std::cout << "\n";
    std::cout << "Test LED:     " << led << "\n";
    std::cout << "Iterations:   " << opts.iterations << "\n";
    uint64_t seed = opts.seed;
    if (seed == 0) {
        seed = std::chrono::steady_clock::now().time_since_epoch().count();
    }
    std::cout << "Seed:         " << seed << "\n";
    std::cout << "Settle time:  " << opts.settle_ms << " ms\n";

    if (opts.confirm && !ask_yes("\nProceed with the monkey test?")) {
        std::cout << "Cancelled by user.\n";
        return 3;
    }

    std::mt19937 rng(static_cast<std::mt19937::result_type>(seed));
    uint64_t next_id = 100;
    auto next_request_id = [&next_id]() { return ++next_id; };

    // --- Baseline: the actual hardware state ------------------------------
    Response base = get_hardware(client, led, next_request_id);
    if (!base.ok) {
        std::cerr << "ERROR: baseline hardware read failed: "
                  << (base.action.empty() ? "no response" : base.action) << "\n";
        return 2;
    }
    State expected = state_from_get(base);
    if (expected.mode != "blink" && expected.mode != "breath") {
        expected.on_ms = 0;
        expected.off_ms = 0;
    }
    State original = expected;
    std::cout << "Original state: " << expected.describe() << "\n";

    // --- Establish step: a full-state update guarantees the first batch
    // runs (ownership acquired, scheduler baseline confirmed on the panel).
    {
        std::string line = "SET " + std::to_string(next_request_id()) + " " + led +
                           " mode=on brightness=128 rgb=255,0,0";
        if (!client.send_line(line)) return 2;
        Response r = parse_response(client.read_line());
        check(r.ok && r.action == "ACCEPTED", "establish: SET mode=on red");

        if (client.send_line("FLUSH " + std::to_string(next_request_id()))) {
            Response f = parse_response(client.read_line());
            if (!f.ok || f.action != "FLUSH") {
                check(false, "establish: FLUSH",
                      f.action.empty() ? "no response (timeout?)" : f.action);
            }
        }
        ::usleep(opts.settle_ms * 1000);
        Response h = get_hardware(client, led, next_request_id);
        check(h.ok && hardware_matches(state_from_get(h), State{"on", 128, "255,0,0", 0, 0}),
              "establish: hardware shows solid red",
              h.ok ? state_from_get(h).describe()
                   : (h.action.empty() ? std::string("no response") : h.action));
        expected = State{"on", 128, "255,0,0", 0, 0};
    }

    // --- Random transition loop -------------------------------------------
    int failed_iterations = 0;
    int invalid_seen = 0;

    for (int iter = 1; iter <= opts.iterations; ++iter) {
        Patch p = make_random_patch(expected, rng);
        bool rejected = false;
        State after;
        resolve_patch(expected, p, after, &rejected);
        if (rejected) ++invalid_seen;

        std::cout << "iter " << iter << ": SET " << (p.empty() ? "(none)" : p.describe())
                  << (rejected ? "   [expect rejection]" : "") << "\n";

        uint64_t id = next_request_id();
        std::string set_line = "SET " + std::to_string(id) + " " + led;
        if (!p.empty()) set_line += " " + p.describe();
        if (!client.send_line(set_line)) {
            check(false, "iter " + std::to_string(iter) + ": SET transport", "write failed");
            break;
        }
        Response r = parse_response(client.read_line());

        if (rejected) {
            // The server must reject with INVALID_ARGUMENT field=on_ms and
            // leave every state untouched.
            bool ok = r.ok == false && r.action == "INVALID_ARGUMENT" &&
                      r.field("field") == "on_ms";
            check(ok, "iter " + std::to_string(iter) + ": invalid patch rejected",
                  ok ? "" : "response: " + (r.action.empty() ? std::string("no response")
                                                             : (r.ok ? "OK " : "ERR ") + r.action));
            if (!ok) {
                ++failed_iterations;
            } else {
                // State must be unchanged.
                Response h = get_hardware(client, led, next_request_id);
                check(h.ok && hardware_matches(state_from_get(h), expected),
                      "iter " + std::to_string(iter) + ": state unchanged after rejection",
                      h.ok ? state_from_get(h).describe() : "no response");
                if (client.send_line("GET " + std::to_string(next_request_id()) + " " + led +
                                     " source=applied")) {
                    Response a = parse_response(client.read_line());
                    check(a.ok && logical_matches(state_from_get(a), expected),
                          "iter " + std::to_string(iter) + ": applied unchanged after rejection",
                          a.ok ? state_from_get(a).describe() : "no response");
                }
            }
            if (failed_iterations >= 20) {
                std::cerr << "Stopping: too many failures.\n";
                break;
            }
            continue;
        }

        // Valid patch: it must be accepted.
        if (!r.ok || r.action != "ACCEPTED") {
            check(false, "iter " + std::to_string(iter) + ": SET accepted",
                  r.ok ? "action=" + r.action : "ERR " + r.action +
                            (r.field("field").empty() ? "" : " field=" + r.field("field")));
            ++failed_iterations;
            if (failed_iterations >= 20) {
                std::cerr << "Stopping: too many failures.\n";
                break;
            }
            continue;
        }

        // Barrier: the hardware must have been written before FLUSH returns.
        bool flush_ok = false;
        std::string flush_detail;
        if (client.send_line("FLUSH " + std::to_string(next_request_id()))) {
            Response f = parse_response(client.read_line());
            flush_ok = f.ok && f.action == "FLUSH";
            if (!flush_ok) {
                flush_detail = f.action.empty() ? std::string("no response (timeout?)")
                                                : (f.ok ? "OK " : "ERR ") + f.action;
            }
        } else {
            flush_detail = "write failed";
        }
        check(flush_ok, "iter " + std::to_string(iter) + ": FLUSH barrier", flush_detail);

        ::usleep(opts.settle_ms * 1000);

        // Verification against the hardware (the reliable ground truth),
        // with retries for the server-wide hardware-read rate limit.
        Response h = get_hardware(client, led, next_request_id);
        std::string hw_detail;
        if (h.ok) {
            hw_detail = hardware_matches(state_from_get(h), after)
                            ? ""
                            : "expected " + after.describe() + ", hardware: " +
                                  state_from_get(h).describe();
        } else {
            hw_detail = h.action.empty() ? std::string("no response")
                                         : (h.ok ? "OK " : "ERR ") + h.action;
        }
        check(h.ok && hardware_matches(state_from_get(h), after),
              "iter " + std::to_string(iter) + ": hardware matches expected", hw_detail);

        // The server's logical states must agree with the hardware truth.
        if (client.send_line("GET " + std::to_string(next_request_id()) + " " + led +
                             " source=applied")) {
            Response a = parse_response(client.read_line());
            check(a.ok && logical_matches(state_from_get(a), after),
                  "iter " + std::to_string(iter) + ": applied matches expected",
                  a.ok ? state_from_get(a).describe() : "no response");
        }
        if (client.send_line("GET " + std::to_string(next_request_id()) + " " + led +
                             " source=desired")) {
            Response d = parse_response(client.read_line());
            check(d.ok && logical_matches(state_from_get(d), after),
                  "iter " + std::to_string(iter) + ": desired matches expected",
                  d.ok ? state_from_get(d).describe() : "no response");
        }

        expected = after;

        if (iter % 20 == 0) {
            std::cout << "  ... " << iter << "/" << opts.iterations << " iterations done\n";
        }
        if (failed_iterations >= 20) {
            std::cerr << "Stopping: too many failures.\n";
            break;
        }
    }

    // --- Restore the original state ----------------------------------------
    std::cout << "\nRestoring original state: " << original.describe() << "\n";
    {
        std::string line = "SET " + std::to_string(next_request_id()) + " " + led +
                           " mode=" + original.mode +
                           " brightness=" + std::to_string(original.brightness) +
                           " rgb=" + original.rgb;
        if (original.mode == "blink" || original.mode == "breath") {
            line += " on_ms=" + std::to_string(original.on_ms) +
                    " off_ms=" + std::to_string(original.off_ms);
        }
        bool restored = false;
        std::string detail;
        if (client.send_line(line)) {
            Response r = parse_response(client.read_line());
            if (r.ok && r.action == "ACCEPTED" &&
                client.send_line("FLUSH " + std::to_string(next_request_id()))) {
                Response f = parse_response(client.read_line());
                if (f.ok && f.action == "FLUSH") {
                    ::usleep(opts.settle_ms * 1000);
                    Response h = get_hardware(client, led, next_request_id);
                    restored = h.ok && hardware_matches(state_from_get(h), original);
                    if (!restored) {
                        detail = h.ok ? state_from_get(h).describe() : "no response";
                    }
                } else {
                    detail = f.action.empty() ? std::string("flush timeout")
                                              : "flush: " + f.action;
                }
            } else {
                detail = r.ok ? "set: " + r.action : "set rejected: " + r.action;
            }
        }
        check(restored, "restore original state", restored ? "" : detail);
        if (!restored) {
            std::cerr << "WARNING: the LED may not match its original state; when this\n"
                      << "         test disconnects the server applies its configured\n"
                      << "         fallback to the LED.\n";
        }
    }

    std::cout << "\nSummary: " << g_checks_passed << " passed, " << g_checks_failed
              << " failed (" << opts.iterations << " iterations, " << invalid_seen
              << " invalid patches exercised)\n";
    if (g_checks_failed == 0) {
        std::cout << "Hardware monkey test PASSED\n";
        return 0;
    }
    std::cout << "Hardware monkey test FAILED\n";
    return 1;
}
