# qBittorrent LXC LED monitor

Runs inside the qBittorrent LXC (`192.168.178.253`), polling the local Web API v2
and maintaining a persistent connection to the NAS LED server. Python 3.8+,
standard library only. No root privileges or torrent-directory access needed.

## Behavior

| Condition | disk1 |
| --- | --- |
| LXC / agent disconnected, once detected by server | Off (server fallback) |
| qBittorrent running with zero torrents | Solid green |
| Active download | Blinking green, 500 ms on / 500 ms off |
| No active downloads, active upload | Solid blue |
| Torrents present, no active downloads/uploads | Solid orange |
| qBittorrent/API unavailable or login/response failure | Solid red |

Precedence: downloading > seeding > stalled; an empty list is red.
Downloading includes `downloading`, `forcedDL`, `metaDL`, `forcedMetaDL`.
Seeding includes `uploading`, `forcedUP`. `stalledUP` is blue, not green.
Other states (paused/stopped, queued, checking, moving, missing files, error,
unknown) are blue when no active transfer takes precedence.

## Changes in this revision

### Send only changed LED targets

The previous version sent SET/FLUSH every API poll. This version caches the
last successfully applied LED target (mode, RGB, brightness and timings).
It sends SET/FLUSH only on a target change, on a fresh connection, or after
a read-only health check finds drift. The cache is committed only after
both SET and FLUSH succeed; a failure cannot incorrectly suppress a retry.
The cache is cleared on close/reconnect, so unchanged qBittorrent state is
still reapplied after LED-server restart. `previous` separately tracks the
last observed qBittorrent classification for transition logging: comparing
only that value would miss reconnects and failed updates.

An empty queue and unavailable API both map to identical red targets, so
switching between those conditions updates the logs but does not write LEDs.
No SET or FLUSH is sent for an unchanged target on an unchanged connection.

Every 30 seconds of unchanged state, GET disk1 source=desired checks connection
health and desired-state drift. This is a read-only logical query, not a
hardware read or an ownership renewal. The server protocol has no PING.
Override with `--health-interval` / `LED_HEALTH_INTERVAL` (seconds, >= 1).
No GET is needed when a transition is already sending SET/FLUSH.
A mismatched/invalid desired state clears the cache and retries the target;
a protocol/transport failure closes and reconnects with backoff.

GET does not expose ownership. If another connection preempts disk1 with the
same target, that ownership change is invisible until its target changes,
it releases to fallback, or this agent changes state. A priority rejection
uses the normal reconnect/backoff path. Dedicate disk1 to this agent unless
you deliberately use priority-based overrides.

### Opt-in API exchange logging

`--verbose` / `-v` enables DEBUG logging of:

- Request method and URL; Referer and whether a Cookie is present.
- Login attempts (form content always redacted).
- Response status, elapsed milliseconds, Content-Type, Set-Cookie presence,
  byte count and JSON response body.
- HTTP errors, transport-error type, and LED desired-state drift.

Cookie/Set-Cookie values, credentials, authentication bodies, and common JSON
secret fields are redacted. Unexpected non-JSON bodies are omitted. JSON
bodies are truncated after 8192 characters. Response contents are logged only
at DEBUG; INFO mode does not generate exchange-body previews.

Verbose torrent responses may reveal torrent names, hashes, tracker URLs,
and filesystem paths. Redaction is best-effort, not an exhaustive data-loss
prevention mechanism. Enable only while troubleshooting and protect logs.
Never share an unreviewed verbose journal publicly.

## NAS configuration

Merge the following access rule into the LED server configuration:

```ini
[access.qbittorrent-lxc]
source=192.168.178.253/32
leds=disk1
verbs=CAPS,GET,SET,RESET,FLUSH
priority=10
max_connections=2
```

Merge this into the existing fallback section (do not duplicate sections):

```ini
[fallback]
disk1=mode=off
```

The server must bind a LAN-reachable address, e.g. `192.168.178.250:39001`,
with the hardware backend enabled. Restart `ugreen-leds-server.service` after
configuration changes. Source-IP authorization uses the address observed by
the NAS: avoid NAT that hides `.253`. Permit TCP 39001 from this LXC. The LED
protocol is not encrypted; use a trusted network. GET must be allowed for
the new read-only health check.

The NAS address `.250` retains the existing Jellyfin agent's default; change
LED_HOST if your server is elsewhere. qBittorrent is polled at
`http://127.0.0.1:8080` inside the LXC, not on the NAS. Its Web UI must be
listening there, or set QB_URL to its reachable address/port.

## First installation inside the LXC

From the repository root, after copying/extracting the files:

```bash
sudo apt-get install python3
getent passwd qbittorrent-led >/dev/null || \
  sudo adduser --system --group --no-create-home qbittorrent-led
sudo install -m 0755 server/clients/qbittorrent_lxc.py /usr/local/bin/qbittorrent_lxc.py
sudo install -m 0644 server/systemd/qbittorrent-led-agent.service /etc/systemd/system/
sudo install -m 0600 /dev/null /etc/qbittorrent-led-agent.env
sudoedit /etc/qbittorrent-led-agent.env
```

