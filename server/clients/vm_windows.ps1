# vm_windows.ps1
# Monitors local login sessions and drives a configurable LED on the NAS LED server.
# (red for no users logged on, green for logged on users)
# Runs persistently; reconnects automatically on drop.
#
# Requirements: PowerShell 5.1+, no extra modules.

param(
    [string]$NasAddress   = '192.168.178.250',   # NAS LAN IP (where ugreen_leds_server listens)
    [int]   $NasPort      = 39001,
    [string]$LedName      = 'disk4',             # LED name to control on the NAS
    [int]   $PollSeconds  = 3,                   # how often to re-check login state
    [int]   $BackoffMaxMs = 30000                # max reconnect delay (30 s)
)

#function Get-ActiveUserCount {
#    # query=(0) means query all sessions; State 'Active' = logged-in interactive session
#    $sessions = query session 2>$null
#    $active = $sessions | Where-Object { $_ -match 'Active' }
#    return @($active).Count
#}

function Get-ActiveUserCount {
    return @(quser 2>$null | Select-Object -Skip 1).Count
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

        while ($tcp.Connected) {
            $userCount = Get-ActiveUserCount

            $color = if ($userCount -gt 0) { '0,255,0' } else { '255,0,0' }
            $desc  = if ($userCount -gt 0) { "green ($userCount user(s) logged in)" } else { 'red (no user logged in)' }

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
