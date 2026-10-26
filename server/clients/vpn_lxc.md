# VPN Gateway LXC LED Supervisor

Runs inside the VPN gateway LXC (`192.168.178.104`), supervising the WireGuard
tunnel (`wg-quick@proton`) through which the qBittorrent LXC (`10.60.0.253`)
routes all of its traffic. Python 3.8+, standard library only.

## Behavior

| Condition | disk1 State | Precedence / Ownership |
| --- | --- | --- |
| **VPN OK** (tunnel active, IPv4 in Endpoint /16, IPv6 blocked) | Unaltered / Idle | Does not take ownership; qBittorrent LXC controls disk1 at priority 20 |
| **VPN Problem** (tunnel down, handshake stale, IPv6 leak, or egress IPv4 mismatch) | **Flashing RED** (500 ms on / 500 ms off) | Preempts disk1 at higher priority (priority 10) |
| **VPN Restored** | Returns to server fallback (`mode=off`) via `RESET` | Releases ownership; qBittorrent LXC re-asserts its normal disk1 state |

### What an "OK" VPN looks like

1. **WireGuard Tunnel Active:**
   - Network interface `proton` exists (`/sys/class/net/proton`).
   - Service `wg-quick@proton.service` is `active`.
   - WireGuard handshake is recent (`latest-handshakes` age < 180 seconds).
2. **No IPv6 Connectivity:**
   - IPv6 is disabled in the kernel (`net.ipv6.conf.all.disable_ipv6=1`) as configured
     by `proton.conf` PostUp scripts.
   - Any attempt to reach external IPv6 hosts (`traceroute -6 icanhazip.com`,
     `curl -6 ifconfig.me`, or TCP connections to public IPv6 endpoints) fails
     with `Cannot assign requested address` or `Network is unreachable`.
   - If any external IPv6 connectivity succeeds, an **IPv6 leak** is flagged.
3. **Public IPv4 Matches Tunnel Endpoint /16 Subnet:**
   - WireGuard Endpoint is extracted from `/etc/wireguard/proton.conf` or `wg show`
     (e.g., `Endpoint = 45.92.33.162:51820` -> `/16` subnet `45.92.0.0/16`).
   - Public egress IPv4 is queried over IPv4 (`curl -4 ifconfig.me` -> `45.92.33.165`).
   - The public IP (`45.92.33.165`) must belong to the tunnel Endpoint's `/16` network
     (`45.92.0.0/16`). If traffic bypasses the VPN (e.g. through the local gateway ISP IP),
     or if public IPv4 cannot be retrieved, a failure is flagged.

## Priority Interaction with qBittorrent LXC

Both agents drive `disk1`, coordinated through server-side priorities in `server.conf`:

```ini
[access.vpn]
source=192.168.178.104/32
leds=disk1
verbs=CAPS,GET,SET,RESET,FLUSH
priority=10
max_connections=1

[access.stealth]
source=10.60.0.253/32
leds=disk1
verbs=CAPS,GET,SET,RESET,FLUSH
priority=20
max_connections=8
```

- In `ugreen_leds_server`, lower numeric priority values represent higher precedence
  (`0` is highest).
- While the VPN is healthy, the VPN supervisor sends **no mutating commands** (`SET`),
  so qBittorrent (`10.60.0.253`, priority 20) freely owns and drives `disk1` (green/blue/orange).
- When a VPN problem is detected, the VPN supervisor issues `SET disk1 mode=blink rgb=255,0,0 ... priority=10`.
  Because priority `10` is strictly closer to `0` than `20`, the LED server **preempts**
  `disk1` ownership, forcing `disk1` into flashing red regardless of qBittorrent's state.
  Subsequent updates from qBittorrent at priority 20 are rejected by the server
  (`ERR ... REJECTED reason=priority`).
- Once the VPN recovers, the supervisor issues `RESET disk1`. This immediately releases
  ownership and restores `disk1` to server fallback (`mode=off`).
- The qBittorrent client periodically verifies logical desired state (every 30s) or on
  torrent state changes. Detecting the fallback state drift, qBittorrent re-asserts its
  desired state with `SET`, restoring its normal indicator.

