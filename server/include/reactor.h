#ifndef UGREEN_SERVER_REACTOR_H
#define UGREEN_SERVER_REACTOR_H

#include "config.h"
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

struct ClientSession {
    uint64_t client_id = 0;
    int fd = -1;
    bool is_unix_socket = false;
    std::string remote_addr;
    bool authenticated = false;
    const AccessRule* rule = nullptr;
    int rule_index = -1;
    int configured_priority = 100;
    double rate_tokens = 0;
    std::chrono::steady_clock::time_point rate_refill_time;
    StreamBuffer recv_buffer;
    std::string send_buffer;
    uint64_t last_activity_gen = 0;
};

class Reactor {
public:
    Reactor(ServerConfig config, std::unique_ptr<LedBackend> backend);
    ~Reactor();

    bool init();
    void run();
    void stop();

    // Single tick iteration for controlled testing
    void poll_once(int timeout_ms = 0);

    // Direct client connection injection for testing
    uint64_t add_client_fd(int fd, bool is_unix, const std::string& addr,
                           const AccessRule* rule);

    // Accessors for test inspection
    Scheduler& scheduler() { return scheduler_; }
    OwnershipManager& ownership() { return ownership_; }
    HardwareWorker& worker() { return worker_; }
    size_t active_clients_count() const { return clients_.size(); }

private:
    bool authorize_and_add(int fd, bool is_unix, const sockaddr_storage& peer, socklen_t peer_len);
    void reject_connection(int fd, const char* reason);
    bool consume_command_token(ClientSession& session);
    bool consume_hw_read_token();

    void handle_listener(int listen_fd, bool is_unix);
    void handle_client_read(uint64_t client_id);
    void handle_client_write(uint64_t client_id);
    void handle_worker_wakeup();
    void on_scheduler_tick();

    void process_command(ClientSession& session, const Request& req);
    void send_response(ClientSession& session, const Response& resp);
    void disconnect_client(uint64_t client_id);

    ServerConfig config_;
    HardwareWorker worker_;
    Scheduler scheduler_;
    OwnershipManager ownership_;

    int unix_listen_fd_ = -1;
    int tcp_listen_fd_ = -1;
    bool running_ = false;

    uint64_t next_client_id_ = 1;
    std::map<uint64_t, ClientSession> clients_;
    std::map<int, size_t> rule_connection_counts_;
    // Startup calibration read attempts per LED (bounded retry on failure).
    std::map<LedId, int> calibration_attempts_;
    double hw_read_tokens_ = 0;
    std::chrono::steady_clock::time_point hw_read_refill_time_;
    std::chrono::steady_clock::time_point last_tick_time_;
};

} // namespace ugreen

#endif // UGREEN_SERVER_REACTOR_H
