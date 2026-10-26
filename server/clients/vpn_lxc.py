#!/usr/bin/env python3
"""
vpn_lxc.py — UGREEN LED Protocol v1 supervisor for VPN gateway (192.168.178.104).

Monitors the WireGuard VPN tunnel that qBittorrent routes through:
  - WireGuard tunnel status (interface existence, systemd service, handshake age).
  - Public IPv4 egress address: must match the /16 network of the tunnel Endpoint.
  - IPv6 leak check: confirms absence of any external IPv6 connectivity.

Behavior:
  - When VPN is OK: does nothing, leaving disk1 free for qBittorrent LXC (priority 20).
  - When VPN has any problem: preempts disk1 at higher priority (priority 10)
    with flashing red LED (mode=blink rgb=255,0,0).
  - When VPN recovers: issues RESET on disk1, releasing ownership so qBittorrent
    can resume controlling disk1.

Requirements: Python 3.8+, standard library only.
"""
import argparse
import http.client
import ipaddress
import logging
import math
import os
import random
import re
import shutil
import signal
import socket
import subprocess
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from typing import Dict, List, NamedTuple, Optional, Tuple

LOG = logging.getLogger("vpn-led")


class VpnStatus(NamedTuple):
    ok: bool
    reason: str
    public_ip: Optional[str] = None
    endpoint_net: Optional[str] = None


class LedError(RuntimeError):
    pass


class IPv4HTTPConnection(http.client.HTTPConnection):
    """HTTPConnection that strictly resolves and connects over IPv4."""

    def connect(self):
        addrs = socket.getaddrinfo(self.host, self.port, socket.AF_INET, socket.SOCK_STREAM)
        if not addrs:
            raise OSError(f"No IPv4 address found for {self.host}")
        family, socktype, proto, _, sockaddr = addrs[0]
        self.sock = socket.socket(family, socktype, proto)
        if self.timeout is not None:
            self.sock.settimeout(self.timeout)
        self.sock.connect(sockaddr)


class IPv4HTTPSConnection(http.client.HTTPSConnection):
    """HTTPSConnection that strictly resolves and connects over IPv4."""

    def connect(self):
        addrs = socket.getaddrinfo(self.host, self.port, socket.AF_INET, socket.SOCK_STREAM)
        if not addrs:
            raise OSError(f"No IPv4 address found for {self.host}")
        family, socktype, proto, _, sockaddr = addrs[0]
        raw_sock = socket.socket(family, socktype, proto)
        if self.timeout is not None:
            raw_sock.settimeout(self.timeout)
        raw_sock.connect(sockaddr)
        self.sock = self._context.wrap_socket(raw_sock, server_hostname=self.host)


class IPv4HTTPHandler(urllib.request.HTTPHandler):
    def http_open(self, req):
        return self.do_open(IPv4HTTPConnection, req)


class IPv4HTTPSHandler(urllib.request.HTTPSHandler):
    def https_open(self, req):
        return self.do_open(IPv4HTTPSConnection, req)


def build_ipv4_opener():
    return urllib.request.build_opener(
        urllib.request.ProxyHandler({}),  # Bypass ambient HTTP proxies
        IPv4HTTPHandler(),
        IPv4HTTPSHandler(),
    )


