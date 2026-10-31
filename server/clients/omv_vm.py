#!/usr/bin/env python3
"""OpenMediaVault guest-side UGREEN netdev LED agent (standard library only)."""
import argparse
import json
import logging
import os
from pathlib import Path
import random
import signal
import socket
import subprocess
import threading
import time

LOG = logging.getLogger("omv-led")
LIST_PARAMS = {"start": 0, "limit": -1, "sortfield": "devicefile", "sortdir": "ASC"}


def rows(value):
    if isinstance(value, dict):
        result = value["data"]
        if value.get("total", len(result)) != len(result):
            raise ValueError("Incomplete RPC list")
    else:
        result = value
    if not isinstance(result, list) or any(not isinstance(x, dict) for x in result):
        raise ValueError("Expected RPC array of objects")
    return result


def boolean(value):
    if type(value) is not bool:
        raise ValueError("Expected JSON boolean, got %r" % (value,))
    return value


def evaluate(info, filesystems, services, smart, tasks, alerts, all_services=False):
    """Precedence: background activity > faults > attention > healthy."""
    faults, attention = [], []
    for fs in rows(filesystems):
        label = fs.get("mountpoint") or fs.get("devicefile") or "filesystem"
        if fs["status"] != 1 or not boolean(fs["mounted"]):
            faults.append("filesystem unavailable/unmounted: " + label)
    for service in rows(services):
        enabled = boolean(service["enabled"])
        running = boolean(service["running"])
        if (all_services or enabled) and not running:
            faults.append("service stopped: " + service["name"])
    for disk in rows(smart):
        if disk["overallstatus"] != "GOOD":
            faults.append("SMART %s: %s" % (disk.get("devicefile", "disk"), disk["overallstatus"]))
    if boolean(info["rebootRequired"]):
        attention.append("reboot required")
    updates = info["availablePkgUpdates"]
    if type(updates) is not int or updates < 0:
        raise ValueError("Invalid availablePkgUpdates")
    if updates:
        attention.append("%d package updates" % updates)
    if boolean(info["configDirty"]):
        attention.append("configuration changes need applying")
    if not isinstance(alerts, list) or any(not isinstance(x, str) for x in alerts):
        raise ValueError("Alerts must be a JSON array of actionable message strings")
    attention.extend(alerts)
    if not isinstance(tasks, (dict, list)):
        raise ValueError("Invalid Exec.enumerate result")
    if tasks:
        return "busy", ["%d OMV background tasks" % len(tasks)] + faults + attention
    if faults:
        return "fault", faults + attention
    if attention:
        return "attention", attention
    return "healthy", ["all checks passed"]


class Monitor:
    def __init__(self, cfg):
        self.cfg = cfg
        self.lock = threading.Lock()
        self.state = "attention"
        self.reasons = ["awaiting first complete OMV snapshot"]
        self.checked = time.monotonic()
        self.smart_cache = None
        self.smart_checked = 0

    def rpc(self, service, method, params=None):
        command = [self.cfg.rpc, service, method]
        # Parameterless methods must not receive a literal "null" argument.
        if params is not None:
            command.append(json.dumps(params))
        try:
            result = subprocess.run(
                command,
                capture_output=True,
                text=True,
                timeout=self.cfg.rpc_timeout,
                check=False,
            )
        except subprocess.TimeoutExpired as exc:
            raise RuntimeError(
                f"{service}.{method}: timed out after "
                f"{self.cfg.rpc_timeout:g}s"
            ) from exc
        stdout = result.stdout.strip()
        stderr = result.stderr.strip()
        if result.returncode != 0:
            raise RuntimeError(
                f"{service}.{method}: exit={result.returncode}; "
                f"stdout={stdout[:2000]!r}; stderr={stderr[:2000]!r}"
            )
        try:
            data = json.loads(stdout)
        except json.JSONDecodeError as exc:
            raise RuntimeError(
                f"{service}.{method}: invalid JSON; "
                f"stdout={stdout[:2000]!r}; stderr={stderr[:2000]!r}"
            ) from exc
        if isinstance(data, dict) and data.get("error"):
            raise RuntimeError(
                f"{service}.{method}: {data['error']!r}"
            )
        return data

    def snapshot(self):
        tasks = self.rpc("Exec", "enumerate")
        # Return promptly while busy; complete health checks follow once idle.
        if not isinstance(tasks, (dict, list)):
            raise ValueError("Invalid Exec.enumerate result")
        if tasks:
            return "busy", ["%d OMV background tasks" % len(tasks)]
        info = self.rpc("System", "getInformation")
        fs = self.rpc("FileSystemMgmt", "getList", LIST_PARAMS)
        services = self.rpc("Services", "getStatus")
        now = time.monotonic()
        if self.smart_cache is None or now - self.smart_checked >= self.cfg.smart_interval:
            self.smart_cache = rows(self.rpc("Smart", "enumerateMonitoredDevices"))
            self.smart_checked = time.monotonic()
        alerts = []
        if self.cfg.alerts_file:
            # An explicitly configured file is mandatory; unreadable/malformed means unknown.
            alerts = json.loads(Path(self.cfg.alerts_file).read_text())
        tasks = self.rpc("Exec", "enumerate")
        return evaluate(info, fs, services, self.smart_cache, tasks, alerts,
                        self.cfg.all_services)

    def set_state(self, state, reasons):
        with self.lock:
            changed = (state, reasons) != (self.state, self.reasons)
            self.state, self.reasons, self.checked = state, reasons, time.monotonic()
        if changed:
            LOG.info("%s: %s", state, "; ".join(reasons))

    def get_state(self):
        with self.lock:
            if time.monotonic() - self.checked > self.cfg.stale_after:
                return "attention"
            return self.state

    def run(self, stop):
        while not stop.is_set():
            try:
                self.set_state(*self.snapshot())
            except (OSError, ValueError, KeyError, TypeError, RuntimeError,
                    subprocess.SubprocessError) as exc:
                LOG.warning("OMV status unknown: %s", exc)
                self.set_state("attention", ["OMV monitoring error: " + str(exc)])
            stop.wait(self.cfg.poll_interval)


