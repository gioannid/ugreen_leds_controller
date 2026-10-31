# OpenMediaVault VM LED client

`omv_vm.py` runs INSIDE the OpenMediaVault VM at 192.168.178.102.
It controls `netdev` on the UGREEN LED server, default 192.168.178.250:39001.
Python 3.8+ and OMV's installed `/usr/sbin/omv-rpc` are required; no pip
packages, WebUI password, HTTP login or inbound listener are needed.

## State and precedence

Highest matching condition wins:

| Condition | LED |
| --- | --- |
| Guest agent disconnected / VM down | Off, via LED server fallback |
| OMV background tasks registered by Exec.enumerate | Blinking green, 500ms/500ms |
| Configured filesystem missing/unmounted, enabled service stopped, SMART assessment other than GOOD | Solid red |
| Updates, reboot required, unapplied configuration, external actionable alerts, or monitoring unknown | Solid orange (255,165,0) |
| Complete successful checks, none of the above | Solid green |

Activity overrides faults to match the requested background-process rule.
Consequently a genuine fault can be hidden while a background task runs.
After activity finishes the agent recomputes the health state.
Disabled services are ignored by default: an intentionally disabled service is
not a failure. Use --all-services if even disabled services must be running.
Empty filesystem or SMART lists count as no exposed items to fail, not proof
that the host's physical storage is healthy. A VM needs device/controller
passthrough for OMV to see physical SMART health.

## Data sources

- Exec.enumerate: the actual OMV registered background processes, not a ps,
  CPU-load, cron, apt-lock or arbitrary Linux process heuristic. Finished tasks
  are not counted. The agent does not attach to tasks or consume their output.
- FileSystemMgmt.getList (synchronous): checks every configured filesystem,
  including missing ones; status must be 1 and mounted must be true. Using only
  enumerateMountedFilesystems would incorrectly omit missing storage.
- Services.getStatus: enabled/running states for dashboard service modules.
- Smart.enumerateDevices: overallstatus must equal GOOD for each exposed device,
  so warnings such as BAD_SECTOR are red as well as outright failures.
- System.getInformation: availablePkgUpdates, rebootRequired, configDirty.
  Updates reflect OMV's existing cached package information; this agent never
  runs apt update or changes packages/configuration.

The code was developed against OMV upstream source inspected in October 2026.
No installed OMV major version was supplied. It validates expected fields and
fails orange on incompatible responses instead of inventing a healthy state.
Run --check on your VM before enabling the service.

## Notification limitation and optional actionable-alert input

The WebUI NotificationService stores notification objects in a browser-memory
BehaviorSubject. There is no global backend inbox corresponding to every
browser's sidebar. Notification.getList returns notification SETTINGS, not
pending alerts, and is deliberately not used here. Thus this client directly
covers backend conditions (updates, reboot, dirty configuration, service,
filesystem and SMART issues), but cannot automatically mirror arbitrary
browser-only errors or plugin notifications. Dismissing an informational toast
is not interpreted as a required remedy.

For additional actionable conditions, a trusted local integration may maintain
an optional JSON array of message strings, e.g.:

```json
["Backup failed; inspect last backup log"]
```

Set `OMV_ALERTS_FILE=/run/omv-led-alerts.json` in /etc/default/omv-led-agent.
A nonempty array means orange unless red or activity takes precedence. Clear
resolved conditions by atomically replacing the file with `[]`. Use a root-owned
file and atomic rename, not truncate-and-write. Once configured, a missing or
malformed file fails orange; it is never silently treated as an empty inbox.
This input requires a producer; the client does not install a notification hook
or claim to collect browser notifications automatically.

## LED-server configuration (NAS / Proxmox host)

Merge the following into the EXISTING server configuration. Do not create
second [server] or [fallback] sections or duplicate source-network rules:

```ini
[server]
tcp_bind=192.168.178.250
tcp_port=39001
init_to_fallback=true

[access.omv-agent]
source=192.168.178.102/32
leds=netdev
verbs=CAPS,GET,SET,RESET,FLUSH
priority=10
max_connections=1

[fallback]
netdev=mode=off
```

Keep all existing unrelated settings, access rules and LED fallbacks. Restart
the LED server after editing its configuration. Only this agent should own
netdev. The most-specific source rule replaces broader LAN rules; source-IP
translation/NAT requires using the actual peer address seen by the LED server.
Restrict TCP/39001 to trusted sources using the host firewall: this protocol
has neither encryption nor in-band authentication.

