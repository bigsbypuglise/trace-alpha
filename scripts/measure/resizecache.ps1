# What does an INTERACTIVE RESIZE actually cost -- spec phase 12's first experiment.
#
# Spec section 2 item 7 predicts a frame-cache thrash under continuous
# aspect-locked drag-resizing and names two independent costs:
#
#   (a) reclaimDecoder() on EVERY resize event, changed size or not;
#   (b) the cache clear, on a real size change only -- which during a drag is
#       every event.
#
# Both are predictions. Neither has ever been measured, and the aspect lock is
# about to make continuous size changes the normal case rather than the fast
# one, so the number has to exist before the design does.
#
# THE COST IS THE ENTRIES DISCARDED, NOT THE CLEARS. Clearing an already-empty
# cache is free, so a drag that "clears the cache sixty times" may have thrown
# away sixty entries or one -- and only the first clear of a run can discard
# anything, because nothing refills it while the pointer is still down. That is
# why setScrubPreviewSize returns a count and the HUD prints `drop`, and it is
# why this script has a leg with NOTHING IN THE CACHE: without it, a large
# `drop` and a small one are indistinguishable from a script that never filled
# the cache in the first place.
#
# The fill is a playback run rather than a scrub drag on purpose. A drag needs
# the docked groove (TRACE_TRANSPORT_BAR=1) or the overlay's track, and the
# thing being measured here is a WINDOW gesture -- putting a transport
# dependency in front of it would mean the two legs differed in the transport as
# well as in the cache.
#
# Read, from the HUD's geometry line:
#   resize N   resizeEvent count
#   chg M      how many of those reached a real preview-size change
#   drop D     how many cache ENTRIES those discarded   <- the cost
#   sync T/Xms total and worst time inside syncScrubPreviewSize itself
#   wm S/E/X   WM_SIZING / WM_ENTERSIZEMOVE / WM_EXITSIZEMOVE
#
# and `ui | gap ... (avg/max)`, which is how long the window could not service a
# mouse move or a repaint. Quote `win WxH` AND `display WxH` with any of it: the
# resize is changing both, which is the whole point.

param(
    [Parameter(Mandatory = $true)][string]$Clip,
    # "fill"   play first, so the cache holds entries when the drag starts
    # "empty"  drag with nothing cached -- the control that says `drop` measures
    #          the cache and not the script
    # "both"   run each in turn (default)
    [ValidateSet("fill", "empty", "both")][string]$Mode = "both",
    [string]$Renderer = "d3d11",
    [double]$FillSeconds = 3.0,
    [double]$SweepSeconds = 1.6,
    # How far the dragged corner travels, in logical px, in and back out again.
    [int]$Travel = 320,
    [string]$OutDir = "$env:TEMP\trace_resizecache"
)

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;using System.Runtime.InteropServices;
public class RZ {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x,int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f,uint x,uint y,uint d,IntPtr e);
  public const uint DOWN=0x0002, UP=0x0004;
}
"@

New-Item -ItemType Directory -Force $OutDir | Out-Null

function Get-TraceWindow {
    $p = Get-Process -Name Trace -ErrorAction SilentlyContinue |
         Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
    if (-not $p) { return $null }
    return $p.MainWindowHandle
}

function Get-Rect([IntPtr]$h) {
    $r = New-Object RZ+RECT
    [RZ]::GetWindowRect($h, [ref]$r) | Out-Null
    return $r
}

function Send-Key([string]$keys) {
    [System.Windows.Forms.SendKeys]::SendWait($keys)
}

# Spin rather than sleep, the way scrub.ps1 and overlay_drag.ps1 do: a synthetic
# gesture that teleports and pauses does not produce the message rate a real
# hand does, and the message rate is exactly what is being counted here.
function Sweep-Corner([int]$fromX, [int]$fromY, [int]$toX, [int]$toY, [double]$secs) {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($true) {
        $t = $sw.Elapsed.TotalSeconds / $secs
        if ($t -ge 1.0) { break }
        [RZ]::SetCursorPos([int]($fromX + ($toX - $fromX) * $t),
                           [int]($fromY + ($toY - $fromY) * $t)) | Out-Null
        $spin = [Diagnostics.Stopwatch]::StartNew()
        while ($spin.Elapsed.TotalMilliseconds -lt 4) { }
    }
    [RZ]::SetCursorPos($toX, $toY) | Out-Null
}

