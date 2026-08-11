# Six RAPID presses of the floating transport's Fast-forward (or Rewind) must
# reach 30x.
#
# WHY THIS IS ITS OWN SCRIPT. transitions.ps1 -LadderOut asks the same question
# of the DOCKED BAR's buttons, and after spec phase 6 the bar is not in the
# layout: the overlay's controls are the only Rewind and Fast-forward that
# exist. A ladder check that only ever runs against the bar is a check of a
# control the user does not have.
#
# AND THE TWO ARE NOT EQUIVALENT, which is the whole point. Windows sends
# down, up, DBLCLK, up for a rapid pair, so the second press of any pair inside
# the double-click interval arrives as WM_LBUTTONDBLCLK and NOT as
# WM_LBUTTONDOWN. Qt's QWidget::mouseDoubleClickEvent forwards to
# mousePressEvent, so the bar's buttons never noticed; the overlay reads raw
# Win32 messages on the D3D11 path and had to be taught the same thing. An
# overlay that swallowed the double-click reached 5x on six presses instead of
# 30x -- every other press lost -- while the bar passed the identical gesture.
#
# TIMING IS PART OF THE MEASUREMENT. At 30x a 412-frame 24fps clip is traversed
# in 0.57s of wall time, which is less than six presses of ordinary mouse dwell.
# Phase 5 found -LadderOut capturing an ENDED run and reporting `speed 2.00x` at
# `frame 406`, which looks exactly like a ladder that wrapped. So: 45ms dwell,
# no settle before the capture, and a 400+ frame clip.
#
# Read `speed` off the captured HUD. Six presses must read 30.00x (or -30.00x),
# not 2.00x and not 5.00x.

param(
    [Parameter(Mandatory = $true)][string]$Clip,
    [string]$Renderer = "d3d11",
    [switch]$Rewind,
    [int]$Presses = 6,
    # Where to put the playhead before pressing, as a fraction of the timeline.
    # -1 means "near the far end from where the run travels": 0.05 forward, 0.95
    # backward. THE REWIND LEG CANNOT PASS WITHOUT THIS. restart.ps1 leaves the
    # playhead at frame 0, so a rewind run reaches the head of the clip
    # immediately and the capture reads `speed 0.00x` -- which looks exactly like
    # a ladder that never climbed, and is instead a run that correctly stopped.
    [double]$StartFraction = -1,
    [string]$OutDir = "$env:TEMP\trace_overlay_ladder"
)

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;using System.Runtime.InteropServices;
public class OL {
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

& "$PSScriptRoot\restart.ps1" -Clip $Clip -Env "TRACE_RENDERER=$Renderer" | Out-Null

$p = Get-Process -Name Trace | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $p) { Write-Output "LADDER: no window"; exit 1 }
$h = $p.MainWindowHandle
[OL]::SetForegroundWindow($h) | Out-Null
Start-Sleep -Milliseconds 600
$r = New-Object OL+RECT
[OL]::GetWindowRect($h, [ref]$r) | Out-Null
$winW = $r.R - $r.L; $winH = $r.B - $r.T
$cx = [int]($winW / 2)

# Panel by DIFFERENCE, never by a predicted offset.
[OL]::SetCursorPos($r.L + $cx, $r.T + 80) | Out-Null
Start-Sleep -Milliseconds 2600
$before = Grab $r.L $r.T $winW $winH
for ($i = 0; $i -lt 8; $i++) {
    [OL]::SetCursorPos($r.L + $cx + $i, $r.T + 80) | Out-Null
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
if ($maxX -lt 0) { Write-Output "LADDER: overlay never appeared"; exit 1 }

$panelW = $maxX - $minX; $panelH = $maxY - $minY
$panelCx = $r.L + $minX + [int]($panelW / 2)
# Fractions of the MEASURED panel, from OverlayModel::layout(): the icon row sits
# at 0.36 of panel height and the two utility controls are 0.1326 of panel width
# either side of centre.
$iconY = $r.T + $minY + [int]($panelH * 0.36)
$btnX = if ($Rewind) { $panelCx - [int]($panelW * 0.1326) } else { $panelCx + [int]($panelW * 0.1326) }
# $(...) around the if, not plain (...): PowerShell 5.1 does not accept `if` as
# an expression in a parenthesised argument, and the parse error names the `if`
# rather than the construct.
$which = if ($Rewind) { "Rewind" } else { "Fast-forward" }
Write-Output ("LADDER: panel {0}x{1}, pressing {2} at ({3},{4})" -f `
    $panelW, $panelH, $which, ($btnX - $r.L), ($iconY - $r.T))

# Position the playhead first, by clicking the overlay's own track. The track is
# at 0.76 of panel height, inset 6% each side, both from OverlayModel::layout().
if ($StartFraction -lt 0) { $StartFraction = if ($Rewind) { 0.95 } else { 0.05 } }
$trackY = $r.T + $minY + [int]($panelH * 0.76)
$inset = [int]($panelW * 0.06)
$trackA = $r.L + $minX + $inset
$trackB = $r.L + $maxX - $inset
$startX = [int]($trackA + ($trackB - $trackA) * $StartFraction)
[OL]::SetCursorPos($startX, $trackY) | Out-Null
Start-Sleep -Milliseconds 200
[OL]::mouse_event([OL]::DOWN, 0, 0, 0, [IntPtr]::Zero)
Start-Sleep -Milliseconds 120
[OL]::mouse_event([OL]::UP, 0, 0, 0, [IntPtr]::Zero)
Start-Sleep -Milliseconds 900
Write-Output ("LADDER: positioned at fraction {0}" -f $StartFraction)

# FastClick: 45ms dwell, no settle. See the header -- ordinary Click dwell alone
# spends more than the whole 30x budget on a 412-frame clip. It is also well
# INSIDE the double-click interval on purpose: that is the case this exists for.
[OL]::SetCursorPos($btnX, $iconY) | Out-Null
Start-Sleep -Milliseconds 200
for ($i = 0; $i -lt $Presses; $i++) {
    [OL]::mouse_event([OL]::DOWN, 0, 0, 0, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 45
    [OL]::mouse_event([OL]::UP, 0, 0, 0, [IntPtr]::Zero)
}

# Grabbed HERE rather than through capture.ps1, and that is a budget decision
# rather than a convenience. capture.ps1 raises the window and sleeps 300ms
# before shooting; at 30x on a 24fps clip 300ms is 216 FRAMES, which on its own
# runs a 412-frame clip off the end and captures an ENDED run reading
# `speed 0.00x`. That is the phase 5 failure exactly -- a leg that cannot pass,
# accusing the app instead of excusing it. The window is already foreground.
$tag = if ($Rewind) { "rew" } else { "ff" }
$png = Join-Path $OutDir ("ladder_{0}_{1}_x{2}.png" -f $Renderer, $tag, $Presses)
$shot = Grab $r.L $r.T $winW $winH
$shot.Save($png)
$shot.Dispose()
$sign = if ($Rewind) { "-" } else { "+" }
Write-Output ("LADDER: captured {0} - read speed from the HUD; {1} presses must read {2}30.00x" `
    -f $png, $Presses, $sign)
