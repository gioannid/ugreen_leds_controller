# Jellyfin LXC LED Monitor client

## Overview

| Signal | Source | `disk3` state |
|---|---|---|
| LXC booted, Jellyfin **not yet confirmed** | Agent startup | **solid red** |
| Jellyfin **running** (first webhook received) | Jellyfin Webhook plugin | **solid green** |
| **Playback active** (≥1 session) | Jellyfin Webhook plugin | **blinking green** |
| **All playback stopped** | Jellyfin Webhook plugin | **solid green** |
| Agent **stops / crashes** | Server fallback on disconnect | **off** (configured via `[fallback]`) |

The agent (`jellyfin_lxc.py`) runs **inside the Jellyfin LXC** and connects out
to `ugreen_leds_server` over TCP.  It simultaneously acts as an HTTP server for
the Jellyfin Webhook plugin so it can react to playback events in real time.

---

## 1. `server.conf` snippets (on the NAS host)

Add an access rule for the LXC's IP and set the fallback so `disk3` turns off
when the LXC is shut down:

```ini
# Jellyfin LXC — controls disk3 only
[access.jellyfin-lxc]
source=192.168.178.XXX/32   # ← replace with your LXC's IP
leds=disk3
verbs=CAPS,GET,SET,RESET,FLUSH
priority=10
max_connections=2
```

```ini
[fallback]
# ...other LEDs...
disk3=mode=off    # applied automatically when the agent disconnects / LXC shuts down
```

---

## 2. Deployment inside the LXC

### 2.1 Install the script

```bash
# Copy the script
sudo cp jellyfin_lxc.py /usr/local/bin/jellyfin_lxc.py
sudo chmod +x /usr/local/bin/jellyfin_lxc.py

# Create a dedicated unprivileged user
sudo adduser --system --no-create-home jellyfin-led
```

### 2.2 Install the systemd service

```bash
sudo cp jellyfin-led-agent.service /etc/systemd/system/
# Edit the service to set your NAS IP if needed:
sudo nano /etc/systemd/system/jellyfin-led-agent.service

sudo systemctl daemon-reload
sudo systemctl enable --now jellyfin-led-agent
```

### 2.3 Verify

```bash
# Check agent logs
journalctl -u jellyfin-led-agent -f

# Quick health check
curl http://localhost:9090/
# → jellyfin-led-agent OK
```

---

## 3. Jellyfin Webhook plugin setup

1. In Jellyfin → **Dashboard → Plugins → Catalog** — install **Webhook**.
2. Restart Jellyfin.
3. Go to **Dashboard → Plugins → Webhook** and add a new webhook:
   - **URL**: `http://localhost:9090/webhook`
     *(use `localhost` if the agent runs in the same LXC; otherwise the LXC's
     IP as seen from Jellyfin)*
   - **Request Method**: POST
   - **Notification Type** (or send-all): at minimum enable:
     - `PlaybackStart`
     - `PlaybackStop`
     - `PlaybackPause`
     - `PlaybackResume`
   - Leave the template body as default JSON (or use the generic template).

The agent reads the `NotificationType` (or legacy `Event`) field from the JSON
body; any other fields are ignored, so any template that includes those works.

---

## 4. LED state diagram

```
LXC boots
    │
    ▼
[BOOTING] ──────────────── disk3: solid RED
    │
    │  first webhook POST received
    ▼
 [IDLE] ─────────────────── disk3: solid GREEN
    │  ▲
    │  │ last playback session stops
    │  │
    ▼  │
[PLAYING] ──────────────── disk3: blinking GREEN
         (blink 500ms on / 500ms off, configurable)

LXC shutdown / agent crash
    → server fallback releases disk3 → disk3: OFF (no active owner)
```

---

## 5. Configuration reference

All parameters can be set via CLI flags **or** environment variables:

| CLI flag | Env var | Default | Description |
|---|---|---|---|
| `--led-host` | `LED_HOST` | `192.168.178.250` | NAS IP of `ugreen_leds_server` |
| `--led-port` | `LED_PORT` | `39001` | TCP port |
| `--led-name` | `LED_NAME` | `disk3` | LED identifier |
| `--webhook-host` | `WEBHOOK_HOST` | `0.0.0.0` | Webhook listener bind address |
| `--webhook-port` | `WEBHOOK_PORT` | `9090` | Webhook listener port |
| `--blink-on-ms` | — | `500` | Playback blink on-time (ms) |
| `--blink-off-ms` | — | `500` | Playback blink off-time (ms) |
| `--brightness` | — | `200` | LED brightness (1–255) |
| `--backoff-max` | — | `30` | Max reconnect back-off (seconds) |
| `-v` / `--verbose` | — | off | Enable debug logging |

To use environment variables, create `/etc/jellyfin-led-agent.env` and
uncomment the `EnvironmentFile=` line in the service file:

```ini
LED_HOST=192.168.178.250
LED_PORT=39001
LED_NAME=disk3
WEBHOOK_PORT=9090
```

---

## 6. How the fallback handles LXC shutdown

```
┌────────────────────────────────────────────────────────────────────┐
│ Scenario                      │ disk3 owner    │ disk3 state       │
│───────────────────────────────│────────────────│───────────────────│
│ LXC booting                   │ LED agent      │ solid RED         │
│ Jellyfin running, no playback │ LED agent      │ solid GREEN       │
│ Active playback               │ LED agent      │ blinking GREEN    │
│ LXC shuts down / crashes      │ — (fallback)   │ OFF  ← automatic  │
│ LXC reboots (briefly down)    │ — (fallback)   │ OFF  ← automatic  │
│ Agent crash / restart         │ — (fallback)   │ OFF  ← automatic  │
└────────────────────────────────────────────────────────────────────┘
```

When the LXC shuts down, the OS closes all TCP sockets. The server detects
the dropped connection, releases ownership of `disk3`, and immediately
applies `[fallback] disk3=mode=off` — no polling, no delay.

When the LXC reboots and the agent reconnects, it re-acquires ownership and
immediately sets `disk3` to solid red again while Jellyfin starts up.
