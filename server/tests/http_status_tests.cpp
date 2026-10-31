// http_status_tests.cpp — unit and end-to-end tests for the HTTP status
// server (include/http_status.h, src/http_status.cpp).
//
// Unit tests drive the pure functions directly (protocol parsing, asset
// JSON parsing/discovery, HTML rendering, request routing). End-to-end
// tests run a real HTTP listener against a fake ugreen_leds_server that
// speaks the real line protocol on a Unix socket or loopback TCP, and
// verify the served pages with a raw TCP client.

#include "http_status.h"
#include "test_framework.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

using namespace ugreen;

namespace {

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------

class TempDir {
public:
    TempDir() {
        char tmpl[] = "/tmp/ugreen_http_test_XXXXXX";
        if (mkdtemp(tmpl)) path_ = tmpl;
    }
    ~TempDir() {
        if (path_.empty()) return;
        DIR* d = opendir(path_.c_str());
        if (d) {
            struct dirent* e;
            while ((e = readdir(d)) != nullptr) {
                std::string n = e->d_name;
                if (n != "." && n != "..")
                    std::remove((path_ + "/" + n).c_str());
            }
            closedir(d);
        }
        rmdir(path_.c_str());
    }
    const std::string& path() const { return path_; }
    std::string file(const std::string& name) const { return path_ + "/" + name; }

private:
    std::string path_;
};

bool write_file(const std::string& path, const std::string& contents) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << contents;
    return f.good();
}

// A layout file with six LEDs (the development configuration).
const char kLayoutJson[] =
    "{\n"
    "  \"image\": \"panel.jpg\",\n"
    "  \"image_width\": 120,\n"
    "  \"image_height\": 40,\n"
    "  \"leds\": {\n"
    "    \"power\":  { \"x\": 10, \"y\": 20, \"radius\": 8, \"label\": \"Power\" },\n"
    "    \"netdev\": { \"x\": 30, \"y\": 20, \"radius\": 5 },\n"
    "    \"disk1\":  { \"x\": 50, \"y\": 20, \"radius\": 5, \"label\": \"Disk1\" },\n"
    "    \"disk2\":  { \"x\": 70, \"y\": 20, \"radius\": 5, \"label\": \"Disk2\" },\n"
    "    \"disk3\":  { \"x\": 90, \"y\": 20, \"radius\": 5, \"label\": \"Disk3\" },\n"
    "    \"disk4\":  { \"x\": 110, \"y\": 20, \"radius\": 5, \"label\": \"Disk4\" }\n"
    "  }\n"
    "}\n";

std::vector<std::string> kAllLeds = {
    "power", "netdev", "disk1", "disk2", "disk3", "disk4"};

// ---------------------------------------------------------------------------
// Unit tests: protocol response parsing
// ---------------------------------------------------------------------------

void test_get_response_parsing() {
    test::run_test("parse_get_response: full blink line", []() {
        LedInfo li = parse_get_response(
            "disk1",
            "OK 5 disk1 GET led=disk1 source=applied mode=blink "
            "brightness=128 rgb=0,255,0 on_ms=100 off_ms=200 status=valid");
        TEST_ASSERT(li.ok);
        TEST_ASSERT_EQ(li.mode, "blink");
        TEST_ASSERT_EQ(li.r, 0);
        TEST_ASSERT_EQ(li.g, 255);
        TEST_ASSERT_EQ(li.b, 0);
        TEST_ASSERT_EQ(li.brightness, 128);
        TEST_ASSERT_EQ(li.on_ms, 100);
        TEST_ASSERT_EQ(li.off_ms, 200);
    });

    test::run_test("parse_get_response: off mode, default-ish values", []() {
        LedInfo li = parse_get_response(
            "power",
            "OK 3 power GET led=power source=applied mode=off brightness=200 "
            "rgb=255,0,0 on_ms=0 off_ms=0 status=valid");
        TEST_ASSERT(li.ok);
        TEST_ASSERT_EQ(li.mode, "off");
        TEST_ASSERT_EQ(li.brightness, 200);
        TEST_ASSERT_EQ(li.r, 255);
    });

    test::run_test("parse_get_response: ERR line is not ok", []() {
        LedInfo li = parse_get_response("disk1", "ERR 5 HARDWARE_ERROR led=disk1");
        TEST_ASSERT(!li.ok);
    });

    test::run_test("parse_get_response: status=invalid is not ok", []() {
        LedInfo li = parse_get_response(
            "disk1",
            "OK 5 disk1 GET led=disk1 source=applied mode=on brightness=200 "
            "rgb=255,0,0 on_ms=0 off_ms=0 status=invalid");
        TEST_ASSERT(!li.ok);
    });

    test::run_test("parse_get_response: out-of-range rgb is not ok", []() {
        LedInfo li = parse_get_response(
            "disk1",
            "OK 5 disk1 GET led=disk1 source=applied mode=on brightness=200 "
            "rgb=999,0,0 on_ms=0 off_ms=0 status=valid");
        TEST_ASSERT(!li.ok);
    });

    test::run_test("parse_get_response: mismatched led= is not ok", []() {
        LedInfo li = parse_get_response(
            "disk1",
            "OK 5 disk2 GET led=disk2 source=applied mode=on brightness=200 "
            "rgb=255,0,0 on_ms=0 off_ms=0 status=valid");
        TEST_ASSERT(!li.ok);
    });

    test::run_test("parse_get_response: unknown fields are tolerated", []() {
        LedInfo li = parse_get_response(
            "disk1",
            "OK 5 disk1 GET led=disk1 source=applied mode=on brightness=1 "
            "rgb=1,2,3 on_ms=0 off_ms=0 status=valid future_field=42");
        TEST_ASSERT(li.ok);
        TEST_ASSERT_EQ(li.mode, "on");
    });
}

