#!/usr/bin/env python3
"""qBittorrent LXC status agent for UGREEN LED Protocol v1 (stdlib only)."""
import argparse
import http.cookiejar
import json
import logging
import math
import os
import random
import signal
import socket
import threading
import time
import urllib.error
import urllib.parse
import urllib.request

LOG = logging.getLogger("qbittorrent-led")
DOWNLOAD_STATES = frozenset(("downloading", "forcedDL", "metaDL", "forcedMetaDL"))
SEED_STATES = frozenset(("uploading", "forcedUP"))
API_LOG_LIMIT = 8192
SECRET_KEYS = frozenset(("password", "passwd", "username", "cookie", "set-cookie",
                         "authorization", "sid", "token", "access_token", "api_key"))


def classify(torrents):
    if not isinstance(torrents, list) or any(
        not isinstance(t, dict) or not isinstance(t.get("state"), str) for t in torrents
    ):
        raise ValueError("invalid torrents/info response")
    if not torrents:
        return "empty"
    states = {t["state"] for t in torrents}
    if states & DOWNLOAD_STATES:
        return "downloading"
    if states & SEED_STATES:
        return "seeding"
    return "stalled"


class ApiError(RuntimeError):
    pass


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


class QBittorrent:
    def __init__(self, url, username, password, timeout):
        parts = urllib.parse.urlsplit(url)
        if parts.scheme not in ("http", "https") or not parts.hostname:
            raise ValueError("QB_URL must be an absolute http(s) URL")
        if parts.username is not None or parts.password is not None:
            raise ValueError("use QB_USERNAME/QB_PASSWORD, not credentials in QB_URL")
        if parts.query or parts.fragment:
            raise ValueError("QB_URL cannot contain a query or fragment")
        self.url = url.rstrip("/")
        self.username, self.password, self.timeout = username, password, timeout
        self.cookies = http.cookiejar.CookieJar()
        self.opener = urllib.request.build_opener(
            urllib.request.ProxyHandler({}),
            urllib.request.HTTPCookieProcessor(self.cookies), NoRedirect(),
        )

    def redact_text(self, text):
        for secret in (self.password, self.username):
            if secret:
                for value in {secret, urllib.parse.quote(secret, safe=""),
                              urllib.parse.quote_plus(secret)}:
                    text = text.replace(value, "<redacted>")
        for cookie in self.cookies:
            if cookie.value:
                text = text.replace(cookie.value, "<redacted>")
        return text

    def log_body(self, method, raw):
        if method.startswith("auth/"):
            return "<redacted authentication body>"
        def clean(value):
            if isinstance(value, dict):
                return {k: "<redacted>" if k.lower() in SECRET_KEYS else clean(v)
                        for k, v in value.items()}
            if isinstance(value, list):
                return [clean(v) for v in value]
            return self.redact_text(value) if isinstance(value, str) else value
        try:
            text = json.dumps(clean(json.loads(raw)), ensure_ascii=False)
        except (ValueError, UnicodeError):
            # Unexpected HTML/plaintext may echo credentials or cookies.
            return "<non-JSON body omitted>"
        text = self.redact_text(text)
        if len(text) > API_LOG_LIMIT:
            text = text[:API_LOG_LIMIT] + "... <truncated>"
        return text

    def log_response(self, method, response, raw, started):
        if not LOG.isEnabledFor(logging.DEBUG):
            return
        # Cookie and Authorization headers are never emitted; SID values are
        # intentionally omitted even on authentication failure.
        LOG.debug("QB API <- status=%s elapsed_ms=%.1f headers=%r bytes=%d body=%r",
                  response.code, (time.monotonic() - started) * 1000,
                  {"Content-Type": self.redact_text(response.headers.get("Content-Type", "")),
                   "Set-Cookie": "<redacted>" if response.headers.get("Set-Cookie") else "<absent>"},
                  len(raw), self.log_body(method, raw))

    def request(self, method, data=None):
        req = urllib.request.Request(
            self.url + "/api/v2/" + method, data=data,
            headers={"Referer": self.url + "/", "User-Agent": "ugreen-qbittorrent-led/2"},
        )
        started = time.monotonic()
        if LOG.isEnabledFor(logging.DEBUG):
            LOG.debug("QB API -> %s %s headers=%r body=%r", req.get_method(),
                      self.redact_text(req.full_url),
                      {"Referer": self.redact_text(self.url + "/"),
                       "Cookie": "<redacted>" if any(self.cookies) else "<absent>"},
                      "<redacted authentication form>" if data is not None else "<empty>")
        try:
            with self.opener.open(req, timeout=self.timeout) as response:
                raw = response.read(16 * 1024 * 1024 + 1)
                self.log_response(method, response, raw, started)
        except urllib.error.HTTPError as exc:
            try:
                # Only inspect HTTP error bodies for opt-in troubleshooting.
                if LOG.isEnabledFor(logging.DEBUG):
                    raw = exc.read(API_LOG_LIMIT + 1)
                    self.log_response(method, exc, raw, started)
            finally:
                exc.close()
            raise
        except OSError as exc:
            LOG.debug("QB API <- transport_error=%s elapsed_ms=%.1f",
                      type(exc).__name__, (time.monotonic() - started) * 1000)
            raise
        if len(raw) > 16 * 1024 * 1024:
            raise ApiError("API response exceeds 16 MiB")
        return raw

    def torrents(self):
        try:
            raw = self.request("torrents/info")
        except urllib.error.HTTPError as exc:
            if exc.code != 403:
                raise ApiError("API HTTP status %d" % exc.code) from None
            if not self.username or not self.password:
                raise ApiError("API requires authentication; set QB_USERNAME and QB_PASSWORD")
            payload = urllib.parse.urlencode({
                "username": self.username, "password": self.password,
            }).encode("utf-8")
            if self.request("auth/login", payload).strip() != b"Ok.":
                raise ApiError("API login rejected (check credentials and IP ban settings)")
            raw = self.request("torrents/info")
        return json.loads(raw)


