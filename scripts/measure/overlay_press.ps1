# Does the FLOATING TRANSPORT's timeline press land exactly, the way a groove
# click does?
#
# Plan section 31.5 item 2 recorded this as open, and named the reason the two
# might differ. A groove click is an absolute set: the AbsoluteSeekSliderStyle
# sets the value and QSlider then emits sliderPressed, so `valueChanged` arrives
# BEFORE `scrubJumpPending_` is set. The overlay does it the other way round --
# `setScrubbing(true)` then `seekToFraction()` -- so the flag is set first. Both
# orders are supposed to end with `scrubJumpPending_` true when the coalescing
# flush runs, which is what makes the first flush a Step landing rather than a
# drag slice; whether they actually do is the question.
#
# THE PLAYHEAD MUST START FAR FROM THE PRESS POINT. Section 31.5 says so
# explicitly and it is the whole design of this script: a press that lands near
# where the playhead already is cannot tell an exact landing from a lazy one,
# because both produce the right picture. Restarting puts the playhead at frame
# 0 and the default press is at 85% of the track.
#
# PRESS AND RELEASE, NEVER A SWEEP. A drag would walk the decoder to the target
# and prove nothing about the press. This is one click.
#
# What to read, in order of what would actually be wrong:
#   delta       0, or the shown frame is not the requested one.
#   target/shown the frame the click asked for, not the one before it.
#   dst         a PLANAR full-resolution destination, not `RGB32/BGRA 640x360`.
#               A preview-resolution landing is the failure that looks fine.
#
# The control leg (-Bar) performs the identical click on the docked groove, so
# the two landings are comparable. Run both: a leg that can only report success
# proves nothing, which is the discipline lifecycle.ps1 already carries.

param(
    [Parameter(Mandatory = $true)][string]$Clip,
    [string]$Renderer = "d3d11",
    # Drive the docked transport groove instead of the overlay track. The
    # control.
    [switch]$Bar,
    # Where on the track to click, as a fraction of its length. Far from frame 0
    # on purpose.
    [double]$Fraction = 0.85,
    [string]$OutDir = "$env:TEMP\trace_overlay_press"
)

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;using System.Runtime.InteropServices;
public class OP {
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

$envs = @("TRACE_RENDERER=$Renderer")
if ($Bar) { $envs += "TRACE_TRANSPORT_BAR=1" }
& "$PSScriptRoot\restart.ps1" -Clip $Clip -Env $envs | Out-Null

$p = Get-Process -Name Trace | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $p) { Write-Output "PRESS: no window"; exit 1 }
$h = $p.MainWindowHandle
[OP]::SetForegroundWindow($h) | Out-Null
Start-Sleep -Milliseconds 600
$r = New-Object OP+RECT
[OP]::GetWindowRect($h, [ref]$r) | Out-Null
$winW = $r.R - $r.L; $winH = $r.B - $r.T
$cx = [int]($winW / 2)

if ($Bar) {
    # The groove, by its unfilled track colour. Longest run, not first match:
    # window chrome carries short runs of similar greys.
    $bmp = Grab $r.L $r.T $winW $winH
    $gy = -1; $gx0 = -1; $gx1 = -1; $best = 0
    for ($y = [int]($winH * 0.25); $y -lt [int]($winH * 0.94); $y++) {
        $run = 0; $start = -1
        for ($x = 0; $x -lt $winW; $x++) {
            $c = $bmp.GetPixel($x, $y)
            if ([Math]::Abs($c.R - 55) -le 6 -and [Math]::Abs($c.G - 55) -le 6 `
                -and [Math]::Abs($c.B - 55) -le 6) {
                if ($start -lt 0) { $start = $x }
                $run++
                if ($run -gt $best) { $best = $run; $gy = $y; $gx0 = $start; $gx1 = $x }
            } else { $run = 0; $start = -1 }
        }
    }
    $bmp.Dispose()
    if ($best -lt 300) { Write-Output "PRESS: groove not found"; exit 1 }
    Write-Output ("PRESS control: groove y={0} x={1}..{2}" -f $gy, $gx0, $gx1)
    $trackY = $r.T + $gy
    $trackA = $r.L + $gx0 + 6
    $trackB = $r.L + $gx1 - 6
} else {
    # The panel, by DIFFERENCE. Never by a predicted offset -- overlay.ps1 spent
    # a phase aiming 1.2px outside every control that way.
    [OP]::SetCursorPos($r.L + $cx, $r.T + 80) | Out-Null
    Start-Sleep -Milliseconds 2600
    $before = Grab $r.L $r.T $winW $winH
    for ($i = 0; $i -lt 8; $i++) {
        [OP]::SetCursorPos($r.L + $cx + $i, $r.T + 80) | Out-Null
        Start-Sleep -Milliseconds 60
    }
    Start-Sleep -Milliseconds 400
    $after = Grab $r.L $r.T $winW $winH

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
    if ($maxX -lt 0) { Write-Output "PRESS: overlay never appeared"; exit 1 }
    Write-Output ("PRESS: panel bbox x {0}..{1} y {2}..{3}" -f $minX, $maxX, $minY, $maxY)
    # Track at 76% of panel height and inset 6% each side; both from
    # OverlayModel::layout(), expressed as fractions of the MEASURED panel so
    # this stays correct at any dpr.
    $trackY = $r.T + [int]($minY + ($maxY - $minY) * 0.76)
    $inset = [int](($maxX - $minX) * 0.06)
    $trackA = $r.L + $minX + $inset
    $trackB = $r.L + $maxX - $inset
}

$clickX = [int]($trackA + ($trackB - $trackA) * $Fraction)
Write-Output ("PRESS: clicking x={0} of track {1}..{2} (fraction {3}), playhead at frame 0" `
    -f ($clickX - $r.L), ($trackA - $r.L), ($trackB - $r.L), $Fraction)

[OP]::SetCursorPos($clickX, $trackY) | Out-Null
Start-Sleep -Milliseconds 250
[OP]::mouse_event([OP]::DOWN, 0, 0, 0, [IntPtr]::Zero)
Start-Sleep -Milliseconds 120
[OP]::mouse_event([OP]::UP, 0, 0, 0, [IntPtr]::Zero)
Start-Sleep -Milliseconds 900

$tag = if ($Bar) { "bar" } else { "overlay" }
$png = Join-Path $OutDir ("press_{0}_{1}.png" -f $Renderer, $tag)
& "$PSScriptRoot\capture.ps1" -Out $png | Out-Null
Write-Output ("PRESS: captured {0} - read target/shown/delta and dst from the HUD" -f $png)
