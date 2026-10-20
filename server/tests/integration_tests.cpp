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

    test::run_test("Mode transition sequence blink-off-blink stays consistent end-to-end", []() {
        // Regression for the field failure "blink succeeded - set off
        // succeeded - blink failed": after the sequence the hardware state,
        // the applied state and the desired state must all agree, and every
        // blink must carry a legal non-zero cycle.
        std::string sock_path = "/tmp/test_ugreen_leds_mt_" + std::to_string(getpid()) + ".sock";

        ReactorConfig config;
        config.unix_socket_path = sock_path;
        config.enable_unix_socket = true;
        config.enable_tcp = false;
        config.scheduler_interval_ms = 10;

        auto backend = std::make_unique<FakeLedBackend>();
        Reactor reactor(config, std::move(backend));
        TEST_ASSERT(reactor.init());

        int client_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        set_socket_timeout(client_fd);
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, sock_path.c_str(), sizeof(addr.sun_path) - 1);
        TEST_ASSERT(connect(client_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
        reactor.poll_once(10);

        auto cmd = [&](const std::string& line) {
            write_all(client_fd, line + "\n");
            for (int i = 0; i < 8; ++i) reactor.poll_once(20);
            return read_line_from_fd(client_fd);
        };

        TEST_ASSERT(cmd("HELLO 1 1").rfind("OK 1 HELLO", 0) == 0);

        // Step 1: blink with an explicit cycle.
        TEST_ASSERT(cmd("SET 2 disk1 mode=blink on_ms=250 off_ms=250").rfind("OK 2 ACCEPTED", 0) == 0);
        TEST_ASSERT_EQ(cmd("FLUSH 3"), "OK 3 FLUSH");
        std::string hw = cmd("GET 4 disk1 source=hardware");
        TEST_ASSERT(hw.find("mode=blink") != std::string::npos);
        TEST_ASSERT(hw.find("on_ms=250") != std::string::npos);
        TEST_ASSERT(hw.find("off_ms=250") != std::string::npos);
        std::string ap = cmd("GET 5 disk1 source=applied");
        TEST_ASSERT(ap.find("mode=blink") != std::string::npos);
        TEST_ASSERT(ap.find("on_ms=250") != std::string::npos);
        TEST_ASSERT(ap.find("off_ms=250") != std::string::npos);

        // Step 2: off.
        TEST_ASSERT(cmd("SET 6 disk1 mode=off").rfind("OK 6 ACCEPTED", 0) == 0);
        TEST_ASSERT_EQ(cmd("FLUSH 7"), "OK 7 FLUSH");
        hw = cmd("GET 8 disk1 source=hardware");
        TEST_ASSERT(hw.find("mode=off") != std::string::npos);
        ap = cmd("GET 9 disk1 source=applied");
        TEST_ASSERT(ap.find("mode=off") != std::string::npos);
        TEST_ASSERT(ap.find("on_ms=0") != std::string::npos);
        TEST_ASSERT(ap.find("off_ms=0") != std::string::npos);

        // Step 3: blink again without timing — the transition that used to
        // fail. It must reach the hardware with a legal cycle.
        TEST_ASSERT(cmd("SET 10 disk1 mode=blink").rfind("OK 10 ACCEPTED", 0) == 0);
        TEST_ASSERT_EQ(cmd("FLUSH 11"), "OK 11 FLUSH");
        hw = cmd("GET 12 disk1 source=hardware");
        TEST_ASSERT(hw.find("mode=blink") != std::string::npos);
        TEST_ASSERT(hw.find("on_ms=500") != std::string::npos);
        TEST_ASSERT(hw.find("off_ms=500") != std::string::npos);
        ap = cmd("GET 13 disk1 source=applied");
        TEST_ASSERT(ap.find("mode=blink") != std::string::npos);
        TEST_ASSERT(ap.find("on_ms=500") != std::string::npos);
        TEST_ASSERT(ap.find("off_ms=500") != std::string::npos);

        // Step 4: timing without mode implies blink (visible in the
        // reported states, not just on the hardware).
        TEST_ASSERT(cmd("SET 14 disk1 mode=on").rfind("OK 14 ACCEPTED", 0) == 0);
        TEST_ASSERT_EQ(cmd("FLUSH 15"), "OK 15 FLUSH");
        TEST_ASSERT(cmd("SET 16 disk1 on_ms=120 off_ms=80").rfind("OK 16 ACCEPTED", 0) == 0);
        TEST_ASSERT_EQ(cmd("FLUSH 17"), "OK 17 FLUSH");
        hw = cmd("GET 18 disk1 source=hardware");
        TEST_ASSERT(hw.find("mode=blink") != std::string::npos);
        TEST_ASSERT(hw.find("on_ms=120") != std::string::npos);
        TEST_ASSERT(hw.find("off_ms=80") != std::string::npos);
        std::string de = cmd("GET 19 disk1 source=desired");
        TEST_ASSERT(de.find("mode=blink") != std::string::npos);

        // Step 5: a single-field update that would overflow the 16-bit cycle
        // against the retained field (120 + 65500 > 65535) must be rejected.
        TEST_ASSERT_EQ(cmd("SET 20 disk1 on_ms=65500"), "ERR 20 INVALID_ARGUMENT field=on_ms");
        de = cmd("GET 21 disk1 source=desired");
        TEST_ASSERT(de.find("on_ms=120") != std::string::npos);

        // Step 6: a zero cycle must be rejected by the parser.
        TEST_ASSERT_EQ(cmd("SET 22 disk1 mode=blink on_ms=0 off_ms=0"),
                       "ERR 22 INVALID_ARGUMENT field=on_ms");

        // Step 7: a zero half-cycle must be rejected — the chip rejects
        // blink/breath commands with on_ms=0 or off_ms=0. Both fields (parser
        // level) and single fields resolving to a zero half against the
        // retained value (state-dependent).
        TEST_ASSERT_EQ(cmd("SET 23 disk1 mode=blink on_ms=0 off_ms=5000"),
                       "ERR 23 INVALID_ARGUMENT field=on_ms");
        TEST_ASSERT_EQ(cmd("SET 24 disk1 mode=blink on_ms=5000 off_ms=0"),
                       "ERR 24 INVALID_ARGUMENT field=on_ms");
        TEST_ASSERT_EQ(cmd("SET 25 disk1 on_ms=0"),
                       "ERR 25 INVALID_ARGUMENT field=on_ms");
        TEST_ASSERT_EQ(cmd("SET 26 disk1 off_ms=0"),
                       "ERR 26 INVALID_ARGUMENT field=on_ms");
        de = cmd("GET 27 disk1 source=desired");
        TEST_ASSERT(de.find("on_ms=120") != std::string::npos);
        TEST_ASSERT(de.find("off_ms=80") != std::string::npos);

        // Step 8: the canonical live failure — a single off_ms update while
        // mode=on resolves to blink(0, off) and must be rejected before any
        // state or ownership change.
        TEST_ASSERT(cmd("SET 28 disk1 mode=on").rfind("OK 28 ACCEPTED", 0) == 0);
        TEST_ASSERT_EQ(cmd("FLUSH 29"), "OK 29 FLUSH");
        TEST_ASSERT_EQ(cmd("SET 30 disk1 off_ms=15000"),
                       "ERR 30 INVALID_ARGUMENT field=on_ms");
        de = cmd("GET 31 disk1 source=desired");
        TEST_ASSERT(de.find("mode=on") != std::string::npos);

        close(client_fd);
        reactor.stop();
        unlink(sock_path.c_str());
    });

    test::run_test("Startup calibration: first off transition reaches a firmware-lit LED", []() {
        // Simulates a NAS whose firmware already has a LED lit when the
        // server starts. The scheduler must not assume "everything is off":
        // the very first SET mode=off has to reach the hardware.
        std::string sock_path = "/tmp/test_ugreen_leds_cal_" + std::to_string(getpid()) + ".sock";

        ReactorConfig config;
        config.unix_socket_path = sock_path;
        config.enable_unix_socket = true;
        config.enable_tcp = false;
        config.scheduler_interval_ms = 10;

        auto backend = std::make_unique<FakeLedBackend>();
        LedState lit;
        lit.mode = OpMode::On;
        lit.brightness = 200;
        lit.rgb = RgbColor{0, 255, 0};
        backend->set_led_state(LedId::Disk1, lit);

        Reactor reactor(config, std::move(backend));
        TEST_ASSERT(reactor.init());

        int client_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        set_socket_timeout(client_fd);
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, sock_path.c_str(), sizeof(addr.sun_path) - 1);
        TEST_ASSERT(connect(client_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
        reactor.poll_once(10);

        auto cmd = [&](const std::string& line) {
            write_all(client_fd, line + "\n");
            for (int i = 0; i < 8; ++i) reactor.poll_once(20);
            return read_line_from_fd(client_fd);
        };

        TEST_ASSERT(cmd("HELLO 1 1").rfind("OK 1 HELLO", 0) == 0);

        // Let the internal calibration reads settle, then confirm the server
        // sees the lit LED.
        std::string hw = cmd("GET 2 disk1 source=hardware");
        TEST_ASSERT(hw.find("mode=on") != std::string::npos);
        TEST_ASSERT(hw.find("rgb=0,255,0") != std::string::npos);
        std::string de = cmd("GET 3 disk1 source=desired");
        TEST_ASSERT(de.find("mode=on") != std::string::npos);

        // The transition that used to be a silent no-op: the server assumed
        // the LED was already off.
        TEST_ASSERT(cmd("SET 4 disk1 mode=off").rfind("OK 4 ACCEPTED", 0) == 0);
        TEST_ASSERT_EQ(cmd("FLUSH 5"), "OK 5 FLUSH");
        hw = cmd("GET 6 disk1 source=hardware");
        TEST_ASSERT(hw.find("mode=off") != std::string::npos);
        std::string ap = cmd("GET 7 disk1 source=applied");
        TEST_ASSERT(ap.find("mode=off") != std::string::npos);

        close(client_fd);
        reactor.stop();
        unlink(sock_path.c_str());
    });

    test::run_test("Local service over Loopback TCP and multi-client priority", []() {
        ReactorConfig config;
        config.enable_unix_socket = false;
        config.enable_tcp = true;
        config.tcp_bind_ip = "127.0.0.1";
        config.tcp_port = find_free_port();
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