class LedError(RuntimeError):
    pass


class LedConnection:
    def __init__(self, host, port, led, timeout):
        self.host, self.port, self.led, self.timeout = host, port, led, timeout
        self.sock, self.reader = None, None
        self.rid = 0
        self.last_applied = None
        self.last_checked = 0.0

    def close(self):
        self.last_applied = None
        self.last_checked = 0.0
        if self.reader is not None:
            self.reader.close()
            self.reader = None
        if self.sock is not None:
            self.sock.close()
            self.sock = None

    def command(self, verb, args="", expected=None):
        self.rid = self.rid % (2**64 - 1) + 1
        self.sock.sendall(("%s %d%s\n" % (
            verb, self.rid, " " + args if args else ""
        )).encode("ascii"))
        raw = self.reader.readline(1026)
        if not raw or not raw.endswith(b"\n") or len(raw.rstrip(b"\r\n")) > 1024:
            raise LedError("closed connection or invalid response framing")
        tokens = raw.decode("ascii").strip().split()
        if len(tokens) >= 3 and tokens[0] == "ERR":
            if tokens[1] not in (str(self.rid), "0"):
                raise LedError("response request-id mismatch")
            raise LedError(" ".join(tokens))
        if tokens[:3] != ["OK", str(self.rid), expected or verb]:
            raise LedError("unexpected response: " + " ".join(tokens))
        return dict(token.split("=", 1) for token in tokens[3:] if "=" in token)

    def connect(self):
        self.close()
        try:
            self.sock = socket.create_connection((self.host, self.port), self.timeout)
            self.sock.settimeout(self.timeout)
            self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
            self.reader = self.sock.makefile("rb")
            self.rid = 0
            hello = self.command("HELLO", "1")
            if hello.get("version") != "1":
                raise LedError("unsupported protocol version")
            caps = self.command("CAPS")
            if self.led not in caps.get("leds", "").split(","):
                raise LedError("requested LED is not in CAPS")
            if not {"on", "blink"}.issubset(caps.get("modes", "").split(",")):
                raise LedError("required LED modes are not available")
        except Exception:
            self.close()
            raise

    @staticmethod
    def target(state, brightness, on_ms, off_ms):
        rgb = {"empty": "0,255,0", "unavailable": "255,0,0",
               "stalled": "255,128,0", "seeding": "0,0,255",
               "downloading": "0,255,0"}[state]
        return {"mode": "blink" if state == "downloading" else "on", "rgb": rgb,
                "brightness": str(brightness), "on_ms": str(on_ms), "off_ms": str(off_ms)}

    def update(self, state, brightness, on_ms, off_ms, health_interval):
        target = self.target(state, brightness, on_ms, off_ms)
        if self.last_applied == target and time.monotonic() - self.last_checked >= health_interval:
            observed = self.command("GET", self.led + " source=desired")
            self.last_checked = time.monotonic()
            if observed.get("status") != "valid" or any(observed.get(k) != v for k, v in target.items()):
                LOG.debug("LED desired-state drift detected; invalidating state cache")
                self.last_applied = None
        if self.last_applied == target:
            return False
        args = self.led + " " + " ".join("%s=%s" % item for item in target.items())
        self.command("SET", args, expected="ACCEPTED")
        self.command("FLUSH")
        # Only acknowledge the cached state after BOTH commands succeed.
        self.last_applied = target
        self.last_checked = time.monotonic()
        return True


