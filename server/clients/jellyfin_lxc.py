#!/usr/bin/env python3
"""
jellyfin_lxc.py — UGREEN LED agent for a Jellyfin LXC container.

Behaviour
---------
  • On start (LXC up, Jellyfin not yet confirmed running):
        disk3  →  solid RED

  • Once Jellyfin is detected up (first webhook received **or** health-poll
    success — whichever comes first):
        disk3  →  solid GREEN

  • While at least one Jellyfin playback session is active:
        disk3  →  blinking green

  • Playback ends / all sessions stop:
        disk3  →  back to solid GREEN

  • On disconnect from LED server: reconnects with exponential back-off
    and re-asserts the current logical state.

Health-poll
-----------
  A background thread periodically polls Jellyfin's /health endpoint
  (falling back to /System/Info/Public) until it gets a response.  This
  is the primary mechanism for detecting Jellyfin after a restart of the
  agent when Jellyfin is already running — webhooks alone cannot signal
  this because Jellyfin only sends them on playback events.

Webhook
-------
  Listens on 0.0.0.0:WEBHOOK_PORT for HTTP POST requests from the Jellyfin
  Webhook plugin.  Configure Jellyfin's webhook URL as:
      http://<LXC-IP>:<WEBHOOK_PORT>/webhook

  The only fields consulted are "NotificationType" (or the older "Event")
  in the JSON body, so any webhook template that includes those works.

  Relevant event names (case-insensitive match):
      playback.start   / PlaybackStart
      playback.stop    / PlaybackStop
      playback.pause   / PlaybackPause   (treated as "still active")
      playback.resume  / PlaybackResume

  Unknown events are silently ignored (they still confirm Jellyfin is up).

Configuration (environment variables or command-line args — see --help):
  LED_HOST              NAS host running ugreen_leds_server (default: 192.168.178.250)
  LED_PORT              TCP port                            (default: 39001)
  LED_NAME              LED to control                      (default: disk3)
  WEBHOOK_HOST          Webhook listener bind address       (default: 0.0.0.0)
  WEBHOOK_PORT          Webhook listener port               (default: 9090)
  JELLYFIN_URL          Jellyfin base URL for health-poll   (default: http://localhost:8096)
  --health-poll-interval  Seconds between health-poll probes (default: 10)

Requirements: Python 3.8+, no third-party packages.
"""

import argparse
import json
import logging
import os
import socket
import sys
import threading
import time
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, HTTPServer
from typing import Optional

# ---------------------------------------------------------------------------
# Logging
# ---------------------------------------------------------------------------
logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(name)s: %(message)s",
    datefmt="%Y-%m-%d %H:%M:%S",
)
log = logging.getLogger("jellyfin-led")

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------
def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description="UGREEN LED agent for a Jellyfin LXC container.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    p.add_argument("--led-host",     default=os.environ.get("LED_HOST", "192.168.178.250"),
                   help="NAS IP running ugreen_leds_server")
    p.add_argument("--led-port",     default=int(os.environ.get("LED_PORT", "39001")), type=int,
                   help="ugreen_leds_server TCP port")
    p.add_argument("--led-name",     default=os.environ.get("LED_NAME", "disk3"),
                   help="LED identifier to control")
    p.add_argument("--webhook-host", default=os.environ.get("WEBHOOK_HOST", "0.0.0.0"),
                   help="Bind address for the Jellyfin webhook listener")
    p.add_argument("--webhook-port", default=int(os.environ.get("WEBHOOK_PORT", "9090")), type=int,
                   help="Port for the Jellyfin webhook listener")
    p.add_argument("--jellyfin-url", default=os.environ.get("JELLYFIN_URL", "http://localhost:8096"),
                   help="Jellyfin base URL for health-polling (used only until first confirmed-up)")
    p.add_argument("--health-poll-interval", default=10, type=int,
                   help="Seconds between Jellyfin health-poll attempts while in BOOTING state")
    p.add_argument("--blink-on-ms",  default=500, type=int,
                   help="Blink on-time in ms during playback")
    p.add_argument("--blink-off-ms", default=500, type=int,
                   help="Blink off-time in ms during playback")
    p.add_argument("--brightness",   default=200, type=int,
                   help="LED brightness (1-255)")
    p.add_argument("--backoff-max",  default=30, type=int,
                   help="Maximum reconnect back-off in seconds")
    p.add_argument("-v", "--verbose", action="store_true",
                   help="Enable debug logging")
    return p.parse_args()


# ---------------------------------------------------------------------------
# LED state machine
# ---------------------------------------------------------------------------
class LedState:
    """
    Logical states for the LED.
    """
    BOOTING   = "booting"    # LXC up, Jellyfin not confirmed → solid red
    IDLE      = "idle"       # Jellyfin running, no playback   → solid green
    PLAYING   = "playing"    # Active playback session(s)      → blink green