The /dev/null install is for first installation only; it erases an existing
configuration if repeated. Environment file example:

```ini
LED_HOST=192.168.178.250
LED_PORT=39001
LED_NAME=disk1
QB_URL=http://127.0.0.1:8080
QB_USERNAME=admin
QB_PASSWORD="replace-with-your-Web-UI-password"
POLL_INTERVAL=5
LED_HEALTH_INTERVAL=30
```

Use systemd EnvironmentFile quoting, not shell export statements. The
root-owned 0600 file is loaded by systemd before dropping privileges; local
root can still inspect process environment variables. Use your configured
Web UI credentials, not an assumed default. Cookie-based login retries once
after HTTP 403 and automatically obtains a new SID after session expiry.
An existing localhost authentication bypass works but is not required.
Wrong credentials can trigger qBittorrent's IP ban. Ambient HTTP proxies are
disabled, redirects refused, Referer set, and HTTPS certificates validated.
QB_URL may include a reverse-proxy base path, but not `/api/v2`, embedded
credentials, a query, or fragment.

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now qbittorrent-led-agent.service
sudo journalctl -u qbittorrent-led-agent.service -f
```

The service deliberately does not depend on the qBittorrent service: it must
stay alive to show red when qBittorrent stops. No torrent modification occurs.

## Upgrade an existing installation

Replace the Python file and restart; no unit or environment changes required:

```bash
sudo install -m 0755 server/clients/qbittorrent_lxc.py /usr/local/bin/qbittorrent_lxc.py
sudo systemctl restart qbittorrent-led-agent.service
```

## Enable verbose logging under systemd

```bash
sudo systemctl edit qbittorrent-led-agent.service
```

Add:

```ini
[Service]
ExecStart=
ExecStart=/usr/bin/python3 /usr/local/bin/qbittorrent_lxc.py --verbose
```

Then:

```bash
sudo systemctl daemon-reload
sudo systemctl restart qbittorrent-led-agent.service
sudo journalctl -u qbittorrent-led-agent.service -f
```

To disable, remove just this ExecStart override, daemon-reload and restart.
Do not add a second foreground agent while the service owns disk1: equal
priority cannot preempt. For foreground troubleshooting stop the service
first and supply credentials through the environment, not command-line flags.
The existing EnvironmentFile is not sourced automatically by foreground Python.

## Options

CLI flags override environment settings. Credentials are environment-only.

| Flag | Environment | Default |
| --- | --- | --- |
| --led-host | LED_HOST | 192.168.178.250 |
| --led-port | LED_PORT | 39001 |
| --led-name | LED_NAME | disk1 |
| --qb-url | QB_URL | http://127.0.0.1:8080 |
| none | QB_USERNAME / QB_PASSWORD | empty |
| --poll-interval | POLL_INTERVAL | 5 seconds |
| --timeout | REQUEST_TIMEOUT | 5 seconds per socket operation |
| --health-interval | LED_HEALTH_INTERVAL | 30 seconds |
| --brightness | LED_BRIGHTNESS | 200 (1..255) |
| --blink-on-ms | BLINK_ON_MS | 500 |
| --blink-off-ms | BLINK_OFF_MS | 500 |
| --backoff-max | BACKOFF_MAX | 30 seconds |
| --verbose / -v | none | off |

Intervals/timeout/backoff must be finite and >= 1 second. Blink half-cycles
must be positive and their sum <= 65535 ms. Increase TimeoutStopSec if
increasing REQUEST_TIMEOUT; stop waits for bounded in-flight operations.

## Shutdown and failure limits

HELLO/CAPS run on each new connection. SET and FLUSH replies are correlated
with request IDs and read with timeouts. Reconnect uses exponential backoff
and jitter, with retries rounded to polling ticks. SIGTERM/SIGINT close the
connection in a finally block so the server restores disk1's off fallback.

Off means the server detected the owning agent's disconnect, not that the
LXC's power state was independently verified. A normal LXC shutdown closes
TCP; an agent crash can also yield off while the LXC is up. Frozen containers
and silent network blackholes may leave the connection open on the server:
protocol v1 has no ownership lease/expiry, so an inside-LXC agent cannot
promise bounded time-to-off for those failures. A server-side lease or
host-side watchdog is required for that guarantee.

## Verification

1. Empty queue: red. Only stalled torrents: blue.
2. Active upload and no download: green. Any active download: blinking green.
3. Keep the state unchanged: no additional SET/FLUSH; one GET per health interval.
4. Stop/restart the LED server without changing torrents: current target reapplies.
5. Stop qBittorrent: red/unavailable; restart restores the current state.
6. Stop the agent normally: NAS fallback off.
7. Enable --verbose: inspect requests/status/JSON; credentials and SID stay redacted.

This revision passed 20 automated tests using mocks/local HTTP fixtures,
including deduplication, reconnect reapplication, failure retries, health
checks, authentication, logging and redaction. It has not been live-tested
against the NAS hardware or your qBittorrent instance.

## References

- Repository server/PROTOCOL.md and server/examples/ugreen-leds-server.conf.
- https://github.com/qbittorrent/qBittorrent/wiki/WebUI-API-(qBittorrent-4.1)
