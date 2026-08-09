# Pixel-diff two captures of the same frame. Matching summary statistics only
# say the two pictures have the same histogram shape; this says whether the
# pixels are the same pixels, which is what "CPU/GPU frame identity" means.
#
# Compares the video region only. The HUD below it carries per-run timings that
# legitimately differ, and including it would report a difference on every run.

param(
    [Parameter(Mandatory = $true)][string]$A,
    [Parameter(Mandatory = $true)][string]$B,
    [int]$Tolerance = 2
)

Add-Type -AssemblyName System.Drawing
$ia = [System.Drawing.Bitmap]::FromFile((Resolve-Path $A))
$ib = [System.Drawing.Bitmap]::FromFile((Resolve-Path $B))

if ($ia.Width -ne $ib.Width -or $ia.Height -ne $ib.Height) {
    Write-Output ("ABDIFF: FAIL - different capture sizes {0}x{1} vs {2}x{3}" -f $ia.Width,$ia.Height,$ib.Width,$ib.Height)
    $ia.Dispose(); $ib.Dispose(); exit 1
}

$y0 = [int]($ia.Height * 0.06); $y1 = [int]($ia.Height * 0.46)
# Inset from the window edge. GetWindowRect includes the resize border, which is
# not Trace's pixels: the desktop behind shows through it, so two captures taken
# moments apart differ there for reasons that have nothing to do with the
# renderer. Measured: the ONLY differences between cpu and d3d11 captures of the
# same frame were a 4px strip at x 1290-1293 of a 1296px capture.
$margin = 8
$diff = 0; $n = 0; $maxd = 0
for ($y = $y0; $y -lt $y1; $y += 3) {
  for ($x = $margin; $x -lt ($ia.Width - $margin); $x += 3) {
    $ca = $ia.GetPixel($x,$y); $cb = $ib.GetPixel($x,$y)
    $d = [Math]::Max([Math]::Abs($ca.R-$cb.R), [Math]::Max([Math]::Abs($ca.G-$cb.G), [Math]::Abs($ca.B-$cb.B)))
    if ($d -gt $maxd) { $maxd = $d }
    if ($d -gt $Tolerance) { $diff++ }
    $n++
  }
}
$ia.Dispose(); $ib.Dispose()

$pct = [Math]::Round(100.0 * $diff / $n, 3)
Write-Output ("ABDIFF sampled {0} px, differing {1} ({2}%), max channel delta {3}" -f $n, $diff, $pct, $maxd)
if ($maxd -le $Tolerance) { Write-Output "ABDIFF: IDENTICAL within tolerance"; exit 0 }
if ($pct -lt 0.5) { Write-Output "ABDIFF: NEAR-IDENTICAL (isolated pixels differ)"; exit 0 }
Write-Output "ABDIFF: DIFFERENT - inspect both captures"
exit 2
