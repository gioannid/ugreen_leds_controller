#include "logger.h"
#include "reactor.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

namespace ugreen {

static bool set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return false;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) != -1;
}

Reactor::Reactor(ServerConfig config, std::unique_ptr<LedBackend> backend)
    : config_(std::move(config)),
      worker_(std::move(backend)),
      scheduler_(worker_.capabilities().supported_leds) {
    // Development convenience: without an explicit access policy, allow Unix
    // socket and loopback connections. Deployments must configure an
    // explicit policy (ARCHITECTURE.md section 5: no match means deny).
    apply_dev_defaults(config_);

    for (const auto& [led, state] : config_.fallbacks) {
        scheduler_.set_fallback(led, state);
    }

    hw_read_tokens_ = config_.hardware_read_rate_limit > 0
                          ? static_cast<double>(config_.hardware_read_rate_limit)
                          : 0;
    hw_read_refill_time_ = std::chrono::steady_clock::now();
    last_tick_time_ = std::chrono::steady_clock::now();
}

Reactor::~Reactor() {
    stop();
}

bool Reactor::init() {
    worker_.start();

    // Startup calibration: read every supported LED so the scheduler's
    // baseline is the actual hardware state, not the "everything is off"
    // assumption. Without this, a first SET that resolves to the assumed
    // state (e.g. mode=off while the firmware has the LED lit) would be a
    // no-op batch and the scheduler would silently disagree with the panel.
    // These reads are internal (client id 0) and bypass the client-facing
    // hardware-read rate limit.
    for (auto led : worker_.capabilities().supported_leds) {
        worker_.submit_read(HardwareReadRequest{kCalibrationClientId, 0, led});
    }

    if (config_.transport.enable_unix_socket) {
        unix_listen_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
        if (unix_listen_fd_ < 0) {
            log::error(std::string("cannot create unix socket: ") + strerror(errno));
        } else {
            set_nonblocking(unix_listen_fd_);
            sockaddr_un addr{};
            addr.sun_family = AF_UNIX;
            std::strncpy(addr.sun_path, config_.transport.unix_socket_path.c_str(), sizeof(addr.sun_path) - 1);
            unlink(config_.transport.unix_socket_path.c_str());

            if (bind(unix_listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
                listen(unix_listen_fd_, 16);
                log::info("listening on unix socket " + config_.transport.unix_socket_path);
            } else {
                log::error("cannot bind unix socket " + config_.transport.unix_socket_path +
                           ": " + strerror(errno));
                close(unix_listen_fd_);
                unix_listen_fd_ = -1;
            }
        }
    }

    if (config_.transport.enable_tcp) {
        tcp_listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (tcp_listen_fd_ < 0) {
            log::error(std::string("cannot create TCP socket: ") + strerror(errno));
        } else {
            set_nonblocking(tcp_listen_fd_);
            int opt = 1;
            setsockopt(tcp_listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(config_.transport.tcp_port);
            inet_pton(AF_INET, config_.transport.tcp_bind_ip.c_str(), &addr.sin_addr);

            if (bind(tcp_listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
                listen(tcp_listen_fd_, 16);
                log::info("listening on TCP " + config_.transport.tcp_bind_ip + ":" +
                          std::to_string(config_.transport.tcp_port));
            } else {
                log::error("cannot bind TCP " + config_.transport.tcp_bind_ip + ":" +
                           std::to_string(config_.transport.tcp_port) + ": " + strerror(errno));
                close(tcp_listen_fd_);
                tcp_listen_fd_ = -1;
            }
        }
    }

    running_ = (unix_listen_fd_ >= 0 || tcp_listen_fd_ >= 0);
    return running_;
}

void Reactor::stop() {
    if (!running_) return;
    running_ = false;

    for (auto& [id, s] : clients_) {
        if (s.fd >= 0) close(s.fd);
    }
    clients_.clear();
    rule_connection_counts_.clear();

    if (unix_listen_fd_ >= 0) {
        close(unix_listen_fd_);
        unlink(config_.transport.unix_socket_path.c_str());
        unix_listen_fd_ = -1;
    }

    if (tcp_listen_fd_ >= 0) {
        close(tcp_listen_fd_);
        tcp_listen_fd_ = -1;
    }

    worker_.stop();
}

uint64_t Reactor::add_client_fd(int fd, bool is_unix, const std::string& addr,
                                const AccessRule* rule) {
    set_nonblocking(fd);
    uint64_t cid = next_client_id_++;

    int rule_idx = rule ? static_cast<int>(rule - config_.access_rules.data()) : -1;

    ClientSession s;
    s.client_id = cid;
    s.fd = fd;
    s.is_unix_socket = is_unix;
    s.remote_addr = addr;
    s.rule = rule;
    s.rule_index = rule_idx;
    s.configured_priority = rule ? rule->priority : 100;
    s.rate_tokens = config_.command_rate_limit > 0
                        ? static_cast<double>(config_.command_rate_limit)
                        : 0;
    s.rate_refill_time = std::chrono::steady_clock::now();
    clients_[cid] = std::move(s);

    if (rule_idx >= 0) {
        rule_connection_counts_[rule_idx]++;
    }
    return cid;
}

void Reactor::reject_connection(int fd, const char* reason) {
    std::string msg = std::string("ERR 0 AUTH_FAILED reason=") + reason + "\n";
    ssize_t n = write(fd, msg.data(), msg.size());
    (void)n;
    close(fd);
}

bool Reactor::authorize_and_add(int fd, bool is_unix, const sockaddr_storage& peer, socklen_t peer_len) {
    set_nonblocking(fd);

    std::string addr_str = is_unix ? "local" : "tcp";
    if (!is_unix && peer.ss_family == AF_INET) {
        char buf[INET_ADDRSTRLEN] = {0};
        const auto* sin = reinterpret_cast<const sockaddr_in*>(&peer);
        inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf));
        addr_str = buf;
    }

    if (config_.max_total_connections > 0 &&
        clients_.size() >= static_cast<size_t>(config_.max_total_connections)) {
        log::warn("connection rejected from " + addr_str + ": server connection limit (" +
                  std::to_string(config_.max_total_connections) + ") reached");
        reject_connection(fd, "server_limit");
        return false;
    }

    const AccessRule* rule = match_access_rule(config_, peer, peer_len, is_unix);
    if (!rule) {
        log::warn("connection rejected from " + addr_str + ": no matching access rule");
        reject_connection(fd, "no_matching_rule");
        return false;
    }

    int rule_idx = static_cast<int>(rule - config_.access_rules.data());
    if (rule->max_connections > 0 && rule_connection_counts_[rule_idx] >= rule->max_connections) {
        log::warn("connection rejected from " + addr_str + ": rule '" + rule->name +
                  "' connection limit (" + std::to_string(rule->max_connections) + ") reached");
        reject_connection(fd, "connection_limit");
        return false;
    }

    uint64_t cid = add_client_fd(fd, is_unix, addr_str, rule);
    log::info("client " + std::to_string(cid) + " connected from " + addr_str +
              " (rule " + rule->name + ", priority " + std::to_string(rule->priority) + ")");
    return true;
}

bool Reactor::consume_command_token(ClientSession& session) {
    int rate = config_.command_rate_limit;
    if (rate <= 0) return true;

    auto now = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(now - session.rate_refill_time).count();
    session.rate_refill_time = now;
    session.rate_tokens = std::min<double>(rate, session.rate_tokens + elapsed * rate);
    if (session.rate_tokens >= 1.0) {
        session.rate_tokens -= 1.0;
        return true;
    }
    return false;
}

bool Reactor::consume_hw_read_token() {
    int rate = config_.hardware_read_rate_limit;
    if (rate <= 0) return true;

    auto now = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(now - hw_read_refill_time_).count();
    hw_read_refill_time_ = now;
    hw_read_tokens_ = std::min<double>(rate, hw_read_tokens_ + elapsed * rate);
    if (hw_read_tokens_ >= 1.0) {
        hw_read_tokens_ -= 1.0;
        return true;
    }
    return false;
}

void Reactor::run() {
    while (running_) {
        poll_once(config_.transport.scheduler_interval_ms);
    }
}

void Reactor::poll_once(int timeout_ms) {
    std::vector<pollfd> pfds;

    if (unix_listen_fd_ >= 0) {
        pfds.push_back(pollfd{unix_listen_fd_, POLLIN, 0});
    }
    if (tcp_listen_fd_ >= 0) {
        pfds.push_back(pollfd{tcp_listen_fd_, POLLIN, 0});
    }
    if (worker_.wakeup_fd() >= 0) {
        pfds.push_back(pollfd{worker_.wakeup_fd(), POLLIN, 0});
    }

    std::vector<uint64_t> client_ids;
    for (const auto& [cid, s] : clients_) {
        // Backpressure: keep reading from every client (input is bounded by
        // recv_buffer and the TCP window) and watch for POLLOUT only while
        // responses are queued, so the queue can drain as the reader consumes
        // output. A reader that never consumes output keeps the queue growing
        // until it overflows kMaxQueuedOutput and the session is dropped
        // (see send_response). Registering POLLOUT unconditionally would make
        // poll() return immediately for every idle-but-writable client,
        // starving the scheduler's time-based ticks and burning CPU.
        short events = POLLIN | (s.send_buffer.empty() ? 0 : POLLOUT);
        pfds.push_back(pollfd{s.fd, events, 0});
        client_ids.push_back(cid);
    }

    int rc = poll(pfds.data(), pfds.size(), timeout_ms);
    if (rc < 0) return;

    size_t idx = 0;
    if (unix_listen_fd_ >= 0) {
        if (pfds[idx].revents & POLLIN) {
            handle_listener(unix_listen_fd_, true);
        }
        ++idx;
    }
    if (tcp_listen_fd_ >= 0) {
        if (pfds[idx].revents & POLLIN) {
            handle_listener(tcp_listen_fd_, false);
        }
        ++idx;
    }
    if (worker_.wakeup_fd() >= 0) {
        if (pfds[idx].revents & POLLIN) {
            handle_worker_wakeup();
        }
        ++idx;
    }

    for (size_t i = 0; i < client_ids.size(); ++i) {
        uint64_t cid = client_ids[i];
        if (clients_.find(cid) == clients_.end()) continue;

        short revents = pfds[idx + i].revents;
        if (revents & (POLLERR | POLLHUP | POLLNVAL)) {
            disconnect_client(cid);
            continue;
        }
        if (revents & POLLIN) {
            handle_client_read(cid);
        }
        if (clients_.find(cid) != clients_.end() && (revents & POLLOUT)) {
            handle_client_write(cid);
        }
    }

    on_scheduler_tick();
}

void Reactor::handle_listener(int listen_fd, bool is_unix) {
    for (int i = 0; i < 64; ++i) {
        sockaddr_storage ss{};
        socklen_t slen = sizeof(ss);
        int client_fd = accept(listen_fd, reinterpret_cast<sockaddr*>(&ss), &slen);
        if (client_fd < 0) return;
        authorize_and_add(client_fd, is_unix, ss, slen);
    }
}

void Reactor::handle_worker_wakeup() {
    uint64_t val = 0;
    (void)read(worker_.wakeup_fd(), &val, sizeof(val));

    auto completed_batches = worker_.pop_completed_batches();
    for (const auto& b : completed_batches) {
        scheduler_.on_batch_complete(b);
        if (b.success) {
            log::debug("hardware batch " + std::to_string(b.batch_id) + " applied " +
                       std::to_string(b.applied_states.size()) + " operation(s), generation " +
                       std::to_string(b.max_generation));
        } else {
            std::string led = b.failed_led ? to_string(*b.failed_led) : "unknown";
            log::warn("hardware batch " + std::to_string(b.batch_id) + " failed on led " + led +
                      ": " + b.error_message);
        }
    }

    auto completed_reads = worker_.pop_completed_reads();
    for (const auto& r : completed_reads) {
        if (r.client_id == kCalibrationClientId) {
            if (r.success) {
                scheduler_.calibrate(r.id, r.state);
                log::debug("startup calibration: " + to_string(r.id) + " is " +
                           to_string(r.state.mode) +
                           (r.state.mode == OpMode::Blink || r.state.mode == OpMode::Breath
                                ? " on_ms=" + std::to_string(r.state.on_ms) +
                                  " off_ms=" + std::to_string(r.state.off_ms)
                                : ""));
            } else {
                log::warn("startup calibration read of " + to_string(r.id) + " failed: " +
                          r.error_message + " (scheduler keeps its assumption until the first write)");
            }
            continue;
        }
        auto it = clients_.find(r.client_id);
        if (it != clients_.end()) {
            if (r.success) {
                log::debug("hardware read of " + to_string(r.id) + " ok (client " +
                           std::to_string(r.client_id) + ")");
                Response resp = Response::ok(r.request_id, "GET", {
                    {"led", to_string(r.id)},
                    {"source", "hardware"},
                    {"mode", to_string(r.state.mode)},
                    {"brightness", std::to_string(r.state.brightness)},
                    {"rgb", to_string(r.state.rgb)},
                    {"on_ms", std::to_string(r.state.on_ms)},
                    {"off_ms", std::to_string(r.state.off_ms)},
                    {"status", "valid"},
                });
                send_response(it->second, resp);
            } else {
                log::warn("hardware read of " + to_string(r.id) + " failed: " + r.error_message);
                Response resp = Response::error(r.request_id, "HARDWARE_ERROR", {
                    {"led", to_string(r.id)},
                });
                send_response(it->second, resp);
            }
        }
    }
}

void Reactor::on_scheduler_tick() {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_tick_time_).count();

    if (elapsed >= config_.transport.scheduler_interval_ms) {
        last_tick_time_ = now;
        if (scheduler_.has_dirty()) {
            auto batch = scheduler_.generate_batch();
            if (batch) {
                log::debug("scheduler: submitted batch " + std::to_string(batch->batch_id) +
                           " with " + std::to_string(batch->operations.size()) + " operation(s)");
                worker_.submit_batch(std::move(*batch));
            }
        }
    }
}

