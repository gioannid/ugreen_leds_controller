# PVE host power LED agent

`pve_host.py` runs on the PVE host, not in a VM or LXC. Python 3.8+,
standard library only; no psutil, PVE API token, root, or I2C access required.
Only the LED server accesses hardware.

## Mapping

Every 2 seconds (configurable), aggregate CPU utilization drives linear RGB
and brightness interpolation with t = CPU% / 100:

    RGB = round(cold + (hot - cold) * t), per channel
    brightness = round(minimum + (maximum - minimum) * t)

Default endpoints deliberately retain all three channels: cool white
(200,220,255), brightness 80, through midpoint (228,202,200), brightness 150,
to warm/reddish white (255,185,145), brightness 220. These are RGB design
choices, not calibrated Kelvin temperatures or perceptually linear colors.
Actual appearance depends on the panel; customize endpoints after testing.
Setting equal brightness endpoints disables brightness modulation.

CPU is aggregate busy time across all logical CPUs, not Linux load average.
It uses deltas of the first eight /proc/stat CPU counters, excludes idle and
iowait from busy time, and does not double-count guest/guest_nice.
Guest VM execution remains included via user/nice. Invalid/reset counters
cause a skipped sample and a fresh baseline. No smoothing is applied.
The first real reading needs one sampling interval.

Memory percentage is 100 * (MemTotal - MemAvailable) / MemTotal, excluding
available/reclaimable memory rather than treating all cache as used. It is
host-wide RAM, not guest configured allocations, swap usage, or PSI.
ZFS ARC and kernel accounting may make this differ from a PVE dashboard;
this is not a dedicated ZFS memory-pressure detector.

Memory > 85% selects hardware blink (500 ms on / 500 ms off); otherwise
solid. CPU still controls color and on-phase brightness while blinking.
Default hysteresis is zero, so equality is solid. Optional hysteresis H
keeps blinking until memory <= threshold - H, reducing threshold chatter.

## Installation

From the repository root on the PVE host:

```sh
install -m 0755 server/clients/pve_host.py /usr/local/bin/pve_host.py
install -m 0644 server/systemd/pve-led-agent.service /etc/systemd/system/
cat >/etc/pve-led-agent.env <<'EOF'
LED_HOST=127.0.0.1
LED_PORT=39001
PRIORITY=20
POLL_INTERVAL=2
MEMORY_THRESHOLD=85
MEMORY_HYSTERESIS=0
COLD_RGB=200,220,255
HOT_RGB=255,185,145
BRIGHTNESS_MIN=80
BRIGHTNESS_MAX=220
BLINK_ON_MS=500
BLINK_OFF_MS=500
EOF
systemctl daemon-reload
systemctl enable --now pve-led-agent.service
journalctl -u pve-led-agent.service -f
```

The server must already be installed/running. For a remote server, change
LED_HOST and authorize the PVE host's source IP. TCP loopback is the default
because the server's supplied unit uses PrivateTmp and its default Unix
socket lives under /tmp. A private /tmp socket is not shared across units.
For Unix transport, set LED_SOCKET to a shared, accessible path outside
/tmp, and configure matching server socket permissions; DynamicUser may need
a local account/group override. Empty LED_SOCKET selects TCP.

The server access rule must allow power and HELLO/CAPS/SET/FLUSH. Priority
20 must be allowed by the matching rule. Smaller numbers are higher priority;
do not override emergency agents unintentionally. Equal priority cannot
preempt an existing owner. No protocol authentication/encryption exists:
keep TCP local or restrict it with server policy and firewall.

## Options and configuration

Run `python3 server/clients/pve_host.py --help` for all options and defaults.
Each value option also supports its uppercase underscore environment name,
e.g. --memory-threshold / MEMORY_THRESHOLD, --priority / PRIORITY,
--test-cpu / TEST_CPU. CLI wins over environment. Environment changes require
`systemctl restart pve-led-agent.service`. Boolean --dry-run and --verbose
are CLI-only. Intervals must be finite and >= 0.1 seconds; a 2-second poll
is the default balance between responsiveness and unnecessary updates.

Other defaults: TIMEOUT=3, BACKOFF_MAX=30, HEALTH_INTERVAL=30.
Brightness must satisfy 1 <= minimum <= maximum <= 255. Blink half-cycles
must be positive with sum <= 65535 ms. Test percentages and thresholds must
be finite and within 0..100.

## Safe testing

Stop the service first so two equal-priority connections do not compete:

```sh
systemctl stop pve-led-agent.service
# Mapping only: no socket or hardware access.
python3 server/clients/pve_host.py --dry-run --test-cpu 50 --test-memory 40
# Actual idle cool-white, solid. Ctrl-C releases ownership.
python3 server/clients/pve_host.py --test-cpu 0 --test-memory 40
# Actual full-load warm-white, blinking; no real host stress.
python3 server/clients/pve_host.py --test-cpu 100 --test-memory 90 --poll-interval 0.5
# Strict boundary: 85 solid, 85.1 blinking with default threshold/hysteresis.
python3 server/clients/pve_host.py --test-cpu 50 --test-memory 85
systemctl start pve-led-agent.service
```

Each foreground command runs until Ctrl-C; run them separately. Try CPU 0,
25, 50, 75 and 100 to tune the white endpoints to your panel. Overrides may
be used independently; unoverridden metrics remain live. No test allocates
memory or generates artificial CPU work.

## Protocol and failure behavior

One persistent connection: HELLO v1, CAPS validation, complete power SET,
then FLUSH after every change. Hardware blinking does not require software
on/off toggling. Unchanged states are not resent until HEALTH_INTERVAL;
periodic full SET/FLUSH detects disconnects and attempts to regain ownership
after an override. All responses are read and request IDs validated. FLUSH
confirms writes, not hardware readback. A higher-priority owner can preempt
at any time. On ERR (including rejection), timeout, or disconnect, close
and reconnect with capped exponential backoff/jitter, then resend the latest
complete state; obsolete samples are never queued. Retry scheduling is
poll-quantized. After restart, re-read CAPS. Persistent policy errors are
logged and retried at the backoff cap, so fix server access rules rather
than expecting automatic authorization.

SIGINT/SIGTERM closes the connection; server fallback applies, not the
pre-agent state. Unexpected statistics failures exit nonzero for systemd
restart. Socket operations are timeout-bounded. This client does not modify
other LEDs, PVE settings, or host workload.

## Validation

The accompanying test file covers mapping endpoints, memory boundaries,
hysteresis, CPU guest accounting, argument validation, and a loopback fake
Protocol v1 server exchange. Run:

```sh
python3 -m unittest discover -s server/clients -p test_pve_host.py -v
```

Hardware appearance and deployment on a real PVE host still require testing.