void test_caps_parsing() {
    test::run_test("parse_caps_leds: normal line", []() {
        auto names = parse_caps_leds(
            "OK 2 CAPS version=1 leds=power,netdev,disk1,disk2,disk3,disk4 "
            "modes=off,on,blink,breath");
        TEST_ASSERT_EQ(names.size(), kAllLeds.size());
        for (size_t i = 0; i < names.size(); ++i) TEST_ASSERT_EQ(names[i], kAllLeds[i]);
    });

    test::run_test("parse_caps_leds: failures yield empty", []() {
        TEST_ASSERT(parse_caps_leds("ERR 2 AUTH_FAILED reason=no_matching_rule")
                        .empty());
        TEST_ASSERT(parse_caps_leds("OK 2 CAPS version=1 modes=off,on").empty());
        TEST_ASSERT(parse_caps_leds("").empty());
    });
}

// ---------------------------------------------------------------------------
// Unit tests: asset JSON parsing and discovery
// ---------------------------------------------------------------------------

void test_asset_json_parsing() {
    test::run_test("parse_asset_json: valid layout file", []() {
        TempDir dir;
        std::string path = dir.file("layout.json");
        TEST_ASSERT(write_file(path, kLayoutJson));
        auto cfg = parse_asset_json(path);
        TEST_ASSERT(cfg.has_value());
        TEST_ASSERT_EQ(cfg->image, "panel.jpg");
        TEST_ASSERT_EQ(cfg->image_width, 120);
        TEST_ASSERT_EQ(cfg->image_height, 40);
        TEST_ASSERT_EQ(cfg->leds.size(), 6u);
        const JsonLed* power = nullptr;
        const JsonLed* disk1 = nullptr;
        for (const auto& l : cfg->leds) {
            if (l.name == "power") power = &l;
            if (l.name == "disk1") disk1 = &l;
        }
        TEST_ASSERT(power && disk1);
        TEST_ASSERT_EQ(power->x, 10);
        TEST_ASSERT_EQ(power->y, 20);
        TEST_ASSERT_EQ(power->radius, 8);
        TEST_ASSERT_EQ(power->label, "Power");
        TEST_ASSERT_EQ(disk1->x, 50);
        TEST_ASSERT_EQ(disk1->label, "Disk1");
        // netdev has no label in the file: it stays empty.
        bool netdev_ok = false;
        for (const auto& l : cfg->leds)
            if (l.name == "netdev") {
                netdev_ok = true;
                TEST_ASSERT(l.label.empty());
                TEST_ASSERT_EQ(l.radius, 5);
            }
        TEST_ASSERT(netdev_ok);
    });

    test::run_test("parse_asset_json: key order, whitespace, escapes", []() {
        TempDir dir;
        std::string path = dir.file("layout.json");
        // Reordered keys, compact style, escaped quote + unicode in a label.
        // netdev omits "radius": it must fall back to the default of 16.
        std::string json = "{\"leds\":{\"disk1\":{\"label\":\"D\\\"isk \\u00e9\","
                           "\"radius\":7,\"y\":3,\"x\":1},"
                           "\"netdev\":{\"y\":4,\"x\":2}},\"image_height\":9,"
                           "\"image\":\"a.jpg\",\"image_width\":4}";
        TEST_ASSERT(write_file(path, json));
        auto cfg = parse_asset_json(path);
        TEST_ASSERT(cfg.has_value());
        TEST_ASSERT_EQ(cfg->leds.size(), 2u);
        TEST_ASSERT_EQ(cfg->leds[0].x, 1);
        TEST_ASSERT_EQ(cfg->leds[0].y, 3);
        TEST_ASSERT_EQ(cfg->leds[0].radius, 7);
        // "D\"isk \xc3\xa9" — the UTF-8 encoding of é.
        TEST_ASSERT_EQ(cfg->leds[0].label, "D\"isk \xc3\xa9");
        const JsonLed* netdev = nullptr;
        for (const auto& l : cfg->leds)
            if (l.name == "netdev") netdev = &l;
        TEST_ASSERT(netdev);
        TEST_ASSERT_EQ(netdev->x, 2);
        TEST_ASSERT_EQ(netdev->y, 4);
        TEST_ASSERT_EQ(netdev->radius, 16);
    });

    test::run_test("parse_asset_json: malformed or incomplete files", []() {
        TempDir dir;
        auto bad = [&](const std::string& name, const std::string& content) {
            std::string path = dir.file(name);
            TEST_ASSERT(write_file(path, content));
            TEST_ASSERT(!parse_asset_json(path).has_value());
        };
        TEST_ASSERT(!parse_asset_json(dir.file("nope.json")).has_value());
        bad("trailing.json", std::string(kLayoutJson) + "x");
        bad("notjson.json", "this is not json");
        bad("array.json", "[1, 2, 3]");
        bad("noimage.json",
            "{\"image_width\":1,\"image_height\":1,\"leds\":{\"a\":{\"x\":0,\"y\":0}}}");
        bad("zerodims.json",
            "{\"image\":\"a.jpg\",\"image_width\":0,\"image_height\":1,"
            "\"leds\":{\"a\":{\"x\":0,\"y\":0}}}");
        bad("noleads.json",
            "{\"image\":\"a.jpg\",\"image_width\":1,\"image_height\":1,\"leds\":{}}");
        bad("ledsnotobj.json",
            "{\"image\":\"a.jpg\",\"image_width\":1,\"image_height\":1,\"leds\":[]}");
        bad("negradius.json",
            "{\"image\":\"a.jpg\",\"image_width\":1,\"image_height\":1,"
            "\"leds\":{\"a\":{\"x\":0,\"y\":0,\"radius\":-1}}}");
    });
}

