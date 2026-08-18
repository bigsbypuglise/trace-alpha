# Drives the renderer-composited overlay through its interaction and fade
# states, capturing each. Input goes through the child surface's own window
# procedure, which is the supported native path for a child HWND -- Qt never
# sees these messages, because the surface takes the hit-test.
#
# Every command the overlay issues is routed back into the existing QAction /
# slider layer, so a pass here is also a check that the renderer owns no
# playback state of its own.
#
# CROSS-BACKEND DIFFING: compare the two directories with abdiff.ps1, but only
# the states whose FRAME is deterministic. 05, 06 and 07 run playback and a
# shuttle, so each backend is on whatever frame it reached when the shot was
# taken -- measured 49% differing at max delta 249, which is a different
# picture and not a different rendering (the HUD headers read `frame 116` and
# `frame 120`). The states to compare are 01-04, paused at -Frame, and 08-12,
# after the drag lands. 08-mid-drag is the best of them: the panel and the
# dragged handle are both on screen and it reads 0 px, max delta 1.
#
# This only became true once the interaction legs started registering. Before
# spec phase 4 fixed the aim, all twelve captures were the same paused frame and
# all twelve read the same 0.619% -- which is the VIDEO band's own backend
# difference, not an overlay-agreement figure, and is what the phase 2 record's
# "312 px (0.619%) across 12 states" was actually measuring.

param(
    [Parameter(Mandatory = $true)][string]$Clip,
    [string]$OutDir = "$env:TEMP\trace_overlay",
    [int]$Frame = 40,
    # The backend to capture on. The cross-backend agreement check needs both
    # sets, and this was hard-coded to d3d11 until spec phase 4 -- so producing
    # the cpu half meant editing the script, which is exactly how a check stops
    # being run. Diff the two directories with abdiff.ps1.
    [ValidateSet("d3d11", "cpu")][string]$Renderer = "d3d11"
)

Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Add-Type @"
using System;using System.Runtime.InteropServices;
public class O {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x,int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f,uint x,uint y,uint d,IntPtr e);
  [DllImport("user32.dll")] public static extern IntPtr GetFocus();
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  public const uint DOWN=0x0002, UP=0x0004;
}
"@

New-Item -ItemType Directory -Force $OutDir | Out-Null
& "$PSScriptRoot\restart.ps1" -Clip $Clip -Env "TRACE_RENDERER=$Renderer","TRACE_OVERLAY_COMPOSITED=1" | Out-Null

$p = Get-Process -Name Trace | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $p) { Write-Output "OVERLAY: no window"; exit 1 }
$h = $p.MainWindowHandle
[O]::SetForegroundWindow($h) | Out-Null
Start-Sleep -Milliseconds 400
for ($i = 0; $i -lt $Frame; $i++) { [System.Windows.Forms.SendKeys]::SendWait("{RIGHT}"); Start-Sleep -Milliseconds 40 }
Start-Sleep -Milliseconds 500

$r = New-Object O+RECT
[O]::GetWindowRect($h, [ref]$r) | Out-Null
$w = $r.R - $r.L

# Panel geometry is FOUND, not predicted.
#
# It used to be predicted, as `0.485 x window height` minus the panel and its
# margin, mirroring OverlayCompositor::layout() at dpr 1. That fraction is the
# bottom of the video SURFACE, which moves whenever the HUD gains or loses a
# line -- the HUD's height depends on the media and on whether there is audio.
# By spec phase 4 it was 17px stale, which put every click 1.2px BELOW the icon
# rect: the captures still looked right, the panel-mean still printed, and not
# one of the interaction legs registered. Twelve captures of a control nobody
# had touched, all passing.
#
# So the panel is located by DIFFERENCE instead: it is exactly what changes
# between the hidden capture and the revealed one. Both are already taken, and
# the size is asserted afterwards, so a layout change breaks the run loudly
# rather than aiming it at the wallpaper.
$cx = $r.L + [int]($w / 2)
$panelTop = 0; $panelLeft = 0; $panelH = 0; $panelW = 0
$iconY = 0; $trackY = 0; $playX = 0; $rewX = 0; $ffX = 0
$startX = 0; $endX = 0; $muteX = 0; $shareX = 0; $fullX = 0; $trackX0 = 0