class LedConnection:
    def __init__(self, cfg):
        self.cfg = cfg
        self.sock = None
        self.reader = None
        self.request_id = 0

    def close(self):
        if self.reader is not None:
            self.reader.close()
            self.reader = None
        if self.sock is not None:
            self.sock.close()
            self.sock = None

    def request(self, verb, arguments="", expected=None):
        self.request_id += 1
        rid = str(self.request_id)
        self.sock.sendall(("%s %s %s\n" % (verb, rid, arguments)).encode("ascii"))
        line = self.reader.readline(4097)
        if not line or len(line) > 4096 or not line.endswith(b"\n"):
            raise OSError("LED server closed connection or returned invalid framing")
        tokens = line.decode("ascii").strip().split()
        if len(tokens) < 3 or tokens[:3] != ["OK", rid, expected or verb]:
            raise OSError("LED server rejected request: " + line.decode("ascii", errors="replace").strip())
        return dict(t.split("=", 1) for t in tokens[3:] if "=" in t)

    def connect(self):
        self.sock = socket.create_connection((self.cfg.led_host, self.cfg.led_port),
                                             timeout=self.cfg.socket_timeout)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
        self.reader = self.sock.makefile("rb")
        self.request_id = 0
        self.request("HELLO", "1")
        caps = self.request("CAPS")
        if self.cfg.led_name not in caps.get("leds", "").split(","):
            raise OSError("Configured LED absent from CAPS")
        if not {"on", "blink"}.issubset(set(caps.get("modes", "").split(","))):
            raise OSError("Required LED modes absent from CAPS")

    def push(self, state):
        rgb = {"healthy": "0,255,0", "busy": "0,255,0",
               "fault": "255,0,0", "attention": "255,165,0"}[state]
        args = "%s mode=%s rgb=%s brightness=%d on_ms=500 off_ms=500" % (
            self.cfg.led_name, "blink" if state == "busy" else "on", rgb, self.cfg.brightness)
        self.request("SET", args, "ACCEPTED")
        self.request("FLUSH")


def parse_args(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--led-host", default=os.environ.get("LED_HOST", "192.168.178.250"))
    p.add_argument("--led-port", type=int, default=int(os.environ.get("LED_PORT", "39001")))
    p.add_argument("--led-name", choices=["netdev", "power"] + ["disk%d" % n for n in range(1, 9)],
                   default=os.environ.get("LED_NAME", "netdev"))
    p.add_argument("--rpc", default="/usr/sbin/omv-rpc")
    p.add_argument("--rpc-timeout", type=float, default=15)
    p.add_argument("--poll-interval", type=float, default=10)
    p.add_argument("--smart-interval", type=float, default=300)
    p.add_argument("--stale-after", type=float, default=120)
    p.add_argument("--socket-timeout", type=float, default=5)
    p.add_argument("--heartbeat", type=float, default=15)
    p.add_argument("--brightness", type=int, default=200)
    p.add_argument("--alerts-file", default=os.environ.get("OMV_ALERTS_FILE"))
    p.add_argument("--all-services", action="store_true",
                   help="Require disabled dashboard services to run too (normally ignored)")
    p.add_argument("--check", action="store_true", help="Print one snapshot without contacting LED server")
    p.add_argument("-v", "--verbose", action="store_true")
    cfg = p.parse_args(argv)
    for key in ("rpc_timeout", "poll_interval", "smart_interval", "stale_after", "socket_timeout", "heartbeat"):
        if getattr(cfg, key) <= 0:
            p.error(key + " must be positive")
    if not 1 <= cfg.brightness <= 255 or not 1 <= cfg.led_port <= 65535:
        p.error("Invalid brightness or port")
    return cfg


def main():
    cfg = parse_args()
    logging.basicConfig(level=logging.DEBUG if cfg.verbose else logging.INFO,
                        format="%(asctime)s %(levelname)s %(message)s")
    monitor = Monitor(cfg)
    if cfg.check:
        try:
            state, reasons = monitor.snapshot()
            print(json.dumps({"state": state, "reasons": reasons}, indent=2))
            return 0
        except Exception as exc:
            print(json.dumps({"state": "attention", "error": str(exc)}, indent=2))
            return 1
    stop = threading.Event()
    for sig in (signal.SIGTERM, signal.SIGINT):
        signal.signal(sig, lambda *_: stop.set())
    threading.Thread(target=monitor.run, args=(stop,), daemon=True, name="omv-poller").start()
    connection = LedConnection(cfg)
    backoff = 1
    try:
        while not stop.is_set():
            try:
                connection.connect()
                last_state, last_sent = None, 0
                while not stop.is_set():
                    state = monitor.get_state()
                    now = time.monotonic()
                    if state != last_state or now - last_sent >= cfg.heartbeat:
                        connection.push(state)
                        last_state, last_sent = state, now
                        backoff = 1
                    stop.wait(1)
            except (OSError, UnicodeError, ValueError) as exc:
                LOG.warning("LED connection failed: %s", exc)
            finally:
                connection.close()
            if not stop.is_set():
                stop.wait(backoff + random.uniform(0, 0.5))
                backoff = min(backoff * 2, 30)
    finally:
        connection.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
