# Visual capture that cannot accidentally pass on a naturally black frame.
#
# The GATE B hazard, in one sentence: the 1080p validation clip opens on a solid
# black frame, so "the video rect is black" was read as "presentation failed"
# and a working D3D11 path was rebuilt on a wrong theory (plan section 17.2).
#
# This script therefore refuses to report success on a frame it has not proved
# has content. It:
#   - steps to a NAMED frame index rather than capturing wherever the app opened;
#   - measures pixel variance and non-black fraction inside the video rect;
#   - reports the frame index it believes it captured, so a caller can assert it;
#   - fails loudly when the rect is uniform, whatever colour it is.
#
# A uniform RED rect means the swapchain cleared and never drew -- that is the
# diagnostic that located the GATE B fault in one run, and TRACE_D3D11_CLEAR_DIAG
# still produces it on demand. A uniform BLACK rect is ambiguous by nature and is
# reported as INDETERMINATE, never as a pass.

param(
    [Parameter(Mandatory = $true)][string]$Out,
    # Frames to step forward from 0 before capturing. Pick one with content.
    [int]$Frame = 0,
    # Fail unless at least this share of sampled pixels differ from the darkest.
    [double]$MinNonBlackFraction = 0.10,
    # Fail unless the sampled luma spread is at least this wide (0-255).
    [double]$MinSpread = 20.0
)

Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Add-Type @"
using System;using System.Runtime.InteropServices;
public class V {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
}
"@

$p = Get-Process -Name Trace -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $p) { Write-Output "VISUAL: no window"; exit 1 }
$h = $p.MainWindowHandle
[V]::SetForegroundWindow($h) | Out-Null
Start-Sleep -Milliseconds 400

# Step to a known frame. Right-arrow is exact stepping and never approximates,
# so the frame index is a property of the gesture rather than of timing.
for ($i = 0; $i -lt $Frame; $i++) {
    [System.Windows.Forms.SendKeys]::SendWait("{RIGHT}")
    Start-Sleep -Milliseconds 45
}
Start-Sleep -Milliseconds 700

$r = New-Object V+RECT
[V]::GetWindowRect($h, [ref]$r) | Out-Null
$w = $r.R - $r.L; $ht = $r.B - $r.T
$bmp = New-Object System.Drawing.Bitmap $w, $ht
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size)
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose()

# Sample the upper region only: the transport and HUD are below the video and
# would supply plenty of "content" on their own, which is exactly the kind of
# false pass this script exists to prevent.
$y0 = [int]($ht * 0.08); $y1 = [int]($ht * 0.45)
$x0 = [int]($w * 0.05);  $x1 = [int]($w * 0.95)
$lum = New-Object System.Collections.ArrayList
$reds = 0; $n = 0
for ($y = $y0; $y -lt $y1; $y += 7) {
  for ($x = $x0; $x -lt $x1; $x += 7) {
    $c = $bmp.GetPixel($x, $y)
    [void]$lum.Add([int](0.299 * $c.R + 0.587 * $c.G + 0.114 * $c.B))
    if ($c.R -gt 180 -and $c.G -lt 60 -and $c.B -lt 60) { $reds++ }
    $n++
  }
}
$bmp.Dispose()

$min = ($lum | Measure-Object -Minimum).Minimum
$max = ($lum | Measure-Object -Maximum).Maximum
$mean = [Math]::Round(($lum | Measure-Object -Average).Average, 1)
$spread = $max - $min
$nonBlack = [Math]::Round((($lum | Where-Object { $_ -gt 16 }).Count) / [double]$n, 3)
$redFrac = [Math]::Round($reds / [double]$n, 3)

Write-Output ("VISUAL frame~{0} min {1} max {2} mean {3} spread {4} nonblack {5} red {6} -> {7}" -f `
  $Frame, $min, $max, $mean, $spread, $nonBlack, $redFrac, $Out)

if ($redFrac -gt 0.5) {
    Write-Output "VISUAL: FAIL - rect is the diagnostic clear colour; the swapchain cleared and never drew"
    exit 2
}
if ($spread -lt $MinSpread -and $nonBlack -lt $MinNonBlackFraction) {
    Write-Output "VISUAL: INDETERMINATE - rect is uniform and dark. This is NOT proof of failure: check the clip's frame $Frame actually has content before concluding anything."
    exit 3
}
if ($spread -lt $MinSpread) {
    Write-Output ("VISUAL: FAIL - rect is uniform (spread {0}); no image detail present" -f $spread)
    exit 4
}
if ($nonBlack -lt $MinNonBlackFraction) {
    Write-Output ("VISUAL: FAIL - only {0} of sampled pixels are above black" -f $nonBlack)
    exit 5
}
Write-Output "VISUAL: PASS - video rect contains image detail"
exit 0