void test_find_asset() {
    test::run_test("find_asset: exact configuration name wins", []() {
        TempDir dir;
        std::string joined;
        for (size_t i = 0; i < kAllLeds.size(); ++i) {
            if (i) joined += '-';
            joined += kAllLeds[i];
        }
        // A decoy that also mentions every LED name (superset layout).
        TEST_ASSERT(write_file(
            dir.file("extra-" + joined + "-disk5.json"),
            std::string("{\"image\":\"decoy.jpg\",\"image_width\":1,"
                        "\"image_height\":1,\"leds\":{\"a\":{\"x\":0,\"y\":0}}}")));
        // The exact configuration-name file must win over the decoy.
        TEST_ASSERT(write_file(dir.file(joined + ".json"), kLayoutJson));
        auto cfg = find_asset(dir.path(), kAllLeds);
        TEST_ASSERT(cfg.has_value());
        TEST_ASSERT_EQ(cfg->image, "panel.jpg");
    });

    test::run_test("find_asset: substring fallback, shortest name wins", []() {
        TempDir dir;
        // No exact-name file; two candidates mention all LEDs.
        TEST_ASSERT(write_file(
            dir.file("short-panel-power-netdev-disk1-disk2-disk3-disk4.json"),
            kLayoutJson));
        TEST_ASSERT(write_file(
            dir.file("longer-panel-power-netdev-disk1-disk2-disk3-disk4-extra.json"),
            std::string("{\"image\":\"decoy.jpg\",\"image_width\":1,"
                        "\"image_height\":1,\"leds\":{\"a\":{\"x\":0,\"y\":0}}}")));
        auto cfg = find_asset(dir.path(), kAllLeds);
        TEST_ASSERT(cfg.has_value());
        TEST_ASSERT_EQ(cfg->image, "panel.jpg");
    });

    test::run_test("find_asset: unparseable candidate is skipped", []() {
        TempDir dir;
        // Shorter (sorted first) but malformed; longer one is valid.
        TEST_ASSERT(write_file(
            dir.file("x-power-netdev-disk1-disk2-disk3-disk4.json"), "not json"));
        TEST_ASSERT(write_file(
            dir.file("xy-power-netdev-disk1-disk2-disk3-disk4.json"), kLayoutJson));
        auto cfg = find_asset(dir.path(), kAllLeds);
        TEST_ASSERT(cfg.has_value());
        TEST_ASSERT_EQ(cfg->image, "panel.jpg");
    });

    test::run_test("find_asset: no candidate covering all LEDs", []() {
        TempDir dir;
        TEST_ASSERT(write_file(
            dir.file("power-netdev-disk1-disk2-disk3.json"), kLayoutJson));
        TEST_ASSERT(!find_asset(dir.path(), kAllLeds).has_value());
    });

    test::run_test("find_asset: missing/empty directory", []() {
        TempDir dir;
        TEST_ASSERT(!find_asset(dir.path(), kAllLeds).has_value());
        TEST_ASSERT(!find_asset("/nonexistent/ugreen_http_test_dir", kAllLeds)
                        .has_value());
        TEST_ASSERT(!find_asset("", kAllLeds).has_value());
        TEST_ASSERT(!find_asset(dir.path(), {}).has_value());
    });
}

// ---------------------------------------------------------------------------
// Unit tests: HTTP helpers and rendering
// ---------------------------------------------------------------------------

void test_http_helpers() {
    test::run_test("url_path: request line variants", []() {
        TEST_ASSERT_EQ(url_path("GET / HTTP/1.1"), "/");
        TEST_ASSERT_EQ(url_path("GET /assets/panel.jpg HTTP/1.1"),
                       "/assets/panel.jpg");
        TEST_ASSERT_EQ(url_path("GET /?x=1 HTTP/1.1"), "/");
        TEST_ASSERT_EQ(url_path("GET /index.html"), "/index.html");
        TEST_ASSERT_EQ(url_path(""), "/");
    });

    test::run_test("mime_type: extensions", []() {
        TEST_ASSERT_EQ(mime_type("a.jpg"), "image/jpeg");
        TEST_ASSERT_EQ(mime_type("a.jpeg"), "image/jpeg");
        TEST_ASSERT_EQ(mime_type("a.png"), "image/png");
        TEST_ASSERT_EQ(mime_type("a.json"), "application/json");
        TEST_ASSERT_EQ(mime_type("a.html"), "text/html; charset=utf-8");
        TEST_ASSERT_EQ(mime_type("noext"), "application/octet-stream");
    });

    test::run_test("HttpResponse::to_wire: exact wire format", []() {
        HttpResponse r;
        r.status_code = 404;
        r.status_text = "Not Found";
        r.content_type = "text/plain";
        r.body = "hi";
        TEST_ASSERT_EQ(
            r.to_wire(),
            "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\n"
            "Content-Length: 2\r\nConnection: close\r\n\r\nhi");
    });
}