class LedAgent:
    """
    Maintains one persistent TCP connection to ugreen_leds_server and
    re-sends the current desired state whenever the logical state changes
    or after a reconnect.
    """

    _COLOR_RED   = "255,0,0"
    _COLOR_GREEN = "0,255,0"

    def __init__(self, host: str, port: int, led: str, cfg: argparse.Namespace):
        self._host        = host
        self._port        = port
        self._led         = led
        self._cfg         = cfg
        self._state       = LedState.BOOTING
        self._play_count  = 0          # number of active playback sessions
        self._lock        = threading.Lock()
        self._sock: Optional[socket.socket] = None
        self._file        = None       # buffered reader
        self._req_id      = 1
        self._connected   = False
        self._want_push   = threading.Event()  # set when state changes

    # ------------------------------------------------------------------
    # Public API (called from webhook thread)
    # ------------------------------------------------------------------

    def notify_jellyfin_up(self) -> None:
        """Called on the first sign of a live Jellyfin."""
        with self._lock:
            if self._state == LedState.BOOTING:
                log.info("Jellyfin is up — switching LED to idle (green)")
                self._state = LedState.IDLE
        self._want_push.set()

    def notify_playback_start(self) -> None:
        with self._lock:
            self._play_count += 1
            if self._state != LedState.PLAYING:
                log.info("Playback started (active sessions: %d) — blinking", self._play_count)
                self._state = LedState.PLAYING
        self._want_push.set()

    def notify_playback_stop(self) -> None:
        with self._lock:
            self._play_count = max(0, self._play_count - 1)
            if self._play_count == 0 and self._state == LedState.PLAYING:
                log.info("All playback stopped — switching LED to idle (green)")
                self._state = LedState.IDLE
            else:
                log.debug("Playback stopped, remaining sessions: %d", self._play_count)
        self._want_push.set()

    # ------------------------------------------------------------------
    # Internal helpers
    # ------------------------------------------------------------------

    def _next_id(self) -> int:
        rid = self._req_id
        self._req_id += 1
        return rid

    def _send(self, line: str) -> None:
        """Send one protocol line."""
        log.debug(">> %s", line)
        self._sock.sendall((line + "\n").encode("ascii"))

    def _recv(self) -> str:
        """Read one response line."""
        line = self._file.readline().rstrip("\r\n")
        log.debug("<< %s", line)
        return line

    def _expect_ok(self, rid: int) -> bool:
        """Read response; return True if OK, False on ERR."""
        resp = self._recv()
        if resp.startswith(f"OK {rid} "):
            return True
        log.warning("Unexpected response (wanted OK %d): %s", rid, resp)
        return False

    def _connect(self) -> bool:
        """
        Open a new TCP connection, do HELLO + CAPS.
        Returns True on success.
        """
        try:
            log.info("Connecting to LED server %s:%d ...", self._host, self._port)
            sock = socket.create_connection((self._host, self._port), timeout=10)
            sock.settimeout(None)          # blocking for readline
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
            self._sock  = sock
            self._file  = sock.makefile("r", encoding="ascii")
            self._req_id = 1

            # HELLO
            rid = self._next_id()
            self._send(f"HELLO {rid} 1")
            resp = self._recv()
            if not resp.startswith(f"OK {rid} HELLO"):
                log.error("HELLO rejected: %s", resp)
                return False
            log.info("Connected: %s", resp)

            # CAPS — verify our LED is present
            rid = self._next_id()
            self._send(f"CAPS {rid}")
            resp = self._recv()
            if not resp.startswith(f"OK {rid} CAPS"):
                log.error("CAPS failed: %s", resp)
                return False
            if self._led not in resp:
                log.error("LED '%s' not listed in CAPS: %s", self._led, resp)
                return False
            log.debug("CAPS ok: %s", resp)

            self._connected = True
            return True

        except OSError as exc:
            log.warning("Connection failed: %s", exc)
            return False

    def _disconnect(self) -> None:
        self._connected = False
        try:
            if self._file:
                self._file.close()
        except OSError:
            pass
        try:
            if self._sock:
                self._sock.close()
        except OSError:
            pass
        self._file = None
        self._sock = None

    def _push_state(self) -> bool:
        """
        Write current logical state to the LED server.
        Returns False if the send failed (caller should reconnect).
        """
        with self._lock:
            state = self._state

        try:
            rid = self._next_id()
            color = self._color_for(state)
            brightness = self._cfg.brightness

            if state == LedState.PLAYING:
                cmd = (
                    f"SET {rid} {self._led} mode=blink "
                    f"rgb={color} brightness={brightness} "
                    f"on_ms={self._cfg.blink_on_ms} off_ms={self._cfg.blink_off_ms}"
                )
            else:
                # BOOTING → solid red, IDLE → solid green
                cmd = (
                    f"SET {rid} {self._led} mode=on "
                    f"rgb={color} brightness={brightness}"
                )

            self._send(cmd)

            # FLUSH to make sure it hits the hardware
            flush_id = self._next_id()
            self._send(f"FLUSH {flush_id}")

            # Read SET response
            if not self._expect_ok(rid):
                return False
            # Read FLUSH response
            if not self._expect_ok(flush_id):
                return False

            log.info("LED %s -> %s (color=%s)", self._led, state, color)
            return True

        except OSError as exc:
            log.warning("Send error: %s", exc)
            return False

    @staticmethod
    def _color_for(state: str) -> str:
        if state == LedState.BOOTING:
            return LedAgent._COLOR_RED
        return LedAgent._COLOR_GREEN  # IDLE and PLAYING both use green

    # ------------------------------------------------------------------
    # Main loop (runs in its own thread)
    # ------------------------------------------------------------------

    def run(self) -> None:
        backoff = 1.0
        last_pushed: Optional[str] = None
        # Periodic re-push interval (s) — exercises the socket even when state
        # is unchanged so dead TCP connections are detected promptly.
        _HEARTBEAT_INTERVAL = 60.0

        while True:
            # Connect (with back-off)
            if not self._connect():
                log.info("Retrying in %.0fs ...", backoff)
                time.sleep(backoff)
                backoff = min(backoff * 2, self._cfg.backoff_max)
                continue

            backoff = 1.0      # reset on success

            # Push current state immediately after (re)connect
            if not self._push_state():
                self._disconnect()
                continue

            with self._lock:
                last_pushed = self._state
            last_heartbeat = time.monotonic()

            # Wait for state changes and keep pushing them
            while self._connected:
                # Wait up to 5 s for a change signal
                changed = self._want_push.wait(timeout=5.0)
                if changed:
                    self._want_push.clear()

                with self._lock:
                    current = self._state

                now = time.monotonic()
                state_changed = current != last_pushed
                heartbeat_due = (now - last_heartbeat) >= _HEARTBEAT_INTERVAL

                if state_changed or heartbeat_due:
                    if not self._push_state():
                        self._disconnect()
                        break
                    last_pushed = current
                    last_heartbeat = now

            # Socket died — reconnect after brief pause
            if not self._connected:
                log.warning("Lost connection to LED server; reconnecting ...")
                self._disconnect()
                time.sleep(1.0)


