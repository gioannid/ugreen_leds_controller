#include "reactor.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <iostream>

namespace ugreen {

static bool set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return false;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) != -1;
}

Reactor::Reactor(ReactorConfig config, std::unique_ptr<LedBackend> backend)
    : config_(std::move(config)),
      worker_(std::move(backend)),
      scheduler_(worker_.capabilities().supported_leds) {
    last_tick_time_ = std::chrono::steady_clock::now();
}

Reactor::~Reactor() {
    stop();
}

bool Reactor::init() {
    worker_.start();

    if (config_.enable_unix_socket) {
        unix_listen_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
        if (unix_listen_fd_ >= 0) {
            set_nonblocking(unix_listen_fd_);
            sockaddr_un addr{};
            addr.sun_family = AF_UNIX;
            std::strncpy(addr.sun_path, config_.unix_socket_path.c_str(), sizeof(addr.sun_path) - 1);
            unlink(config_.unix_socket_path.c_str());

            if (bind(unix_listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
                listen(unix_listen_fd_, 16);
            } else {
                close(unix_listen_fd_);
                unix_listen_fd_ = -1;
            }
        }
    }

    if (config_.enable_tcp) {
        tcp_listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (tcp_listen_fd_ >= 0) {
            set_nonblocking(tcp_listen_fd_);
            int opt = 1;
            setsockopt(tcp_listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(config_.tcp_port);
            inet_pton(AF_INET, config_.tcp_bind_ip.c_str(), &addr.sin_addr);

            if (bind(tcp_listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
                listen(tcp_listen_fd_, 16);
            } else {
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

    if (unix_listen_fd_ >= 0) {
        close(unix_listen_fd_);
        unlink(config_.unix_socket_path.c_str());
        unix_listen_fd_ = -1;
    }

    if (tcp_listen_fd_ >= 0) {
        close(tcp_listen_fd_);
        tcp_listen_fd_ = -1;
    }

    worker_.stop();
}

uint64_t Reactor::add_client_fd(int fd, bool is_unix, const std::string& addr) {
    set_nonblocking(fd);
    uint64_t cid = next_client_id_++;
    ClientSession s;
    s.client_id = cid;
    s.fd = fd;
    s.is_unix_socket = is_unix;
    s.remote_addr = addr;
    s.default_priority = 100;
    clients_[cid] = std::move(s);
    return cid;
}

void Reactor::run() {
    while (running_) {
        poll_once(config_.scheduler_interval_ms);
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
        short events = POLLIN;
        if (!s.send_buffer.empty()) {
            events |= POLLOUT;
        }
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
    sockaddr_storage ss{};
    socklen_t slen = sizeof(ss);
    int client_fd = accept(listen_fd, reinterpret_cast<sockaddr*>(&ss), &slen);
    if (client_fd < 0) return;

    std::string addr_str = is_unix ? "unix" : "tcp";
    if (!is_unix && ss.ss_family == AF_INET) {
        char buf[INET_ADDRSTRLEN] = {0};
        auto* sin = reinterpret_cast<sockaddr_in*>(&ss);
        inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf));
        addr_str = buf;
    }

    add_client_fd(client_fd, is_unix, addr_str);
}

void Reactor::handle_worker_wakeup() {
    uint64_t val = 0;
    (void)read(worker_.wakeup_fd(), &val, sizeof(val));

    auto completed_batches = worker_.pop_completed_batches();
    for (const auto& b : completed_batches) {
        scheduler_.on_batch_complete(b);
    }

    auto completed_reads = worker_.pop_completed_reads();
    for (const auto& r : completed_reads) {
        auto it = clients_.find(r.client_id);
        if (it != clients_.end()) {
            if (r.success) {
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

    if (elapsed >= config_.scheduler_interval_ms) {
        last_tick_time_ = now;
        if (scheduler_.has_dirty()) {
            auto batch = scheduler_.generate_batch();
            if (batch) {
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
        disconnect_client(client_id);
        return;
    }
    if (n == 0) {
        disconnect_client(client_id);
        return;
    }

    if (session.recv_buffer.append(buf, n) == StreamBuffer::AppendResult::BufferOverflow) {
        disconnect_client(client_id);
        return;
    }

    std::string line;
    bool line_overflow = false;
    while (session.recv_buffer.extract_line(line, line_overflow)) {
        if (line_overflow) {
            disconnect_client(client_id);
            return;
        }

        if (line.empty()) continue;

        auto result = ProtocolParser::parse_line(line);
        if (result.status == ParseResult::Status::Error) {
            if (result.error_response) {
                send_response(session, *result.error_response);
            }
            if (result.should_close) {
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
        disconnect_client(client_id);
        return;
    }
    session.send_buffer.erase(0, n);
}

void Reactor::send_response(ClientSession& session, const Response& resp) {
    std::string serialized = resp.serialize();
    if (session.send_buffer.size() + serialized.size() > kMaxQueuedOutput) {
        // Output buffer full per Section 5 limits
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

    auto released_leds = ownership_.release_client(client_id);
    for (auto id : released_leds) {
        scheduler_.apply_fallback(id);
    }
    scheduler_.cancel_barriers_for_client(client_id);

    if (it->second.fd >= 0) {
        close(it->second.fd);
    }
    clients_.erase(it);

    // Trigger update immediately if fallback states were applied
    if (!released_leds.empty()) {
        auto batch = scheduler_.generate_batch();
        if (batch) {
            worker_.submit_batch(std::move(*batch));
        }
    }
}

void Reactor::process_command(ClientSession& session, const Request& req) {
    auto caps = worker_.capabilities();

    // Handshake enforcement: HELLO must be the first command
    if (!session.authenticated) {
        if (req.verb != Verb::Hello) {
            Response err = Response::error(req.request_id, "PROTOCOL_ERROR", {{"reason", "hello_required"}});
            send_response(session, err);
            disconnect_client(session.client_id);
            return;
        }
    }

    switch (req.verb) {
        case Verb::Hello: {
            session.authenticated = true;
            std::string access = session.is_unix_socket ? "local" : session.remote_addr;
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
            int prio = req.patch.priority.value_or(session.default_priority);

            if (req.target.is_all) {
                if (!ownership_.can_mutate_all(caps.supported_leds, session.client_id, prio)) {
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

                if (!req.reply_none) {
                    Response resp = Response::ok(req.request_id, "ACCEPTED", {
                        {"generation", std::to_string(gen)},
                    });
                    send_response(session, resp);
                }
            } else {
                if (!caps.is_led_supported(req.target.id)) {
                    Response err = Response::error(req.request_id, "UNSUPPORTED_LED", {
                        {"led", to_string(req.target.id)},
                    });
                    send_response(session, err);
                    return;
                }

                if (!ownership_.can_mutate(req.target.id, session.client_id, prio)) {
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
                Response err = Response::error(req.request_id, "UNSUPPORTED_LED", {
                    {"led", to_string(req.target.id)},
                });
                send_response(session, err);
                return;
            }

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
                Response resp = Response::ok(req.request_id, "RESET");
                send_response(session, resp);
            } else {
                if (!caps.is_led_supported(req.target.id)) {
                    Response err = Response::error(req.request_id, "UNSUPPORTED_LED", {
                        {"led", to_string(req.target.id)},
                    });
                    send_response(session, err);
                    return;
                }

                if (!ownership_.can_reset(req.target.id, session.client_id, session.default_priority)) {
                    Response err = Response::error(req.request_id, "REJECTED", {
                        {"led", to_string(req.target.id)},
                        {"reason", "priority"},
                    });
                    send_response(session, err);
                    return;
                }

                ownership_.reset(req.target.id, session.client_id, session.default_priority);
                scheduler_.apply_fallback(req.target.id);
                Response resp = Response::ok(req.request_id, "RESET");
                send_response(session, resp);
            }
            break;
        }

        case Verb::Flush: {
            uint64_t target_gen = session.last_activity_gen;
            uint64_t cid = session.client_id;
            uint64_t req_id = req.request_id;

            scheduler_.add_flush_barrier(cid, req_id, target_gen, [this, cid, req_id](bool ok, const std::string& /*err*/) {
                auto it = clients_.find(cid);
                if (it != clients_.end()) {
                    if (ok) {
                        send_response(it->second, Response::ok(req_id, "FLUSH"));
                    } else {
                        send_response(it->second, Response::error(req_id, "HARDWARE_ERROR"));
                    }
                }
            });
            break;
        }

        default:
            Response err = Response::error(req.request_id, "INVALID_COMMAND");
            send_response(session, err);
            break;
    }
}

} // namespace ugreen