# THE TOP CHROME REVEALS WITH THE PANEL, SO THE DIFFERENCE IS NO LONGER ONE
# REGION (UI redesign roadmap step 7). Both fade in on the same reveal state, so
# a bounding box over every changed pixel spans from the strip at the top of the
# client area down to the panel and reads ~1253x675 -- which is a correct
# observation of a two-part interface and a useless answer to "where is the
# transport". The scan starts below the strip, and the strip's own height plus
# the title bar is the only thing skipped.
$chromeSkip = 90

function LocatePanel([string]$hiddenPng, [string]$revealedPng) {
    $a = [System.Drawing.Bitmap]::FromFile($hiddenPng)
    $b = [System.Drawing.Bitmap]::FromFile($revealedPng)
    $minX = [int]::MaxValue; $minY = [int]::MaxValue; $maxX = -1; $maxY = -1
    for ($y = $chromeSkip; $y -lt [Math]::Min($a.Height, $b.Height); $y += 2) {
        for ($x = 0; $x -lt [Math]::Min($a.Width, $b.Width); $x += 2) {
            $ca = $a.GetPixel($x, $y); $cb = $b.GetPixel($x, $y)
            if ([Math]::Abs($ca.R - $cb.R) + [Math]::Abs($ca.G - $cb.G) + [Math]::Abs($ca.B - $cb.B) -gt 24) {
                if ($x -lt $minX) { $minX = $x }; if ($x -gt $maxX) { $maxX = $x }
                if ($y -lt $minY) { $minY = $y }; if ($y -gt $maxY) { $maxY = $y }
            }
        }
    }
    $a.Dispose(); $b.Dispose()
    if ($maxY -lt 0) { return $null }
    return @{ x = $minX; y = $minY; w = $maxX - $minX + 1; h = $maxY - $minY + 1 }
}

function Shot([string]$tag) {
    $rr = New-Object O+RECT
    [O]::GetWindowRect($h, [ref]$rr) | Out-Null
    $b = New-Object System.Drawing.Bitmap ($rr.R-$rr.L), ($rr.B-$rr.T)
    $g = [System.Drawing.Graphics]::FromImage($b)
    $g.CopyFromScreen($rr.L, $rr.T, 0, 0, $b.Size)
    $b.Save((Join-Path $OutDir "$tag.png"), [System.Drawing.Imaging.ImageFormat]::Png)
    # Mean brightness of the panel band: the fade states are distinguishable by
    # it, so "did it fade" is measured rather than eyeballed. Reported as `--`
    # until the panel has been located, which is after the first two captures --
    # a mean over a band whose position is not yet known would be a number for
    # the title bar, printed in the column that says `panel`.
    $mean = "--"
    if ($panelH -gt 0) {
        $sum = 0; $n = 0
        for ($y = $panelTop; $y -lt ($panelTop + $panelH); $y += 4) {
          for ($x = ($panelLeft - $rr.L); $x -lt ($panelLeft - $rr.L + $panelW); $x += 4) {
            if ($x -ge 0 -and $y -ge 0 -and $x -lt $b.Width -and $y -lt $b.Height) {
              $c = $b.GetPixel($x,$y); $sum += ($c.R + $c.G + $c.B) / 3.0; $n++
            }
          }
        }
        $mean = [Math]::Round($sum / [Math]::Max(1,$n), 1)
    }
    $g.Dispose(); $b.Dispose()
    Write-Output ("OVERLAY {0,-24} panel-mean {1}" -f $tag, $mean)
}

# NOT named Move: that is a built-in alias for Move-Item, which silently
# shadows a function of the same name and turns every pointer move into a
# failed file operation. The first run of this script did exactly that and
# reported twelve identical captures.
function Pt([int]$x, [int]$y) { [O]::SetCursorPos($x, $y) | Out-Null; Start-Sleep -Milliseconds 120 }
function Tap([int]$x, [int]$y) {
    Pt $x $y
    [O]::mouse_event([O]::DOWN,0,0,0,[IntPtr]::Zero); Start-Sleep -Milliseconds 90
    [O]::mouse_event([O]::UP,0,0,0,[IntPtr]::Zero); Start-Sleep -Milliseconds 500
}

