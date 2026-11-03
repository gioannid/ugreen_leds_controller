#!/usr/bin/env python3
"""PVE host CPU/memory indicator for UGREEN LED Protocol v1 (stdlib only)."""
import argparse
import logging
import math
import os
import random
import signal
import socket
import threading
import time

LOG = logging.getLogger("pve-led")


def cpu_snapshot():
    with open("/proc/stat", encoding="ascii") as f:
        fields = f.readline().split()
    if fields[0] != "cpu" or len(fields) < 9:
        raise ValueError("Invalid aggregate /proc/stat counters")
    # guest/guest_nice are already included in user/nice: use first eight only.
    values = tuple(map(int, fields[1:9]))
    return values


def cpu_percent(previous, current):
    delta = [b - a for a, b in zip(previous, current)]
    if any(x < 0 for x in delta) or sum(delta) <= 0:
        return None  # counter reset/hotplug: rebaseline rather than invent load
    return 100.0 * (sum(delta) - delta[3] - delta[4]) / sum(delta)


def memory_percent():
    with open("/proc/meminfo", encoding="ascii") as f:
        values = {k: int(v.split()[0]) for k, v in
                  (line.split(":", 1) for line in f)}
    total, available = values["MemTotal"], values["MemAvailable"]
    if total <= 0 or not 0 <= available <= total:
        raise ValueError("Invalid MemTotal/MemAvailable")
    return 100.0 * (total - available) / total


def rgb(value):
    try:
        parts = tuple(map(int, value.split(",")))
        if len(parts) != 3 or any(not 0 <= x <= 255 for x in parts) or not any(parts):
            raise ValueError()
        return parts
    except ValueError:
        raise argparse.ArgumentTypeError("RGB must be R,G,B (0..255), not all zero")


def target(cfg, cpu, memory, blinking=False):
    fraction = max(0.0, min(1.0, cpu / 100.0))
    color = tuple(round(a + (b - a) * fraction)
                  for a, b in zip(cfg.cold_rgb, cfg.hot_rgb))
    brightness = round(cfg.brightness_min +
                       (cfg.brightness_max - cfg.brightness_min) * fraction)
    if blinking:
        blinking = memory > cfg.memory_threshold - cfg.memory_hysteresis
    else:
        blinking = memory > cfg.memory_threshold
    return {"mode": "blink" if blinking else "on",
            "rgb": ",".join(map(str, color)), "brightness": str(brightness),
            "on_ms": str(cfg.blink_on_ms), "off_ms": str(cfg.blink_off_ms)}, blinking


class LedError(RuntimeError):
    pass


class Connection:
    def __init__(self, cfg):
        self.cfg, self.sock, self.reader, self.rid = cfg, None, None, 0

    def close(self):
        if self.reader is not None:
            self.reader.close()
            self.reader = None
        if self.sock is not None:
            self.sock.close()
            self.sock = None

    def command(self, verb, args="", expected=None):
        self.rid = self.rid % (2**64 - 1) + 1
        self.sock.sendall(f"{verb} {self.rid} {args}\n".encode("ascii"))
        raw = self.reader.readline(1026)
        if not raw.endswith(b"\n") or len(raw.rstrip(b"\r\n")) > 1024:
            raise LedError("EOF or invalid response framing")
        tokens = raw.decode("ascii").split()
        if len(tokens) >= 3 and tokens[0] == "ERR":
            raise LedError(" ".join(tokens))
        if tokens[:3] != ["OK", str(self.rid), expected or verb]:
            raise LedError(f"Unexpected response: {tokens!r}")
        return dict(t.split("=", 1) for t in tokens[3:] if "=" in t)

    def connect(self):
        self.close()
        self.rid = 0
        if self.cfg.led_socket:
            self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            self.sock.settimeout(self.cfg.timeout)
            self.sock.connect(self.cfg.led_socket)
        else:
            self.sock = socket.create_connection(
                (self.cfg.led_host, self.cfg.led_port), self.cfg.timeout)
            self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
        self.sock.settimeout(self.cfg.timeout)
        self.reader = self.sock.makefile("rb")
        if self.command("HELLO", "1").get("version") != "1":
            raise LedError("Unsupported protocol version")
        caps = self.command("CAPS")
        if "power" not in caps.get("leds", "").split(","):
            raise LedError("Server has no power LED")
        if not {"on", "blink"}.issubset(caps.get("modes", "").split(",")):
            raise LedError("Server lacks on/blink modes")
        LOG.info("Connected to LED server")