# ---------------------------------------------------------------------------
# Jellyfin webhook HTTP handler
# ---------------------------------------------------------------------------
# Each tuple contains all recognised prefixes for that category.
# Jellyfin's Webhook plugin uses camelCase names (e.g. "PlaybackStart"),
# which after lowercasing become "playbackstart" — no dots.  We include
# both the dot-separated form (for future-proofing) and the concatenated
# form that Jellyfin actually sends.
_PLAYBACK_START_PREFIXES  = ("playbackstart",  "playback.start",  "mediaplay",   "media.play")
_PLAYBACK_STOP_PREFIXES   = ("playbackstop",   "playback.stop",
                              "playbackpause",  "playback.pause",
                              "mediastop",      "media.stop",
                              "mediapause",     "media.pause")
_PLAYBACK_RESUME_PREFIXES = ("playbackresume", "playback.resume", "mediaunpause", "media.unpause")


def _normalise_event(raw: str) -> str:
    """Lowercase and strip separator characters (spaces, dots, underscores).

    This collapses both camelCase-derived and dot/underscore-separated event
    names into a single canonical form so one set of prefix checks covers all
    variants:
        PlaybackStart  ->  playbackstart
        playback.start ->  playbackstart
        Playback_Start ->  playbackstart
    """
    return raw.lower().replace(" ", "").replace(".", "").replace("_", "")