void test_render_html() {
    test::run_test("render_html: overlay + table with asset", []() {
        LedInfo d1;
        d1.name = "disk1"; d1.mode = "on"; d1.r = 0; d1.g = 255; d1.b = 0;
        d1.brightness = 128; d1.ok = true;
        LedInfo pw;
        pw.name = "power"; pw.mode = "off"; pw.ok = true;
        LedInfo nd;
        nd.name = "netdev"; nd.mode = "blink"; nd.r = 0; nd.g = 255; nd.b = 255;
        nd.on_ms = 100; nd.off_ms = 100; nd.ok = true;

        AssetConfig asset;
        asset.image = "panel.jpg";
        asset.image_width = 120;
        asset.image_height = 40;
        JsonLed a_pw; a_pw.name = "power"; a_pw.x = 10; a_pw.y = 20; a_pw.radius = 8;
        a_pw.label = "Power";
        JsonLed a_nd; a_nd.name = "netdev"; a_nd.x = 30; a_nd.y = 20;
        JsonLed a_d1; a_d1.name = "disk1"; a_d1.x = 50; a_d1.y = 20; a_d1.label = "Disk1";
        JsonLed a_d5; a_d5.name = "disk5"; a_d5.x = 90; a_d5.y = 20;  // not reported
        asset.leds = {a_pw, a_nd, a_d1, a_d5};

        std::string html = render_html({d1, pw, nd}, asset, "/assets");

        // Image card
        TEST_ASSERT(html.find("<div class=\"nas-wrap\"") != std::string::npos);
        TEST_ASSERT(html.find("<img src=\"/assets/panel.jpg\"") != std::string::npos);
        TEST_ASSERT(html.find("viewBox=\"0 0 120 40\"") != std::string::npos);
        // Live green circle at disk1's position, mostly opaque
        TEST_ASSERT(html.find("cx=\"50\" cy=\"20\"") != std::string::npos);
        TEST_ASSERT(html.find("rgba(0,255,0,0.82)") != std::string::npos);
        // Off LED drawn dim
        TEST_ASSERT(html.find("rgba(70,70,80,0.55)") != std::string::npos);
        // Blinking LED carries the animation class
        TEST_ASSERT(html.find("class=\"anim-blink\"") != std::string::npos);
        // Position known from the layout but absent from the server state
        TEST_ASSERT(html.find("class=\"led-missing\"") != std::string::npos);
        // Table rows: all three queried LEDs
        TEST_ASSERT(html.find("disk1<span class=\"led-label\">Disk1</span>") !=
                    std::string::npos);
        TEST_ASSERT(html.find("rgb(0,255,0)") != std::string::npos);
        TEST_ASSERT(html.find("100 / 100 ms") != std::string::npos);
        TEST_ASSERT(html.find("<span class=\"badge off\">Off</span>") !=
                    std::string::npos);
    });

    test::run_test("render_html: without asset the table stands alone", []() {
        LedInfo d1;
        d1.name = "disk1"; d1.mode = "on"; d1.r = 0; d1.g = 255; d1.b = 0;
        d1.ok = true;
        std::string html = render_html({d1}, std::nullopt, "/assets");
        TEST_ASSERT(html.find("<div class=\"nas-wrap\"") == std::string::npos);
        TEST_ASSERT(html.find("<svg") == std::string::npos);
        TEST_ASSERT(html.find("<table>") != std::string::npos);
        TEST_ASSERT(html.find(">disk1<") != std::string::npos);
    });

    test::run_test("render_html: unavailable LED is marked as such", []() {
        LedInfo d1;
        d1.name = "disk1";  // ok stays false
        std::string html = render_html({d1}, std::nullopt, "/assets");
        TEST_ASSERT(html.find("unavailable") != std::string::npos);
    });
}

void test_handle_request() {
    test::run_test("handle_request: index page with injected query", []() {
        TempDir dir;
        std::string joined;
        for (size_t i = 0; i < kAllLeds.size(); ++i) {
            if (i) joined += '-';
            joined += kAllLeds[i];
        }
        TEST_ASSERT(write_file(dir.file(joined + ".json"), kLayoutJson));

        HttpStatusConfig cfg;
        cfg.assets_dir = dir.path();
        LedInfo d1;
        d1.name = "disk1"; d1.mode = "on"; d1.ok = true;
        HttpResponse r = handle_request("/", cfg, [d1] {
            return std::vector<LedInfo>{d1, LedInfo{}};
        });
        TEST_ASSERT_EQ(r.status_code, 200);
        TEST_ASSERT(r.content_type.find("text/html") != std::string::npos);
        TEST_ASSERT(r.body.find(">disk1<") != std::string::npos);
        TEST_ASSERT(r.body.find("<div class=\"nas-wrap\"") != std::string::npos);
    });

    test::run_test("handle_request: empty query yields 503 page", []() {
        HttpStatusConfig cfg;
        HttpResponse r = handle_request("/", cfg, [] {
            return std::vector<LedInfo>{};
        });
        TEST_ASSERT_EQ(r.status_code, 503);
        TEST_ASSERT(r.body.find("Cannot reach LED server") != std::string::npos);
    });

    test::run_test("handle_request: asset file served with MIME type", []() {
        TempDir dir;
        const std::string png_bytes = "\x89PNG\r\n\x1a\n0123";
        TEST_ASSERT(write_file(dir.file("panel.png"), png_bytes));
        HttpStatusConfig cfg;
        cfg.assets_dir = dir.path();
        HttpResponse r = handle_request("/assets/panel.png", cfg, [] {
            return std::vector<LedInfo>{};
        });
        TEST_ASSERT_EQ(r.status_code, 200);
        TEST_ASSERT_EQ(r.content_type, "image/png");
        TEST_ASSERT(r.body == png_bytes);
    });

    test::run_test("handle_request: traversal and subpaths rejected", []() {
        TempDir dir;
        TEST_ASSERT(write_file(dir.file("secret.txt"), "x"));
        HttpStatusConfig cfg;
        cfg.assets_dir = dir.path();
        auto q = [] { return std::vector<LedInfo>{}; };
        TEST_ASSERT_EQ(handle_request("/assets/../secret.txt", cfg, q).status_code, 403);
        TEST_ASSERT_EQ(handle_request("/assets/sub/f", cfg, q).status_code, 404);
        TEST_ASSERT_EQ(handle_request("/assets", cfg, q).status_code, 404);
        // No assets dir configured at all.
        HttpStatusConfig none;
        TEST_ASSERT_EQ(handle_request("/assets/a.jpg", none, q).status_code, 404);
    });

    test::run_test("handle_request: unknown paths are 404", []() {
        HttpStatusConfig cfg;
        auto q = [] { return std::vector<LedInfo>{}; };
        TEST_ASSERT_EQ(handle_request("/nope", cfg, q).status_code, 404);
        TEST_ASSERT_EQ(handle_request("/index.html", cfg, q).status_code, 503);
    });
}



