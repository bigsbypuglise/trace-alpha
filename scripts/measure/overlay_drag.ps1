# What does the floating transport cost DURING A DRAG?
#
# `overlay_cost.ps1` answers a different and easier question: a static overlay
# held visible through a playback run. That was the right test for the spike --
# it proves the atlas is not re-rasterised or re-uploaded per frame -- and it is
# the wrong test for the shipping path, because the case that can actually hurt
# is the one where the overlay and the scrub worker want the same UI thread:
#
#   - the fade runs on a 16ms QTimer and asks for a repaint on every tick;
#   - pointer motion reveals it, so a drag re-arms that timer continuously;
#   - and a drag is the one gesture whose whole quality is a UI-thread property.
#
# THE CONTROL IS NOT "the same drag with the overlay off". There is no overlay
# track to drag when it is off, so that comparison measures a drag against no
# drag at all -- which it duly did on the first run here, reporting `paints 0/1`
# and a playhead that never moved. The control is the VALIDATED TRANSPORT-GROOVE
# DRAG: the same reversal sequence, the same sweep durations, the same 4ms
# spin-paced pointer steps, driving the same async scrub path. What differs is
# the input route and whether a fade is running.
#
# Read `ui gap max` -- how long the window could not service a mouse move or a
# repaint -- and `hitch`, which is threshold-independent. NOT `stalls`: it is
# measured against the refresh rate and this box has been observed at both
# 239.999Hz and 60Hz.
#
# The overlay panel is located by DIFFERENCE rather than by assuming a y offset:
# one capture with it faded out, one after revealing it, and the bounding box of
# what changed is the panel. Guessing an offset is what the groove scan in
# scrub.ps1 already had to stop doing twice.

param(
    [Parameter(Mandatory = $true)][string]$Clip,
    [string]$Renderer = "d3d11",
    [string]$OutDir = "$env:TEMP\trace_overlay_drag"
)

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;using System.Runtime.InteropServices;
public class OD {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x,int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f,uint x,uint y,uint d,IntPtr e);
  public const uint DOWN=0x0002, UP=0x0004;
}
"@

New-Item -ItemType Directory -Force $OutDir | Out-Null

function Grab([int]$L, [int]$T, [int]$W, [int]$H) {
    $bmp = New-Object System.Drawing.Bitmap $W, $H
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($L, $T, 0, 0, $bmp.Size)
    $g.Dispose()
    return $bmp
}

# scrub.ps1's sweep, verbatim in shape: spin rather than sleep, because a
# synthetic drag that teleports and pauses overstates how well the shuttle keeps
# up.
function Sweep([int]$from, [int]$to, [int]$y, [double]$secs) {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($true) {
        $t = $sw.Elapsed.TotalSeconds / $secs
        if ($t -ge 1.0) { break }
        [OD]::SetCursorPos([int]($from + ($to - $from) * $t), $y) | Out-Null
        $spin = [Diagnostics.Stopwatch]::StartNew()
        while ($spin.Elapsed.TotalMilliseconds -lt 4) { }
    }
    [OD]::SetCursorPos($to, $y) | Out-Null
}

