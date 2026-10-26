# vm_windows.ps1
# Monitors local login sessions and Windows Update status, driving a configurable
# LED on the NAS LED server.
# (orange for pending updates, green for logged on users, red for no users logged on)
# Runs persistently; reconnects automatically on drop.
#
# Requirements: PowerShell 5.1+, no extra modules.

param(
    [string]$NasAddress        = '192.168.178.250',   # NAS LAN IP (where ugreen_leds_server listens)
    [int]   $NasPort           = 39001,
    [string]$LedName           = 'disk4',             # LED name to control on the NAS
    [int]   $PollSeconds       = 3,                   # how often to re-check login state
    [int]   $UpdatePollSeconds = 30,                  # how often to re-check Windows update state
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

$reqId   = 1
$backoff = 1000   # ms

while ($true) {
    try {
        Write-Host "[$(Get-Date -f 'HH:mm:ss')] Connecting to $NasAddress`:$NasPort ..."
        $tcp = New-Object Net.Sockets.TcpClient
        $tcp.Connect($NasAddress, $NasPort)
        $stream = $tcp.GetStream()
        $w = New-Object IO.StreamWriter($stream, [Text.Encoding]::ASCII)
        $r = New-Object IO.StreamReader($stream, [Text.Encoding]::ASCII)

        # Session init
        Send-Line $w "HELLO $reqId 1"; $reqId++
        $resp = Read-Line $r
        Write-Host "  HELLO -> $resp"
        if ($resp -notmatch '^OK') { throw "HELLO rejected: $resp" }

        $backoff = 1000  # reset on successful connect

        $lastColor = $null
        $lastUpdateCheck = [DateTime]::MinValue
        $cachedUpdateCount = 0

        while ($tcp.Connected) {
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

            if ($color -ne $lastColor) {
                Write-Host "[$(Get-Date -f 'HH:mm:ss')] $LedName -> $desc"
                Send-Line $w "SET $reqId $LedName mode=on rgb=$color brightness=200"
                $resp = Read-Line $r
                Write-Host "  SET   -> $resp"
                if ($resp -match '^OK') {
                    Send-Line $w "FLUSH $($reqId + 1)"; $reqId += 2
                    $resp = Read-Line $r
                    Write-Host "  FLUSH -> $resp"
                } else {
                    $reqId++
                }
                $lastColor = $color
            }

            Start-Sleep -Seconds $PollSeconds
        }
    }
    catch {
        Write-Host "[$(Get-Date -f 'HH:mm:ss')] Error: $_. Reconnecting in $([int]($backoff/1000))s ..."
    }
    finally {
        try { $tcp.Close() } catch {}
    }

    Start-Sleep -Milliseconds $backoff
    $backoff = [Math]::Min($backoff * 2, $BackoffMaxMs)
}
