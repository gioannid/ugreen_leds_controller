#ifndef UGREEN_SERVER_CONFIG_H
#define UGREEN_SERVER_CONFIG_H

#include "led_state.h"
#include "protocol.h"

#include <sys/socket.h>

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

struct sockaddr_storage;

namespace ugreen {

// One access policy entry (an [access.<name>] section).
// A connection is matched against every rule; the most specific source
// prefix wins and is the complete policy. No match means deny.
struct AccessRule {
    std::string name;
    // 0 = Unix socket ("local"), 4 = IPv4, 6 = IPv6
    int family = 0;
    int prefix_len = 0;
    uint32_t addr4 = 0;      // IPv4 network address, host byte order
    uint8_t addr8[16] = {};  // IPv6 network address

    bool all_leds = true;
    std::vector<LedId> leds;
    std::set<Verb> verbs;    // empty means all verbs allowed
    int priority = 100;      // ceiling: clients may only request >= this value
    size_t max_connections = 0;  // 0 = unlimited
    bool source_set = false; // populated by the config parser
};

// Transport-only settings (kept separate so tests can construct a minimal config).
struct ReactorConfig {
    std::string unix_socket_path = "/tmp/ugreen_leds.sock";
    std::string tcp_bind_ip = "127.0.0.1";
    int tcp_port = 39001;
    int scheduler_interval_ms = 20;
    bool enable_unix_socket = true;
    bool enable_tcp = true;
};

// Complete server configuration (transport + access policy + limits).
struct ServerConfig {
    ReactorConfig transport;
    std::vector<AccessRule> access_rules;
    std::map<LedId, LedState> fallbacks;

    int max_total_connections = 64;      // 0 = unlimited
    int command_rate_limit = 500;        // commands/sec per connection, 0 = unlimited
    int hardware_read_rate_limit = 10;   // reads/sec server-wide, 0 = unlimited
    std::string lock_file;               // process lock file, empty = disabled

    ServerConfig() = default;
    // Allows constructing a transport-only config (development default:
    // empty access policy is replaced by a permissive local-only policy).
    ServerConfig(const ReactorConfig& rc) : transport(rc) {}
};

// Loads and validates a configuration file. Returns false with a descriptive
// error on any syntax or semantic problem (the server must refuse to start).
bool load_server_config(const std::string& path, ServerConfig& out, std::string& error);

// Adds a permissive local-only policy (Unix socket + 127.0.0.0/8, all LEDs,
// all verbs, priority ceiling 0, no connection cap). Used when no access
// rules are configured so the service stays usable in development.
void apply_dev_defaults(ServerConfig& config);

// Most specific rule matching the peer address, or nullptr when denied.
const AccessRule* match_access_rule(const ServerConfig& config, const sockaddr_storage& peer,
                                    socklen_t peer_len, bool is_unix);

// Permission helpers for command handling.
bool rule_allows_led(const AccessRule* rule, LedId id);
bool rule_allows_all_leds(const AccessRule* rule, const std::vector<LedId>& supported);
bool rule_allows_verb(const AccessRule* rule, Verb verb);

// Human-readable source description ("local", "192.168.178.30/32", ...).
std::string describe_rule_source(const AccessRule& rule);

} // namespace ugreen

#endif // UGREEN_SERVER_CONFIG_H
