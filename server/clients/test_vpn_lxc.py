#!/usr/bin/env python3
"""Unit tests for vpn_lxc.py."""
import io
import ipaddress
import os
import socket
import tempfile
import unittest
from unittest.mock import MagicMock, patch

import vpn_lxc


class TestVpnSupervisor(unittest.TestCase):
    def setUp(self):
        self.tmpdir = tempfile.TemporaryDirectory()
        self.conf_path = os.path.join(self.tmpdir.name, "proton.conf")
        with open(self.conf_path, "w") as f:
            f.write("""[Interface]
PrivateKey = xxxxx=
Address = 10.2.0.2/32

[Peer]
PublicKey = yyyyy=
AllowedIPs = 0.0.0.0/0
Endpoint = 45.92.33.162:51820
# Endpoint = [2001:ac8:8d:5::10]:51820
PersistentKeepalive = 25
""")

    def tearDown(self):
        self.tmpdir.cleanup()

    def test_get_endpoint_network_from_conf(self):
        sup = vpn_lxc.VpnSupervisor(
            interface="proton",
            conf_path=self.conf_path,
            endpoint_override="",
            max_handshake_age=180,
            ipv4_urls=["http://ifconfig.me/ip"],
            ipv6_targets=[("icanhazip.com", 80)],
            timeout=2.0,
        )
        net = sup.get_endpoint_network()
        self.assertEqual(net, ipaddress.IPv4Network("45.92.0.0/16"))

    def test_get_endpoint_network_override(self):
        sup = vpn_lxc.VpnSupervisor(
            interface="proton",
            conf_path="/nonexistent/conf",
            endpoint_override="198.51.100.5:51820",
            max_handshake_age=180,
            ipv4_urls=["http://ifconfig.me/ip"],
            ipv6_targets=[("icanhazip.com", 80)],
            timeout=2.0,
        )
        net = sup.get_endpoint_network()
        self.assertEqual(net, ipaddress.IPv4Network("198.51.0.0/16"))

    @patch("os.path.exists", return_value=False)
    def test_evaluate_interface_missing(self, mock_exists):
        sup = vpn_lxc.VpnSupervisor(
            interface="proton",
            conf_path=self.conf_path,
            endpoint_override="45.92.33.162:51820",
            max_handshake_age=180,
            ipv4_urls=["http://ifconfig.me/ip"],
            ipv6_targets=[("icanhazip.com", 80)],
            timeout=2.0,
        )
        status = sup.evaluate()
        self.assertFalse(status.ok)
        self.assertIn("does not exist", status.reason)

    @patch("os.path.exists", return_value=True)
    @patch("shutil.which", return_value=None)
    def test_evaluate_ipv6_leak(self, mock_which, mock_exists):
        sup = vpn_lxc.VpnSupervisor(
            interface="proton",
            conf_path=self.conf_path,
            endpoint_override="45.92.33.162:51820",
            max_handshake_age=180,
            ipv4_urls=["http://ifconfig.me/ip"],
            ipv6_targets=[("icanhazip.com", 80)],
            timeout=2.0,
        )
        # Mock IPv6 check finding connectivity
        sup.check_ipv6_connectivity = MagicMock(return_value=(True, "IPv6 connect succeeded"))
        status = sup.evaluate()
        self.assertFalse(status.ok)
        self.assertIn("IPv6 leak detected", status.reason)

    @patch("os.path.exists", return_value=True)
    @patch("shutil.which", return_value=None)
    def test_evaluate_ipv4_mismatch(self, mock_which, mock_exists):
        sup = vpn_lxc.VpnSupervisor(
            interface="proton",
            conf_path=self.conf_path,
            endpoint_override="45.92.33.162:51820",
            max_handshake_age=180,
            ipv4_urls=["http://ifconfig.me/ip"],
            ipv6_targets=[("icanhazip.com", 80)],
            timeout=2.0,
        )
        sup.check_ipv6_connectivity = MagicMock(return_value=(False, "blocked"))
        # Public IP from ISP instead of VPN endpoint /16
        sup.fetch_public_ipv4 = MagicMock(return_value=("85.74.12.3", "ok"))
        status = sup.evaluate()
        self.assertFalse(status.ok)
        self.assertIn("NOT in tunnel Endpoint /16 network", status.reason)
        self.assertIn("45.92.0.0/16", status.reason)

    @patch("os.path.exists", return_value=True)
    @patch("shutil.which", return_value=None)
    def test_evaluate_all_ok(self, mock_which, mock_exists):
        sup = vpn_lxc.VpnSupervisor(
            interface="proton",
            conf_path=self.conf_path,
            endpoint_override="45.92.33.162:51820",
            max_handshake_age=180,
            ipv4_urls=["http://ifconfig.me/ip"],
            ipv6_targets=[("icanhazip.com", 80)],
            timeout=2.0,
        )
        sup.check_ipv6_connectivity = MagicMock(return_value=(False, "blocked"))
        # Public IP inside 45.92.0.0/16
        sup.fetch_public_ipv4 = MagicMock(return_value=("45.92.33.165", "ok"))
        status = sup.evaluate()
        self.assertTrue(status.ok)
        self.assertEqual(status.public_ip, "45.92.33.165")
        self.assertEqual(status.endpoint_net, "45.92.0.0/16")