class LedConnection:
    """Manages connection to the UGREEN LED server via Protocol v1."""

    def __init__(self, host: str, port: int, led: str, priority: int, timeout: float):
        self.host = host
        self.port = port
        self.led = led
        self.priority = priority
        self.timeout = timeout
        self.sock: Optional[socket.socket] = None
        self.reader = None
        self.rid = 0
        self.owns_led = False
        self.last_applied: Optional[Dict[str, str]] = None
        self.last_checked = 0.0

    def close(self):
        self.owns_led = False
        self.last_applied = None
        self.last_checked = 0.0
        if self.reader is not None:
            try:
                self.reader.close()
            except Exception:
                pass
            self.reader = None
        if self.sock is not None:
            try:
                self.sock.close()
            except Exception:
                pass
            self.sock = None

    def command(self, verb: str, args: str = "", expected: Optional[str] = None) -> Dict[str, str]:
        if self.sock is None or self.reader is None:
            raise LedError("Not connected to LED server")
        self.rid = (self.rid % (2**64 - 1)) + 1
        cmd_str = f"{verb} {self.rid}"
        if args:
            cmd_str += f" {args}"
        cmd_str += "\n"
        self.sock.sendall(cmd_str.encode("ascii"))

        raw = self.reader.readline(1026)
        if not raw or not raw.endswith(b"\n") or len(raw.rstrip(b"\r\n")) > 1024:
            raise LedError("Closed connection or invalid response framing")
        tokens = raw.decode("ascii").strip().split()
        if len(tokens) >= 3 and tokens[0] == "ERR":
            if tokens[1] not in (str(self.rid), "0"):
                raise LedError("Response request-id mismatch")
            raise LedError(" ".join(tokens))
        if tokens[:3] != ["OK", str(self.rid), expected or verb]:
            raise LedError(f"Unexpected response: {' '.join(tokens)}")
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
                raise LedError(f"Unsupported protocol version: {hello.get('version')}")
            caps = self.command("CAPS")
            if self.led not in caps.get("leds", "").split(","):
                raise LedError(f"Target LED '{self.led}' not available in server CAPS")
            if "blink" not in caps.get("modes", "").split(","):
                raise LedError("Mode 'blink' not supported by LED server")
            LOG.info("Connected to LED server at %s:%d (matched access=%s)",
                     self.host, self.port, hello.get("access", "unknown"))
        except Exception:
            self.close()
            raise

    def target(self, brightness: int, on_ms: int, off_ms: int) -> Dict[str, str]:
        return {
            "mode": "blink",
            "rgb": "255,0,0",
            "brightness": str(brightness),
            "on_ms": str(on_ms),
            "off_ms": str(off_ms),
            "priority": str(self.priority),
        }

    def set_problem(self, brightness: int, on_ms: int, off_ms: int, health_interval: float) -> bool:
        """Sets disk1 flashing red with high priority (10)."""
        target = self.target(brightness, on_ms, off_ms)
        # If already applied, periodically verify logical desired state hasn't drifted
        if self.last_applied == target and (time.monotonic() - self.last_checked >= health_interval):
            try:
                observed = self.command("GET", f"{self.led} source=desired")
                self.last_checked = time.monotonic()
                if observed.get("status") != "valid" or any(observed.get(k) != v for k, v in target.items()):
                    LOG.debug("LED desired-state drift detected; re-asserting problem state")
                    self.last_applied = None
            except LedError as exc:
                LOG.warning("Failed to query LED desired state: %s", exc)
                self.last_applied = None

        if self.last_applied == target:
            return False

        args = f"{self.led} " + " ".join(f"{k}={v}" for k, v in target.items())
        self.command("SET", args, expected="ACCEPTED")
        self.command("FLUSH")
        self.last_applied = target
        self.last_checked = time.monotonic()
        self.owns_led = True
        return True

    def reset_ok(self) -> bool:
        """Releases disk1 ownership via RESET so qBittorrent LXC can resume control."""
        if not self.owns_led and self.last_applied is None:
            return False
        LOG.info("Releasing %s ownership via RESET (tunnel restored)", self.led)
        try:
            self.command("RESET", self.led)
            self.command("FLUSH")
        except LedError as exc:
            LOG.warning("Failed to reset LED ownership: %s", exc)
        finally:
            self.owns_led = False
            self.last_applied = None
            self.last_checked = time.monotonic()
        return True


