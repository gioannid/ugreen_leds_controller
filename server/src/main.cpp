#include "config.h"
#include "fake_backend.h"
#include "reactor.h"

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <csignal>
#include <iostream>
#include <optional>
#include <string>

static ugreen::Reactor* g_reactor = nullptr;

static void handle_signal(int /*signo*/) {
    if (g_reactor) {
        g_reactor->stop();
    }
}

int main(int argc, char* argv[]) {
    ugreen::ServerConfig config;
    std::string config_path;

    std::optional<std::string> cli_socket;
    std::optional<std::string> cli_bind;
    std::optional<std::string> cli_lock;
    std::optional<int> cli_port;
    std::optional<int> cli_interval;
    bool cli_no_unix = false;
    bool cli_no_tcp = false;
    bool cli_help = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--socket" && i + 1 < argc) {
            cli_socket = argv[++i];
        } else if (arg == "--bind" && i + 1 < argc) {
            cli_bind = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            cli_port = std::stoi(argv[++i]);
        } else if (arg == "--interval" && i + 1 < argc) {
            cli_interval = std::stoi(argv[++i]);
        } else if (arg == "--config" && i + 1 < argc) {
            config_path = argv[++i];
        } else if (arg == "--lock-file" && i + 1 < argc) {
            cli_lock = argv[++i];
        } else if (arg == "--no-unix") {
            cli_no_unix = true;
        } else if (arg == "--no-tcp") {
            cli_no_tcp = true;
        } else if (arg == "--help" || arg == "-h") {
            cli_help = true;
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            return 1;
        }
    }

    if (cli_help) {
        std::cout << "Usage: ugreen_leds_server [options]\n"
                  << "  --config <path>     Load configuration file (access rules, fallbacks, limits)\n"
                  << "  --socket <path>     Unix domain socket path\n"
                  << "  --bind <ip>         TCP bind address (default: " << config.transport.tcp_bind_ip << ")\n"
                  << "  --port <port>       TCP port (default: " << config.transport.tcp_port << ")\n"
                  << "  --interval <ms>     Scheduler interval in ms\n"
                  << "  --lock-file <path>  Process lock file to prevent duplicate instances\n"
                  << "  --no-unix           Disable Unix socket listener\n"
                  << "  --no-tcp            Disable TCP listener\n"
                  << "  --help              Display this help message\n"
                  << "\nCommand-line options override values from --config.\n"
                  << "Without --config a permissive local-only development policy is used;\n"
                  "production deployments must configure an explicit access policy.\n";
        return 0;
    }

    if (!config_path.empty()) {
        std::string err;
        if (!ugreen::load_server_config(config_path, config, err)) {
            std::cerr << "Config error: " << err << "\n";
            return 1;
        }
    }

    if (cli_socket) config.transport.unix_socket_path = *cli_socket;
    if (cli_bind) config.transport.tcp_bind_ip = *cli_bind;
    if (cli_port) config.transport.tcp_port = *cli_port;
    if (cli_interval) config.transport.scheduler_interval_ms = *cli_interval;
    if (cli_lock) config.lock_file = *cli_lock;
    if (cli_no_unix) config.transport.enable_unix_socket = false;
    if (cli_no_tcp) config.transport.enable_tcp = false;

    int lock_fd = -1;
    if (!config.lock_file.empty()) {
        lock_fd = open(config.lock_file.c_str(), O_RDWR | O_CREAT, 0644);
        if (lock_fd < 0) {
            std::cerr << "Cannot open lock file: " << config.lock_file << "\n";
            return 1;
        }
        if (flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
            std::cerr << "Another ugreen_leds_server instance holds " << config.lock_file << "\n";
            close(lock_fd);
            return 1;
        }
    }

    auto backend = std::make_unique<ugreen::FakeLedBackend>();
    ugreen::Reactor reactor(config, std::move(backend));
    g_reactor = &reactor;

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    std::cout << "Starting ugreen_leds_server...\n";
    if (config.transport.enable_unix_socket) {
        std::cout << "  Unix socket: " << config.transport.unix_socket_path << "\n";
    }
    if (config.transport.enable_tcp) {
        std::cout << "  TCP listener: " << config.transport.tcp_bind_ip << ":" << config.transport.tcp_port << "\n";
    }
    std::cout << "  Scheduler interval: " << config.transport.scheduler_interval_ms << " ms\n";
    std::cout << "  Access rules (" << config.access_rules.size() << "):\n";
    for (const auto& rule : config.access_rules) {
        std::string leds_str = rule.all_leds ? "all" : "";
        for (size_t i = 0; i < rule.leds.size(); ++i) {
            if (i > 0) leds_str += ",";
            leds_str += ugreen::to_string(rule.leds[i]);
        }
        std::string verbs_str;
        for (auto v : rule.verbs) {
            if (!verbs_str.empty()) verbs_str += ",";
            verbs_str += ugreen::to_string(v);
        }
        if (verbs_str.empty()) verbs_str = "all";
        std::cout << "    " << rule.name << ": source=" << ugreen::describe_rule_source(rule)
                  << " leds=" << leds_str
                  << " verbs=" << verbs_str
                  << " priority=" << rule.priority;
        std::cout << " max_connections=";
        if (rule.max_connections == 0) {
            std::cout << "unlimited";
        } else {
            std::cout << rule.max_connections;
        }
        std::cout << "\n";
    }

    if (!reactor.init()) {
        std::cerr << "Failed to initialize reactor listeners.\n";
        return 1;
    }

    std::cout << "ugreen_leds_server running. Press Ctrl+C to stop.\n";
    reactor.run();
    std::cout << "ugreen_leds_server stopped cleanly.\n";

    if (lock_fd >= 0) {
        close(lock_fd);
    }
    return 0;
}