// ---------------------------------------------------------------------------
// End-to-end tests: a real HTTP listener + a fake LED server
// ---------------------------------------------------------------------------

// A minimal ugreen_leds_server stand-in speaking the real line protocol.
class FakeLedServer {
public:
    ~FakeLedServer() { stop(); }

    struct State {
        std::string mode = "off";
        int brightness = 200;
        std::string rgb = "255,0,0";
        int on_ms = 0, off_ms = 0;
    };

    bool start_unix(const std::string& path) {
        if (path.size() >= sizeof(sockaddr_un{}.sun_path)) return false;
        listen_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
        if (listen_fd_ < 0) return false;
        std::remove(path.c_str());
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
        if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            close(listen_fd_);
            listen_fd_ = -1;
            return false;
        }
        if (listen(listen_fd_, 8) != 0) {
            close(listen_fd_);
            listen_fd_ = -1;
            return false;
        }
        tcp_ = false;
        unix_path_ = path;
        return launch();
    }

    bool start_tcp() {
        listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) return false;
        int yes = 1;
        setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = 0;  // ephemeral
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            close(listen_fd_);
            listen_fd_ = -1;
            return false;
        }
        if (listen(listen_fd_, 8) != 0) {
            close(listen_fd_);
            listen_fd_ = -1;
            return false;
        }
        socklen_t len = sizeof(addr);
        getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len);
        port_ = ntohs(addr.sin_port);
        tcp_ = true;
        return launch();
    }

    void stop() {
        if (!running_.exchange(false)) return;
        if (listen_fd_ >= 0) shutdown(listen_fd_, SHUT_RDWR);
        if (thread_.joinable()) thread_.join();
        if (listen_fd_ >= 0) {
            close(listen_fd_);
            listen_fd_ = -1;
        }
        std::lock_guard<std::mutex> lock(handlers_mu_);
        // Wake handler threads blocked in read(): a persistent client
        // (LedStatusClient) never closes its end on its own, and close()
        // from this thread would not unblock a reader in another thread —
        // shutdown() delivers EOF to the peer and unblocks the pending
        // read. The accept loop is already joined, so no fd number can be
        // reused in the meantime.
        for (int fd : accepted_fds_) shutdown(fd, SHUT_RDWR);
        accepted_fds_.clear();
        for (auto& h : handlers_)
            if (h.joinable()) h.join();
        handlers_.clear();
        if (!unix_path_.empty()) std::remove(unix_path_.c_str());
    }

    int port() const { return port_; }

    // How many connections the fake accepted / HELLO handshakes it served.
    // Used to prove the status server keeps one persistent connection
    // instead of reconnecting per page.
    int connections() const { return connections_.load(); }
    int hellos() const { return hellos_.load(); }

    void add_led(const std::string& name, State st) {
        std::lock_guard<std::mutex> lock(mu_);
        led_order_.push_back(name);
        states_[name] = st;
    }

    // Change an already-seeded LED's state (no CAPS-order side effects).
    void update_led(const std::string& name, State st) {
        std::lock_guard<std::mutex> lock(mu_);
        states_[name] = st;
    }

    // When set, accepted connections are held open without any response
    // (exercises the client-side I/O timeout).
    void set_hang(bool hang) { hang_ = hang; }

