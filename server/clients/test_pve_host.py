import contextlib
import io
import socket
import threading
import unittest
from unittest.mock import patch
import pve_host as p


class Tests(unittest.TestCase):
    def test_endpoints(self):
        c = p.parse_args([])
        for cpu, color, brightness in [(0, "200,220,255", "80"),
                                       (50, "228,202,200", "150"),
                                       (100, "255,185,145", "220")]:
            state, blink = p.target(c, cpu, 40)
            self.assertEqual((state["rgb"], state["brightness"], blink),
                             (color, brightness, False))

    def test_threshold(self):
        c = p.parse_args([])
        self.assertFalse(p.target(c, 0, 85)[1])
        self.assertTrue(p.target(c, 0, 85.1)[1])
        self.assertFalse(p.target(c, 0, 85, True)[1])

    def test_hysteresis(self):
        c = p.parse_args(["--memory-hysteresis", "3"])
        self.assertTrue(p.target(c, 0, 83, True)[1])
        self.assertFalse(p.target(c, 0, 82, True)[1])
        self.assertFalse(p.target(c, 0, 83, False)[1])

    def test_cpu(self):
        self.assertEqual(p.cpu_percent((0,) * 8, (20, 0, 20, 50, 10, 0, 0, 0)), 40)
        self.assertIsNone(p.cpu_percent((1,) * 8, (0,) * 8))
        with patch("builtins.open", return_value=io.StringIO("cpu 20 0 20 50 10 0 0 0 15 0\n")):
            self.assertEqual(sum(p.cpu_snapshot()), 100)

    def test_memory(self):
        with patch("builtins.open", return_value=io.StringIO("MemTotal: 1000 kB\nMemAvailable: 150 kB\n")):
            self.assertEqual(p.memory_percent(), 85)

    def test_validation(self):
        for args in (["--poll-interval", "nan"], ["--test-cpu", "101"],
                     ["--brightness-min", "0"], ["--blink-on-ms", "65535"],
                     ["--cold-rgb", "0,0,0"]):
            with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                p.parse_args(args)

    def test_protocol(self):
        listener = socket.socket()
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        received, errors = [], []
        def server():
            try:
                conn, _ = listener.accept()
                with conn, conn.makefile("rb") as reader:
                    for action, fields in [("HELLO", "version=1"),
                                           ("CAPS", "leds=power modes=on,blink"),
                                           ("ACCEPTED", "generation=1"), ("FLUSH", "")]:
                        line = reader.readline().decode().split()
                        received.append(line)
                        conn.sendall(f"OK {line[1]} {action} {fields}\n".encode())
            except Exception as exc:
                errors.append(exc)
        thread = threading.Thread(target=server, daemon=True)
        thread.start()
        c = p.parse_args(["--led-port", str(listener.getsockname()[1])])
        led = p.Connection(c)
        try:
            led.connect()
            led.command("SET", "power mode=on rgb=200,220,255 brightness=80", "ACCEPTED")
            led.command("FLUSH")
        finally:
            led.close()
            listener.close()
            thread.join(5)
        self.assertFalse(errors)
        self.assertEqual([r[0] for r in received], ["HELLO", "CAPS", "SET", "FLUSH"])


if __name__ == "__main__":
    unittest.main()