# 1. hidden: park the pointer off the video and wait past the auto-hide.
Pt ($r.L + 30) ($r.B - 30)
Start-Sleep -Milliseconds 2600
Shot "01-hidden"

# 2. reveal by pointer movement over the video
Pt ($cx) ($r.T + 200); Pt ($cx + 4) ($r.T + 204)
Start-Sleep -Milliseconds 400
Shot "02-revealed"

# Everything below aims at the panel that just appeared.
$panel = LocatePanel (Join-Path $OutDir "01-hidden.png") (Join-Path $OutDir "02-revealed.png")
if ($null -eq $panel) { Write-Output "OVERLAY: FAIL - panel not found between hidden and revealed"; exit 1 }
# THE PANEL IS AN EDGE-TO-EDGE STRIP AS OF UI REDESIGN ROADMAP STEP 5, so what
# is asserted changed shape as well as size: the height is the design's 56
# logical px and the WIDTH is however wide the window is. Checking a width band
# the way the 460px panel was checked would now be checking the window size, so
# the width is asserted against the captured window instead -- which is the
# property that actually distinguishes "the strip drew" from "something else
# did".
#
# The height tolerance is wide at the bottom because the strip's top row is its
# 7%-alpha border, which over a dark frame can be within the difference
# threshold and get clipped off the located box.
$expectedW = $r.R - $r.L
if ($panel.h -lt 44 -or $panel.h -gt 68) {
    Write-Output ("OVERLAY: FAIL - located region {0}x{1} is not the 56px transport strip" -f $panel.w, $panel.h)
    exit 1
}
if ($panel.w -lt ($expectedW - 40)) {
    Write-Output ("OVERLAY: FAIL - strip is {0} wide against a {1} window; it should be edge to edge" -f $panel.w, $expectedW)
    exit 1
}
$panelTop = $panel.y; $panelLeft = $r.L + $panel.x; $panelH = $panel.h; $panelW = $panel.w
$panelCx = $r.L + $panel.x + [int]($panel.w / 2)
# EVERY OFFSET HERE COMES FROM OverlayModel::layout() AND IS EXPRESSED AS A
# MULTIPLE OF THE MEASURED STRIP HEIGHT, never as a logical pixel count. The
# strip is located by difference and its height measured, so a multiple of it is
# dpr-independent by construction -- which an absolute offset is not, and which
# is why this script spent a whole phase aiming 1.2px outside every control.
#
# It is a multiple of the HEIGHT rather than a fraction of the width now,
# because step 5's strip is edge to edge: its controls are pinned at a fixed
# distance from each end and do not move when the window is resized, so a
# fraction of the width would drift with the window and hit nothing. Every
# figure below is (logical px from that end) / 56.
#
#   pad 14 + util 36 + gap 2 + play 40, all from layout():
#     go-to-start  14 + 18                 =  32 -> 0.5714
#     rewind       14 + 38 + 18            =  70 -> 1.2500
#     play         14 + 76 + 20            = 110 -> 1.9643
#     fast-forward 14 + 76 + 42 + 18       = 150 -> 2.6786
#     go-to-end    14 + 76 + 42 + 38 + 18  = 188 -> 3.3571
#     mute         + 38                    = 226 -> 4.0357
#   and from the RIGHT edge, share outermost:
#     share        14 + 18                 =  32 -> 0.5714
#     fullscreen   14 + 36 + 12 + 1 + 12 + 18 = 93 -> 1.6607
$iconY  = $r.T + $panelTop + [int]($panelH * 0.5)
# The track shares the strip's centre line now; it is no longer a separate row.
$trackY = $iconY
$startX = $panelLeft + [int]($panelH * 0.5714)
$rewX   = $panelLeft + [int]($panelH * 1.2500)
$playX  = $panelLeft + [int]($panelH * 1.9643)
$ffX    = $panelLeft + [int]($panelH * 2.6786)
$endX   = $panelLeft + [int]($panelH * 3.3571)
$muteX  = $panelLeft + [int]($panelH * 4.0357)
$shareX = $panelLeft + $panelW - [int]($panelH * 0.5714)
$fullX  = $panelLeft + $panelW - [int]($panelH * 1.6607)
Write-Output ("OVERLAY strip {0}x{1} at ({2},{3}) - icons y={4}, start/rew/play/ff/end/mute x={5}/{6}/{7}/{8}/{9}/{10}, full/share x={11}/{12}" -f `
    $panelW, $panelH, $panel.x, $panelTop, ($iconY - $r.T), ($startX - $r.L), ($rewX - $r.L), ($playX - $r.L), `
    ($ffX - $r.L), ($endX - $r.L), ($muteX - $r.L), ($fullX - $r.L), ($shareX - $r.L))