class VpnSupervisor:
    """Supervises WireGuard status, IPv6 connectivity leaks, and public IPv4 /16 match."""

    def __init__(self, interface: str, conf_path: str, endpoint_override: str,
                 max_handshake_age: float, ipv4_urls: List[str], ipv6_targets: List[Tuple[str, int]],
                 timeout: float):
        self.interface = interface
        self.conf_path = conf_path
        self.endpoint_override = endpoint_override
        self.max_handshake_age = max_handshake_age
        self.ipv4_urls = ipv4_urls
        self.ipv6_targets = ipv6_targets
        self.timeout = timeout
        self.ipv4_opener = build_ipv4_opener()
        self._cached_endpoint_ip: Optional[str] = None
        self._cached_endpoint_net: Optional[ipaddress.IPv4Network] = None

    def get_endpoint_network(self) -> ipaddress.IPv4Network:
        """Determines the /16 IPv4 network of the WireGuard tunnel Endpoint."""
        raw_endpoint = self.endpoint_override.strip()

        # Try reading endpoint from 'wg show <iface> endpoints' if available
        if not raw_endpoint and shutil.which("wg"):
            try:
                out = subprocess.check_output(
                    ["wg", "show", self.interface, "endpoints"],
                    stderr=subprocess.DEVNULL, timeout=self.timeout
                ).decode("ascii", errors="ignore")
                for line in out.strip().splitlines():
                    parts = line.split()
                    if len(parts) >= 2:
                        raw_endpoint = parts[1].strip()
                        break
            except Exception as exc:
                LOG.debug("Could not read endpoint via 'wg show': %s", exc)

        # Try parsing from WireGuard config file
        if not raw_endpoint and os.path.isfile(self.conf_path):
            try:
                with open(self.conf_path, "r", encoding="utf-8", errors="ignore") as f:
                    for line in f:
                        line_stripped = line.strip()
                        if line_stripped.startswith(("#", ";")):
                            continue
                        if re.match(r"^Endpoint\s*=", line_stripped, re.IGNORECASE):
                            val = line_stripped.split("=", 1)[1].strip()
                            val = re.split(r"[#;]", val, maxsplit=1)[0].strip()
                            if val:
                                raw_endpoint = val
                                break
            except Exception as exc:
                LOG.debug("Could not read endpoint from %s: %s", self.conf_path, exc)

        if not raw_endpoint:
            raise ValueError(f"WireGuard endpoint not found (check {self.conf_path} or pass --endpoint)")

        # Strip port: [ipv6]:port, [ipv6], or host:port
        host = raw_endpoint
        if host.startswith("["):
            end = host.find("]")
            if end != -1:
                host = host[1:end]
        elif ":" in host:
            parts = host.rsplit(":", 1)
            if parts[1].isdigit():
                host = parts[0]

        # If host is already an IP, parse it directly without DNS lookup
        try:
            ipaddress.ip_address(host)
            endpoint_ip = host
        except ValueError:
            # Resolve hostname to IPv4
            try:
                addrs = socket.getaddrinfo(host, None, socket.AF_INET, socket.SOCK_STREAM)
                endpoint_ip = addrs[0][4][0]
            except Exception as exc:
                raise ValueError(f"Failed to resolve WireGuard endpoint host '{host}': {exc}") from exc

        # Calculate /16 network
        net = ipaddress.ip_network(f"{endpoint_ip}/16", strict=False)
        if endpoint_ip != self._cached_endpoint_ip:
            LOG.info("WireGuard tunnel Endpoint: %s (host=%s) -> /16 network: %s",
                     endpoint_ip, host, net)
            self._cached_endpoint_ip = endpoint_ip
            self._cached_endpoint_net = net
        return net

    def check_wireguard_status(self) -> Tuple[bool, str]:
        """Validates that the WireGuard interface is up and active."""
        # 1. Interface presence in /sys/class/net
        sys_path = f"/sys/class/net/{self.interface}"
        if not os.path.exists(sys_path):
            return False, f"WireGuard interface '{self.interface}' does not exist"

        # 2. Check systemd service if systemctl is available
        if shutil.which("systemctl"):
            service = f"wg-quick@{self.interface}"
            try:
                res = subprocess.run(
                    ["systemctl", "is-active", service],
                    stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                    text=True, timeout=self.timeout
                )
                status = res.stdout.strip()
                if status != "active":
                    return False, f"Service {service} status is '{status}' (expected 'active')"
            except Exception as exc:
                LOG.debug("systemctl is-active check failed: %s", exc)

        # 3. Check handshake freshness if 'wg' binary is present
        if shutil.which("wg"):
            try:
                out = subprocess.check_output(
                    ["wg", "show", self.interface, "latest-handshakes"],
                    stderr=subprocess.DEVNULL, timeout=self.timeout
                ).decode("ascii", errors="ignore")
                for line in out.strip().splitlines():
                    parts = line.split()
                    if len(parts) >= 2:
                        ts = int(parts[1])
                        if ts == 0:
                            return False, "WireGuard tunnel has no recorded handshake"
                        elapsed = time.time() - ts
                        if elapsed > self.max_handshake_age:
                            return False, (f"WireGuard handshake stale (last handshake "
                                          f"{elapsed:.0f}s ago > limit {self.max_handshake_age:.0f}s)")
            except Exception as exc:
                LOG.debug("wg show latest-handshakes check error: %s", exc)

        return True, "WireGuard interface is active"

    def check_ipv6_connectivity(self) -> Tuple[bool, str]:
        """
        Checks for any external IPv6 connectivity.
        Returns (has_connectivity, details).
        If ANY connection succeeds, IPv6 connectivity exists (which is a LEAK/FAILURE).
        """
        for host, port in self.ipv6_targets:
            # Resolve IPv6 targets (handles domain names like icanhazip.com and numeric IPv6)
            try:
                addrs = socket.getaddrinfo(host, port, socket.AF_INET6, socket.SOCK_STREAM)
            except OSError:
                continue

            for _, socktype, proto, _, sockaddr in addrs:
                s = None
                try:
                    s = socket.socket(socket.AF_INET6, socktype, proto)
                    s.settimeout(min(self.timeout, 2.0))
                    # Attempt connection
                    s.connect(sockaddr)
                    # If connect succeeded, IPv6 connectivity is active!
                    return True, f"IPv6 connect succeeded to [{sockaddr[0]}]:{sockaddr[1]} ({host})"
                except OSError as exc:
                    # Connection refused from a remote host indicates traffic reached it!
                    if exc.errno == getattr(socket, "ECONNREFUSED", 111):
                        return True, f"IPv6 connection refused by remote [{sockaddr[0]}]:{sockaddr[1]} (reachable)"
                    # EADDRNOTAVAIL (99), ENETUNREACH (101), EHOSTUNREACH (113), ETIMEDOUT: blocked
                    LOG.debug("IPv6 connect to [%s]:%s failed as expected: %s",
                              sockaddr[0], sockaddr[1], exc)
                finally:
                    if s is not None:
                        try:
                            s.close()
                        except Exception:
                            pass
        return False, "IPv6 traffic is blocked/unreachable"

    def fetch_public_ipv4(self) -> Tuple[Optional[str], str]:
        """Queries public IPv4 reflection services strictly over IPv4."""
        errors = []
        for url in self.ipv4_urls:
            req = urllib.request.Request(
                url,
                headers={"User-Agent": "ugreen-vpn-led/1.0", "Accept": "text/plain"}
            )
            started = time.monotonic()
            try:
                with self.ipv4_opener.open(req, timeout=self.timeout) as resp:
                    raw = resp.read(256).decode("ascii", errors="ignore").strip()
                    # Match IPv4 address pattern
                    m = re.search(r"\b(?:\d{1,3}\.){3}\d{1,3}\b", raw)
                    if m:
                        ip_str = m.group(0)
                        # Validate as proper IPv4
                        ipaddress.IPv4Address(ip_str)
                        LOG.debug("Fetched public IPv4 %s from %s (elapsed=%.1fms)",
                                  ip_str, url, (time.monotonic() - started) * 1000)
                        return ip_str, f"Fetched from {url}"
                    errors.append(f"{url}: unexpected response '{raw}'")
            except Exception as exc:
                errors.append(f"{url}: {type(exc).__name__} ({exc})")

        return None, "; ".join(errors)

    def evaluate(self) -> VpnStatus:
        """Performs full evaluation of VPN health."""
        # 1. WireGuard tunnel status
        wg_ok, wg_msg = self.check_wireguard_status()
        if not wg_ok:
            return VpnStatus(ok=False, reason=wg_msg)

        # 2. Determine Endpoint /16 network
        try:
            endpoint_net = self.get_endpoint_network()
        except ValueError as exc:
            return VpnStatus(ok=False, reason=str(exc))

        # 3. Check for IPv6 leaks
        ipv6_leak, ipv6_msg = self.check_ipv6_connectivity()
        if ipv6_leak:
            return VpnStatus(ok=False, reason=f"IPv6 leak detected: {ipv6_msg}",
                             endpoint_net=str(endpoint_net))

        # 4. Fetch public IPv4 and verify it is inside Endpoint /16
        pub_ip, ip_msg = self.fetch_public_ipv4()
        if not pub_ip:
            return VpnStatus(ok=False, reason=f"Failed to obtain public IPv4: {ip_msg}",
                             endpoint_net=str(endpoint_net))

        pub_addr = ipaddress.IPv4Address(pub_ip)
        if pub_addr not in endpoint_net:
            return VpnStatus(
                ok=False,
                reason=(f"Public IPv4 {pub_ip} is NOT in tunnel Endpoint /16 network "
                        f"{endpoint_net} (traffic not going through VPN tunnel)"),
                public_ip=pub_ip,
                endpoint_net=str(endpoint_net),
            )

        return VpnStatus(
            ok=True,
            reason=f"Tunnel active, public IPv4 {pub_ip} in {endpoint_net}, IPv6 blocked",
            public_ip=pub_ip,
            endpoint_net=str(endpoint_net),
        )