def _make_webhook_handler(agent: LedAgent):
    class WebhookHandler(BaseHTTPRequestHandler):
        server_version = "JellyfinLedAgent/1.0"
        _agent = agent

        def do_POST(self):
            # Read body
            length = int(self.headers.get("Content-Length", 0))
            raw = self.rfile.read(length) if length else b""

            # Always ACK immediately so Jellyfin doesn't retry
            self.send_response(204)
            self.end_headers()

            # Any valid POST from Jellyfin means it's alive
            self._agent.notify_jellyfin_up()

            try:
                data = json.loads(raw) if raw else {}
            except json.JSONDecodeError:
                log.debug("Webhook: non-JSON body ignored")
                return

            # Jellyfin sends "NotificationType" (plugin >=10.8) or "Event" (older)
            raw_event: str = (
                data.get("NotificationType")
                or data.get("Event")
                or data.get("Type")
                or ""
            )
            event = _normalise_event(raw_event)

            log.debug("Webhook event: %r (normalised: %r)", raw_event, event)

            if any(event.startswith(p) for p in _PLAYBACK_START_PREFIXES):
                self._agent.notify_playback_start()
            elif any(event.startswith(p) for p in _PLAYBACK_RESUME_PREFIXES):
                # Resume = playback is active again
                self._agent.notify_playback_start()
            elif any(event.startswith(p) for p in _PLAYBACK_STOP_PREFIXES):
                self._agent.notify_playback_stop()

        def do_GET(self):
            """Simple health endpoint — curl http://<LXC-IP>:<PORT>/"""
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.end_headers()
            self.wfile.write(b"jellyfin-led-agent OK\n")

        def log_message(self, fmt, *args):  # suppress default CLF logging
            log.debug("HTTP: " + fmt, *args)

    return WebhookHandler


# ---------------------------------------------------------------------------
# Jellyfin health-poll (background thread)
# ---------------------------------------------------------------------------

def _jellyfin_health_poll(agent: LedAgent, cfg: argparse.Namespace) -> None:
    """
    Periodically polls the Jellyfin HTTP health endpoint until Jellyfin
    responds with a 2xx status.  Once confirmed alive, calls
    ``agent.notify_jellyfin_up()`` and exits — the agent will stay in
    IDLE/PLAYING from that point on and webhooks take over.

    This covers the common scenario where Jellyfin was already running
    when the agent (re)started, so no initial webhook is ever fired.
    """
    base = cfg.jellyfin_url.rstrip("/")
    interval = cfg.health_poll_interval

    # Try the modern /health endpoint first; fall back to the public info
    # endpoint which has been present since Jellyfin 10.x.
    endpoints = [f"{base}/health", f"{base}/System/Info/Public"]

    log.info("Health-poll: will probe Jellyfin at %s every %ds until it responds",
             base, interval)

    while True:
        # Once we leave BOOTING the poll thread is no longer needed.
        with agent._lock:  # noqa: SLF001  (intentional internal access)
            if agent._state != LedState.BOOTING:  # noqa: SLF001
                log.debug("Health-poll: Jellyfin already confirmed up — exiting poll thread")
                return

        for url in endpoints:
            try:
                with urllib.request.urlopen(url, timeout=5) as resp:
                    if resp.status < 300:
                        log.info("Health-poll: Jellyfin responded on %s (HTTP %d) — marking up",
                                 url, resp.status)
                        agent.notify_jellyfin_up()
                        return
            except urllib.error.HTTPError as exc:
                # Any HTTP error (e.g. 401 Unauthorized) still means the
                # server is alive and listening.
                log.info("Health-poll: Jellyfin responded on %s (HTTP %d) — marking up",
                         url, exc.code)
                agent.notify_jellyfin_up()
                return
            except (urllib.error.URLError, OSError) as exc:
                log.debug("Health-poll: %s unreachable (%s)", url, exc)

        time.sleep(interval)



def main() -> None:
    cfg = parse_args()
    if cfg.verbose:
        logging.getLogger().setLevel(logging.DEBUG)

    log.info("Starting Jellyfin LED agent")
    log.info("  LED server : %s:%d", cfg.led_host, cfg.led_port)
    log.info("  LED name   : %s", cfg.led_name)
    log.info("  Webhook    : %s:%d", cfg.webhook_host, cfg.webhook_port)
    log.info("  Jellyfin   : %s (health-poll every %ds)", cfg.jellyfin_url, cfg.health_poll_interval)

    agent = LedAgent(cfg.led_host, cfg.led_port, cfg.led_name, cfg)

    # LED agent runs in a background daemon thread
    agent_thread = threading.Thread(target=agent.run, name="led-agent", daemon=True)
    agent_thread.start()

    # Health-poll thread: detects Jellyfin coming up independently of webhooks
    poll_thread = threading.Thread(
        target=_jellyfin_health_poll, args=(agent, cfg),
        name="jellyfin-health-poll", daemon=True,
    )
    poll_thread.start()

    # Webhook server runs in the main thread
    handler_cls = _make_webhook_handler(agent)
    httpd = HTTPServer((cfg.webhook_host, cfg.webhook_port), handler_cls)
    log.info("Webhook server listening on %s:%d", cfg.webhook_host, cfg.webhook_port)

    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        log.info("Shutting down ...")
        httpd.shutdown()
        sys.exit(0)


if __name__ == "__main__":
    main()