def parse_args(argv=None):
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.ArgumentDefaultsHelpFormatter)
    def option(name, default, kind=str, help=""):
        p.add_argument("--" + name, type=kind,
                       default=os.getenv(name.upper().replace("-", "_"), default), help=help)
    option("led-host", "127.0.0.1", help="TCP server host")
    option("led-port", 39001, int, "TCP server port")
    option("led-socket", "", help="Unix socket path; overrides TCP")
    option("priority", 20, int, "Priority: lower number wins; must be permitted by server")
    option("poll-interval", 2.0, float, "CPU sampling/update interval, seconds")
    option("timeout", 3.0, float, "Socket timeout, seconds")
    option("backoff-max", 30.0, float, "Maximum reconnect delay, seconds")
    option("health-interval", 30.0, float, "Reassert complete state and FLUSH, seconds")
    option("cold-rgb", "200,220,255", rgb, "Idle cool-white endpoint")
    option("hot-rgb", "255,185,145", rgb, "Full-load warm-white endpoint")
    option("brightness-min", 80, int, "Brightness at 0%% CPU")
    option("brightness-max", 220, int, "Brightness at 100%% CPU")
    option("memory-threshold", 85.0, float, "Blink when memory percent exceeds this")
    option("memory-hysteresis", 0.0, float, "Optional recovery margin, percentage points")
    option("blink-on-ms", 500, int, "Hardware blink on-time")
    option("blink-off-ms", 500, int, "Hardware blink off-time")
    option("test-cpu", None, float, "Override CPU percent without generating load")
    option("test-memory", None, float, "Override memory percent without allocating RAM")
    p.add_argument("--dry-run", action="store_true", help="Print states; never connect")
    p.add_argument("-v", "--verbose", action="store_true")
    c = p.parse_args(argv)
    for name in ("poll_interval", "timeout", "backoff_max", "health_interval"):
        value = getattr(c, name)
        if not math.isfinite(value) or value < 0.1:
            p.error(name + " must be finite and >= 0.1 seconds")
    if not 1 <= c.led_port <= 65535 or not 0 <= c.priority <= 2**31 - 1:
        p.error("Invalid port or priority")
    if not 1 <= c.brightness_min <= c.brightness_max <= 255:
        p.error("Require 1 <= brightness-min <= brightness-max <= 255")
    for name in ("memory_threshold", "memory_hysteresis", "test_cpu", "test_memory"):
        value = getattr(c, name)
        if value is not None and (not math.isfinite(value) or not 0 <= value <= 100):
            p.error(name + " must be finite and in 0..100")
    if c.memory_hysteresis > c.memory_threshold:
        p.error("Memory hysteresis exceeds threshold")
    if min(c.blink_on_ms, c.blink_off_ms) < 1 or c.blink_on_ms + c.blink_off_ms > 65535:
        p.error("Blink half-cycles must be positive; sum <= 65535")
    return c


def run(c, stop):
    led = Connection(c)
    previous = cpu_snapshot() if c.test_cpu is None else None
    last, blinking, last_sent = None, False, 0.0
    retry_at, backoff = 0.0, 1.0
    try:
        while not stop.is_set():
            if previous is not None:
                if stop.wait(c.poll_interval):
                    break
                current = cpu_snapshot()
                cpu = cpu_percent(previous, current)
                previous = current
                if cpu is None:
                    continue
            else:
                cpu = c.test_cpu
            memory = memory_percent() if c.test_memory is None else c.test_memory
            state, blinking = target(c, cpu, memory, blinking)
            LOG.debug("CPU %.1f%% memory %.1f%% state=%s", cpu, memory, state)
            now = time.monotonic()
            if c.dry_run:
                LOG.info("CPU %.1f%% memory %.1f%% %s", cpu, memory, state)
            elif now >= retry_at:
                try:
                    if led.sock is None:
                        led.connect()
                        last = None
                    health_due = now - last_sent >= c.health_interval
                    if state != last or health_due:
                        args = "power " + " ".join(f"{k}={v}" for k, v in state.items())
                        led.command("SET", args + f" priority={c.priority}", "ACCEPTED")
                        # Low update frequency: verify completion, not merely acceptance.
                        led.command("FLUSH")
                        last, last_sent = state.copy(), time.monotonic()
                    backoff, retry_at = 1.0, 0.0
                except (OSError, ValueError, LedError) as exc:
                    LOG.warning("LED update failed: %s", exc)
                    led.close()
                    last = None
                    delay = min(c.backoff_max, backoff * random.uniform(0.8, 1.2))
                    retry_at = time.monotonic() + delay
                    backoff = min(c.backoff_max, backoff * 2)
            if previous is None:
                stop.wait(c.poll_interval)
    finally:
        # Disconnect releases ownership and restores the server-configured fallback.
        led.close()


def main():
    c = parse_args()
    logging.basicConfig(level=logging.DEBUG if c.verbose else logging.INFO,
                        format="%(asctime)s %(levelname)s %(message)s")
    stop = threading.Event()
    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, lambda *_: stop.set())
    try:
        run(c, stop)
    except (OSError, ValueError, KeyError) as exc:
        LOG.error("Host statistics unavailable: %s", exc)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