def parse_ipv6_targets(raw: str) -> List[Tuple[str, int]]:
    targets = []
    for item in raw.split(","):
        item = item.strip()
        if not item:
            continue
        # Supports [2606:4700::1]:80, 2606:4700::1:80, or hostname:80
        m = re.match(r"^\[?([^\]]+)\]?:(\d+)$", item)
        if m:
            targets.append((m.group(1), int(m.group(2))))
        else:
            targets.append((item, 80))
    return targets


def parse_args(argv=None):
    p = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    p.add_argument("--led-host", default=os.getenv("LED_HOST", "192.168.178.250"),
                   help="UGREEN LED server host/IP")
    p.add_argument("--led-port", type=int, default=int(os.getenv("LED_PORT", "39001")),
                   help="UGREEN LED server TCP port")
    p.add_argument("--led-name", default=os.getenv("LED_NAME", "disk1"),
                   choices=[f"disk{i}" for i in range(1, 9)],
                   help="LED identifier to control")
    p.add_argument("--priority", type=int, default=int(os.getenv("LED_PRIORITY", "10")),
                   help="Command priority ceiling (closer to 0 is higher; access rule ceiling is 10)")
    p.add_argument("--poll-interval", type=float, default=float(os.getenv("POLL_INTERVAL", "5")),
                   help="VPN supervision poll interval in seconds")
    p.add_argument("--timeout", type=float, default=float(os.getenv("REQUEST_TIMEOUT", "5")),
                   help="Network request timeout in seconds")
    p.add_argument("--health-interval", type=float, default=float(os.getenv("LED_HEALTH_INTERVAL", "30")),
                   help="LED desired-state drift check interval in seconds")
    p.add_argument("--brightness", type=int, default=int(os.getenv("LED_BRIGHTNESS", "200")),
                   help="LED brightness (1-255)")
    p.add_argument("--blink-on-ms", type=int, default=int(os.getenv("BLINK_ON_MS", "500")),
                   help="Blink on-time in milliseconds during problem state")
    p.add_argument("--blink-off-ms", type=int, default=int(os.getenv("BLINK_OFF_MS", "500")),
                   help="Blink off-time in milliseconds during problem state")
    p.add_argument("--backoff-max", type=float, default=float(os.getenv("BACKOFF_MAX", "30")),
                   help="Maximum reconnect backoff delay in seconds")
    p.add_argument("--wg-interface", default=os.getenv("WG_INTERFACE", "proton"),
                   help="WireGuard interface name")
    p.add_argument("--wg-conf", default=os.getenv("WG_CONF", "/etc/wireguard/proton.conf"),
                   help="Path to WireGuard configuration file")
    p.add_argument("--endpoint", default=os.getenv("WG_ENDPOINT", ""),
                   help="WireGuard Endpoint host/IP override (auto-detected if empty)")
    p.add_argument("--max-handshake-age", type=float,
                   default=float(os.getenv("MAX_HANDSHAKE_AGE", "180")),
                   help="Maximum acceptable age of WireGuard handshake in seconds")
    p.add_argument("--ipv4-urls",
                   default=os.getenv("IPV4_URLS", "http://ifconfig.me/ip,http://icanhazip.com,https://api.ipify.org"),
                   help="Comma-separated URLs for public IPv4 reflection")
    p.add_argument("--ipv6-targets",
                   default=os.getenv("IPV6_TARGETS",
                                     "2606:4700::6810:b8f1:80,2606:4700:4700::1111:53,"
                                     "2001:4860:4860::8888:53,icanhazip.com:80,ifconfig.me:80"),
                   help="Comma-separated target:port pairs for IPv6 connectivity detection")
    p.add_argument("-v", "--verbose", action="store_true",
                   help="Enable debug logging")

    cfg = p.parse_args(argv)

    if not 1 <= cfg.led_port <= 65535:
        p.error("LED port must be between 1 and 65535")
    if not 1 <= cfg.brightness <= 255:
        p.error("LED brightness must be between 1 and 255")
    if not 0 <= cfg.priority <= 2**31 - 1:
        p.error("Priority must be non-negative integer")
    if any(not math.isfinite(x) or x < 1 for x in (
        cfg.poll_interval, cfg.timeout, cfg.backoff_max, cfg.health_interval
    )):
        p.error("Intervals and timeouts must be finite numbers >= 1")
    if min(cfg.blink_on_ms, cfg.blink_off_ms) < 1 or (cfg.blink_on_ms + cfg.blink_off_ms) > 65535:
        p.error("Blink timings must be positive and their sum <= 65535")

    return cfg