# 3. hover the Play control
Pt $playX $iconY; Start-Sleep -Milliseconds 350
Shot "03-hover-play"

# 4. press and hold (pressed state), then release -> click
Pt $playX $iconY
[O]::mouse_event([O]::DOWN,0,0,0,[IntPtr]::Zero); Start-Sleep -Milliseconds 250
Shot "04-pressed"
[O]::mouse_event([O]::UP,0,0,0,[IntPtr]::Zero); Start-Sleep -Milliseconds 700
Shot "05-after-play-click"

# 5. pause again, then drive the two side controls. BOTH ARE SHUTTLE PRESSES
# NOW -- the right one since spec phase 4 and the left one since phase 5 -- so
# 06 is a forward run at 5x (two rungs) and 07 is a REVERSE run at 2x, not a
# frame stepped back. That is what makes these three states non-deterministic
# across backends, and it is also the only place the re-pointed hooks are
# actually executed: read `speed` and `shuttle` off the HUD in 06 and 07 rather
# than trusting that the wiring is right because it compiles.
Tap $playX $iconY
Tap $ffX $iconY
Tap $ffX $iconY
Shot "06-after-two-ff"
Tap $rewX $iconY
Shot "07-after-one-rewind"

# 6. timeline drag
#
# IT STARTS PAST THE BUTTON CLUSTER AND THE POSITION READOUT, which the old
# `$panelLeft + 40` no longer clears: on the 460px panel the track began 24px in,
# and on the strip the first 244+ logical px are buttons and the readout. A drag
# beginning there would press Go to Start and then sweep the pointer across the
# rest of the row, which reads as a passing drag and tests nothing.
$trackX0 = $panelLeft + [int]($panelH * 5.5)
Pt $trackX0 $trackY
[O]::mouse_event([O]::DOWN,0,0,0,[IntPtr]::Zero); Start-Sleep -Milliseconds 150
for ($i = 0; $i -lt 12; $i++) { Pt ($trackX0 + $i * 30) $trackY }
Shot "08-mid-drag"
[O]::mouse_event([O]::UP,0,0,0,[IntPtr]::Zero); Start-Sleep -Milliseconds 700
Shot "09-after-drag"

# 7. keyboard still belongs to Qt after all that clicking
[System.Windows.Forms.SendKeys]::SendWait("{RIGHT}")
Start-Sleep -Milliseconds 400
Shot "10-after-key"
$fg = [O]::GetForegroundWindow()
Write-Output ("OVERLAY foreground-is-main-window: {0}" -f ($fg -eq $h))