def parse_args(argv=None):
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.ArgumentDefaultsHelpFormatter)
    p.add_argument("--led-host", default=os.getenv("LED_HOST", "192.168.178.250"))
    p.add_argument("--led-port", type=int, default=os.getenv("LED_PORT", "39001"))
    p.add_argument("--led-name", choices=["disk%d" % i for i in range(1, 9)],
                   default=os.getenv("LED_NAME", "disk1"))
    p.add_argument("--qb-url", default=os.getenv("QB_URL", "http://127.0.0.1:8080"))
    p.add_argument("--poll-interval", type=float, default=os.getenv("POLL_INTERVAL", "5"))
    p.add_argument("--timeout", type=float, default=os.getenv("REQUEST_TIMEOUT", "5"))
    p.add_argument("--health-interval", type=float, default=os.getenv("LED_HEALTH_INTERVAL", "30"))
    p.add_argument("--brightness", type=int, default=os.getenv("LED_BRIGHTNESS", "200"))
    p.add_argument("--blink-on-ms", type=int, default=os.getenv("BLINK_ON_MS", "500"))
    p.add_argument("--blink-off-ms", type=int, default=os.getenv("BLINK_OFF_MS", "500"))
    p.add_argument("--backoff-max", type=float, default=os.getenv("BACKOFF_MAX", "30"))
    p.add_argument("-v", "--verbose", action="store_true")
    cfg = p.parse_args(argv)
    if not 1 <= cfg.led_port <= 65535 or not 1 <= cfg.brightness <= 255:
        p.error("invalid LED port or brightness")
    if cfg.led_name not in ["disk%d" % i for i in range(1, 9)]:
        p.error("invalid LED name")
    if any(not math.isfinite(x) or x < 1 for x in (
        cfg.poll_interval, cfg.timeout, cfg.backoff_max, cfg.health_interval
    )):
        p.error("intervals, timeout and backoff maximum must be finite and >= 1")
    if min(cfg.blink_on_ms, cfg.blink_off_ms) < 1 or cfg.blink_on_ms + cfg.blink_off_ms > 65535:
        p.error("blink half-cycles must be positive and their sum <= 65535")
    return cfg


def run(cfg, stop):
    qb = QBittorrent(cfg.qb_url, os.getenv("QB_USERNAME", ""),
                     os.getenv("QB_PASSWORD", ""), cfg.timeout)
    led = LedConnection(cfg.led_host, cfg.led_port, cfg.led_name, cfg.timeout)
    previous, api_problem = None, None
    backoff, retry_at = 1.0, 0.0
    try:
        while not stop.is_set():
            started = time.monotonic()
            try:
                state = classify(qb.torrents())
                if api_problem is not None:
                    LOG.info("qBittorrent API recovered")
                api_problem = None
            except (OSError, ValueError, ApiError) as exc:
                state = "unavailable"
                problem = type(exc).__name__
                if problem != api_problem:
                    LOG.warning("qBittorrent API unavailable (%s); showing red; "
                                "check Web UI, credentials and port", problem)
                api_problem = problem
            if stop.is_set():
                break
            if state != previous:
                LOG.info("%s -> %s", cfg.led_name, state)
                previous = state
            if time.monotonic() >= retry_at:
                try:
                    if led.sock is None:
                        led.connect()
                        LOG.info("Connected to LED server")
                    # update() compares the last successfully applied target,
                    # not previous (which records observed states only).
                    led.update(state, cfg.brightness, cfg.blink_on_ms,
                               cfg.blink_off_ms, cfg.health_interval)
                    backoff, retry_at = 1.0, 0.0
                except (OSError, ValueError, LedError) as exc:
                    LOG.warning("LED update failed: %s", exc)
                    led.close()
                    delay = min(cfg.backoff_max, backoff * random.uniform(0.8, 1.2))
                    retry_at = time.monotonic() + delay
                    backoff = min(cfg.backoff_max, backoff * 2)
            stop.wait(max(0, cfg.poll_interval - (time.monotonic() - started)))
    finally:
        led.close()
        LOG.info("LED connection closed; server fallback applies")


def main():
    cfg = parse_args()
    logging.basicConfig(level=logging.DEBUG if cfg.verbose else logging.INFO,
                        format="%(asctime)s [%(levelname)s] %(message)s")
    stop = threading.Event()
    for sig in (signal.SIGTERM, signal.SIGINT):
        signal.signal(sig, lambda signum, frame: stop.set())
    try:
        run(cfg, stop)
    except ValueError as exc:
        LOG.error("Configuration error: %s", exc)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
