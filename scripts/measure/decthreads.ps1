# Sweep TRACE_DECODE_THREADS on one clip and report decode cost, throughput and CPU.
#
# CPU is sampled across the PLAY WINDOW ONLY -- process TotalProcessorTime is
# cumulative, so a snapshot taken before and after the play span and differenced
# is the only figure that describes the run rather than the whole session
# (which includes open, streaminfo and the first decode).
#
# Reported as "cores busy" = cpu-seconds / wall-seconds, because a percentage of
# a 32-thread machine is the number people misread. 32.0 means the box is
# saturated; 8.0 means eight of sixteen physical cores' worth of work.
param(
    [Parameter(Mandatory = $true)][string]$Clip,
    [Parameter(Mandatory = $true)][int[]]$Threads,
    [string]$Exe,
    [int]$Seconds = 11,
    [string[]]$ExtraEnv = @(),
    [string]$OutDir = "$env:TEMP\trace_decthreads",
    [string]$Tag = "run"
)

$measure = $PSScriptRoot
New-Item -ItemType Directory -Force $OutDir | Out-Null

Add-Type @"
using System;
using System.Runtime.InteropServices;
public class FgT { [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h); }
"@

foreach ($t in $Threads) {
    $env0 = @("TRACE_TRANSPORT_BAR=1") + $ExtraEnv
    # -1 means "do not set the variable at all", i.e. the shipping default.
    if ($t -ge 0) { $env0 += "TRACE_DECODE_THREADS=$t" }
    $label = if ($t -lt 0) { "default" } else { "$t" }

    $args = @{ Clip = $Clip; Env = $env0; SettleSeconds = 8 }
    if ($Exe) { $args.Exe = $Exe }
    & "$measure\restart.ps1" @args | Out-Null

    $p = Get-Process -Name Trace -ErrorAction SilentlyContinue |
         Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
    if (-not $p) { Write-Output "$label : no window"; continue }

    [FgT]::SetForegroundWindow($p.MainWindowHandle) | Out-Null
    Start-Sleep -Milliseconds 400

    # Snapshot, play, snapshot. Refresh() is required or the cached value is
    # returned and every reading is identical.
    $p.Refresh()
    $cpu0 = $p.TotalProcessorTime
    $sw = [Diagnostics.Stopwatch]::StartNew()
    (New-Object -ComObject WScript.Shell).SendKeys(" ")
    Start-Sleep -Seconds $Seconds
    $sw.Stop()
    $p.Refresh()
    $cpu1 = $p.TotalProcessorTime
    $ws = [math]::Round($p.WorkingSet64 / 1MB, 0)

    $cores = ($cpu1 - $cpu0).TotalSeconds / $sw.Elapsed.TotalSeconds
    $png = Join-Path $OutDir ("{0}_{1}.png" -f $Tag, $label)
    & "$measure\capture.ps1" -Out $png | Out-Null
    Write-Output ("{0,-8} cores {1,5:N1} / 32  ({2,4:N0}%)  ws {3} MB  -> {4}" -f `
        $label, $cores, ($cores / 32 * 100), $ws, (Split-Path $png -Leaf))
}
Get-Process -Name Trace -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
