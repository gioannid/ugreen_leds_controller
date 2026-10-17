#include "config.h"
#include "fake_backend.h"
#include "reactor.h"
#include "test_framework.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <string>

using namespace ugreen;

static void set_socket_timeout(int fd) {
    timeval tv{2, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

static std::string read_line_from_fd(int fd) {
    std::string line;
    char c = 0;
    while (true) {
        ssize_t n = read(fd, &c, 1);
        if (n == 0) break;
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (c == '\n') break;
        line += c;
    }
    if (!line.empty() && line.back() == '\r') line.pop_back();
    return line;
}

static void write_all(int fd, const std::string& str) {
    size_t total = 0;
    while (total < str.size()) {
        ssize_t n = write(fd, str.data() + total, str.size() - total);
        if (n <= 0) break;
        total += n;
    }
}

// Finds a free loopback TCP port (avoids clashes with unrelated processes).
static int find_free_port() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    sockaddr_in local{};
    socklen_t slen = sizeof(local);
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&local), &slen) != 0) {
        close(fd);
        return -1;
    }
    int port = ntohs(local.sin_port);
    close(fd);
    return port;
}

// Connect a TCP client bound to a specific loopback source address, so the
// server sees the chosen peer IP for access-rule matching.
static int connect_from(const std::string& local_ip, int port, Reactor& reactor) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    set_socket_timeout(fd);

    sockaddr_in src{};
    src.sin_family = AF_INET;
    src.sin_port = 0;
    if (inet_pton(AF_INET, local_ip.c_str(), &src.sin_addr) != 1) {
        close(fd);
        return -1;
    }
    if (bind(fd, reinterpret_cast<sockaddr*>(&src), sizeof(src)) != 0) {
        close(fd);
        return -1;
    }

    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, "127.0.0.1", &dst.sin_addr);
    if (connect(fd, reinterpret_cast<sockaddr*>(&dst), sizeof(dst)) != 0) {
        close(fd);
        return -1;
    }
    reactor.poll_once(10);
    return fd;
}

static AccessRule make_rule(const std::string& name, const std::string& cidr,
                            bool all_leds, int priority, size_t max_conns) {
    AccessRule r;
    r.name = name;
    r.source_set = true;
    r.all_leds = all_leds;
    r.priority = priority;
    r.max_connections = max_conns;

    std::string net = cidr;
    std::string prefix = "32";
    auto slash = net.find('/');
    if (slash != std::string::npos) {
        prefix = net.substr(slash + 1);
        net = net.substr(0, slash);
    }
    r.family = 4;
    r.prefix_len = std::stoi(prefix);
    // AccessRule stores IPv4 addresses in host byte order (see config.cpp)
    struct in_addr ia;
    inet_pton(AF_INET, net.c_str(), &ia);
    r.addr4 = ntohl(ia.s_addr);
    return r;
}

