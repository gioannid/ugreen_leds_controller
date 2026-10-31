#ifndef UGREEN_SERVER_HTTP_STATUS_H
#define UGREEN_SERVER_HTTP_STATUS_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// Lightweight HTTP status server for ugreen_leds_server.
//
// The implementation is split into two layers:
//   - this header + src/http_status.cpp: all of the status-page logic, free
//     of process-global state and unit-testable (protocol-response parsing,
//     asset JSON parsing and discovery, HTML rendering, request routing, the
//     socket server and the LED-server client);
//   - src/http_status_server.cpp: the executable's main() and CLI parsing.
//
// Serves:
//   GET /          — HTML status page (auto-refreshes every 5 s)
//   GET /assets/*  — static files from the --assets-dir directory
//
// If the assets directory contains a layout JSON matching the current set of
// LEDs (named after the LED configuration, e.g.
// "power-netdev-disk1-disk2-disk3-disk4.json"), the page overlays the live
// LED state on the NAS photo; in either case a readable status table is
// rendered below.

namespace ugreen {

// One LED as reported by the LED server for the status page.
struct LedInfo {
    std::string name;
    std::string mode;   // "off" | "on" | "blink" | "breath"; empty when unknown
    int r = 255, g = 0, b = 0;
    int brightness = 200;
    int on_ms = 0, off_ms = 0;
    bool ok = false;    // false = could not query / state not confirmed
};

// One LED position from an asset layout JSON file.
struct JsonLed {
    std::string name;
    int x = 0;
    int y = 0;
    int radius = 16;
    std::string label;
};

// A parsed asset layout: a photo of the NAS panel plus LED positions.
struct AssetConfig {
    std::string image;   // filename inside the assets directory
    int image_width = 0;
    int image_height = 0;
    std::vector<JsonLed> leds;
};

// A complete HTTP response, ready to be written to the socket.
struct HttpResponse {
    int status_code = 200;
    std::string status_text = "OK";
    std::string content_type = "text/plain; charset=utf-8";
    std::string body;

    // "HTTP/1.1 <code> <text>\r\n..." plus body (Connection: close).
    std::string to_wire() const;
};

struct HttpStatusConfig {
    // LED server endpoint.
    std::string led_sock = "/tmp/ugreen_leds.sock";
    std::string led_host = "127.0.0.1";
    int led_port = 39001;
    bool use_tcp = false;
    int led_timeout_ms = 2000;  // I/O timeout while querying the LED server

    // Assets.
    std::string assets_dir;
    std::string assets_url_prefix = "/assets";

