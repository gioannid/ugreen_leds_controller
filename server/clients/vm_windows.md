# Windows VM LED Monitor client

## Overview

| Signal | Source | LED state |
|---|---|---|
| VM **pending Windows updates** (download/install/reboot) | Windows 11 VM (`[access.win]`) | `$LedName` **solid orange** |
| VM booted, **no user logged in** | Windows 11 VM (`[access.win]`) | `$LedName` **solid red** |
| VM **≥1 user logged in** | Windows 11 VM (`[access.win]`) | `$LedName` **solid green** |
| VM **powered off / TCP connection dropped** | Server fallback on disconnect | `$LedName` **off** |

No PVE-side script is needed. The server's built-in **disconnect-fallback** mechanism handles the "VM is down" case automatically: when the Windows agent's TCP connection drops (because the VM shut down, crashed, or rebooted), the server releases ownership of `$LedName` and applies the configured `[fallback]` state — `mode=off` — without any external intervention.

This means the only moving part is the **single PowerShell agent** running inside the VM.

---

## 1. `server.conf` snippets

Only two changes are needed:

```ini
# Windows 11 VM — controls $LedName only
[access.win]
source=192.168.178.103/32
leds=$LedName
verbs=CAPS,GET,SET,RESET,FLUSH
priority=10
max_connections=2
```

```ini
[fallback]
# ...other LEDs...
$LedName=mode=off    # applied automatically when the Windows agent disconnects
```

---

## 2. Windows 11 VM — PowerShell persistent agent

Copy `vm_windows.ps1` to `C:\ProgramData` and run via a
**Task Scheduler** task at system startup (runs as SYSTEM, before logon).



### Task Scheduler setup (run at startup, as SYSTEM)

```powershell
# Run once as administrator to register the task:
$action  = New-ScheduledTaskAction -Execute 'C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe' `
               -Argument '-NoProfile -NonInteractive -WindowStyle Hidden -ExecutionPolicy Bypass -File "C:\ProgramData\vm_windows.ps1"'
$trigger = New-ScheduledTaskTrigger -AtStartup
$settings = New-ScheduledTaskSettingsSet -RestartCount 10 -RestartInterval (New-TimeSpan -Minutes 1) `
               -ExecutionTimeLimit ([TimeSpan]::Zero)
Register-ScheduledTask -TaskName 'UgreenLedAgent' -Action $action -Trigger $trigger `
    -RunLevel Highest -User 'SYSTEM' -Settings $settings -Force
```

> [!IMPORTANT]
> The task must start **before user logon** (trigger = at startup, user = SYSTEM).
> `quser` works from SYSTEM context and sees all sessions including the locked/login-screen
> state. Running as SYSTEM also permits querying the Windows Update COM API
> (`Microsoft.Update.Session`) and pending reboot registry keys without requiring interactive user elevation.

---

## 3. How the fallback handles VM shutdown

```
┌────────────────────────────────────────────────────────────────┐
│ Scenario                  │ $LedName owner    │ $LedName state        │
│───────────────────────────│───────────────────│───────────────────────│
│ VM running, updates pend. │ Windows agent     │ solid ORANGE          │
│ VM running, user logged in│ Windows agent     │ solid GREEN           │
│ VM running, at login scrn │ Windows agent     │ solid RED             │
│ VM shuts down / crashes   │ — (fallback)      │ OFF  ← automatic      │
│ VM reboots (briefly down) │ — (fallback)      │ OFF  ← automatic      │
│ Windows agent PS error    │ — (fallback)      │ OFF  ← automatic      │
└────────────────────────────────────────────────────────────────┘
```

When the VM shuts down or crashes, the OS closes all TCP sockets. The server
detects the dropped connection, releases ownership of `$LedName`, and immediately
applies `[fallback] $LedName=mode=off` — no polling, no delay, no PVE involvement.

When the VM boots again and the Windows agent reconnects and sends its first
`SET`, it re-acquires ownership and the colour reappears.