The off fallback is REQUIRED: a guest cannot send an off command after sudden
power loss. The server restores off when the owning TCP connection disconnects,
and at startup with init_to_fallback=true. Graceful shutdown closes the
connection. Agent failure/network loss also produces off, not just VM shutdown.
Hard VM loss or a blackholed network can leave a stale LED until the server
notices TCP failure. The server has no ownership lease/expiry. Configure/test
server-side TCP keepalive or add an independent host/Proxmox watchdog if a
bounded VM-down detection deadline is required. Client heartbeats alone cannot
guarantee a server-side disconnect deadline when the client disappears.

## Install inside 192.168.178.102

From the repository/archive root, as root:

```sh
install -m 0755 server/clients/omv_vm.py /usr/local/bin/omv_vm.py
install -m 0644 server/systemd/omv-led-agent.service /etc/systemd/system/
/usr/bin/python3 /usr/local/bin/omv_vm.py --check
systemctl daemon-reload
systemctl enable --now omv-led-agent.service
journalctl -u omv-led-agent.service -f
```

The service runs as root for local administrative OMV RPC access. OMV engine
performs the actual health queries; the client only reads statuses. The unit
intentionally does not isolate /tmp because OMV IPC/status files must remain
visible. It does not require the VM to bind its specific IP locally; the IP is
used in the LED server's access rule.

Optional /etc/default/omv-led-agent (shell-style systemd environment file):

```sh
LED_HOST=192.168.178.250
LED_PORT=39001
LED_NAME=netdev
# OMV_ALERTS_FILE=/run/omv-led-alerts.json
```

Restart the agent after changing environment configuration. For CLI-only
settings use `systemctl edit omv-led-agent` and replace ExecStart, first clearing
it with an empty ExecStart= line. See `omv_vm.py --help` for all settings.

## Timing, recovery and diagnostics

Default polling is 10s after a completed snapshot, each RPC has a 15s timeout,
and SMART is refreshed every 300s to avoid unnecessary repeated disk probes.
SMART enumeration may wake disks, like equivalent dashboard queries; increase
--smart-interval if needed. SMART state can be stale for that interval. Activity
is checked before expensive checks and again before final state selection;
short tasks entirely between polls can be missed. While busy, full health
queries wait until activity ends. A polling thread and a separate LED loop
prevent RPC delays from blocking LED connection maintenance. Snapshots older
than 120s fail orange. RPC/schema/read errors also fail orange, not green/off.

A persistent TCP session performs HELLO, CAPS, SET and FLUSH with request-ID
validation, socket deadlines, complete state reassertion on reconnect and
bounded exponential retry with jitter. State is refreshed at least every 15s
while connected. Only netdev is modified by default. SIGTERM/SIGINT close the
connection and release the LED to fallback; service stop therefore shows off.

Run these non-destructive checks:

```sh
python3 server/clients/test_omv_vm.py
sudo /usr/local/bin/omv_vm.py --check
systemctl status omv-led-agent
```

Verify --check against your dashboard, observe an existing real background
task, and test stopping/starting the agent. Test a normal VM shutdown and a
forced shutdown separately to measure TCP disconnect latency. Do not deliberately
fail production disks, filesystems or services just to test the indicator.
The supplied automated tests use synthetic statuses and a localhost fake LED
server; they do not validate your real OMV VM, NAS hardware or service sandbox.

## Source references

UGREEN: server/PROTOCOL.md, server/examples/ugreen-leds-server.conf,
server/clients/jellyfin_lxc.py and server/systemd/jellyfin-led-agent.service in
https://github.com/gioannid/ugreen_leds_controller (inspected commit
ba7973b0b585d69eaf754d68e4b7a61a245f2dce).

OMV upstream: https://github.com/openmediavault/openmediavault
- deb/openmediavault/usr/sbin/omv-rpc
- deb/openmediavault/usr/share/openmediavault/engined/rpc/{exec,system,services,filesystemmgmt,smart,notification}.inc
- deb/openmediavault/usr/share/openmediavault/engined/module/ssh.inc
- deb/openmediavault/usr/share/php/openmediavault/system/storage/smartinformation.inc
- deb/openmediavault/workbench/src/app/shared/services/notification.service.ts
