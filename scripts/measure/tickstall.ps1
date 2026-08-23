# Is the frame tick being DELIVERED?
#
# Every other smoothness instrument in this project measures what the tick DID.
# This one exists for the fault where the tick was not called at all: on the
# title-bar stall (docs/titlebar-tick-stall.md) the handler max was 0.77ms
# against a period max of 512ms, so no cost counter moved -- and the HUD's own
# `stalls`/`hitch` read `0 of 0` because both of their sample sites are inside
# the SCRUB path and there is no drag in progress during playback. A dozen
# harness runs of this gesture therefore came back clean on an empty counter.
#
# WHAT TO READ, and it is one comparison:
#
#   sched ... | tick-late N of M (>1.5x) | tick-stall N (>100ms) | sizemove N max Xms
#   period last/avg/MAX | handler last/avg
#
#   period max >> handler max   ->  DELIVERY. The tick was not called.
#   period max ~= handler max   ->  WORK. The tick was called and took that long.
#
# `sizemove` is the subset delivered while DefWindowProc's modal move/size loop
# owned the message pump, so it is non-zero on a caption press and zero on an
# ordinary overrun. That field is the attribution; the count alone cannot make
# it.
#
# SYNTHETIC INPUT IS NOT THE OWNER'S HAND AND THIS SCRIPT DOES NOT PRETEND
# OTHERWISE. audiodrag.ps1 drove this same gesture across eleven configurations
# and reported a clean negative -- which, it turns out, is because it was
# reading the audio ring rather than the tick. This script's -Mode caption DOES
# fire the counters (measured 302.6ms, 1920x1200@59.999 Parsec-class display,
# against 512ms from a real hand on the physical panel), so treat it as a smoke
# test and a LOWER BOUND. A clean result here is not evidence the fault is gone;
# the owner's hand on the caption is the judge.
#
# -Mode idle is the control and is not optional. A stall count without a
# same-duration control run is a number, not a comparison.

param(
    [Parameter(Mandatory = $true)][string]$Clip,
    [ValidateSet('idle', 'caption')][string]$Mode = 'idle',
    [int]$Seconds = 11,
    # The HUD clips on a narrow window (the phase 12 diagnostic limitation) and
    # the sched line is long. Widening is free on this material and does not
    # move the video rect on portrait media; quote `win`/`display` from the run.
    [int]$Width = 2200,
    [string[]]$Env = @(),
    [string]$OutDir = "$env:TEMP\trace_tickstall"
)

Add-Type @"
using System;
using System.Runtime.InteropServices;
public class TS {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x,int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f,int dx,int dy,uint d,UIntPtr e);
  [DllImport("user32.dll")] public static extern void keybd_event(byte v,byte s,uint f,UIntPtr e);
}
"@

# SetForegroundWindow fails SILENTLY from a background process, and a run whose
# keys went somewhere else looks exactly like a feature that is missing. Verify
# by reading the foreground back, with the synthetic Alt tap as the unlock.
function Focus-Trace([IntPtr]$h) {
    [TS]::SetForegroundWindow($h) | Out-Null
    Start-Sleep -Milliseconds 250
    if ([TS]::GetForegroundWindow() -eq $h) { return $true }
    [TS]::keybd_event(0xA4,0,0,[UIntPtr]::Zero); [TS]::keybd_event(0xA4,0,2,[UIntPtr]::Zero)
    [TS]::SetForegroundWindow($h) | Out-Null
    Start-Sleep -Milliseconds 250
    return [TS]::GetForegroundWindow() -eq $h
}

New-Item -ItemType Directory -Force $OutDir | Out-Null

# A scratch INI for the reason cadence.ps1 gives: Loop left on re-establishes
# the playback timeline at every wrap and zeroes these counters with it.
$ini = Join-Path $OutDir "tickstall-scratch.ini"
Remove-Item $ini -ErrorAction SilentlyContinue
$Env = @($Env) + @("TRACE_SETTINGS_FILE=$ini")

& "$PSScriptRoot\restart.ps1" -Clip $Clip -Env $Env -SettleSeconds 5 | Out-Null
& "$PSScriptRoot\widen.ps1" -Width $Width | Out-Null
Start-Sleep -Milliseconds 800

$p = Get-Process -Name Trace -ErrorAction SilentlyContinue |
     Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $p) { Write-Output "TICKSTALL: no Trace window"; exit 1 }
$h = $p.MainWindowHandle
if (-not (Focus-Trace $h)) { Write-Output "TICKSTALL: could not take the foreground -- run void"; exit 1 }

(New-Object -ComObject WScript.Shell).SendKeys(" ")
Start-Sleep -Seconds 3

if ($Mode -eq 'caption') {
    # The REAL Windows caption, not Trace's own strip: the fault is specific to
    # DefWindowProc's modal loop, and a press on the picture does not reproduce
    # it (recorded in docs/titlebar-tick-stall.md).
    $r = New-Object TS+RECT
    [TS]::GetWindowRect($h, [ref]$r) | Out-Null
    $cx = [int](($r.L + $r.R) / 2); $cy = $r.T + 15
    [TS]::SetCursorPos($cx, $cy) | Out-Null
    Start-Sleep -Milliseconds 200
    [TS]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero)      # LEFTDOWN
    Start-Sleep -Milliseconds 200
    # A short crawl, then HOLD. The owner's reproduction needs no movement at
    # all; the crawl is only what reliably gets Windows into the move loop from
    # synthetic input.
    for ($i = 1; $i -le 40; $i++) {
        [TS]::SetCursorPos(($cx + $i * 2), $cy) | Out-Null
        Start-Sleep -Milliseconds 25
    }
    Start-Sleep -Seconds $Seconds
    [TS]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero)      # LEFTUP
    Start-Sleep -Milliseconds 800
} else {
    Start-Sleep -Seconds $Seconds
}

$png = Join-Path $OutDir ("tickstall_{0}.png" -f $Mode)
& "$PSScriptRoot\capture.ps1" -Out $png -HudOnly | Out-Null
Get-Process -Name Trace -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue

Write-Output ""
Write-Output "  mode $Mode -> $png"
Write-Output "  READ:  sched ... tick-late / tick-stall / sizemove max"
Write-Output "         period last/avg/MAX  against  handler last/avg"
Write-Output "         period max >> handler max = the tick was NOT CALLED."
Write-Output "  The smooth/drag line's stalls+hitch are DRAG counters and will"
Write-Output "  read 0 of 0 here. That is the blind spot, not a clean result."
