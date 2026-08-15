# A/B the read-ahead worker (TRACE_IO_READAHEAD) against the shipping
# per-request async path, both under the same synthetic bandwidth throttle
# (TRACE_IO_INJECT_KBPS), on a LOCAL file forced through the async worker
# via TRACE_REMOTE_IO=1.
#
# This is a SYNTHETIC comparison. It reproduces a slow-link *read pattern*
# on local media -- it does not reproduce a real cold LucidLink profile,
# which was previously calibrated against one live comparison that this
# session has no way to repeat (no live remote access). Do not quote these
# numbers as LucidLink performance; they are only valid as a relative A/B
# between read-ahead on and off under the identical synthetic link.
#
# Reads results from %TEMP%\trace_iolog.txt (TRACE_IO_LOG=1), which the app
# appends to on file close -- so this script closes Trace itself rather than
# relying on restart.ps1's next invocation to do it, and reads the last line
# after each run.

param(
    [Parameter(Mandatory = $true)][string]$Clip,
    [string]$Exe,
    [int]$Kbps = 0,               # bandwidth CAP, kbit/s (0 = no cap)
    [int]$DelayMs = 0,            # fixed per-read-call latency, ms (0 = none)
    [int]$Seconds = 10,
    [int]$CapMB = 24
)

$measure = $PSScriptRoot
$logPath = Join-Path $env:TEMP "trace_iolog.txt"

function Run-One([string]$label, [string[]]$extraEnv) {
    if (Test-Path $logPath) { Remove-Item $logPath -Force }
    # TRACE_RT_DROP=0: the real-time playback drop (2026-08-13) asks for a
    # JUMP -- real random access -- whenever decode+I/O together miss the
    # frame deadline, and on intra-only media a jump is exactly what a
    # read-ahead rebase treats as "this is a seek, throw the buffer away".
    # That is a real, separate mechanism and not what this experiment is
    # measuring; disable it so a throttle heavy enough to miss real time
    # reads as I/O latency rather than as a cascade of forced seeks.
    $envArgs = @("TRACE_REMOTE_IO=1", "TRACE_IO_LOG=1", "TRACE_TRANSPORT_BAR=1", "TRACE_NO_AUDIO=1", "TRACE_RT_DROP=0") + $extraEnv
    $args = @{ Clip = $Clip; Env = $envArgs; SettleSeconds = 3 }
    if ($Exe) { $args.Exe = $Exe }
    & "$measure\restart.ps1" @args | Out-Null
    & "$measure\play.ps1" -Seconds $Seconds | Out-Null

    $p = Get-Process -Name Trace -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($p) {
        $p.CloseMainWindow() | Out-Null
        $waited = 0
        while (-not $p.HasExited -and $waited -lt 5000) { Start-Sleep -Milliseconds 200; $waited += 200; $p.Refresh() }
        if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
    }
    Start-Sleep -Milliseconds 300

    if (-not (Test-Path $logPath)) { Write-Output "$label : NO LOG WRITTEN"; return }
    $line = Get-Content $logPath -Tail 1
    Write-Output "$label : $line"
}

$throttleEnv = @()
if ($Kbps -gt 0) { $throttleEnv += "TRACE_IO_INJECT_KBPS=$Kbps" }
if ($DelayMs -gt 0) { $throttleEnv += "TRACE_IO_INJECT_DELAY_MS=$DelayMs" }

Write-Output "=== SYNTHETIC A/B: cap ${Kbps}kbps delay ${DelayMs}ms, $Seconds s play, readahead cap ${CapMB}MB ==="
Run-One "off (legacy per-request)" $throttleEnv
Run-One "readahead              " ($throttleEnv + @("TRACE_IO_READAHEAD=1", "TRACE_IO_READAHEAD_MB=$CapMB"))

Get-Process -Name Trace -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