void Reactor::handle_client_read(uint64_t client_id) {
    auto it = clients_.find(client_id);
    if (it == clients_.end()) return;
    ClientSession& session = it->second;

    char buf[4096];
    ssize_t n = read(session.fd, buf, sizeof(buf));
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        }
        log::debug("client " + std::to_string(client_id) + ": read error: " + std::string(strerror(errno)));
        disconnect_client(client_id);
        return;
    }
    if (n == 0) {
        disconnect_client(client_id);
        return;
    }

    if (session.recv_buffer.append(buf, n) == StreamBuffer::AppendResult::BufferOverflow) {
        log::warn("client " + std::to_string(client_id) + ": receive buffer overflow, disconnecting");
        disconnect_client(client_id);
        return;
    }

    std::string line;
    bool line_overflow = false;
    while (session.recv_buffer.extract_line(line, line_overflow)) {
        if (line_overflow) {
            log::warn("client " + std::to_string(client_id) + ": line exceeds " +
                      std::to_string(kMaxLineLength) + " byte limit, disconnecting");
            disconnect_client(client_id);
            return;
        }

        if (line.empty()) continue;

        auto result = ProtocolParser::parse_line(line);
        if (result.status == ParseResult::Status::Error) {
            if (result.error_response) {
                std::string resp = result.error_response->serialize();
                if (!resp.empty() && resp.back() == '\n') resp.pop_back();
                log::debug("client " + std::to_string(client_id) + ": protocol error " + resp +
                           " line: " + line);
                send_response(session, *result.error_response);
            }
            if (result.should_close) {
                log::warn("client " + std::to_string(client_id) +
                          ": framing violation, disconnecting");
                disconnect_client(client_id);
                return;
            }
        } else if (result.status == ParseResult::Status::Success && result.request) {
            process_command(session, *result.request);
        }
    }
}