function Grab([string]$path) {
    & "$PSScriptRoot\capture.ps1" -Out $path -HudOnly | Out-Null
}

# THE HUD IS NOT REBUILT BY A RESIZE, AND THE FIRST RUN OF THIS SCRIPT READ THE
# PRE-DRAG TEXT AND CALLED IT A DRAG THAT NEVER HAPPENED.
#
# refreshHud() runs on transport actions and playback ticks; nothing calls it on
# resizeEvent. So a paused window that is resized redraws the HUD at the new
# size with the OLD string in it -- `win`, `display` and these counters all
# stale -- and the run reported `wm 0/0/0` while its own capture was 200px
# narrower than the shot before it. Fifth stale instrument in five phases.
#
# The fix is here rather than in resizeEvent on purpose: adding a refreshHud()
# there would put several hundred bytes of QString construction on the very path
# this script exists to price, and the first measurement of a path must not
# include the instrument. A short play run refreshes the HUD through the
# playback tick, AFTER a real paint -- which matters, because `display` is
# measured BY the paint (phase 10) and a refresh scheduled before one reports
# the previous size. It adds cache inserts, so read `drop` and not `cache N/M`
# for the cache answer; `drop` is cumulative and a later play cannot change it.
function Refresh-Hud {
    Send-Key " "
    Start-Sleep -Milliseconds 500
    Send-Key " "
    Start-Sleep -Milliseconds 400
}

Add-Type -AssemblyName System.Windows.Forms

$modes = if ($Mode -eq "both") { @("empty", "fill") } else { @($Mode) }

foreach ($leg in $modes) {
    Write-Output ""
    Write-Output "=== leg: $leg ==="

    & "$PSScriptRoot\restart.ps1" -Clip $Clip -Env @("TRACE_RENDERER=$Renderer") | Out-Null
    $h = Get-TraceWindow
    if (-not $h) { Write-Output "no window"; exit 1 }
    [RZ]::SetForegroundWindow($h) | Out-Null
    Start-Sleep -Milliseconds 600

    if ($leg -eq "fill") {
        # Play, then stop. Presented frames enter the reverse cache, so this
        # leaves real entries for the resize to discard.
        Send-Key " "
        Start-Sleep -Seconds $FillSeconds
        Send-Key " "
        Start-Sleep -Milliseconds 600
    }

    Grab "$OutDir\$leg-1-before.png"

    $r = Get-Rect $h
    # Inside the outer rect by 2px: that is the resize border, not the client
    # area. Landing in the client area would start an overlay drag instead and
    # the run would look like a resize that produced no WM_SIZING at all.
    $cornerX = $r.R - 2
    $cornerY = $r.B - 2
    [RZ]::SetCursorPos($cornerX, $cornerY) | Out-Null
    Start-Sleep -Milliseconds 250

    [RZ]::mouse_event([RZ]::DOWN, 0, 0, 0, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 80
    # In, then back out. One direction alone would leave the window at a
    # different size for each leg and make the two `display` figures
    # incomparable; in-and-out returns it to roughly where it started.
    Sweep-Corner $cornerX $cornerY ($cornerX - $Travel) ($cornerY - [int]($Travel * 0.6)) ($SweepSeconds / 2)
    Sweep-Corner ($cornerX - $Travel) ($cornerY - [int]($Travel * 0.6)) $cornerX $cornerY ($SweepSeconds / 2)
    [RZ]::mouse_event([RZ]::UP, 0, 0, 0, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 700

    Refresh-Hud
    Grab "$OutDir\$leg-2-after.png"

    $r2 = Get-Rect $h
    Write-Output ("  window  {0}x{1} -> {2}x{3}" -f ($r.R - $r.L), ($r.B - $r.T), ($r2.R - $r2.L), ($r2.B - $r2.T))
    Write-Output "  captures: $OutDir\$leg-1-before.png  $OutDir\$leg-2-after.png"
    Write-Output "  read `resize/chg/drop/sync/wm` off the geometry line, and `cache N/M` off the cache line."
}

Write-Output ""
Write-Output "Both legs share one binary and one gesture; the only difference is whether"
Write-Output "the cache had anything in it when the drag started. If `drop` does not"
Write-Output "separate them, the fill did not work -- check `cache N/M` on the -1-before shots."