void run_access_tests() {
    std::cout << "Running Access Tests...\n";

    test::run_test("Connection without matching rule is denied", []() {
        ServerConfig config;
        config.transport.enable_unix_socket = false;
        config.transport.enable_tcp = true;
        config.transport.tcp_bind_ip = "127.0.0.1";
        config.transport.tcp_port = find_free_port();
        config.transport.scheduler_interval_ms = 10;
        config.access_rules.push_back(make_rule("only-host", "127.0.0.2/32", true, 0, 0));

        auto backend = std::make_unique<FakeLedBackend>();
        Reactor reactor(config, std::move(backend));
        TEST_ASSERT(reactor.init());

        // Allowed source
        int ok_fd = connect_from("127.0.0.2", config.transport.tcp_port, reactor);
        TEST_ASSERT(ok_fd >= 0);
        write_all(ok_fd, "HELLO 1 1\n");
        reactor.poll_once(10);
        std::string resp = read_line_from_fd(ok_fd);
        TEST_ASSERT_EQ(resp, "OK 1 HELLO version=1 access=127.0.0.2");
        close(ok_fd);

        // Denied source: pre-HELLO error then close
        int bad_fd = connect_from("127.0.0.3", config.transport.tcp_port, reactor);
        TEST_ASSERT(bad_fd >= 0);
        resp = read_line_from_fd(bad_fd);
        TEST_ASSERT_EQ(resp, "ERR 0 AUTH_FAILED reason=no_matching_rule");

        close(ok_fd);
        close(bad_fd);
        reactor.stop();
    });

    test::run_test("LED restriction enforced for SET/GET/FLUSH", []() {
        ServerConfig config;
        config.transport.enable_unix_socket = false;
        config.transport.enable_tcp = true;
        config.transport.tcp_bind_ip = "127.0.0.1";
        config.transport.tcp_port = find_free_port();
        config.transport.scheduler_interval_ms = 10;
        AccessRule rule = make_rule("disk-only", "127.0.0.2/32", false, 0, 0);
        rule.leds.push_back(LedId::Disk1);
        rule.leds.push_back(LedId::Disk2);
        config.access_rules.push_back(rule);

        auto backend = std::make_unique<FakeLedBackend>();
        Reactor reactor(config, std::move(backend));
        TEST_ASSERT(reactor.init());

        int fd = connect_from("127.0.0.2", config.transport.tcp_port, reactor);
        TEST_ASSERT(fd >= 0);
        write_all(fd, "HELLO 1 1\n");
        reactor.poll_once(10);
        std::string resp = read_line_from_fd(fd);
        TEST_ASSERT(resp.rfind("OK 1 HELLO", 0) == 0);

        // Allowed LED
        write_all(fd, "SET 2 disk1 mode=on brightness=100\n");
        reactor.poll_once(10);
        resp = read_line_from_fd(fd);
        TEST_ASSERT(resp.rfind("OK 2 ACCEPTED", 0) == 0);

        // LED outside the rule
        write_all(fd, "SET 3 power mode=off\n");
        reactor.poll_once(10);
        resp = read_line_from_fd(fd);
        TEST_ASSERT_EQ(resp, "ERR 3 FORBIDDEN led=power");

        // GET restricted LED
        write_all(fd, "GET 4 power source=desired\n");
        reactor.poll_once(10);
        resp = read_line_from_fd(fd);
        TEST_ASSERT_EQ(resp, "ERR 4 FORBIDDEN led=power");

        // GET allowed LED
        write_all(fd, "GET 5 disk2 source=desired\n");
        reactor.poll_once(10);
        resp = read_line_from_fd(fd);
        TEST_ASSERT(resp.rfind("OK 5 GET", 0) == 0);

        // SET all requires full LED access
        write_all(fd, "SET 6 all mode=off\n");
        reactor.poll_once(10);
        resp = read_line_from_fd(fd);
        TEST_ASSERT_EQ(resp, "ERR 6 FORBIDDEN led=all");

        close(fd);
        reactor.stop();
    });

    test::run_test("Verb restriction enforced", []() {
        ServerConfig config;
        config.transport.enable_unix_socket = false;
        config.transport.enable_tcp = true;
        config.transport.tcp_bind_ip = "127.0.0.1";
        config.transport.tcp_port = find_free_port();
        config.transport.scheduler_interval_ms = 10;
        AccessRule rule = make_rule("readonly", "127.0.0.2/32", true, 0, 0);
        rule.verbs.insert(Verb::Caps);
        rule.verbs.insert(Verb::Get);
        config.access_rules.push_back(rule);

        auto backend = std::make_unique<FakeLedBackend>();
        Reactor reactor(config, std::move(backend));
        TEST_ASSERT(reactor.init());

        int fd = connect_from("127.0.0.2", config.transport.tcp_port, reactor);
        TEST_ASSERT(fd >= 0);
        write_all(fd, "HELLO 1 1\n");
        reactor.poll_once(10);
        std::string resp = read_line_from_fd(fd);
        TEST_ASSERT(resp.rfind("OK 1 HELLO", 0) == 0);

        write_all(fd, "SET 2 disk1 mode=on\n");
        reactor.poll_once(10);
        resp = read_line_from_fd(fd);
        TEST_ASSERT_EQ(resp, "ERR 2 FORBIDDEN verb=SET");

        write_all(fd, "RESET 3 disk1\n");
        reactor.poll_once(10);
        resp = read_line_from_fd(fd);
        TEST_ASSERT_EQ(resp, "ERR 3 FORBIDDEN verb=RESET");

        write_all(fd, "FLUSH 4\n");
        reactor.poll_once(10);
        resp = read_line_from_fd(fd);
        TEST_ASSERT_EQ(resp, "ERR 4 FORBIDDEN verb=FLUSH");

        write_all(fd, "GET 5 disk1 source=desired\n");
        reactor.poll_once(10);
        resp = read_line_from_fd(fd);
        TEST_ASSERT(resp.rfind("OK 5 GET", 0) == 0);

        close(fd);
        reactor.stop();
    });

    test::run_test("Priority ceiling enforced, stronger rule can preempt", []() {
        ServerConfig config;
        config.transport.enable_unix_socket = false;
        config.transport.enable_tcp = true;
        config.transport.tcp_bind_ip = "127.0.0.1";
        config.transport.tcp_port = find_free_port();
        config.transport.scheduler_interval_ms = 10;
        config.access_rules.push_back(make_rule("agent", "127.0.0.2/32", true, 50, 0));
        config.access_rules.push_back(make_rule("admin", "127.0.0.3/32", true, 0, 0));

        auto backend = std::make_unique<FakeLedBackend>();
        Reactor reactor(config, std::move(backend));
        TEST_ASSERT(reactor.init());

        int agent = connect_from("127.0.0.2", config.transport.tcp_port, reactor);
        int admin = connect_from("127.0.0.3", config.transport.tcp_port, reactor);
        TEST_ASSERT(agent >= 0 && admin >= 0);

        write_all(agent, "HELLO 1 1\n");
        write_all(admin, "HELLO 1 1\n");
        reactor.poll_once(10);
        std::string resp = read_line_from_fd(agent);
        TEST_ASSERT(resp.rfind("OK 1 HELLO", 0) == 0);
        resp = read_line_from_fd(admin);
        TEST_ASSERT(resp.rfind("OK 1 HELLO", 0) == 0);

        // Agent (ceiling 50) may not claim a strictly stronger priority
        write_all(agent, "SET 2 disk1 mode=on priority=40\n");
        reactor.poll_once(10);
        resp = read_line_from_fd(agent);
        TEST_ASSERT_EQ(resp, "ERR 2 FORBIDDEN led=disk1 reason=priority");

        // Omitted priority uses the configured value
        write_all(agent, "SET 3 disk1 mode=on brightness=100\n");
        reactor.poll_once(10);
        resp = read_line_from_fd(agent);
        TEST_ASSERT(resp.rfind("OK 3 ACCEPTED", 0) == 0);

        // Weaker explicit priority is fine
        write_all(agent, "SET 4 disk1 brightness=120 priority=60\n");
        reactor.poll_once(10);
        resp = read_line_from_fd(agent);
        TEST_ASSERT(resp.rfind("OK 4 ACCEPTED", 0) == 0);

        // Admin (ceiling 0) preempts the agent's LED
        write_all(admin, "SET 10 disk1 brightness=200 priority=10\n");
        reactor.poll_once(10);
        resp = read_line_from_fd(admin);
        TEST_ASSERT(resp.rfind("OK 10 ACCEPTED", 0) == 0);

        // Agent can no longer update the preempted LED
        write_all(agent, "SET 5 disk1 brightness=10 priority=50\n");
        reactor.poll_once(10);
        resp = read_line_from_fd(agent);
        TEST_ASSERT_EQ(resp, "ERR 5 REJECTED led=disk1 reason=priority");

        close(agent);
        close(admin);
        reactor.stop();
    });

    test::run_test("Per-rule connection limit enforced and released", []() {
        ServerConfig config;
        config.transport.enable_unix_socket = false;
        config.transport.enable_tcp = true;
        config.transport.tcp_bind_ip = "127.0.0.1";
        config.transport.tcp_port = find_free_port();
        config.transport.scheduler_interval_ms = 10;
        config.access_rules.push_back(make_rule("limited", "127.0.0.2/32", true, 0, 1));

        auto backend = std::make_unique<FakeLedBackend>();
        Reactor reactor(config, std::move(backend));
        TEST_ASSERT(reactor.init());

        int fd1 = connect_from("127.0.0.2", config.transport.tcp_port, reactor);
        TEST_ASSERT(fd1 >= 0);
        write_all(fd1, "HELLO 1 1\n");
        reactor.poll_once(10);
        std::string resp = read_line_from_fd(fd1);
        TEST_ASSERT(resp.rfind("OK 1 HELLO", 0) == 0);

        int fd2 = connect_from("127.0.0.2", config.transport.tcp_port, reactor);
        TEST_ASSERT(fd2 >= 0);
        resp = read_line_from_fd(fd2);
        TEST_ASSERT_EQ(resp, "ERR 0 AUTH_FAILED reason=connection_limit");
        close(fd2);

        // After the first client disconnects the slot is released
        close(fd1);
        reactor.poll_once(10);
        int fd3 = connect_from("127.0.0.2", config.transport.tcp_port, reactor);
        TEST_ASSERT(fd3 >= 0);
        write_all(fd3, "HELLO 1 1\n");
        reactor.poll_once(10);
        resp = read_line_from_fd(fd3);
        TEST_ASSERT(resp.rfind("OK 1 HELLO", 0) == 0);

        close(fd3);
        reactor.stop();
    });

    test::run_test("Server-wide connection limit enforced", []() {
        ServerConfig config;
        config.transport.enable_unix_socket = false;
        config.transport.enable_tcp = true;
        config.transport.tcp_bind_ip = "127.0.0.1";
        config.transport.tcp_port = find_free_port();
        config.transport.scheduler_interval_ms = 10;
        config.max_total_connections = 1;
        config.access_rules.push_back(make_rule("a", "127.0.0.2/32", true, 0, 0));
        config.access_rules.push_back(make_rule("b", "127.0.0.3/32", true, 0, 0));

        auto backend = std::make_unique<FakeLedBackend>();
        Reactor reactor(config, std::move(backend));
        TEST_ASSERT(reactor.init());

        int fd1 = connect_from("127.0.0.2", config.transport.tcp_port, reactor);
        TEST_ASSERT(fd1 >= 0);
        write_all(fd1, "HELLO 1 1\n");
        reactor.poll_once(10);
        std::string resp = read_line_from_fd(fd1);
        TEST_ASSERT(resp.rfind("OK 1 HELLO", 0) == 0);

        int fd2 = connect_from("127.0.0.3", config.transport.tcp_port, reactor);
        TEST_ASSERT(fd2 >= 0);
        resp = read_line_from_fd(fd2);
        TEST_ASSERT_EQ(resp, "ERR 0 AUTH_FAILED reason=server_limit");

        close(fd1);
        close(fd2);
        reactor.stop();
    });

    test::run_test("Per-connection command rate limit", []() {
        ServerConfig config;
        config.transport.enable_unix_socket = false;
        config.transport.enable_tcp = true;
        config.transport.tcp_bind_ip = "127.0.0.1";
        config.transport.tcp_port = find_free_port();
        config.transport.scheduler_interval_ms = 10;
        config.command_rate_limit = 3;
        config.access_rules.push_back(make_rule("local", "127.0.0.2/32", true, 0, 0));

        auto backend = std::make_unique<FakeLedBackend>();
        Reactor reactor(config, std::move(backend));
        TEST_ASSERT(reactor.init());

        int fd = connect_from("127.0.0.2", config.transport.tcp_port, reactor);
        TEST_ASSERT(fd >= 0);

        // Burst = 3 tokens: HELLO + two SETs pass, the next is rate limited.
        write_all(fd,
                  "HELLO 1 1\n"
                  "SET 2 disk1 mode=on\n"
                  "SET 3 disk2 mode=on\n"
                  "SET 4 disk3 mode=on\n");
        reactor.poll_once(10);

        std::string r1 = read_line_from_fd(fd);
        TEST_ASSERT(r1.rfind("OK 1 HELLO", 0) == 0);
        std::string r2 = read_line_from_fd(fd);
        TEST_ASSERT(r2.rfind("OK 2 ACCEPTED", 0) == 0);
        std::string r3 = read_line_from_fd(fd);
        TEST_ASSERT(r3.rfind("OK 3 ACCEPTED", 0) == 0);
        std::string r4 = read_line_from_fd(fd);
        TEST_ASSERT_EQ(r4, "ERR 4 RATE_LIMITED reason=command_rate");

        // Tokens refill over time
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        reactor.poll_once(10);
        write_all(fd, "SET 5 disk4 mode=on\n");
        reactor.poll_once(10);
        std::string r5 = read_line_from_fd(fd);
        TEST_ASSERT(r5.rfind("OK 5 ACCEPTED", 0) == 0);

        close(fd);
        reactor.stop();
    });

    test::run_test("Server-wide hardware read rate limit", []() {
        ServerConfig config;
        config.transport.enable_unix_socket = false;
        config.transport.enable_tcp = true;
        config.transport.tcp_bind_ip = "127.0.0.1";
        config.transport.tcp_port = find_free_port();
        config.transport.scheduler_interval_ms = 10;
        config.hardware_read_rate_limit = 1;
        config.access_rules.push_back(make_rule("local", "127.0.0.2/32", true, 0, 0));

        auto backend = std::make_unique<FakeLedBackend>();
        Reactor reactor(config, std::move(backend));
        TEST_ASSERT(reactor.init());

        int fd = connect_from("127.0.0.2", config.transport.tcp_port, reactor);
        TEST_ASSERT(fd >= 0);
        write_all(fd, "HELLO 1 1\n");
        reactor.poll_once(10);
        std::string resp = read_line_from_fd(fd);
        TEST_ASSERT(resp.rfind("OK 1 HELLO", 0) == 0);

        // One read token: first GET is queued, the rest are rate limited.
        write_all(fd,
                  "GET 2 disk1 source=hardware\n"
                  "GET 3 disk2 source=hardware\n"
                  "GET 4 disk3 source=hardware\n");
        for (int i = 0; i < 5; ++i) {
            reactor.poll_once(20);
        }

        std::string e1 = read_line_from_fd(fd);
        TEST_ASSERT_EQ(e1, "ERR 3 RATE_LIMITED reason=hardware_read");
        std::string e2 = read_line_from_fd(fd);
        TEST_ASSERT_EQ(e2, "ERR 4 RATE_LIMITED reason=hardware_read");

        // The queued read completes
        std::string ok = read_line_from_fd(fd);
        TEST_ASSERT(ok.rfind("OK 2 GET", 0) == 0);
        TEST_ASSERT(ok.find("source=hardware") != std::string::npos);

        close(fd);
        reactor.stop();
    });

    test::run_test("Slow client backpressure: overflowing reader is disconnected", []() {
        ServerConfig config;
        config.transport.enable_unix_socket = false;
        config.transport.enable_tcp = true;
        config.transport.tcp_bind_ip = "127.0.0.1";
        config.transport.tcp_port = find_free_port();
        config.transport.scheduler_interval_ms = 5;
        config.command_rate_limit = 0;  // unlimited, so output volume is the variable
        config.access_rules.push_back(make_rule("local", "127.0.0.2/32", true, 0, 0));

        auto backend = std::make_unique<FakeLedBackend>();
        Reactor reactor(config, std::move(backend));
        TEST_ASSERT(reactor.init());

        // Client with a small, fixed receive buffer: setting SO_RCVBUF makes
        // the kernel skip rcvbuf auto-tuning, so the total kernel absorption
        // stays small and the server's bounded send queue actually overflows.
        int fd = connect_from("127.0.0.2", config.transport.tcp_port, reactor);
        TEST_ASSERT(fd >= 0);
        uint32_t rcvbuf = 65536;
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
        write_all(fd, "HELLO 1 1\n");
        reactor.poll_once(10);
        std::string resp = read_line_from_fd(fd);
        TEST_ASSERT(resp.rfind("OK 1 HELLO", 0) == 0);

        // Flood the server with more GETs than the output kernel buffers can
        // absorb (tens of MB of responses vs a few MB of kernel absorption),
        // while never reading the socket. Draining the socket here would let
        // the server's writes succeed forever and prevent the overflow, so
        // the client must stay silent until the server tears the connection
        // down.
        std::string flood;
        for (int i = 0; i < 100000; ++i) {
            flood += "GET " + std::to_string(100 + i) + " disk1 source=desired\n";
        }

        size_t sent = 0;
        while (sent < flood.size()) {
            ssize_t n = send(fd, flood.data() + sent, flood.size() - sent,
                             MSG_NOSIGNAL | MSG_DONTWAIT);
            if (n > 0) {
                sent += static_cast<size_t>(n);
            } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                reactor.poll_once(2);
            } else {
                // EPIPE/ECONNRESET: the server dropped the connection
                break;
            }
        }

        // No-drain phase: keep driving the reactor without reading the
        // socket. The server processes the input backlog, the output kernel
        // buffer fills (nobody is reading), the bounded send queue overflows
        // kMaxQueuedOutput and the server disconnects the stuck client.
        // A 1-byte non-blocking send probes for the teardown (EPIPE) so the
        // loop can stop as soon as it happens.
        for (int i = 0; i < 3000; ++i) {
            reactor.poll_once(2);
            char probe = ' ';
            ssize_t n = send(fd, &probe, 1, MSG_NOSIGNAL | MSG_DONTWAIT);
            if (n < 0 && (errno == EPIPE || errno == ECONNRESET)) break;
        }

        // The server must have torn down the connection: either a clean EOF
        // or a RST (ECONNRESET) once the buffered data is drained.
        bool torn_down = false;
        char buf[65536];
        for (int i = 0; i < 300; ++i) {
            reactor.poll_once(20);
            ssize_t n = read(fd, buf, sizeof(buf));
            if (n < 0) {
                if (errno == ECONNRESET) {
                    torn_down = true;
                }
                break;
            }
            if (n == 0) {
                torn_down = true;
                break;
            }
        }
        TEST_ASSERT(torn_down);
        close(fd);
        reactor.poll_once(10);

        // Service must remain responsive to other clients
        int fd2 = connect_from("127.0.0.2", config.transport.tcp_port, reactor);
        TEST_ASSERT(fd2 >= 0);
        write_all(fd2, "HELLO 1 1\n");
        reactor.poll_once(10);
        resp = read_line_from_fd(fd2);
        TEST_ASSERT(resp.rfind("OK 1 HELLO", 0) == 0);

        close(fd2);
        reactor.stop();
    });
}
