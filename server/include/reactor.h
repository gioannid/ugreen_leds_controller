#ifndef UGREEN_SERVER_REACTOR_H
#define UGREEN_SERVER_REACTOR_H

#include "hardware_worker.h"
#include "ownership.h"
#include "protocol.h"
#include "scheduler.h"

#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ugreen {

struct ReactorConfig {
    std::string unix_socket_path = "/tmp/ugreen_leds.sock";
    std::string tcp_bind_ip = "127.0.0.1";
    int tcp_port = 39001;
    int scheduler_interval_ms = 20;
    bool enable_unix_socket = true;
    bool enable_tcp = true;
};

struct ClientSession {
    uint64_t client_id = 0;
    int fd = -1;
    bool is_unix_socket = false;
    std::string remote_addr;
    bool authenticated = false;
    int default_priority = 100;
    StreamBuffer recv_buffer;
    std::string send_buffer;
    uint64_t last_activity_gen = 0;
};

class Reactor {
public:
    Reactor(ReactorConfig config, std::unique_ptr<LedBackend> backend);
    ~Reactor();

    bool init();
    void run();
    void stop();

    // Single tick iteration for controlled testing
    void poll_once(int timeout_ms = 0);

    // Direct client connection injection for testing
    uint64_t add_client_fd(int fd, bool is_unix, const std::string& addr);

    // Accessors for test inspection
    Scheduler& scheduler() { return scheduler_; }
    OwnershipManager& ownership() { return ownership_; }
    HardwareWorker& worker() { return worker_; }
    size_t active_clients_count() const { return clients_.size(); }

private:
    void handle_listener(int listen_fd, bool is_unix);
    void handle_client_read(uint64_t client_id);
    void handle_client_write(uint64_t client_id);
    void handle_worker_wakeup();
    void on_scheduler_tick();

    void process_command(ClientSession& session, const Request& req);
    void send_response(ClientSession& session, const Response& resp);
    void disconnect_client(uint64_t client_id);

    ReactorConfig config_;
    HardwareWorker worker_;
    Scheduler scheduler_;
    OwnershipManager ownership_;

    int unix_listen_fd_ = -1;
    int tcp_listen_fd_ = -1;
    bool running_ = false;

    uint64_t next_client_id_ = 1;
    std::map<uint64_t, ClientSession> clients_;
    std::chrono::steady_clock::time_point last_tick_time_;
};

} // namespace ugreen

#endif // UGREEN_SERVER_REACTOR_H
