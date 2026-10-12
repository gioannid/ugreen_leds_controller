#include "fake_backend.h"
#include "reactor.h"

#include <csignal>
#include <iostream>
#include <string>

static ugreen::Reactor* g_reactor = nullptr;

static void handle_signal(int /*signo*/) {
    if (g_reactor) {
        g_reactor->stop();
    }
}

int main(int argc, char* argv[]) {
    ugreen::ReactorConfig config;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--socket" && i + 1 < argc) {
            config.unix_socket_path = argv[++i];
        } else if (arg == "--bind" && i + 1 < argc) {
            config.tcp_bind_ip = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            config.tcp_port = std::stoi(argv[++i]);
        } else if (arg == "--interval" && i + 1 < argc) {
            config.scheduler_interval_ms = std::stoi(argv[++i]);
        } else if (arg == "--no-unix") {
            config.enable_unix_socket = false;
        } else if (arg == "--no-tcp") {
            config.enable_tcp = false;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: ugreen_leds_server [options]\n"
                      << "  --socket <path>     Unix domain socket path (default: " << config.unix_socket_path << ")\n"
                      << "  --bind <ip>         TCP bind address (default: " << config.tcp_bind_ip << ")\n"
                      << "  --port <port>       TCP port (default: " << config.tcp_port << ")\n"
                      << "  --interval <ms>     Scheduler interval in ms (default: " << config.scheduler_interval_ms << ")\n"
                      << "  --no-unix           Disable Unix socket listener\n"
                      << "  --no-tcp            Disable TCP listener\n"
                      << "  --help              Display this help message\n";
            return 0;
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            return 1;
        }
    }

    auto backend = std::make_unique<ugreen::FakeLedBackend>();
    ugreen::Reactor reactor(config, std::move(backend));
    g_reactor = &reactor;

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    std::cout << "Starting ugreen_leds_server...\n";
    if (config.enable_unix_socket) {
        std::cout << "  Unix socket: " << config.unix_socket_path << "\n";
    }
    if (config.enable_tcp) {
        std::cout << "  TCP listener: " << config.tcp_bind_ip << ":" << config.tcp_port << "\n";
    }
    std::cout << "  Scheduler interval: " << config.scheduler_interval_ms << " ms\n";

    if (!reactor.init()) {
        std::cerr << "Failed to initialize reactor listeners.\n";
        return 1;
    }

    std::cout << "ugreen_leds_server running. Press Ctrl+C to stop.\n";
    reactor.run();
    std::cout << "ugreen_leds_server stopped cleanly.\n";

    return 0;
}
