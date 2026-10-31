#!/usr/bin/env python3
import importlib.util
from pathlib import Path
import socket
import threading
import unittest

spec = importlib.util.spec_from_file_location("omv_vm", Path(__file__).with_name("omv_vm.py"))
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)

class AgentTests(unittest.TestCase):
    def setUp(self):
        self.info = {"rebootRequired": False, "availablePkgUpdates": 0, "configDirty": False}
        self.fs = [{"status": 1, "mounted": True, "mountpoint": "/srv/data"}]
        self.services = [{"name": "nfs", "enabled": True, "running": True}]
        self.smart = [{"devicefile": "/dev/sda", "overallstatus": "GOOD"}]
        self.tasks, self.alerts = {}, []

    def state(self, **kwargs):
        return m.evaluate(self.info, self.fs, self.services, self.smart, self.tasks, self.alerts, **kwargs)[0]

    def test_healthy(self):
        self.assertEqual(self.state(), "healthy")

    def test_each_attention_condition(self):
        for key, value in [("rebootRequired", True), ("availablePkgUpdates", 1), ("configDirty", True)]:
            with self.subTest(key=key):
                old = self.info[key]; self.info[key] = value
                self.assertEqual(self.state(), "attention")
                self.info[key] = old
        self.alerts.append("repair needed")
        self.assertEqual(self.state(), "attention")

    def test_filesystem_missing_or_unmounted(self):
        self.fs[0]["status"] = 3
        self.assertEqual(self.state(), "fault")
        self.fs[0].update(status=1, mounted=False)
        self.assertEqual(self.state(), "fault")

    def test_services(self):
        self.services[0]["running"] = False
        self.assertEqual(self.state(), "fault")
        self.services[0]["enabled"] = False
        self.assertEqual(self.state(), "healthy")
        self.assertEqual(self.state(all_services=True), "fault")

    def test_smart_warning(self):
        self.smart[0]["overallstatus"] = "BAD_SECTOR"
        self.assertEqual(self.state(), "fault")

    def test_precedence(self):
        self.info["rebootRequired"] = True
        self.fs[0]["status"] = 3
        self.assertEqual(self.state(), "fault")
        self.tasks = {"/tmp/task": {}}
        self.assertEqual(self.state(), "busy")

    def test_schema_failure(self):
        self.services[0]["running"] = "false"
        with self.assertRaises(ValueError): self.state()
        with self.assertRaises(ValueError): m.rows({"total": 2, "data": [{}]})

    def test_stale_becomes_attention(self):
        monitor = m.Monitor(m.parse_args([]))
        monitor.set_state("healthy", [])
        monitor.checked -= 121
        self.assertEqual(monitor.get_state(), "attention")

    def test_protocol_and_colors(self):
        listener = socket.socket()
        listener.bind(("127.0.0.1", 0)); listener.listen()
        port = listener.getsockname()[1]
        commands = []
        def server():
            with listener:
                conn, _ = listener.accept()
                with conn, conn.makefile("rb") as reader:
                    for line in reader:
                        tokens = line.decode().split(); commands.append(tokens)
                        verb, rid = tokens[:2]
                        action = "ACCEPTED" if verb == "SET" else verb
                        extra = " leds=netdev modes=on,blink,off" if verb == "CAPS" else ""
                        conn.sendall(("OK %s %s%s\n" % (rid, action, extra)).encode())
        thread = threading.Thread(target=server); thread.start()
        client = m.LedConnection(m.parse_args(["--led-host", "127.0.0.1", "--led-port", str(port)]))
        try:
            client.connect()
            for state in ("healthy", "busy", "fault", "attention"): client.push(state)
        finally:
            client.close(); thread.join(timeout=5)
        self.assertFalse(thread.is_alive())
        sets = [x for x in commands if x[0] == "SET"]
        self.assertIn("mode=blink", sets[1])
        self.assertIn("rgb=255,0,0", sets[2])
        self.assertIn("rgb=255,165,0", sets[3])
        self.assertEqual(sum(x[0] == "FLUSH" for x in commands), 4)

if __name__ == "__main__": unittest.main()