void Reactor::handle_client_write(uint64_t client_id) {
    auto it = clients_.find(client_id);
    if (it == clients_.end()) return;
    ClientSession& session = it->second;

    if (session.send_buffer.empty()) return;

    ssize_t n = write(session.fd, session.send_buffer.data(), session.send_buffer.size());
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        }
        log::debug("client " + std::to_string(client_id) + ": write error: " + std::string(strerror(errno)));
        disconnect_client(client_id);
        return;
    }
    session.send_buffer.erase(0, n);
}

void Reactor::send_response(ClientSession& session, const Response& resp) {
    std::string serialized = resp.serialize();
    if (session.send_buffer.size() + serialized.size() > kMaxQueuedOutput) {
        // Output buffer full per Section 5 limits
        log::warn("client " + std::to_string(session.client_id) + ": output queue overflow, disconnecting");
        disconnect_client(session.client_id);
        return;
    }

    if (session.send_buffer.empty() && session.fd >= 0) {
        ssize_t n = write(session.fd, serialized.data(), serialized.size());
        if (n > 0) {
            if (static_cast<size_t>(n) < serialized.size()) {
                session.send_buffer.append(serialized.substr(n));
            }
            return;
        } else if (n < 0 && (errno != EAGAIN && errno != EWOULDBLOCK)) {
            log::debug("client " + std::to_string(session.client_id) + ": write error: " + std::string(strerror(errno)));
            disconnect_client(session.client_id);
            return;
        }
    }

    session.send_buffer.append(serialized);
}