def run(cfg, stop: threading.Event):
    ipv4_urls = [u.strip() for u in cfg.ipv4_urls.split(",") if u.strip()]
    ipv6_targets = parse_ipv6_targets(cfg.ipv6_targets)

    supervisor = VpnSupervisor(
        interface=cfg.wg_interface,
        conf_path=cfg.wg_conf,
        endpoint_override=cfg.endpoint,
        max_handshake_age=cfg.max_handshake_age,
        ipv4_urls=ipv4_urls,
        ipv6_targets=ipv6_targets,
        timeout=cfg.timeout,
    )

    led = LedConnection(cfg.led_host, cfg.led_port, cfg.led_name, cfg.priority, cfg.timeout)

    previous_ok: Optional[bool] = None
    backoff, retry_at = 1.0, 0.0

    LOG.info("VPN supervisor agent started for %s (supervising %s; LED target %s@%s:%d)",
             cfg.wg_interface, cfg.wg_conf, cfg.led_name, cfg.led_host, cfg.led_port)

    try:
        while not stop.is_set():
            started = time.monotonic()

            # Evaluate VPN status
            status = supervisor.evaluate()

            if previous_ok is None or status.ok != previous_ok:
                if status.ok:
                    LOG.info("VPN STATUS OK: %s", status.reason)
                else:
                    LOG.warning("VPN STATUS PROBLEM: %s", status.reason)
                previous_ok = status.ok

            # Handle LED updates
            if time.monotonic() >= retry_at:
                try:
                    if led.sock is None:
                        led.connect()

                    if status.ok:
                        # If VPN is OK and we previously claimed ownership, release it
                        if led.owns_led:
                            led.reset_ok()
                    else:
                        # VPN has problem: take priority and flash red
                        led.set_problem(cfg.brightness, cfg.blink_on_ms, cfg.blink_off_ms,
                                        cfg.health_interval)

                    backoff, retry_at = 1.0, 0.0
                except (OSError, ValueError, LedError) as exc:
                    LOG.warning("LED server communication failed: %s", exc)
                    led.close()
                    delay = min(cfg.backoff_max, backoff * random.uniform(0.8, 1.2))
                    retry_at = time.monotonic() + delay
                    backoff = min(cfg.backoff_max, backoff * 2)

            # Sleep until next poll interval
            elapsed = time.monotonic() - started
            stop.wait(max(0.0, cfg.poll_interval - elapsed))

    finally:
        LOG.info("Stopping VPN supervisor agent...")
        if led.sock is not None and led.owns_led:
            try:
                led.reset_ok()
            except Exception as exc:
                LOG.debug("Error releasing LED on shutdown: %s", exc)
        led.close()
        LOG.info("VPN supervisor agent stopped (LED connection closed; server fallback applies)")


def main():
    cfg = parse_args()
    logging.basicConfig(
        level=logging.DEBUG if cfg.verbose else logging.INFO,
        format="%(asctime)s [%(levelname)s] %(message)s",
        datefmt="%Y-%m-%d %H:%M:%S",
    )
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