# 8. THE FOUR CONTROLS UI REDESIGN ROADMAP STEP 5 ADDED, each asserted by an
# observable rather than by "the click did not crash".
#
# Go to Start and Go to End are checked by MEASURING THE PLAYED TRACK, which is
# the one thing on the strip that says where the playhead is and is readable
# without OCR: the accent runs from the track's left edge to the thumb, so it is
# ~0% of the track at frame 0 and ~100% at the last frame. Mute is checked by
# the mute cell's own pixels changing, because the glyph swaps between volume
# and volume-muted -- a click that did nothing would leave them identical.
#
# Each has a NEGATIVE half by construction: Go to End must move the fill UP and
# Go to Start must bring it back DOWN, so a build whose buttons do nothing fails
# both rather than passing one by accident.
function AccentFraction([string]$png) {
    $bmp = [System.Drawing.Bitmap]::FromFile($png)
    try {
        $y = $trackY - $r.T
        if ($y -lt 0 -or $y -ge $bmp.Height) { return -1.0 }
        $x0 = $trackX0 - $r.L
        $lit = 0; $seen = 0
        for ($x = $x0; $x -lt $bmp.Width; $x++) {
            $c = $bmp.GetPixel($x, $y)
            # #5AC8E8 with room for the alpha it is composited at.
            $isAccent = ([math]::Abs($c.R - 0x5A) -lt 46) -and ([math]::Abs($c.G - 0xC8) -lt 46) `
                        -and ([math]::Abs($c.B - 0xE8) -lt 46) -and ($c.B -gt $c.R + 20)
            $seen++
            if ($isAccent) { $lit++ }
        }
        if ($seen -le 0) { return -1.0 }
        return [math]::Round($lit / $seen, 3)
    } finally { $bmp.Dispose() }
}

Pt $playX $iconY; Start-Sleep -Milliseconds 200
Tap $endX $iconY; Start-Sleep -Milliseconds 700
Shot "13-after-go-to-end"
$fracEnd = AccentFraction (Join-Path $OutDir "13-after-go-to-end.png")
Tap $startX $iconY; Start-Sleep -Milliseconds 700
Shot "14-after-go-to-start"
$fracStart = AccentFraction (Join-Path $OutDir "14-after-go-to-start.png")
Write-Output ("OVERLAY go-to-end/start played-track fraction: {0} -> {1}" -f $fracEnd, $fracStart)
if ($fracEnd -lt 0 -or $fracStart -lt 0) {
    Write-Output "OVERLAY: FAIL - could not sample the played track"
} elseif ($fracEnd -lt 0.55) {
    Write-Output ("OVERLAY: FAIL - Go to End left the played track at {0}" -f $fracEnd)
} elseif ($fracStart -gt 0.12) {
    Write-Output ("OVERLAY: FAIL - Go to Start left the played track at {0}" -f $fracStart)
} else {
    Write-Output "OVERLAY go-to-end / go-to-start: PASS"
}

Tap $muteX $iconY; Start-Sleep -Milliseconds 450
Shot "15-after-mute"
Tap $muteX $iconY; Start-Sleep -Milliseconds 450
Shot "16-after-unmute"
# COMPARED AGAINST THE OTHER HOVERED STATE, NOT AGAINST THE ONE BEFORE THE
# POINTER ARRIVED -- and the first version of this check got that wrong in a way
# that PASSED. Sampling 15 against 14 reads 625 of 625 changed pixels, because
# moving the pointer onto the control raises its hover plate and brightens its
# glyph: the region changes completely whether or not the click did anything, so
# the assertion would hold on a build where Mute is inert. 15 and 16 are both
# taken with the pointer on the control and differ only in the muted state, so
# what is left is the glyph swap and nothing else.
$muteDelta = 0
$b14 = [System.Drawing.Bitmap]::FromFile((Join-Path $OutDir "16-after-unmute.png"))
$b15 = [System.Drawing.Bitmap]::FromFile((Join-Path $OutDir "15-after-mute.png"))
try {
    $mx = $muteX - $r.L; $my = $iconY - $r.T
    for ($dy = -12; $dy -le 12; $dy++) {
        for ($dx = -12; $dx -le 12; $dx++) {
            $p1 = $b14.GetPixel($mx + $dx, $my + $dy); $p2 = $b15.GetPixel($mx + $dx, $my + $dy)
            if ([math]::Abs($p1.R - $p2.R) + [math]::Abs($p1.G - $p2.G) + [math]::Abs($p1.B - $p2.B) -gt 24) {
                $muteDelta++
            }
        }
    }
} finally { $b14.Dispose(); $b15.Dispose() }
Write-Output ("OVERLAY mute glyph changed pixels (hovered vs hovered): {0} of 625" -f $muteDelta)
if ($muteDelta -lt 20) {
    Write-Output "OVERLAY: FAIL - the mute button did not change its glyph"
} else {
    Write-Output "OVERLAY mute: PASS"
}

# 9. fade out by leaving
Pt ($r.L + 30) ($r.B - 30)
Start-Sleep -Milliseconds 400
Shot "11-fading"
Start-Sleep -Milliseconds 2600
Shot "12-hidden-again"

Get-Process -Name Trace -ErrorAction SilentlyContinue | Stop-Process -Force
Write-Output "OVERLAY captures in $OutDir"