private:
    bool launch() {
        running_ = true;
        thread_ = std::thread([this] { accept_loop(); });
        return true;
    }

    void accept_loop() {
        while (running_) {
            int fd = accept(listen_fd_, nullptr, nullptr);
            if (fd < 0) {
                if (errno == EINTR) continue;
                break;
            }
            connections_.fetch_add(1);
            {
                std::lock_guard<std::mutex> lock(handlers_mu_);
                accepted_fds_.push_back(fd);
                handlers_.emplace_back([this, fd] { handle_conn(fd); });
            }
        }
    }

    static std::string read_line_blocking(int fd) {
        std::string line;
        char c = 0;
        while (read(fd, &c, 1) == 1) {
            if (c == '\n') break;
            if (c != '\r') line += c;
        }
        return line;
    }

    void handle_conn(int fd) {
        if (hang_) {
            // Hold the connection open until the peer gives up.
            char c = 0;
            while (read(fd, &c, 1) == 1) {}
            close(fd);
            return;
        }
        while (true) {
            std::string line = read_line_blocking(fd);
            if (line.empty()) break;
            std::istringstream ss(line);
            std::string verb, rid, rest;
            ss >> verb >> rid;
            std::string reply;
            if (verb == "HELLO") {
                hellos_.fetch_add(1);
                reply = "OK " + rid + " HELLO version=1 access=local";
            } else if (verb == "CAPS") {
                std::string leds;
                {
                    std::lock_guard<std::mutex> lock(mu_);
                    for (const auto& n : led_order_)
                        leds += (leds.empty() ? "" : ",") + n;
                }
                reply = "OK " + rid + " CAPS version=1 leds=" + leds +
                        " modes=off,on,blink,breath";
            } else if (verb == "GET") {
                std::string led;
                ss >> led;
                if (led.empty()) {
                    close(fd);
                    return;
                }
                std::string st_line;
                {
                    std::lock_guard<std::mutex> lock(mu_);
                    auto it = states_.find(led);
                    if (it == states_.end()) {
                        close(fd);
                        return;
                    }
                    const State& st = it->second;
                    char buf[256];
                    snprintf(buf, sizeof(buf),
                             "OK %s GET led=%s source=applied mode=%s "
                             "brightness=%d rgb=%s on_ms=%d off_ms=%d "
                             "status=valid",
                             rid.c_str(), led.c_str(), st.mode.c_str(),
                             st.brightness, st.rgb.c_str(), st.on_ms,
                             st.off_ms);
                    st_line = buf;
                }
                reply = st_line;
            } else {
                reply = "ERR " + rid + " UNKNOWN_COMMAND verb=" + verb;
            }
            std::string out = reply + "\n";
            size_t sent = 0;
            while (sent < out.size()) {
                ssize_t n = write(fd, out.data() + sent, out.size() - sent);
                if (n <= 0) {
                    close(fd);
                    return;
                }
                sent += static_cast<size_t>(n);
            }
        }
        close(fd);
    }

    int listen_fd_ = -1;
    int port_ = 0;
    bool tcp_ = false;
    bool hang_ = false;
    std::string unix_path_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::mutex mu_;
    std::vector<std::string> led_order_;
    std::map<std::string, State> states_;
    std::mutex handlers_mu_;
    std::vector<std::thread> handlers_;
    std::vector<int> accepted_fds_;  // guarded by handlers_mu_
    std::atomic<int> connections_{0};
    std::atomic<int> hellos_{0};
};

struct HttpResult {
    int status_code = 0;
    std::string status_line;
    std::string headers;
    std::string body;
};

// One-shot HTTP GET against 127.0.0.1:<port>; reads until EOF (the server
// always closes the connection).
HttpResult http_get(int port, const std::string& path, int timeout_ms = 5000) {
    HttpResult res;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return res;
    timeval tv{};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close(fd);
        return res;
    }
    std::string req = "GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                      "Connection: close\r\n\r\n";
    size_t sent = 0;
    bool ok = true;
    while (sent < req.size()) {
        ssize_t n = write(fd, req.data() + sent, req.size() - sent);
        if (n <= 0) {
            ok = false;
            break;
        }
        sent += static_cast<size_t>(n);
    }
    std::string all;
    char chunk[4096];
    while (ok) {
        ssize_t n = read(fd, chunk, sizeof(chunk));
        if (n <= 0) break;
        all.append(chunk, static_cast<size_t>(n));
    }
    close(fd);
    size_t hdr_end = all.find("\r\n\r\n");
    if (hdr_end == std::string::npos) {
        res.status_line = all;
        return res;
    }
    res.headers = all.substr(0, hdr_end);
    res.body = all.substr(hdr_end + 4);
    size_t nl = res.headers.find("\n");
    res.status_line = res.headers.substr(0, nl);
    if (!res.status_line.empty() && res.status_line.back() == '\r')
        res.status_line.pop_back();
    sscanf(res.status_line.c_str(), "HTTP/1.1 %d", &res.status_code);
    return res;
}

// Seed a fake LED server with the development six-LED configuration.
void seed_fake_leds(FakeLedServer& fake) {
    using F = FakeLedServer::State;
    fake.add_led("power", F{"off", 200, "255,0,0", 0, 0});
    fake.add_led("netdev", F{"blink", 200, "0,255,255", 100, 100});
    fake.add_led("disk1", F{"on", 128, "0,255,0", 0, 0});
    fake.add_led("disk2", F{"off", 200, "255,0,0", 0, 0});
    fake.add_led("disk3", F{"breath", 100, "255,255,255", 500, 500});
    fake.add_led("disk4", F{"on", 255, "255,0,255", 0, 0});
}

void assert_page_shows_states(const HttpResult& page) {
    TEST_ASSERT_EQ(page.status_code, 200);
    const std::string& b = page.body;
    // Photo card with the overlay
    TEST_ASSERT(b.find("<div class=\"nas-wrap\"") != std::string::npos);
    TEST_ASSERT(b.find("<img src=\"/assets/panel.jpg\"") != std::string::npos);
    // disk1: live green at its layout position
    TEST_ASSERT(b.find("rgba(0,255,0,0.82)") != std::string::npos);
    // netdev: blinking animation
    TEST_ASSERT(b.find("class=\"anim-blink\"") != std::string::npos);
    TEST_ASSERT(b.find("Blink (100/100 ms)") != std::string::npos);
    // disk3: breath timing in the table
    TEST_ASSERT(b.find("500 / 500 ms") != std::string::npos);
    // disk4: magenta swatch
    TEST_ASSERT(b.find("rgb(255,0,255)") != std::string::npos);
    // Off LED (power) shown dim
    TEST_ASSERT(b.find("rgba(70,70,80,0.55)") != std::string::npos);
    TEST_ASSERT(b.find("<span class=\"badge off\">Off</span>") != std::string::npos);
    // Every reported LED has a table row
    for (const auto& name : kAllLeds)
        TEST_ASSERT(b.find(">" + name + "<") != std::string::npos);
}

