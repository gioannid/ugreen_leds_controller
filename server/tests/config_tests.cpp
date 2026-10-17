#include "config.h"
#include "test_framework.h"

#include <arpa/inet.h>
#include <cstdio>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <fstream>
#include <string>

using namespace ugreen;

static std::string tmp_config_path(const std::string& name) {
    return "/tmp/ugreen_cfg_test_" + std::to_string(getpid()) + "_" + name + ".conf";
}

static void write_tmp(const std::string& path, const std::string& content) {
    std::ofstream out(path);
    out << content;
}

static bool load_ok(const std::string& content, ServerConfig& out, std::string& err) {
    std::string path = tmp_config_path("case");
    write_tmp(path, content);
    bool rc = load_server_config(path, out, err);
    std::remove(path.c_str());
    return rc;
}

static sockaddr_storage make_inet4(const char* ip) {
    sockaddr_storage ss{};
    ss.ss_family = AF_INET;
    auto* sin = reinterpret_cast<sockaddr_in*>(&ss);
    inet_pton(AF_INET, ip, &sin->sin_addr);
    return ss;
}

void run_config_tests() {
    std::cout << "Running Config Tests...\n";

    test::run_test("Parse valid full configuration", []() {
        ServerConfig cfg;
        std::string err;
        bool ok = load_ok(
            "[server]\n"
            "unix_socket=/tmp/x.sock\n"
            "tcp_bind=127.0.0.1\n"
            "tcp_port=40001\n"
            "scheduler_interval_ms=15\n"
            "max_connections=8\n"
            "command_rate_limit=25\n"
            "hardware_read_rate_limit=5\n"
            "lock_file=/tmp/x.lock\n"
            "[access.local-admin]\n"
            "source=local\n"
            "leds=all\n"
            "priority=0\n"
            "max_connections=8\n"
            "[access.agent]\n"
            "source=192.168.178.120/32\n"
            "leds=disk1,disk2\n"
            "verbs=GET,SET\n"
            "priority=10\n"
            "[fallback]\n"
            "power=mode=on brightness=100\n"
            "disk1=mode=blink rgb=255,0,0 on_ms=50 off_ms=50\n",
            cfg, err);
        TEST_ASSERT(ok);
        TEST_ASSERT_EQ(cfg.transport.tcp_port, 40001);
        TEST_ASSERT_EQ(cfg.transport.scheduler_interval_ms, 15);
        TEST_ASSERT_EQ(cfg.max_total_connections, 8);
        TEST_ASSERT_EQ(cfg.command_rate_limit, 25);
        TEST_ASSERT_EQ(cfg.hardware_read_rate_limit, 5);
        TEST_ASSERT_EQ(cfg.lock_file, "/tmp/x.lock");
        TEST_ASSERT_EQ(cfg.access_rules.size(), 2u);

        const auto& r0 = cfg.access_rules[0];
        TEST_ASSERT(r0.source_set);
        TEST_ASSERT_EQ(r0.family, 0);
        TEST_ASSERT(r0.all_leds);
        TEST_ASSERT(r0.verbs.empty());
        TEST_ASSERT_EQ(r0.priority, 0);
        TEST_ASSERT_EQ(r0.max_connections, 8u);

        const auto& r1 = cfg.access_rules[1];
        TEST_ASSERT_EQ(r1.family, 4);
        TEST_ASSERT_EQ(r1.prefix_len, 32);
        TEST_ASSERT(!r1.all_leds);
        TEST_ASSERT_EQ(r1.leds.size(), 2u);
        TEST_ASSERT(r1.leds[0] == LedId::Disk1);
        TEST_ASSERT(r1.leds[1] == LedId::Disk2);
        TEST_ASSERT(r1.verbs.count(Verb::Get) == 1);
        TEST_ASSERT(r1.verbs.count(Verb::Set) == 1);
        TEST_ASSERT(r1.verbs.count(Verb::Reset) == 0);

        TEST_ASSERT(cfg.fallbacks.count(LedId::Power) == 1);
        TEST_ASSERT(cfg.fallbacks.at(LedId::Power).mode == OpMode::On);
        TEST_ASSERT_EQ(cfg.fallbacks.at(LedId::Power).brightness, 100);
        TEST_ASSERT(cfg.fallbacks.count(LedId::Disk1) == 1);
        TEST_ASSERT(cfg.fallbacks.at(LedId::Disk1).mode == OpMode::Blink);
        TEST_ASSERT_EQ(cfg.fallbacks.at(LedId::Disk1).on_ms, 50);
    });

    test::run_test("Most specific source prefix wins", []() {
        ServerConfig cfg;
        std::string err;
        bool ok = load_ok(
            "[access.lan]\n"
            "source=192.168.1.0/24\n"
            "leds=all\n"
            "[access.host]\n"
            "source=192.168.1.50/32\n"
            "leds=disk1\n",
            cfg, err);
        TEST_ASSERT(ok);

        socklen_t len = sizeof(sockaddr_in);
        auto lan_peer = make_inet4("192.168.1.7");
        auto host_peer = make_inet4("192.168.1.50");
        auto outside = make_inet4("10.0.0.1");

        const AccessRule* r = match_access_rule(cfg, lan_peer, len, false);
        TEST_ASSERT(r != nullptr);
        TEST_ASSERT_EQ(r->name, "lan");

        r = match_access_rule(cfg, host_peer, len, false);
        TEST_ASSERT(r != nullptr);
        TEST_ASSERT_EQ(r->name, "host");

        r = match_access_rule(cfg, outside, len, false);
        TEST_ASSERT(r == nullptr);
    });

    test::run_test("Local source matches Unix socket only", []() {
        ServerConfig cfg;
        std::string err;
        bool ok = load_ok(
            "[access.local-admin]\n"
            "source=local\n"
            "leds=all\n",
            cfg, err);
        TEST_ASSERT(ok);

        socklen_t len = sizeof(sockaddr_in);
        auto tcp_peer = make_inet4("127.0.0.1");
        TEST_ASSERT(match_access_rule(cfg, tcp_peer, len, false) == nullptr);

        sockaddr_storage ss{};
        ss.ss_family = AF_UNIX;
        TEST_ASSERT(match_access_rule(cfg, ss, sizeof(sockaddr_un), true) != nullptr);
    });

    test::run_test("Duplicate networks rejected", []() {
        ServerConfig cfg;
        std::string err;
        bool ok = load_ok(
            "[access.a]\n"
            "source=192.168.1.50/32\n"
            "[access.b]\n"
            "source=192.168.1.0/24\n",
            cfg, err);
        TEST_ASSERT(ok);

        ok = load_ok(
            "[access.a]\n"
            "source=192.168.1.50/32\n"
            "[access.b]\n"
            "source=192.168.1.50/32\n",
            cfg, err);
        TEST_ASSERT(!ok);
        TEST_ASSERT(err.find("duplicate") != std::string::npos);
    });

    test::run_test("Duplicate rule names rejected", []() {
        ServerConfig cfg;
        std::string err;
        bool ok = load_ok(
            "[access.a]\n"
            "source=192.168.1.0/24\n"
            "[access.a]\n"
            "source=10.0.0.0/8\n",
            cfg, err);
        TEST_ASSERT(!ok);
    });

    test::run_test("Missing source rejected", []() {
        ServerConfig cfg;
        std::string err;
        bool ok = load_ok(
            "[access.a]\n"
            "leds=all\n",
            cfg, err);
        TEST_ASSERT(!ok);
    });

    test::run_test("Invalid values rejected", []() {
        ServerConfig cfg;
        std::string err;

        bool ok = load_ok("[server]\ntcp_port=70000\n", cfg, err);
        TEST_ASSERT(!ok);

        ok = load_ok("[server]\ntcp_bind=not-an-ip\n", cfg, err);
        TEST_ASSERT(!ok);

        ok = load_ok("[access.a]\nsource=192.168.1.50\npriority=3000000000\n", cfg, err);
        TEST_ASSERT(!ok);

        ok = load_ok("[access.a]\nsource=local\nverbs=BOGUS\n", cfg, err);
        TEST_ASSERT(!ok);

        ok = load_ok("[fallback]\nnonexistentled=mode=on\n", cfg, err);
        TEST_ASSERT(!ok);

        ok = load_ok("[fallback]\n"
                     "power=mode=blink on_ms=100 off_ms=100\n"
                     "power=mode=on\n",
                     cfg, err);
        TEST_ASSERT(!ok);
    });

    test::run_test("Unknown section and key rejected", []() {
        ServerConfig cfg;
        std::string err;
        bool ok = load_ok("[bogus]\nfoo=bar\n", cfg, err);
        TEST_ASSERT(!ok);

        ok = load_ok("[server]\nunknown_key=1\n", cfg, err);
        TEST_ASSERT(!ok);
    });

    test::run_test("Dev defaults provide permissive local policy", []() {
        ServerConfig cfg;
        apply_dev_defaults(cfg);
        TEST_ASSERT_EQ(cfg.access_rules.size(), 2u);

        socklen_t len = sizeof(sockaddr_in);
        auto loopback = make_inet4("127.0.0.2");
        auto other = make_inet4("192.168.1.50");

        const AccessRule* r = match_access_rule(cfg, loopback, len, false);
        TEST_ASSERT(r != nullptr);
        TEST_ASSERT(r->all_leds);
        TEST_ASSERT_EQ(r->priority, 0);

        r = match_access_rule(cfg, other, len, false);
        TEST_ASSERT(r == nullptr);
    });

    test::run_test("IPv6 source parsing and matching", []() {
        ServerConfig cfg;
        std::string err;
        bool ok = load_ok(
            "[access.ipv6]\n"
            "source=fd00::/8\n"
            "leds=all\n",
            cfg, err);
        TEST_ASSERT(ok);
        TEST_ASSERT_EQ(cfg.access_rules.size(), 1u);
        TEST_ASSERT_EQ(cfg.access_rules[0].family, 6);
        TEST_ASSERT_EQ(cfg.access_rules[0].prefix_len, 8);
        TEST_ASSERT_EQ(describe_rule_source(cfg.access_rules[0]), "fd00::/8");
    });

    test::run_test("Verb and LED permission helpers", []() {
        AccessRule rule;
        rule.all_leds = false;
        rule.leds.push_back(LedId::Disk1);
        rule.verbs.insert(Verb::Get);
        rule.verbs.insert(Verb::Set);

        TEST_ASSERT(rule_allows_led(&rule, LedId::Disk1));
        TEST_ASSERT(!rule_allows_led(&rule, LedId::Power));
        TEST_ASSERT(rule_allows_led(nullptr, LedId::Power));

        TEST_ASSERT(rule_allows_verb(&rule, Verb::Get));
        TEST_ASSERT(!rule_allows_verb(&rule, Verb::Reset));
        TEST_ASSERT(rule_allows_verb(nullptr, Verb::Reset));

        std::vector<LedId> supported{LedId::Power, LedId::Disk1, LedId::Disk2};
        TEST_ASSERT(!rule_allows_all_leds(&rule, supported));
        AccessRule all_rule;
        all_rule.all_leds = true;
        TEST_ASSERT(rule_allows_all_leds(&all_rule, supported));
    });
}
