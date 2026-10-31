// http_status_server — executable entry point for the HTTP status server.
//
// All of the actual logic lives in src/http_status.cpp (see
// include/http_status.h); this file only parses the command line and runs
// the server until interrupted.
//
// Usage:
//   http_status_server [options]
//
// The server keeps one persistent connection to the LED server (Unix socket
// or TCP, PROTOCOL.md §10.7), re-running the HELLO/CAPS handshake only after
// a failure, and serves:
//
//   GET /          — HTML status page (auto-refreshes every 5 s)
//   GET /assets/*  — static files from the --assets-dir directory
//
// If the assets directory contains a <config>.json layout file matching the
// current set of LEDs (by name pattern in the filename, e.g.
// "power-netdev-disk1-disk2-disk3-disk4.json"), the status page overlays the
// live LED state on the NAS photo; below it (always, also when no matching
// layout exists) the LED states are listed in a table.

#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

#include "http_status.h"

namespace {

volatile std::sig_atomic_t g_stop_requested = 0;

void on_signal(int) { g_stop_requested = 1; }

void print_usage() {
    std::cout
        << "Usage: http_status_server [options]\n\n"
        << "  --led-socket <path>  Unix socket of the LED server (default: "
           "/tmp/ugreen_leds.sock)\n"
        << "  --led-host   <ip>    Connect to the LED server over TCP instead\n"
        << "  --led-port   <port>  LED server TCP port (default: 39001)\n"
        << "  --led-timeout <ms>   Timeout for querying the LED server (default: 2000)\n"
        << "  --assets-dir <path>  Directory containing the NAS background image and\n"
        << "                       JSON layout files (default: none)\n"
        << "  --bind       <ip>    HTTP listen address (default: 0.0.0.0)\n"
        << "  --port       <port>  HTTP listen port    (default: 8080)\n"
        << "  --help               Show this message\n\n"
        << "The HTTP server serves a single status page at GET / that:\n"
        << "  - Queries the running LED server for the current applied LED state\n"
        << "  - Overlays coloured SVG circles at the hardware LED positions on the\n"
        << "    NAS background photo when a matching layout JSON is found in\n"
        << "    --assets-dir (named after the LED configuration, e.g.\n"
        << "    power-netdev-disk1-disk2-disk3-disk4.json)\n"
        << "  - Lists all LED states in a table below the image\n"
        << "  - Auto-refreshes every 5 seconds\n\n"
        << "Static files under /assets/* are served from --assets-dir.\n";
}

}  // namespace

int main(int argc, char* argv[]) {
    ugreen::HttpStatusConfig cfg;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto need_value = [&]() -> const char* {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for " << arg << "\n";
                std::exit(1);
            }
            return argv[++i];
        };
        if ((arg == "--led-socket" || arg == "--socket") && i + 1 < argc) {
            cfg.led_sock = argv[++i];
            cfg.use_tcp = false;
        } else if (arg == "--led-host" && i + 1 < argc) {
            cfg.led_host = argv[++i];
            cfg.use_tcp = true;
        } else if (arg == "--led-port" && i + 1 < argc) {
            cfg.led_port = std::atoi(need_value());
        } else if (arg == "--led-timeout" && i + 1 < argc) {
            cfg.led_timeout_ms = std::atoi(need_value());
            if (cfg.led_timeout_ms <= 0) cfg.led_timeout_ms = 2000;
        } else if (arg == "--assets-dir" && i + 1 < argc) {
            cfg.assets_dir = argv[++i];
        } else if (arg == "--bind" && i + 1 < argc) {
            cfg.http_bind = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            cfg.http_port = std::atoi(need_value());
        } else if (arg == "--help" || arg == "-h") {
            print_usage();
            return 0;
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            return 1;
        }
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::signal(SIGPIPE, SIG_IGN);  // writes to a gone client must not kill us

    ugreen::HttpStatusServer server(cfg);
    if (!server.start()) {
        std::cerr << "Failed to bind " << cfg.http_bind << ":" << cfg.http_port
                  << " (is another instance already running?)\n";
        return 1;
    }

    std::cout << "http_status_server listening on "
              << (cfg.http_bind == "0.0.0.0" ? "0.0.0.0" : cfg.http_bind) << ":"
              << server.port() << "\n"
              << "  LED source: "
              << (cfg.use_tcp
                      ? cfg.led_host + ":" + std::to_string(cfg.led_port)
                      : cfg.led_sock) << "\n"
              << "  Assets dir: " << (cfg.assets_dir.empty() ? "(none)" : cfg.assets_dir) << "\n";

    while (!g_stop_requested)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

    server.stop();
    std::cout << "http_status_server stopped\n";
    return 0;
}
