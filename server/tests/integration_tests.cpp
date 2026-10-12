#include "fake_backend.h"
#include "reactor.h"
#include "test_framework.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <thread>

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

void run_integration_tests() {
    std::cout << "Running Integration Tests...\n";

    test::run_test("Local service over Unix domain socket", []() {
        std::string sock_path = "/tmp/test_ugreen_leds_" + std::to_string(getpid()) + ".sock";

        ReactorConfig config;
        config.unix_socket_path = sock_path;
        config.enable_unix_socket = true;
        config.enable_tcp = false;
        config.scheduler_interval_ms = 10;

        auto backend = std::make_unique<FakeLedBackend>();
        Reactor reactor(config, std::move(backend));
        TEST_ASSERT(reactor.init());

        // Connect client
        int client_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        set_socket_timeout(client_fd);
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, sock_path.c_str(), sizeof(addr.sun_path) - 1);
        TEST_ASSERT(connect(client_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);

        // Advance reactor to accept connection
        reactor.poll_once(10);

        // Command before HELLO should fail and disconnect
        write_all(client_fd, "CAPS 1\n");
        reactor.poll_once(10);
        std::string resp = read_line_from_fd(client_fd);
        TEST_ASSERT_EQ(resp, "ERR 1 PROTOCOL_ERROR reason=hello_required");

        close(client_fd);
        reactor.poll_once(10);

        // Reconnect
        client_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        set_socket_timeout(client_fd);
        TEST_ASSERT(connect(client_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
        reactor.poll_once(10);

        // HELLO
        write_all(client_fd, "HELLO 2 1\n");
        reactor.poll_once(10);
        resp = read_line_from_fd(client_fd);
        TEST_ASSERT_EQ(resp, "OK 2 HELLO version=1 access=local");

        // CAPS
        write_all(client_fd, "CAPS 3\n");
        reactor.poll_once(10);
        resp = read_line_from_fd(client_fd);
        TEST_ASSERT(resp.rfind("OK 3 CAPS", 0) == 0);

        // SET
        write_all(client_fd, "SET 4 disk1 mode=on brightness=128 rgb=0,255,0\n");
        reactor.poll_once(10);
        resp = read_line_from_fd(client_fd);
        TEST_ASSERT(resp.rfind("OK 4 ACCEPTED generation=", 0) == 0);

        // GET desired
        write_all(client_fd, "GET 5 disk1 source=desired\n");
        reactor.poll_once(10);
        resp = read_line_from_fd(client_fd);
        TEST_ASSERT(resp.find("mode=on") != std::string::npos);
        TEST_ASSERT(resp.find("brightness=128") != std::string::npos);
        TEST_ASSERT(resp.find("rgb=0,255,0") != std::string::npos);

        // FLUSH barrier
        write_all(client_fd, "FLUSH 6\n");
        // Run reactor ticks until worker completes batch
        for (int i = 0; i < 5; ++i) {
            reactor.poll_once(20);
        }
        resp = read_line_from_fd(client_fd);
        TEST_ASSERT_EQ(resp, "OK 6 FLUSH");

        // GET applied after flush
        write_all(client_fd, "GET 7 disk1 source=applied\n");
        reactor.poll_once(10);
        resp = read_line_from_fd(client_fd);
        TEST_ASSERT(resp.find("brightness=128") != std::string::npos);

        // GET hardware
        write_all(client_fd, "GET 8 disk1 source=hardware\n");
        for (int i = 0; i < 5; ++i) {
            reactor.poll_once(20);
        }
        resp = read_line_from_fd(client_fd);
        TEST_ASSERT(resp.find("source=hardware") != std::string::npos);
        TEST_ASSERT(resp.find("brightness=128") != std::string::npos);

        // RESET
        write_all(client_fd, "RESET 9 disk1\n");
        reactor.poll_once(10);
        resp = read_line_from_fd(client_fd);
        TEST_ASSERT_EQ(resp, "OK 9 RESET");

        close(client_fd);
        reactor.stop();
        unlink(sock_path.c_str());
    });

    test::run_test("Local service over Loopback TCP and multi-client priority", []() {
        ReactorConfig config;
        config.enable_unix_socket = false;
        config.enable_tcp = true;
        config.tcp_bind_ip = "127.0.0.1";
        config.tcp_port = 39876;
        config.scheduler_interval_ms = 10;

        auto backend = std::make_unique<FakeLedBackend>();
        Reactor reactor(config, std::move(backend));
        TEST_ASSERT(reactor.init());

        auto connect_tcp = [&]() -> int {
            int fd = socket(AF_INET, SOCK_STREAM, 0);
            set_socket_timeout(fd);
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(config.tcp_port);
            inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
            int rc = connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
            TEST_ASSERT(rc == 0);
            reactor.poll_once(10);
            return fd;
        };

        int client_a = connect_tcp();
        int client_b = connect_tcp();

        // Client A HELLO
        write_all(client_a, "HELLO 1 1\n");
        reactor.poll_once(10);
        std::string resp = read_line_from_fd(client_a);
        TEST_ASSERT_EQ(resp, "OK 1 HELLO version=1 access=127.0.0.1");

        // Client B HELLO
        write_all(client_b, "HELLO 2 1\n");
        reactor.poll_once(10);
        resp = read_line_from_fd(client_b);
        TEST_ASSERT_EQ(resp, "OK 2 HELLO version=1 access=127.0.0.1");

        // Client A claims disk1 with priority 50
        write_all(client_a, "SET 10 disk1 mode=on brightness=100 priority=50\n");
        reactor.poll_once(10);
        resp = read_line_from_fd(client_a);
        TEST_ASSERT(resp.rfind("OK 10 ACCEPTED", 0) == 0);

        // Client B tries to update disk1 with lower priority 80 -> rejected!
        write_all(client_b, "SET 20 disk1 brightness=200 priority=80\n");
        reactor.poll_once(10);
        resp = read_line_from_fd(client_b);
        TEST_ASSERT_EQ(resp, "ERR 20 REJECTED led=disk1 reason=priority");

        // Client B tries to update disk1 with higher priority 10 (< 50) -> preempts!
        write_all(client_b, "SET 21 disk1 brightness=220 priority=10\n");
        reactor.poll_once(10);
        resp = read_line_from_fd(client_b);
        TEST_ASSERT(resp.rfind("OK 21 ACCEPTED", 0) == 0);

        // Client A now rejected because B owns it with priority 10
        write_all(client_a, "SET 11 disk1 brightness=150 priority=50\n");
        reactor.poll_once(10);
        resp = read_line_from_fd(client_a);
        TEST_ASSERT_EQ(resp, "ERR 11 REJECTED led=disk1 reason=priority");

        // Test SET with reply=none
        // Send both commands together so the GET is processed in the same read
        write_all(client_b, "SET 22 disk2 brightness=77 reply=none\nGET 23 disk2 source=desired\n");
        for (int i = 0; i < 3; ++i) reactor.poll_once(10);
        resp = read_line_from_fd(client_b);
        TEST_ASSERT(resp.rfind("OK 23 GET", 0) == 0);
        TEST_ASSERT(resp.find("brightness=77") != std::string::npos);

        close(client_a);
        close(client_b);
        reactor.stop();
    });
}