foreach ($mode in @("on", "off")) {
    $envs = @("TRACE_RENDERER=$Renderer")
    # SPEC PHASE 6 INVERTED THE DEFAULT. The overlay used to be opt-in and the
    # docked bar always present, so "on" needed a knob and "off" needed none.
    # The overlay is the transport now and the bar is what has to be asked for --
    # and asking for it is what puts a groove on screen for the control leg to
    # find. Setting nothing here would give BOTH legs the overlay and no groove,
    # which would fail as "groove not found" rather than silently, but only
    # because the scan happens to assert itself.
    if ($mode -eq "off") { $envs += "TRACE_TRANSPORT_BAR=1" }
    & "$PSScriptRoot\restart.ps1" -Clip $Clip -Env $envs | Out-Null

    $p = Get-Process -Name Trace | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
    if (-not $p) { Write-Output "no window"; exit 1 }
    $h = $p.MainWindowHandle
    [OD]::SetForegroundWindow($h) | Out-Null
    Start-Sleep -Milliseconds 600
    $r = New-Object OD+RECT
    [OD]::GetWindowRect($h, [ref]$r) | Out-Null
    $winW = $r.R - $r.L; $winH = $r.B - $r.T
    $cx = [int]($winW / 2)

    if ($mode -eq "on") {
        # Park away from the panel and wait past the 2s auto-hide, so the
        # "hidden" reference really is hidden.
        [OD]::SetCursorPos($r.L + $cx, $r.T + 80) | Out-Null
        Start-Sleep -Milliseconds 2600
        $before = Grab $r.L $r.T $winW $winH
        for ($i = 0; $i -lt 8; $i++) {
            [OD]::SetCursorPos($r.L + $cx + $i, $r.T + 80) | Out-Null
            Start-Sleep -Milliseconds 60
        }
        Start-Sleep -Milliseconds 400
        $after = Grab $r.L $r.T $winW $winH

        # Bounding box of what changed, restricted to the video band so the
        # HUD's own churn cannot claim the box.
        $bandTo = [int]($winH * 0.55)
        $minX = $winW; $maxX = -1; $minY = $winH; $maxY = -1
        for ($y = 0; $y -lt $bandTo; $y += 2) {
            for ($x = 0; $x -lt $winW; $x += 2) {
                $ca = $before.GetPixel($x, $y); $cb = $after.GetPixel($x, $y)
                $d = [Math]::Max([Math]::Abs($ca.R - $cb.R),
                     [Math]::Max([Math]::Abs($ca.G - $cb.G), [Math]::Abs($ca.B - $cb.B)))
                if ($d -gt 12) {
                    if ($x -lt $minX) { $minX = $x }; if ($x -gt $maxX) { $maxX = $x }
                    if ($y -lt $minY) { $minY = $y }; if ($y -gt $maxY) { $maxY = $y }
                }
            }
        }
        $before.Dispose(); $after.Dispose()
        if ($maxX -lt 0) { Write-Output "overlay never appeared"; exit 1 }
        Write-Output ("panel bbox x {0}..{1}  y {2}..{3}" -f $minX, $maxX, $minY, $maxY)

        # Track at 76% of panel height, inset each side. Both come from
        # OverlayModel::layout() -- the fraction moved from 0.72 at spec phase 6,
        # when the panel grew to hold a 44px play control. The inset is taken as
        # a fraction of the measured panel so this does not need to know the dpr.
        $sy = $r.T + [int]($minY + ($maxY - $minY) * 0.76)
        $inset = [int](($maxX - $minX) * 0.06)
        $sxA = $r.L + $minX + $inset
        $sxB = $r.L + $maxX - $inset
    } else {
        # The control: the transport groove, found by its track colour. Longest
        # run, not first match -- window chrome contains short runs of similar
        # greys.
        $bmp = Grab $r.L $r.T $winW $winH
        $grooveY = -1; $gx0 = -1; $gx1 = -1; $best = 0
        for ($y = [int]($winH * 0.25); $y -lt [int]($winH * 0.94); $y++) {
            $run = 0; $start = -1
            for ($x = 0; $x -lt $winW; $x++) {
                $c = $bmp.GetPixel($x, $y)
                if ([Math]::Abs($c.R - 55) -le 6 -and [Math]::Abs($c.G - 55) -le 6 `
                    -and [Math]::Abs($c.B - 55) -le 6) {
                    if ($start -lt 0) { $start = $x }
                    $run++
                    if ($run -gt $best) { $best = $run; $grooveY = $y; $gx0 = $start; $gx1 = $x }
                } else { $run = 0; $start = -1 }
            }
        }
        $bmp.Dispose()
        if ($best -lt 300) { Write-Output "groove not found"; exit 1 }
        Write-Output ("groove y={0} x={1}..{2}" -f $grooveY, $gx0, $gx1)
        $sy = $r.T + $grooveY
        $sxA = $r.L + $gx0 + 6
        $sxB = $r.L + $gx1 - 6
    }

    # scrub.ps1 -Reversals, exactly: hard direction changes under one continuous
    # press, running into both ends. The gesture set that found 2523d77.
    $mid = [int](($sxA + $sxB) / 2)
    [OD]::SetCursorPos($sxA, $sy) | Out-Null
    Start-Sleep -Milliseconds 200
    [OD]::mouse_event([OD]::DOWN, 0, 0, 0, [IntPtr]::Zero)
    Sweep $sxA $sxB $sy 0.5
    Sweep $sxB $sxA $sy 0.5
    Sweep $sxA $mid  $sy 0.3
    Sweep $mid  $sxB $sy 0.4
    Sweep $sxB $mid  $sy 0.3
    Sweep $mid  $sxA $sy 0.5
    Start-Sleep -Milliseconds 400
    [OD]::mouse_event([OD]::UP, 0, 0, 0, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 700

    $png = Join-Path $OutDir ("drag_{0}_{1}.png" -f $Renderer, $mode)
    & "$PSScriptRoot\capture.ps1" -Out $png | Out-Null
    Write-Output ("captured {0}" -f $png)
    Get-Process -Name Trace -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 400
}