void test_led_status_client() {
    test::run_test("LedStatusClient: one persistent connection across queries", []() {
        TempDir dir;
        const std::string sock = dir.file("led.sock");
        FakeLedServer fake;
        TEST_ASSERT(fake.start_unix(sock));
        seed_fake_leds(fake);

        HttpStatusConfig cfg;
        cfg.led_sock = sock;
        cfg.led_timeout_ms = 300;
        LedStatusClient client(cfg);

        auto r1 = client.query();
        TEST_ASSERT_EQ(r1.size(), kAllLeds.size());
        TEST_ASSERT_EQ(r1.front().name, "power");  // CAPS order preserved
        auto r2 = client.query();
        TEST_ASSERT_EQ(r2.size(), kAllLeds.size());
        // One connection and one HELLO/CAPS handshake for two queries
        // (the old behavior ran the whole handshake per query).
        TEST_ASSERT_EQ(fake.connections(), 1);
        TEST_ASSERT_EQ(fake.hellos(), 1);
    });

    test::run_test("LedStatusClient: unreachable server is backed off, not hammered", []() {
        TempDir dir;
        const std::string sock = dir.file("led.sock");
        FakeLedServer fake;
        TEST_ASSERT(fake.start_unix(sock));
        seed_fake_leds(fake);

        HttpStatusConfig cfg;
        cfg.led_sock = sock;
        cfg.led_timeout_ms = 300;
        LedStatusClient client(cfg);
        TEST_ASSERT_EQ(client.query().size(), kAllLeds.size());

        // The server goes away: the stale connection fails, the retry
        // cannot connect, and a backoff is scheduled.
        fake.stop();
        TEST_ASSERT(client.query().empty());
        TEST_ASSERT(client.query().empty());  // still within the backoff window
        TEST_ASSERT(client.query().empty());
        // No new connection attempt was made: the first query already
        // consumed the retry, the rest were suppressed by the backoff.
        TEST_ASSERT_EQ(fake.connections(), 1);
    });
}

