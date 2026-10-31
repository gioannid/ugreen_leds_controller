# vm_windows.ps1
# Monitors local login sessions and Windows Update status, driving a configurable
# LED on the NAS LED server.
# (orange for pending updates, green for logged on users, red for no users logged on)
# Runs persistently; reconnects automatically on any drop, including when the
# LED server itself restarts. A periodic heartbeat re-asserts the current state
# so a dead connection is detected even while the LED colour stays unchanged.
#
# Requirements: PowerShell 5.1+, no extra modules.

param(
    [string]$NasAddress        = '192.168.178.250',   # NAS LAN IP (where ugreen_leds_server listens)
    [int]   $NasPort           = 39001,
    [string]$LedName           = 'disk4',             # LED name to control on the NAS
    [int]   $PollSeconds       = 3,                   # how often to re-check login state
    [int]   $UpdatePollSeconds = 30,                  # how often to re-check Windows update state
    [int]   $HeartbeatSeconds  = 60,                  # re-assert state even when unchanged (liveness probe)
    [int]   $ConnectTimeoutMs  = 10000,               # max wait for a TCP connect (10 s)
    [int]   $SocketTimeoutMs   = 30000,               # max wait for a send/recv (30 s)
    [int]   $BackoffMaxMs      = 30000                # max reconnect delay (30 s)
)

function Get-ActiveUserCount {
    return @(quser 2>$null | Select-Object -Skip 1).Count
}

function Get-PendingUpdateCount {
    $count = 0
    $session = $null
    $searcher = $null
    try {
        $session = New-Object -ComObject Microsoft.Update.Session
        $searcher = $session.CreateUpdateSearcher()
        try { $searcher.Online = $false } catch {}
        $searchResult = $searcher.Search("IsHidden=0 and IsInstalled=0")
        $count = $searchResult.Updates.Count
    }
    catch {
        # COM or service unavailable (e.g. wuauserv stopped)
    }
    finally {
        if ($null -ne $searcher) {
            try { [System.Runtime.InteropServices.Marshal]::ReleaseComObject($searcher) | Out-Null } catch {}
        }
        if ($null -ne $session) {
            try { [System.Runtime.InteropServices.Marshal]::ReleaseComObject($session) | Out-Null } catch {}
        }
    }

    if ($count -eq 0) {
        try {
            $wuReboot  = Test-Path "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\WindowsUpdate\Auto Update\RebootRequired"
            $cbsReboot = Test-Path "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Component Based Servicing\RebootPending"
            $usoReboot = Test-Path "HKLM:\SOFTWARE\Microsoft\WindowsUpdate\Orchestrator\RebootRequired"
            if ($wuReboot -or $cbsReboot -or $usoReboot) {
                $count = 1
            }
        }
        catch {}
    }

    return $count
}

function Send-Line([IO.StreamWriter]$w, [string]$line) {
    $w.WriteLine($line)
    $w.Flush()
}

function Read-Line([IO.StreamReader]$r) {
    return $r.ReadLine()
}

function Connect-LedServer([string]$server, [int]$port, [int]$timeoutMs) {
    # Bounded TCP connect: a down NAS must not stall the retry loop for the
    # OS default (~21 s) on every attempt.
    $tcp = New-Object Net.Sockets.TcpClient
    try {
        $async = $tcp.BeginConnect($server, $port, $null, $null)
        if (-not $async.AsyncWaitHandle.WaitOne($timeoutMs)) {
            throw "connect to $server`:$port timed out after $([int]($timeoutMs / 1000))s"
        }
        $tcp.EndConnect($async)
    }
    catch {
        $tcp.Close()
        throw
    }
    return $tcp
}

function Push-LedState([IO.StreamWriter]$w, [IO.StreamReader]$r, [string]$led, [string]$color, [bool]$flush) {
    # SET (plus FLUSH when $flush) with strict response validation.
    # Throws on EOF (server closed the connection) or on any non-OK reply, so
    # the caller's catch block triggers a reconnect.
    $rid = $script:reqId; $script:reqId++
    Send-Line $w "SET $rid $led mode=on rgb=$color brightness=200"
    $resp = Read-Line $r
    Write-Host "  SET   -> $resp"
    if ($null -eq $resp) { throw "connection closed during SET (server restart?)" }
    if ($resp -notmatch "^OK $rid ACCEPTED") { throw "SET rejected: $resp" }

    if ($flush) {
        $rid = $script:reqId; $script:reqId++
        Send-Line $w "FLUSH $rid"
        $resp = Read-Line $r
        Write-Host "  FLUSH -> $resp"
        if ($null -eq $resp) { throw "connection closed during FLUSH" }
        if ($resp -notmatch "^OK $rid FLUSH") { throw "FLUSH failed: $resp" }
    }
}