## Installation inside the VPN Gateway LXC (`192.168.178.104`)

From the repository root (or after copying `vpn_lxc.py` and `vpn-led-agent.service`):

```bash
sudo install -m 0755 server/clients/vpn_lxc.py /usr/local/bin/vpn_lxc.py
sudo install -m 0644 server/systemd/vpn-led-agent.service /etc/systemd/system/
sudo install -m 0600 /dev/null /etc/vpn-led-agent.env
sudoedit /etc/vpn-led-agent.env
```

### Environment Configuration (`/etc/vpn-led-agent.env`)

```ini
LED_HOST=192.168.178.250
LED_PORT=39001
LED_NAME=disk1
LED_PRIORITY=10
WG_INTERFACE=proton
WG_CONF=/etc/wireguard/proton.conf
POLL_INTERVAL=5
REQUEST_TIMEOUT=5
LED_HEALTH_INTERVAL=30
LED_BRIGHTNESS=200
BLINK_ON_MS=500
BLINK_OFF_MS=500
MAX_HANDSHAKE_AGE=180
```

### Service Management

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now vpn-led-agent.service
sudo journalctl -u vpn-led-agent.service -f
```

## CLI Reference

```text
usage: vpn_lxc.py [-h] [--led-host LED_HOST] [--led-port LED_PORT]
                  [--led-name {disk1..disk8}] [--priority PRIORITY]
                  [--poll-interval POLL_INTERVAL] [--timeout TIMEOUT]
                  [--health-interval HEALTH_INTERVAL] [--brightness BRIGHTNESS]
                  [--blink-on-ms BLINK_ON_MS] [--blink-off-ms BLINK_OFF_MS]
                  [--backoff-max BACKOFF_MAX] [--wg-interface WG_INTERFACE]
                  [--wg-conf WG_CONF] [--endpoint ENDPOINT]
                  [--max-handshake-age MAX_HANDSHAKE_AGE]
                  [--ipv4-urls IPV4_URLS] [--ipv6-targets IPV6_TARGETS] [-v]
```

| Flag | Environment Variable | Default | Description |
| --- | --- | --- | --- |
| `--led-host` | `LED_HOST` | `192.168.178.250` | UGREEN LED server IP |
| `--led-port` | `LED_PORT` | `39001` | UGREEN LED server TCP port |
| `--led-name` | `LED_NAME` | `disk1` | Target LED name |
| `--priority` | `LED_PRIORITY` | `10` | Command priority (rule ceiling is 10) |
| `--poll-interval` | `POLL_INTERVAL` | `5` | Check interval in seconds |
| `--timeout` | `REQUEST_TIMEOUT` | `5` | Network query timeout in seconds |
| `--health-interval` | `LED_HEALTH_INTERVAL` | `30` | State drift verification interval |
| `--brightness` | `LED_BRIGHTNESS` | `200` | Brightness (1-255) |
| `--blink-on-ms` | `BLINK_ON_MS` | `500` | Blink half-cycle on (ms) |
| `--blink-off-ms` | `BLINK_OFF_MS` | `500` | Blink half-cycle off (ms) |
| `--wg-interface` | `WG_INTERFACE` | `proton` | WireGuard interface name |
| `--wg-conf` | `WG_CONF` | `/etc/wireguard/proton.conf` | WireGuard configuration path |
| `--endpoint` | `WG_ENDPOINT` | *(auto-detected)* | Override WireGuard endpoint host/IP |
| `--max-handshake-age` | `MAX_HANDSHAKE_AGE` | `180` | Max seconds since last WireGuard handshake |
| `--ipv4-urls` | `IPV4_URLS` | `http://ifconfig.me/ip,...` | Comma-separated public IPv4 reflection services |
| `--ipv6-targets` | `IPV6_TARGETS` | `2606:4700::6810:b8f1:80,...` | Comma-separated target:port IPv6 probe endpoints |
| `-v`, `--verbose` | — | `False` | Enable debug logging |