void run_http_status_e2e_tests() {
    test::run_test("e2e: full page over Unix-socket LED server", []() {
        TempDir dir;
        std::string joined;
        for (size_t i = 0; i < kAllLeds.size(); ++i) {
            if (i) joined += '-';
            joined += kAllLeds[i];
        }
        TEST_ASSERT(write_file(dir.file(joined + ".json"), kLayoutJson));
        const std::string jpg_bytes = "\xff\xd8\xff\xe0" "fakejpegdata\xff\xd9";
        TEST_ASSERT(write_file(dir.file("panel.jpg"), jpg_bytes));

        FakeLedServer fake;
        TEST_ASSERT(fake.start_unix(dir.file("led.sock")));
        seed_fake_leds(fake);

        HttpStatusConfig cfg;
        cfg.led_sock = dir.file("led.sock");
        cfg.assets_dir = dir.path();
        cfg.http_bind = "127.0.0.1";
        cfg.http_port = 0;
        HttpStatusServer server(cfg);
        TEST_ASSERT(server.start());

        HttpResult page = http_get(server.port(), "/");
        assert_page_shows_states(page);

        HttpResult jpg = http_get(server.port(), "/assets/panel.jpg");
        TEST_ASSERT_EQ(jpg.status_code, 200);
        TEST_ASSERT(jpg.headers.find("Content-Type: image/jpeg") !=
                    std::string::npos);
        TEST_ASSERT(jpg.body == jpg_bytes);

        HttpResult missing = http_get(server.port(), "/nope");
        TEST_ASSERT_EQ(missing.status_code, 404);

        server.stop();
        fake.stop();
    });

    test::run_test("e2e: full page over TCP LED server", []() {
        TempDir dir;
        FakeLedServer fake;
        TEST_ASSERT(fake.start_tcp());
        seed_fake_leds(fake);

        // No assets: the table must still be complete.
        HttpStatusConfig cfg;
        cfg.use_tcp = true;
        cfg.led_host = "127.0.0.1";
        cfg.led_port = fake.port();
        cfg.http_bind = "127.0.0.1";
        cfg.http_port = 0;
        HttpStatusServer server(cfg);
        TEST_ASSERT(server.start());

        HttpResult page = http_get(server.port(), "/");
        TEST_ASSERT_EQ(page.status_code, 200);
        TEST_ASSERT(page.body.find("<div class=\"nas-wrap\"") ==
                    std::string::npos);
        TEST_ASSERT(page.body.find("<table>") != std::string::npos);
        for (const auto& name : kAllLeds)
            TEST_ASSERT(page.body.find(">" + name + "<") != std::string::npos);
        TEST_ASSERT(page.body.find("Blink (100/100 ms)") != std::string::npos);
        TEST_ASSERT(page.body.find("rgb(255,0,255)") != std::string::npos);

        server.stop();
        fake.stop();
    });

    test::run_test("e2e: unreachable LED server yields 503", []() {
        TempDir dir;
        FakeLedServer fake;
        TEST_ASSERT(fake.start_unix(dir.file("led.sock")));  // socket exists...
        fake.stop();  // ...but nothing listens on it

        HttpStatusConfig cfg;
        cfg.led_sock = dir.file("led.sock");
        cfg.led_timeout_ms = 300;
        cfg.http_bind = "127.0.0.1";
        cfg.http_port = 0;
        HttpStatusServer server(cfg);
        TEST_ASSERT(server.start());

        HttpResult page = http_get(server.port(), "/");
        TEST_ASSERT_EQ(page.status_code, 503);
        TEST_ASSERT(page.body.find("Cannot reach LED server") != std::string::npos);

        server.stop();
    });

    test::run_test("e2e: hanging LED server times out, not forever", []() {
        TempDir dir;
        FakeLedServer fake;
        TEST_ASSERT(fake.start_unix(dir.file("led.sock")));
        seed_fake_leds(fake);
        fake.set_hang(true);  // accept connections but never answer

        HttpStatusConfig cfg;
        cfg.led_sock = dir.file("led.sock");
        cfg.led_timeout_ms = 300;
        cfg.http_bind = "127.0.0.1";
        cfg.http_port = 0;
        HttpStatusServer server(cfg);
        TEST_ASSERT(server.start());

        auto t0 = std::chrono::steady_clock::now();
        HttpResult page = http_get(server.port(), "/", 10000);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t0)
                      .count();
        TEST_ASSERT_EQ(page.status_code, 503);
        TEST_ASSERT(ms < 5000);  // bounded by the 300 ms client timeout

        server.stop();
        fake.stop();
    });

    test::run_test("e2e: persistent connection — one handshake for many pages", []() {
        TempDir dir;
        const std::string sock = dir.file("led.sock");
        FakeLedServer fake;
        TEST_ASSERT(fake.start_unix(sock));
        seed_fake_leds(fake);

        HttpStatusConfig cfg;
        cfg.led_sock = sock;
        cfg.http_bind = "127.0.0.1";
        cfg.http_port = 0;
        HttpStatusServer server(cfg);
        TEST_ASSERT(server.start());

        for (int i = 0; i < 3; ++i) {
            HttpResult page = http_get(server.port(), "/");
            TEST_ASSERT_EQ(page.status_code, 200);
            TEST_ASSERT(page.body.find(">disk1<") != std::string::npos);
        }
        // The LED server saw exactly one connection and one HELLO/CAPS
        // handshake, no matter how many pages were served (the old
        // behavior re-ran the whole handshake per page and flooded the
        // server log).
        TEST_ASSERT_EQ(fake.connections(), 1);
        TEST_ASSERT_EQ(fake.hellos(), 1);

        server.stop();
        fake.stop();
    });

    test::run_test("e2e: reconnects and recovers after the LED server restarts", []() {
        TempDir dir;
        const std::string sock = dir.file("led.sock");
        FakeLedServer fake;
        TEST_ASSERT(fake.start_unix(sock));
        seed_fake_leds(fake);

        HttpStatusConfig cfg;
        cfg.led_sock = sock;
        cfg.http_bind = "127.0.0.1";
        cfg.http_port = 0;
        HttpStatusServer server(cfg);
        TEST_ASSERT(server.start());
        TEST_ASSERT_EQ(http_get(server.port(), "/").status_code, 200);
        TEST_ASSERT_EQ(fake.connections(), 1);

        fake.stop();  // LED server goes away (connection closed, socket removed)

        FakeLedServer fake2;
        TEST_ASSERT(fake2.start_unix(sock));
        seed_fake_leds(fake2);
        fake2.update_led("disk4", FakeLedServer::State{"off", 200, "255,0,0", 0, 0});

        // The viewer's persistent connection is dead; the next page request
        // must notice, reconnect to the new server, and show its (changed)
        // state — without waiting out any backoff.
        HttpResult page = http_get(server.port(), "/");
        TEST_ASSERT_EQ(page.status_code, 200);
        TEST_ASSERT(page.body.find("rgb(255,0,255)") == std::string::npos);
        for (const auto& name : kAllLeds)
            TEST_ASSERT(page.body.find(">" + name + "<") != std::string::npos);
        TEST_ASSERT_EQ(fake2.connections(), 1);

        server.stop();
        fake2.stop();
    });

    test::run_test("e2e: backoff paces retries while the LED server is hung", []() {
        TempDir dir;
        const std::string sock = dir.file("led.sock");
        FakeLedServer fake;
        TEST_ASSERT(fake.start_unix(sock));
        seed_fake_leds(fake);
        fake.set_hang(true);  // accepts connections but never answers

        HttpStatusConfig cfg;
        cfg.led_sock = sock;
        cfg.led_timeout_ms = 300;
        cfg.http_bind = "127.0.0.1";
        cfg.http_port = 0;
        HttpStatusServer server(cfg);
        TEST_ASSERT(server.start());

        // First page: the connection attempt times out (300 ms) → 503, and
        // a 1 s backoff is scheduled.
        HttpResult p1 = http_get(server.port(), "/", 10000);
        TEST_ASSERT_EQ(p1.status_code, 503);
        int conns = fake.connections();
        TEST_ASSERT_EQ(conns, 1);  // exactly one attempt before backing off

        // While the backoff is in effect, page requests do not even try to
        // connect (no hammering of the unreachable server).
        TEST_ASSERT_EQ(http_get(server.port(), "/", 10000).status_code, 503);
        TEST_ASSERT_EQ(http_get(server.port(), "/", 10000).status_code, 503);
        TEST_ASSERT_EQ(fake.connections(), conns);

        // Once the 1 s backoff expires, a new attempt is made.
        std::this_thread::sleep_for(std::chrono::milliseconds(1200));
        TEST_ASSERT_EQ(http_get(server.port(), "/", 10000).status_code, 503);
        TEST_ASSERT_EQ(fake.connections(), conns + 1);

        server.stop();
        fake.stop();
    });
}

}  // namespace

void run_http_status_tests() {
    // The LED client writes into sockets whose peer may have closed (the
    // reconnect/restart tests, and a server that dies mid-query); ignore
    // SIGPIPE process-wide for this suite, exactly like the real
    // http_status_server binary does in main().
    signal(SIGPIPE, SIG_IGN);

    std::cout << "Running HTTP Status Server Tests...\n";
    test_get_response_parsing();
    test_caps_parsing();
    test_asset_json_parsing();
    test_find_asset();
    test_http_helpers();
    test_render_html();
    test_handle_request();
    test_led_status_client();
    run_http_status_e2e_tests();
}
