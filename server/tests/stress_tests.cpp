#include "fake_backend.h"
#include "reactor.h"
#include "test_framework.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using namespace ugreen;

static void set_socket_timeout(int fd) {
    timeval tv{5, 0};
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

static int connect_tcp(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    set_socket_timeout(fd);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static long read_vmrss_kb() {
    std::ifstream f("/proc/self/status");
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("VmRSS:", 0) == 0) {
            try {
                return std::stol(line.substr(6));
            } catch (...) {
                return 0;
            }
        }
    }
    return 0;
}

void run_stress_tests() {
    std::cout << "Running Stress Tests...\n";

    test::run_test("Connection churn without unbounded memory growth", []() {
        ReactorConfig rc;
        rc.enable_unix_socket = false;
        rc.enable_tcp = true;
        rc.tcp_bind_ip = "127.0.0.1";
        rc.tcp_port = find_free_port();
        rc.scheduler_interval_ms = 5;

        auto backend = std::make_unique<FakeLedBackend>();
        Reactor reactor(rc, std::move(backend));
        TEST_ASSERT(reactor.init());

        const int kIterations = 1000;
        long rss_before = read_vmrss_kb();
        int port = rc.tcp_port;

        for (int i = 0; i < kIterations; ++i) {
            int fd = connect_tcp(port);
            TEST_ASSERT(fd >= 0);
            reactor.poll_once(5);

            write_all(fd, "HELLO 1 1\n");
            reactor.poll_once(5);
            std::string resp = read_line_from_fd(fd);
            TEST_ASSERT(resp.rfind("OK 1 HELLO", 0) == 0);

            write_all(fd, "CAPS 2\n");
            reactor.poll_once(5);
            resp = read_line_from_fd(fd);
            TEST_ASSERT(resp.rfind("OK 2 CAPS", 0) == 0);

            close(fd);
            reactor.poll_once(5);
        }

        // Settle: drain any queued work and let the allocator release pages
        for (int i = 0; i < 10; ++i) {
            reactor.poll_once(20);
        }
        long rss_after = read_vmrss_kb();

        reactor.stop();

        long delta_kb = rss_after - rss_before;
        // A clean reactor releases per-connection state; allow generous
        // headroom for allocator retention but catch leak-class growth.
        TEST_ASSERT(delta_kb < 32 * 1024);
    });

    test::run_test("Sustained parallel client load", []() {
        // Rate limits disabled: this test measures sustained throughput,
        // not rate limiting.
        ServerConfig config;
        config.transport.enable_unix_socket = false;
        config.transport.enable_tcp = true;
        config.transport.tcp_bind_ip = "127.0.0.1";
        config.transport.tcp_port = find_free_port();
        config.transport.scheduler_interval_ms = 5;
        config.command_rate_limit = 0;
        config.hardware_read_rate_limit = 0;

        auto backend = std::make_unique<FakeLedBackend>();
        Reactor reactor(config, std::move(backend));
        TEST_ASSERT(reactor.init());

        std::thread reactor_thread([&]() {
            reactor.run();
        });

        // One thread per LED (6 LEDs), each exclusively owning its LED so
        // ownership never rejects. 300 SET/GET cycles each.
        const LedId leds[] = {LedId::Power, LedId::Netdev,
                              LedId::Disk1, LedId::Disk2,
                              LedId::Disk3, LedId::Disk4};
        const int kThreads = 6;
        const int kCycles = 300;

        std::vector<std::thread> workers;
        std::atomic<int> ok_commands{0};
        std::atomic<int> bad_commands{0};

        int port = config.transport.tcp_port;
        for (int t = 0; t < kThreads; ++t) {
            workers.emplace_back([&, t]() {
                int fd = connect_tcp(port);
                if (fd < 0) {
                    bad_commands.fetch_add(1);
                    return;
                }

                int good = 0;
                bool failed = false;

                auto expect_ok = [&](int req_id, const std::string& cmd) {
                    write_all(fd, cmd);
                    std::string resp = read_line_from_fd(fd);
                    if (resp.rfind("OK " + std::to_string(req_id), 0) != 0) {
                        failed = true;
                        return false;
                    }
                    ++good;
                    return true;
                };

                if (!expect_ok(1, "HELLO 1 1\n")) {
                    close(fd);
                    bad_commands.fetch_add(1);
                    return;
                }

                std::string led = to_string(leds[t]);
                for (int c = 0; c < kCycles && !failed; ++c) {
                    int set_id = 100 + c * 2;
                    int get_id = 101 + c * 2;
                    std::string set_cmd = "SET " + std::to_string(set_id) + " " + led +
                                          " mode=on brightness=" + std::to_string(c % 251 + 1) + "\n";
                    if (!expect_ok(set_id, set_cmd)) break;
                    std::string get_cmd = "GET " + std::to_string(get_id) + " " + led +
                                          " source=desired\n";
                    if (!expect_ok(get_id, get_cmd)) break;
                }

                close(fd);
                if (failed) {
                    bad_commands.fetch_add(1);
                } else {
                    ok_commands.fetch_add(good);
                }
            });
        }

        for (auto& w : workers) {
            w.join();
        }

        reactor.stop();
        reactor_thread.join();

        TEST_ASSERT_EQ(bad_commands.load(), 0);
        TEST_ASSERT_EQ(ok_commands.load(), kThreads * (1 + kCycles * 2));
    });

    test::run_test("Hardware failure storm: errors reported, service stable", []() {
        ReactorConfig rc;
        rc.enable_unix_socket = false;
        rc.enable_tcp = true;
        rc.tcp_bind_ip = "127.0.0.1";
        rc.tcp_port = find_free_port();
        rc.scheduler_interval_ms = 5;

        auto backend = std::make_unique<FakeLedBackend>();
        auto* backend_ptr = backend.get();
        Reactor reactor(rc, std::move(backend));
        TEST_ASSERT(reactor.init());

        int fd = connect_tcp(rc.tcp_port);
        TEST_ASSERT(fd >= 0);
        reactor.poll_once(5);
        write_all(fd, "HELLO 1 1\n");
        reactor.poll_once(5);
        std::string resp = read_line_from_fd(fd);
        TEST_ASSERT(resp.rfind("OK 1 HELLO", 0) == 0);

        // Storm: 100 SET + FLUSH rounds while every apply fails
        backend_ptr->inject_apply_failure(true);
        for (int i = 0; i < 100; ++i) {
            int set_id = 100 + i * 2;
            int flush_id = 101 + i * 2;
            write_all(fd, "SET " + std::to_string(set_id) +
                          " disk1 brightness=" + std::to_string(i % 251 + 1) + "\n");
            reactor.poll_once(5);
            resp = read_line_from_fd(fd);
            TEST_ASSERT(resp.rfind("OK " + std::to_string(set_id), 0) == 0);

            write_all(fd, "FLUSH " + std::to_string(flush_id) + "\n");
            for (int p = 0; p < 10 && !resp.empty(); ++p) {
                reactor.poll_once(20);
            }
            resp = read_line_from_fd(fd);
            TEST_ASSERT_EQ(resp, "ERR " + std::to_string(flush_id) + " HARDWARE_ERROR");
        }

        // Recovery: hardware comes back, next flush succeeds
        backend_ptr->inject_apply_failure(false);
        write_all(fd, "SET 500 disk1 brightness=42\n");
        reactor.poll_once(5);
        resp = read_line_from_fd(fd);
        TEST_ASSERT(resp.rfind("OK 500 ACCEPTED", 0) == 0);

        write_all(fd, "FLUSH 501\n");
        for (int p = 0; p < 20; ++p) {
            reactor.poll_once(20);
            // Peek: stop once a full line is available
            char probe;
            ssize_t n = recv(fd, &probe, 1, MSG_PEEK);
            if (n > 0) break;
        }
        resp = read_line_from_fd(fd);
        TEST_ASSERT_EQ(resp, "OK 501 FLUSH");

        // Service still fully functional
        write_all(fd, "GET 600 disk1 source=desired\n");
        reactor.poll_once(5);
        resp = read_line_from_fd(fd);
        TEST_ASSERT(resp.rfind("OK 600 GET", 0) == 0);

        close(fd);
        reactor.stop();
    });
}