    // HTTP listener.
    std::string http_bind = "0.0.0.0";
    int http_port = 8080;
};

// ---------------------------------------------------------------------------
// LED-server protocol parsing
// ---------------------------------------------------------------------------

// Parse one "OK <id> GET led=... source=... mode=... ..." response line
// expected for LED `led_name`. ok=false is returned for ERR lines,
// unparseable values, a mismatched led= field, or status=invalid.
LedInfo parse_get_response(const std::string& led_name, const std::string& line);

// Extract the LED name list from a CAPS response line.
// Empty on failure.
std::vector<std::string> parse_caps_leds(const std::string& line);

// ---------------------------------------------------------------------------
// Assets
// ---------------------------------------------------------------------------

// Parse an asset layout JSON file. nullopt when the file is missing or
// malformed (must be a JSON object with a non-empty "image" string, positive
// integer "image_width"/"image_height", and a "leds" object with at least
// one entry).
std::optional<AssetConfig> parse_asset_json(const std::string& path);

// Find the layout for the given set of LED names inside `assets_dir`:
// first an exact "<name1>-<name2>-....json" match (names in the given
// order, i.e. CAPS order), then any *.json file whose name mentions every
// LED name (the shortest name is the most specific match). nullopt when
// nothing matches or matches are unparseable.
std::optional<AssetConfig> find_asset(const std::string& assets_dir,
                                      const std::vector<std::string>& led_names);

// ---------------------------------------------------------------------------
// HTTP helpers
// ---------------------------------------------------------------------------

// Extract the URL path from an HTTP request line ("GET /x HTTP/1.1" → "/x").
// Query strings are stripped; an empty or version-less request line yields
// its path part, or "/" when there is none.
std::string url_path(const std::string& request_line);

// MIME type by file extension.
std::string mime_type(const std::string& filename);

// Render the status page: the NAS photo with an SVG LED overlay (only when
// `asset` is set) and a readable status table for every queried LED.
std::string render_html(const std::vector<LedInfo>& leds,
                        const std::optional<AssetConfig>& asset,
                        const std::string& assets_url_prefix);

// Route one request path to a response. `query` fetches the current LED
// state (an empty vector means the LED server is unreachable and produces a
// 503 page); injecting it keeps the routing unit-testable.
using LedQuery = std::function<std::vector<LedInfo>()>;
HttpResponse handle_request(const std::string& path,
                            const HttpStatusConfig& cfg,
                            const LedQuery& query);

// ---------------------------------------------------------------------------
// LED-server client and server
// ---------------------------------------------------------------------------

// Persistent, read-only client for the LED server (PROTOCOL.md §10.7: one
// persistent connection per agent, not one per command).
//
// The client keeps a single connection open for as long as it is healthy:
// HELLO + CAPS are exchanged once at (re)connect time, and every query after
// that sends only GET commands. When a query fails because the connection
// died (LED server restarted, connection reset, unparseable response), the
// next query reconnects and retries once immediately. When the LED server
// cannot be reached at all, retries are paced with a capped exponential
// backoff (1 s, 2 s, 4 s, ..., up to 30 s) that resets on the first
// success, so a stopped server is not probed on every page refresh.
//
// Not thread-safe: the owning HttpStatusServer drives it from its single
// accept loop.
class LedStatusClient {
public:
    // Backoff pacing for failed connection attempts (same 1 s base / 30 s
    // cap as the Python agents' --backoff-max).
    static constexpr double kBackoffBaseSec = 1.0;
    static constexpr double kBackoffMaxSec = 30.0;

    explicit LedStatusClient(const HttpStatusConfig& cfg);
    ~LedStatusClient();
    LedStatusClient(const LedStatusClient&) = delete;
    LedStatusClient& operator=(const LedStatusClient&) = delete;

    // Applied state of every LED the server reports (CAPS order). Returns an
    // empty vector when the server is unreachable (connection failed, timed
    // out, or a post-failure backoff is still in effect) or reports no LEDs.
    std::vector<LedInfo> query();

    // Drop the open connection (idempotent). The next query reconnects.
    void close();

private:
    bool connect();  // fresh socket + HELLO + CAPS; false on any failure
    bool fetch_all(std::vector<LedInfo>& out);
    bool transact(const char* verb, const std::string& args, std::string& response);
    void pace_retry();  // schedule the next retry at the current backoff
    uint64_t next_rid();

    HttpStatusConfig cfg_;
    int fd_ = -1;
    bool connected_ = false;
    uint64_t next_rid_ = 0;  // request ids, scoped to the connection
    std::vector<std::string> leds_;  // LED names in CAPS order
    double backoff_sec_ = kBackoffBaseSec;
    double retry_at_ = 0.0;  // steady-clock seconds; 0 = not backed off
};

// Small single-threaded HTTP server. start() binds the listener and spawns
// the accept thread; stop() joins it. port() returns the actual bound port
// (use http_port = 0 for an ephemeral one, as the tests do).
//
// Page requests are served from a single persistent LedStatusClient
// connection to the LED server (see its docs for the reconnect/backoff
// behavior).
class HttpStatusServer {
public:
    explicit HttpStatusServer(HttpStatusConfig cfg);
    ~HttpStatusServer();
    HttpStatusServer(const HttpStatusServer&) = delete;
    HttpStatusServer& operator=(const HttpStatusServer&) = delete;

    bool start();  // false on socket/bind/listen error
    void stop();
    int port() const { return bound_port_; }

private:
    void run_loop();

    HttpStatusConfig cfg_;
    LedStatusClient led_client_;  // declared after cfg_ (initialization order)
    int listen_fd_ = -1;
    int bound_port_ = 0;
    std::thread thread_;
    std::atomic<bool> running_{false};
};

} // namespace ugreen

#endif // UGREEN_SERVER_HTTP_STATUS_H
