#include "config.h"
#include "fake_backend.h"
#include "i2c_led_backend.h"
#include "logger.h"
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
    std::optional<std::string> cli_log_file;
    std::optional<std::string> cli_backend;
    bool cli_verbose = false;
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
        } else if (arg == "--log-file" && i + 1 < argc) {
            cli_log_file = argv[++i];
        } else if (arg == "--backend" && i + 1 < argc) {
            cli_backend = argv[++i];
        } else if (arg == "--verbose") {
            cli_verbose = true;
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
                    << "  --log-file <path>   Write log messages to a file (default: screen)\n"
                    << "  --backend <name>    LED backend: i2c (real hardware, default) or fake (simulator)\n"
                    << "  --verbose           Enable debug-level logging for troubleshooting\n"
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
    if (cli_log_file) config.log_file = *cli_log_file;
    if (cli_verbose) config.verbose = true;
    if (cli_no_unix) config.transport.enable_unix_socket = false;
    if (cli_no_tcp) config.transport.enable_tcp = false;
    if (cli_backend) config.backend = *cli_backend;
    if (config.backend != "fake" && config.backend != "i2c") {
        std::cerr << "Invalid backend '" << config.backend << "' (expected fake or i2c)\n";
        return 1;
    }

    // Make the effective policy visible in the startup log (the Reactor
    // would apply the same defaults to its own copy otherwise).
    ugreen::apply_dev_defaults(config);

    std::string log_error;
    if (!ugreen::Logger::instance().init(ugreen::LogConfig{config.log_file, config.verbose}, log_error)) {
        std::cerr << "Logging error: " << log_error << "\n";
        return 1;
    }

    int lock_fd = -1;
    if (!config.lock_file.empty()) {
        lock_fd = open(config.lock_file.c_str(), O_RDWR | O_CREAT, 0644);
        if (lock_fd < 0) {
            ugreen::log::error("cannot open lock file " + config.lock_file);
            return 1;
        }
        if (flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
            ugreen::log::error("another ugreen_leds_server instance holds " + config.lock_file);
            close(lock_fd);
            return 1;
        }
    }

    std::unique_ptr<ugreen::LedBackend> backend;
    if (config.backend == "i2c") {
        std::unique_ptr<ugreen::I2cLedBackend> i2c;
        try {
            i2c = std::make_unique<ugreen::I2cLedBackend>();
        } catch (const std::exception& ex) {
            ugreen::log::error("I2C backend failed to initialize: " + std::string(ex.what()));
            return 1;
        }
        std::string i2c_device = i2c->device_path();
        auto caps = i2c->capabilities();
        if (caps.supported_leds.empty()) {
            ugreen::log::error("no LEDs detected on I2C controller " + i2c_device +
                               "; check that the i2c-dev module is loaded, the device node is "
                               "accessible, and no competing LED controller (e.g. the led_ugreen "
                               "kernel module) is active");
            return 1;
        }
        std::string leds_str;
        for (auto l : caps.supported_leds) {
            if (!leds_str.empty()) leds_str += ",";
            leds_str += ugreen::to_string(l);
        }
        ugreen::log::info("  i2c backend: device=" + i2c_device +
                          " supported_leds=" + leds_str);
        backend = std::move(i2c);
    } else {
        backend = std::make_unique<ugreen::FakeLedBackend>();
        ugreen::log::warn("backend=fake: LED simulator in use, real hardware will not be controlled");
    }
    ugreen::Reactor reactor(config, std::move(backend));
    g_reactor = &reactor;

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    ugreen::log::info("starting ugreen_leds_server (verbose=" +
                      std::string(config.verbose ? "on" : "off") + ", log_file=" +
                      (config.log_file.empty() ? "screen" : config.log_file) + ")");
    if (config.transport.enable_unix_socket) {
        ugreen::log::info("  unix socket: " + config.transport.unix_socket_path);
    }
    if (config.transport.enable_tcp) {
        ugreen::log::info("  tcp listener: " + config.transport.tcp_bind_ip + ":" +
                          std::to_string(config.transport.tcp_port));
    }
    ugreen::log::info("  scheduler interval: " + std::to_string(config.transport.scheduler_interval_ms) + " ms");
    ugreen::log::info("  access rules (" + std::to_string(config.access_rules.size()) + "):");
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
        std::string max_conn = rule.max_connections == 0
                                   ? "unlimited"
                                   : std::to_string(rule.max_connections);
        ugreen::log::info("    " + rule.name + ": source=" + ugreen::describe_rule_source(rule) +
                          " leds=" + leds_str +
                          " verbs=" + verbs_str +
                          " priority=" + std::to_string(rule.priority) +
                          " max_connections=" + max_conn);
    }

    if (!reactor.init()) {
        ugreen::log::error("failed to initialize reactor listeners");
        return 1;
    }

    ugreen::log::info("ugreen_leds_server running");
    reactor.run();
    ugreen::log::info("ugreen_leds_server stopped cleanly");

    if (lock_fd >= 0) {
        close(lock_fd);
    }
    return 0;
}