$reqId   = 1
$backoff = 1000   # ms

while ($true) {
    $tcp       = $null
    $connected = $false
    try {
        Write-Host "[$(Get-Date -f 'HH:mm:ss')] Connecting to $NasAddress`:$NasPort ..."
        $tcp = Connect-LedServer $NasAddress $NasPort $ConnectTimeoutMs
        $stream = $tcp.GetStream()
        $w = New-Object IO.StreamWriter($stream, [Text.Encoding]::ASCII)
        $r = New-Object IO.StreamReader($stream, [Text.Encoding]::ASCII)
        # Bound the blocking I/O so a network partition (no FIN/RST ever
        # arrives) cannot hang the agent: a stalled reply surfaces as a
        # timeout and falls through to the reconnect path below.
        $tcp.SendTimeout    = $SocketTimeoutMs
        $tcp.ReceiveTimeout = $SocketTimeoutMs

        # Session init
        $rid = $reqId; $reqId++
        Send-Line $w "HELLO $rid 1"
        $resp = Read-Line $r
        Write-Host "  HELLO -> $resp"
        if ($null -eq $resp) { throw "connection closed during HELLO" }
        if ($resp -notmatch "^OK $rid HELLO") { throw "HELLO rejected: $resp" }
        $connected = $true

        $backoff = 1000  # reset on successful connect

        $lastColor         = $null   # forces re-assert of current state on (re)connect
        $lastUpdateCheck   = [DateTime]::MinValue
        $cachedUpdateCount = 0
        $lastPush          = [DateTime]::MinValue

        while ($true) {
            $now = [DateTime]::UtcNow
            if (($now - $lastUpdateCheck).TotalSeconds -ge $UpdatePollSeconds) {
                $cachedUpdateCount = Get-PendingUpdateCount
                $lastUpdateCheck = $now
            }

            $userCount   = Get-ActiveUserCount
            $updateCount = $cachedUpdateCount

            if ($updateCount -gt 0) {
                $color = '255,128,0'
                $userDesc = if ($userCount -gt 0) { "$userCount user(s) logged in" } else { 'no user logged in' }
                $desc  = "orange ($updateCount update(s) pending, $userDesc)"
            }
            elseif ($userCount -gt 0) {
                $color = '0,255,0'
                $desc  = "green ($userCount user(s) logged in)"
            }
            else {
                $color = '255,0,0'
                $desc  = 'red (no user logged in)'
            }

            $stateChanged = ($color -ne $lastColor)
            $heartbeatDue = (($now - $lastPush).TotalSeconds -ge $HeartbeatSeconds)

            if ($stateChanged -or $heartbeatDue) {
                if ($stateChanged) {
                    Write-Host "[$(Get-Date -f 'HH:mm:ss')] $LedName -> $desc"
                } else {
                    Write-Host "[$(Get-Date -f 'HH:mm:ss')] $LedName heartbeat ($desc)"
                }
                # Re-pushing the same state is a no-op for the server (no
                # hardware write), but the SET round-trip proves the socket
                # is alive: a dead connection (e.g. the LED server restarted)
                # surfaces here as an error/timeout and triggers a reconnect.
                Push-LedState -w $w -r $r -led $LedName -color $color -flush $stateChanged
                $lastColor = $color
                $lastPush  = [DateTime]::UtcNow
            }

            Start-Sleep -Seconds $PollSeconds
        }
    }
    catch {
        if ($connected) {
            Write-Host "[$(Get-Date -f 'HH:mm:ss')] Connection lost: $_. Reconnecting in $([int]($backoff / 1000))s ..."
        }
        else {
            Write-Host "[$(Get-Date -f 'HH:mm:ss')] Connect failed: $_. Retrying in $([int]($backoff / 1000))s ..."
        }
    }
    finally {
        if ($null -ne $tcp) { try { $tcp.Close() } catch {} }
    }

    Start-Sleep -Milliseconds $backoff
    $backoff = [Math]::Min($backoff * 2, $BackoffMaxMs)
}
