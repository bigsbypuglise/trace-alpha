# Drives the renderer-composited overlay through its interaction and fade
# states, capturing each. Input goes through the child surface's own window
# procedure, which is the supported native path for a child HWND -- Qt never
# sees these messages, because the surface takes the hit-test.
#
# Every command the overlay issues is routed back into the existing QAction /
# slider layer, so a pass here is also a check that the renderer owns no
# playback state of its own.

param(
    [Parameter(Mandatory = $true)][string]$Clip,
    [string]$OutDir = "$env:TEMP\trace_overlay",
    [int]$Frame = 40
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
& "$PSScriptRoot\restart.ps1" -Clip $Clip -Env "TRACE_RENDERER=d3d11","TRACE_OVERLAY_COMPOSITED=1" | Out-Null

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

# Panel geometry mirrors OverlayCompositor::layout() at dpr 1: a 460x76 panel,
# 28px above the bottom of the VIDEO surface. The video surface ends where the
# transport begins, ~0.485 of the captured window height in these runs.
$surfaceH = [int](($r.B - $r.T) * 0.485)
$panelTop = $surfaceH - 76 - 28
$cx = $r.L + [int]($w / 2)
$iconY = $r.T + $panelTop + [int](76 * 0.30)
$trackY = $r.T + $panelTop + [int](76 * 0.72)
$panelLeft = $r.L + [int](($w - 460) / 2)

$playX = $cx
$rewX  = $cx - [int](30 * 1.9)
$ffX   = $cx + [int](30 * 1.9)

function Shot([string]$tag) {
    $rr = New-Object O+RECT
    [O]::GetWindowRect($h, [ref]$rr) | Out-Null
    $b = New-Object System.Drawing.Bitmap ($rr.R-$rr.L), ($rr.B-$rr.T)
    $g = [System.Drawing.Graphics]::FromImage($b)
    $g.CopyFromScreen($rr.L, $rr.T, 0, 0, $b.Size)
    $b.Save((Join-Path $OutDir "$tag.png"), [System.Drawing.Imaging.ImageFormat]::Png)
    # Mean brightness of the panel band: the fade states are distinguishable by
    # it, so "did it fade" is measured rather than eyeballed.
    $sum = 0; $n = 0
    for ($y = $panelTop; $y -lt ($panelTop + 76); $y += 4) {
      for ($x = [int](($rr.R-$rr.L)/2) - 230; $x -lt [int](($rr.R-$rr.L)/2) + 230; $x += 4) {
        if ($x -ge 0 -and $y -ge 0 -and $x -lt $b.Width -and $y -lt $b.Height) {
          $c = $b.GetPixel($x,$y); $sum += ($c.R + $c.G + $c.B) / 3.0; $n++
        }
      }
    }
    $g.Dispose(); $b.Dispose()
    Write-Output ("OVERLAY {0,-24} panel-mean {1}" -f $tag, [Math]::Round($sum / [Math]::Max(1,$n), 1))
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

# 3. hover the Play control
Pt $playX $iconY; Start-Sleep -Milliseconds 350
Shot "03-hover-play"

# 4. press and hold (pressed state), then release -> click
Pt $playX $iconY
[O]::mouse_event([O]::DOWN,0,0,0,[IntPtr]::Zero); Start-Sleep -Milliseconds 250
Shot "04-pressed"
[O]::mouse_event([O]::UP,0,0,0,[IntPtr]::Zero); Start-Sleep -Milliseconds 700
Shot "05-after-play-click"

# 5. pause again, then step forward twice via the overlay
Tap $playX $iconY
Tap $ffX $iconY
Tap $ffX $iconY
Shot "06-after-two-ff"
Tap $rewX $iconY
Shot "07-after-one-rewind"

# 6. timeline drag
Pt ($panelLeft + 40) $trackY
[O]::mouse_event([O]::DOWN,0,0,0,[IntPtr]::Zero); Start-Sleep -Milliseconds 150
for ($i = 0; $i -lt 12; $i++) { Pt ($panelLeft + 40 + $i * 30) $trackY }
Shot "08-mid-drag"
[O]::mouse_event([O]::UP,0,0,0,[IntPtr]::Zero); Start-Sleep -Milliseconds 700
Shot "09-after-drag"

# 7. keyboard still belongs to Qt after all that clicking
[System.Windows.Forms.SendKeys]::SendWait("{RIGHT}")
Start-Sleep -Milliseconds 400
Shot "10-after-key"
$fg = [O]::GetForegroundWindow()
Write-Output ("OVERLAY foreground-is-main-window: {0}" -f ($fg -eq $h))

# 8. fade out by leaving
Pt ($r.L + 30) ($r.B - 30)
Start-Sleep -Milliseconds 400
Shot "11-fading"
Start-Sleep -Milliseconds 2600
Shot "12-hidden-again"

Get-Process -Name Trace -ErrorAction SilentlyContinue | Stop-Process -Force
Write-Output "OVERLAY captures in $OutDir"