class DummySocket:
    def __init__(self, responses):
        self.responses = responses
        self.sent = []

    def sendall(self, data):
        self.sent.append(data.decode("ascii"))

    def makefile(self, mode):
        return io.BytesIO(b"".join(r.encode("ascii") for r in self.responses))

    def settimeout(self, timeout):
        pass

    def setsockopt(self, level, optname, value):
        pass

    def close(self):
        pass


class TestLedConnection(unittest.TestCase):
    def test_protocol_flow(self):
        responses = [
            "OK 1 HELLO version=1 access=192.168.178.104\n",
            "OK 2 CAPS version=1 leds=power,netdev,disk1,disk2 modes=off,on,blink\n",
            "OK 3 ACCEPTED generation=10\n",
            "OK 4 FLUSH\n",
            "OK 5 RESET\n",
            "OK 6 FLUSH\n",
        ]
        dummy_sock = DummySocket(responses)
        with patch("socket.create_connection", return_value=dummy_sock):
            led = vpn_lxc.LedConnection(
                host="192.168.178.250",
                port=39001,
                led="disk1",
                priority=10,
                timeout=2.0,
            )
            led.connect()
            self.assertEqual(dummy_sock.sent[0], "HELLO 1 1\n")
            self.assertEqual(dummy_sock.sent[1], "CAPS 2\n")

            # Problem occurs -> sets flashing red at priority 10
            changed = led.set_problem(brightness=200, on_ms=500, off_ms=500, health_interval=30)
            self.assertTrue(changed)
            self.assertTrue(led.owns_led)
            self.assertIn("SET 3 disk1 mode=blink rgb=255,0,0 brightness=200 on_ms=500 off_ms=500 priority=10\n",
                          dummy_sock.sent)
            self.assertIn("FLUSH 4\n", dummy_sock.sent)

            # Second call without change does nothing
            changed2 = led.set_problem(brightness=200, on_ms=500, off_ms=500, health_interval=30)
            self.assertFalse(changed2)

            # Tunnel restored -> issues RESET
            reset_done = led.reset_ok()
            self.assertTrue(reset_done)
            self.assertFalse(led.owns_led)
            self.assertIn("RESET 5 disk1\n", dummy_sock.sent)
            self.assertIn("FLUSH 6\n", dummy_sock.sent)

            # Second reset does nothing
            reset_done2 = led.reset_ok()
            self.assertFalse(reset_done2)

    def test_drift_detection(self):
        # When desired state drifted (status=invalid or mismatch), set_problem re-sends SET/FLUSH
        responses = [
            "OK 1 HELLO version=1 access=192.168.178.104\n",
            "OK 2 CAPS version=1 leds=disk1 modes=on,blink\n",
            "OK 3 ACCEPTED generation=1\n",
            "OK 4 FLUSH\n",
            # GET returns drifted state (mode=off from another reset)
            "OK 5 GET led=disk1 source=desired mode=off brightness=200 rgb=255,0,0 on_ms=500 off_ms=500 priority=10 status=valid\n",
            "OK 6 ACCEPTED generation=2\n",
            "OK 7 FLUSH\n",
        ]
        dummy_sock = DummySocket(responses)
        with patch("socket.create_connection", return_value=dummy_sock):
            led = vpn_lxc.LedConnection(
                host="192.168.178.250", port=39001, led="disk1", priority=10, timeout=2.0
            )
            led.connect()
            led.set_problem(200, 500, 500, health_interval=0.0)

            # Trigger drift check with health_interval=0.0
            changed = led.set_problem(200, 500, 500, health_interval=0.0)
            self.assertTrue(changed)
            self.assertIn("GET 5 disk1 source=desired\n", dummy_sock.sent)
            self.assertIn("SET 6 disk1 mode=blink rgb=255,0,0 brightness=200 on_ms=500 off_ms=500 priority=10\n",
                          dummy_sock.sent)


class TestHelpers(unittest.TestCase):
    def test_parse_ipv6_targets(self):
        targets = vpn_lxc.parse_ipv6_targets("2606:4700::1:80, [2001:db8::1]:53, icanhazip.com:443, test.com")
        self.assertEqual(targets, [
            ("2606:4700::1", 80),
            ("2001:db8::1", 53),
            ("icanhazip.com", 443),
            ("test.com", 80),
        ])

    def test_parse_args_defaults(self):
        cfg = vpn_lxc.parse_args([])
        self.assertEqual(cfg.led_host, "192.168.178.250")
        self.assertEqual(cfg.led_port, 39001)
        self.assertEqual(cfg.led_name, "disk1")
        self.assertEqual(cfg.priority, 10)
        self.assertEqual(cfg.wg_interface, "proton")
        self.assertEqual(cfg.wg_conf, "/etc/wireguard/proton.conf")


if __name__ == "__main__":
    unittest.main()