void Reactor::disconnect_client(uint64_t client_id) {
    auto it = clients_.find(client_id);
    if (it == clients_.end()) return;

    if (!it->second.send_buffer.empty() && it->second.fd >= 0) {
        (void)write(it->second.fd, it->second.send_buffer.data(), it->second.send_buffer.size());
    }

    int rule_idx = it->second.rule_index;
    if (rule_idx >= 0) {
        auto rit = rule_connection_counts_.find(rule_idx);
        if (rit != rule_connection_counts_.end()) {
            if (--rit->second == 0) {
                rule_connection_counts_.erase(rit);
            }
        }
    }

    auto released_leds = ownership_.release_client(client_id);
    for (auto id : released_leds) {
        scheduler_.apply_fallback(id);
    }
    scheduler_.cancel_barriers_for_client(client_id);

    if (it->second.fd >= 0) {
        close(it->second.fd);
    }
    clients_.erase(it);

    log::info("client " + std::to_string(client_id) + " disconnected (" + it->second.remote_addr +
              ", released " + std::to_string(released_leds.size()) + " leds)");

    // Trigger update immediately if fallback states were applied
    if (!released_leds.empty()) {
        auto batch = scheduler_.generate_batch();
        if (batch) {
            log::debug("scheduler: submitted fallback batch " + std::to_string(batch->batch_id) +
                       " with " + std::to_string(batch->operations.size()) + " operation(s)");
            worker_.submit_batch(std::move(*batch));
        }
    }
}

void Reactor::process_command(ClientSession& session, const Request& req) {
    auto caps = worker_.capabilities();

    // Handshake enforcement: HELLO must be the first command
    if (!session.authenticated) {
        if (req.verb != Verb::Hello) {
            log::debug("client " + std::to_string(session.client_id) + ": " +
                       to_string(req.verb) + " before HELLO, disconnecting");
            Response err = Response::error(req.request_id, "PROTOCOL_ERROR", {{"reason", "hello_required"}});
            send_response(session, err);
            disconnect_client(session.client_id);
            return;
        }
    }

    // Per-connection command rate limit (token bucket, burst = rate)
    if (!consume_command_token(session)) {
        log::debug("client " + std::to_string(session.client_id) + ": command rate limit exceeded (request " +
                   std::to_string(req.request_id) + ")");
        send_response(session, Response::error(req.request_id, "RATE_LIMITED", {
            {"reason", "command_rate"},
        }));
        return;
    }

    // Verb permission from the matched access rule
    if (req.verb != Verb::Hello && !rule_allows_verb(session.rule, req.verb)) {
        log::warn("client " + std::to_string(session.client_id) + ": forbidden verb " +
                  to_string(req.verb) + " (request " + std::to_string(req.request_id) + ")");
        send_response(session, Response::error(req.request_id, "FORBIDDEN", {
            {"verb", to_string(req.verb)},
        }));
        return;
    }

    switch (req.verb) {
        case Verb::Hello: {
            session.authenticated = true;
            std::string access = session.is_unix_socket ? "local" : session.remote_addr;
            log::info("client " + std::to_string(session.client_id) + " authenticated (access=" + access +
                      ", priority " + std::to_string(session.configured_priority) + ")");
            Response resp = Response::ok(req.request_id, "HELLO", {
                {"version", "1"},
                {"access", access},
            });
            send_response(session, resp);
            break;
        }

        case Verb::Caps: {
            std::string leds_str;
            for (size_t i = 0; i < caps.supported_leds.size(); ++i) {
                if (i > 0) leds_str += ",";
                leds_str += to_string(caps.supported_leds[i]);
            }
            Response resp = Response::ok(req.request_id, "CAPS", {
                {"version", "1"},
                {"leds", leds_str},
                {"modes", "off,on,blink,breath"},
            });
            send_response(session, resp);
            break;
        }

        case Verb::Set: {
            int prio = req.patch.priority.value_or(session.configured_priority);

            // Priority ceiling: clients may not claim a priority strictly
            // closer to 0 than their configured access rule allows.
            if (req.patch.priority && *req.patch.priority < session.configured_priority) {
                std::string led_str = req.target.is_all ? "all" : to_string(req.target.id);
                log::warn("client " + std::to_string(session.client_id) + ": requested priority " +
                          std::to_string(*req.patch.priority) + " above rule ceiling " +
                          std::to_string(session.configured_priority) + " for " + led_str);
                send_response(session, Response::error(req.request_id, "FORBIDDEN", {
                    {"led", led_str},
                    {"reason", "priority"},
                }));
                return;
            }

            if (req.target.is_all) {
                if (!rule_allows_all_leds(session.rule, caps.supported_leds)) {
                    log::warn("client " + std::to_string(session.client_id) + ": SET all not allowed (request " +
                              std::to_string(req.request_id) + ")");
                    send_response(session, Response::error(req.request_id, "FORBIDDEN", {
                        {"led", "all"},
                    }));
                    return;
                }

                // SET all is atomic: the cycle must be valid against the
                // current state of every target LED (a single-field update
                // can overflow the 16-bit cycle against a retained field).
                std::string cycle_err;
                for (auto led : caps.supported_leds) {
                    if (!validate_patch_cycle(scheduler_.get_desired(led), req.patch, &cycle_err)) {
                        send_response(session, Response::error(req.request_id, "INVALID_ARGUMENT", {
                            {"field", cycle_err},
                        }));
                        return;
                    }
                }

                if (!ownership_.can_mutate_all(caps.supported_leds, session.client_id, prio)) {
                    log::debug("client " + std::to_string(session.client_id) + ": SET all rejected by priority (request " +
                               std::to_string(req.request_id) + ")");
                    Response err = Response::error(req.request_id, "REJECTED", {
                        {"led", "all"},
                        {"reason", "priority"},
                    });
                    send_response(session, err);
                    return;
                }

                ownership_.acquire_or_preempt_all(caps.supported_leds, session.client_id, prio);
                uint64_t gen = 0;
                for (auto led : caps.supported_leds) {
                    gen = scheduler_.apply_patch(led, req.patch);
                }
                session.last_activity_gen = gen;
                log::debug("client " + std::to_string(session.client_id) + ": SET all accepted (generation " +
                           std::to_string(gen) + ")");

                if (!req.reply_none) {
                    Response resp = Response::ok(req.request_id, "ACCEPTED", {
                        {"generation", std::to_string(gen)},
                    });
                    send_response(session, resp);
                }
            } else {
                if (!caps.is_led_supported(req.target.id)) {
                    log::debug("client " + std::to_string(session.client_id) + ": unsupported led " +
                               to_string(req.target.id) + " (request " + std::to_string(req.request_id) + ")");
                    Response err = Response::error(req.request_id, "UNSUPPORTED_LED", {
                        {"led", to_string(req.target.id)},
                    });
                    send_response(session, err);
                    return;
                }

                // State-dependent cycle validation: the parser can only check
                // an on_ms/off_ms pair within one SET, but a single-field
                // update may still overflow the 16-bit cycle against the
                // retained value (or enter a zero cycle). Reject before the
                // SET acquires ownership.
                std::string cycle_err;
                if (!validate_patch_cycle(scheduler_.get_desired(req.target.id), req.patch, &cycle_err)) {
                    log::debug("client " + std::to_string(session.client_id) + ": SET " +
                               to_string(req.target.id) + " rejected, invalid cycle (request " +
                               std::to_string(req.request_id) + ")");
                    send_response(session, Response::error(req.request_id, "INVALID_ARGUMENT", {
                        {"field", cycle_err},
                    }));
                    return;
                }

                if (!rule_allows_led(session.rule, req.target.id)) {
                    log::warn("client " + std::to_string(session.client_id) + ": led " +
                              to_string(req.target.id) + " not allowed (request " +
                              std::to_string(req.request_id) + ")");
                    send_response(session, Response::error(req.request_id, "FORBIDDEN", {
                        {"led", to_string(req.target.id)},
                    }));
                    return;
                }

                if (!ownership_.can_mutate(req.target.id, session.client_id, prio)) {
                    log::debug("client " + std::to_string(session.client_id) + ": SET " +
                               to_string(req.target.id) + " rejected by priority (request " +
                               std::to_string(req.request_id) + ")");
                    Response err = Response::error(req.request_id, "REJECTED", {
                        {"led", to_string(req.target.id)},
                        {"reason", "priority"},
                    });
                    send_response(session, err);
                    return;
                }

                ownership_.acquire_or_preempt(req.target.id, session.client_id, prio);
                uint64_t gen = scheduler_.apply_patch(req.target.id, req.patch);
                session.last_activity_gen = gen;
                log::debug("client " + std::to_string(session.client_id) + ": SET " +
                           to_string(req.target.id) + " accepted (generation " + std::to_string(gen) + ")");

                if (!req.reply_none) {
                    Response resp = Response::ok(req.request_id, "ACCEPTED", {
                        {"generation", std::to_string(gen)},
                    });
                    send_response(session, resp);
                }
            }
            break;
        }

        case Verb::Get: {
            if (!caps.is_led_supported(req.target.id)) {
                log::debug("client " + std::to_string(session.client_id) + ": unsupported led " +
                           to_string(req.target.id) + " (request " + std::to_string(req.request_id) + ")");
                Response err = Response::error(req.request_id, "UNSUPPORTED_LED", {
                    {"led", to_string(req.target.id)},
                });
                send_response(session, err);
                return;
            }

            if (!rule_allows_led(session.rule, req.target.id)) {
                log::warn("client " + std::to_string(session.client_id) + ": led " +
                          to_string(req.target.id) + " not allowed (request " +
                          std::to_string(req.request_id) + ")");
                send_response(session, Response::error(req.request_id, "FORBIDDEN", {
                    {"led", to_string(req.target.id)},
                }));
                return;
            }

            log::debug("client " + std::to_string(session.client_id) + ": GET " +
                       to_string(req.target.id) + " source=" + req.source +
                       " (request " + std::to_string(req.request_id) + ")");

            if (req.source == "desired") {
                auto st = scheduler_.get_desired(req.target.id);
                Response resp = Response::ok(req.request_id, "GET", {
                    {"led", to_string(req.target.id)},
                    {"source", "desired"},
                    {"mode", to_string(st.mode)},
                    {"brightness", std::to_string(st.brightness)},
                    {"rgb", to_string(st.rgb)},
                    {"on_ms", std::to_string(st.on_ms)},
                    {"off_ms", std::to_string(st.off_ms)},
                    {"status", st.valid ? "valid" : "invalid"},
                });
                send_response(session, resp);
            } else if (req.source == "applied") {
                auto st = scheduler_.get_applied(req.target.id);
                Response resp = Response::ok(req.request_id, "GET", {
                    {"led", to_string(req.target.id)},
                    {"source", "applied"},
                    {"mode", to_string(st.mode)},
                    {"brightness", std::to_string(st.brightness)},
                    {"rgb", to_string(st.rgb)},
                    {"on_ms", std::to_string(st.on_ms)},
                    {"off_ms", std::to_string(st.off_ms)},
                    {"status", st.valid ? "valid" : "invalid"},
                });
                send_response(session, resp);
            } else if (req.source == "hardware") {
                if (!consume_hw_read_token()) {
                    log::debug("client " + std::to_string(session.client_id) + ": hardware read rate limit exceeded (request " +
                               std::to_string(req.request_id) + ")");
                    send_response(session, Response::error(req.request_id, "RATE_LIMITED", {
                        {"reason", "hardware_read"},
                    }));
                    return;
                }
                worker_.submit_read(HardwareReadRequest{session.client_id, req.request_id, req.target.id});
            }
            break;
        }

        case Verb::Reset: {
            if (req.target.is_all) {
                auto released = ownership_.release_client(session.client_id);
                for (auto led : released) {
                    scheduler_.apply_fallback(led);
                }
                log::debug("client " + std::to_string(session.client_id) + ": RESET all (released " +
                           std::to_string(released.size()) + " leds)");
                Response resp = Response::ok(req.request_id, "RESET");
                send_response(session, resp);
            } else {
                if (!caps.is_led_supported(req.target.id)) {
                    log::debug("client " + std::to_string(session.client_id) + ": unsupported led " +
                               to_string(req.target.id) + " (request " + std::to_string(req.request_id) + ")");
                    Response err = Response::error(req.request_id, "UNSUPPORTED_LED", {
                        {"led", to_string(req.target.id)},
                    });
                    send_response(session, err);
                    return;
                }

                if (!rule_allows_led(session.rule, req.target.id)) {
                    log::warn("client " + std::to_string(session.client_id) + ": led " +
                              to_string(req.target.id) + " not allowed (request " +
                              std::to_string(req.request_id) + ")");
                    send_response(session, Response::error(req.request_id, "FORBIDDEN", {
                        {"led", to_string(req.target.id)},
                    }));
                    return;
                }

                if (!ownership_.can_reset(req.target.id, session.client_id, session.configured_priority)) {
                    log::debug("client " + std::to_string(session.client_id) + ": RESET " +
                               to_string(req.target.id) + " rejected by priority (request " +
                               std::to_string(req.request_id) + ")");
                    Response err = Response::error(req.request_id, "REJECTED", {
                        {"led", to_string(req.target.id)},
                        {"reason", "priority"},
                    });
                    send_response(session, err);
                    return;
                }

                ownership_.reset(req.target.id, session.client_id, session.configured_priority);
                scheduler_.apply_fallback(req.target.id);
                log::debug("client " + std::to_string(session.client_id) + ": RESET " +
                           to_string(req.target.id) + " (request " + std::to_string(req.request_id) + ")");
                Response resp = Response::ok(req.request_id, "RESET");
                send_response(session, resp);
            }
            break;
        }

        case Verb::Flush: {
            uint64_t target_gen = session.last_activity_gen;
            uint64_t cid = session.client_id;
            uint64_t req_id = req.request_id;
            log::debug("client " + std::to_string(cid) + ": FLUSH barrier (generation " +
                       std::to_string(target_gen) + ", request " + std::to_string(req_id) + ")");

            scheduler_.add_flush_barrier(cid, req_id, target_gen, [this, cid, req_id](bool ok, const std::string& /*err*/) {
                auto it = clients_.find(cid);
                if (it != clients_.end()) {
                    if (ok) {
                        send_response(it->second, Response::ok(req_id, "FLUSH"));
                    } else {
                        log::debug("client " + std::to_string(cid) + ": FLUSH failed (request " +
                                   std::to_string(req_id) + ")");
                        send_response(it->second, Response::error(req_id, "HARDWARE_ERROR"));
                    }
                }
            });
            break;
        }

        default:
            log::debug("client " + std::to_string(session.client_id) + ": invalid command (request " +
                       std::to_string(req.request_id) + ")");
            Response err = Response::error(req.request_id, "INVALID_COMMAND");
            send_response(session, err);
            break;
    }
}

} // namespace ugreen
