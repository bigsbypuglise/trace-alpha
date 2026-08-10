# Cut the video rect out of a window capture, and PROVE it is the video rect.
#
# abfilter.ps1 compares Trace's pixels against references generated at the exact
# size Trace drew at. That comparison is meaningless if the crop is off by even
# one pixel: a one-pixel shift on a 6x-reduced picture moves every sample onto a
# different source neighbourhood, which reads as a filtering difference. Plan
# section 21.2 is the precedent -- a sub-pixel destination offset was mistaken for
# a filter-quality difference and cost a session, and the instrument that settled
# it (abshift.ps1) exists because the geometry had to be ruled out first.
#
# So the rect is found rather than assumed, and then asserted:
#   - find the STAGE's vertical extent as the longest run of pure black in a
#     pillarbox column, which excludes the chrome without a magic offset;
#   - within that band, the run of non-black COLUMNS containing the horizontal
#     centre is the video's x range;
#   - within that x range, the longest run of non-black ROWS is its y range;
#   - require the result to match -Expect exactly, which the HUD supplies as
#     `display WxH`.
#
# Two earlier versions were wrong and both failed the assertion rather than
# lying, which is the only reason this converged. A plain bounding box caught the
# title bar. Runs-through-the-centre did too, for a reason worth recording: the
# title bar is a FULL-WIDTH band of (32,32,32), so every column in the scan is
# "lit" and the centre run spans the whole window. Restricting to the stage first
# is what fixes it, and the discriminator is exact rather than a threshold --
# Trace's stage is (0,0,0) while the chrome is 32 and the HUD backing is 18. A
# brightness floor cannot separate them: the left edge of this very picture
# measures luma 33.
#
# The assertion is still the whole point. A frame with dark edges under-reports
# whatever the scan strategy; either way the size stops matching what the
# renderer says it drew, and this fails instead of quietly handing abfilter a
# misaligned crop.

param(
    [Parameter(Mandatory = $true)][string]$In,
    [Parameter(Mandatory = $true)][string]$Out,
    # "WxH" from the HUD's `display` field, in device pixels.
    [Parameter(Mandatory = $true)][string]$Expect,
    # Luma above which a pixel counts as picture rather than stage. The stage is
    # painted pure black; 12 leaves room for capture noise without swallowing
    # genuinely dark picture content.
    [int]$BlackFloor = 12
)

Add-Type -AssemblyName System.Drawing

if ($Expect -notmatch '^(\d+)x(\d+)$') { Write-Output "bad -Expect '$Expect', want WxH"; exit 1 }
$ew = [int]$Matches[1]; $eh = [int]$Matches[2]

$src = [System.Drawing.Bitmap]::FromFile((Resolve-Path $In).Path)
$w = $src.Width; $h = $src.Height

# Only the part of the window above the transport. The HUD text is bright and
# would otherwise extend every run all the way down.
$yTo = [int]($h * 0.55)
# Inset past the resize border: it is desktop showing through, not Trace's pixels
# (the same 8px margin abdiff.ps1 uses, and for the reason recorded there).
$margin = 8

# One LockBits pass. GetPixel over a 1296x477 band is 600k marshalled calls and
# took longer than the capture being measured.
$bd = $src.LockBits((New-Object System.Drawing.Rectangle 0, 0, $w, $h),
                    [System.Drawing.Imaging.ImageLockMode]::ReadOnly,
                    [System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
$stride = $bd.Stride
$buf = New-Object byte[] ($stride * $h)
[System.Runtime.InteropServices.Marshal]::Copy($bd.Scan0, $buf, 0, $buf.Length)
$src.UnlockBits($bd)

function Test-Lit([int]$x, [int]$y) {
    $i = $y * $stride + $x * 3
    return (0.114 * $buf[$i] + 0.587 * $buf[$i + 1] + 0.299 * $buf[$i + 2]) -gt $BlackFloor
}
function Test-PureBlack([int]$x, [int]$y) {
    $i = $y * $stride + $x * 3
    return ($buf[$i] -eq 0 -and $buf[$i + 1] -eq 0 -and $buf[$i + 2] -eq 0)
}

# The stage's vertical extent, from a column out in the pillarbox. Pure black is
# an exact match, not a threshold, which is what separates the stage from the
# chrome above it and the HUD backing below.
$probeX = $margin + 4
$sBest = 0; $sStart = -1; $run = 0; $start = -1
for ($y = $margin; $y -lt $yTo; $y++) {
    if (Test-PureBlack $probeX $y) {
        if ($start -lt 0) { $start = $y }
        $run++
        if ($run -gt $sBest) { $sBest = $run; $sStart = $start }
    } else { $run = 0; $start = -1 }
}
if ($sBest -lt 50) {
    $src.Dispose()
    Write-Output "CROP: FAIL - no pillarbox found at x=$probeX (longest black run $sBest px)."
    Write-Output "      This scan needs the stage to be wider than the picture. Make the"
    Write-Output "      window wider than the media's aspect ratio and capture again."
    exit 1
}
$bandTop = $sStart; $bandBottom = $sStart + $sBest - 1
Write-Output ("stage band y {0}..{1} (from the pillarbox at x={2})" -f $bandTop, $bandBottom, $probeX)

# Columns within the stage band only. A column is "lit" if anything in the band
# is above the floor.
$lit = New-Object bool[] $w
for ($x = $margin; $x -lt ($w - $margin); $x++) {
    for ($y = $bandTop; $y -le $bandBottom; $y++) {
        if (Test-Lit $x $y) { $lit[$x] = $true; break }
    }
}
$cx = [int]($w / 2)
if (-not $lit[$cx]) { $src.Dispose(); Write-Output "CROP: FAIL - window centre column is black; no picture?"; exit 1 }
$minX = $cx; while ($minX -gt $margin -and $lit[$minX - 1]) { $minX-- }
$maxX = $cx; while ($maxX -lt ($w - $margin - 1) -and $lit[$maxX + 1]) { $maxX++ }

# Then rows, within that x range and the stage band only.
$litY = New-Object bool[] $h
for ($y = $bandTop; $y -le $bandBottom; $y++) {
    for ($x = $minX; $x -le $maxX; $x++) {
        if (Test-Lit $x $y) { $litY[$y] = $true; break }
    }
}
$bestLen = 0; $bestStart = -1; $run = 0; $start = -1
for ($y = $bandTop; $y -le $bandBottom; $y++) {
    if ($litY[$y]) {
        if ($start -lt 0) { $start = $y }
        $run++
        if ($run -gt $bestLen) { $bestLen = $run; $bestStart = $start }
    } else { $run = 0; $start = -1 }
}
if ($bestStart -lt 0) { $src.Dispose(); Write-Output "CROP: FAIL - no lit rows in the centre columns"; exit 1 }
$minY = $bestStart; $maxY = $bestStart + $bestLen - 1

$fw = $maxX - $minX + 1; $fh = $maxY - $minY + 1
Write-Output ("found rect {0}x{1} at {2},{3}  (expected {4}x{5})" -f $fw, $fh, $minX, $minY, $ew, $eh)

if ($fw -ne $ew -or $fh -ne $eh) {
    $src.Dispose()
    Write-Output "CROP: FAIL - found rect does not match what the renderer reported it drew."
    Write-Output "      Either the frame has dark edges (bounding box under-reports) or"
    Write-Output "      chrome was caught (over-reports). Do NOT feed this to abfilter."
    exit 2
}

$dst = New-Object System.Drawing.Bitmap $fw, $fh
$g = [System.Drawing.Graphics]::FromImage($dst)
$g.DrawImage($src, (New-Object System.Drawing.Rectangle 0, 0, $fw, $fh),
             (New-Object System.Drawing.Rectangle $minX, $minY, $fw, $fh),
             [System.Drawing.GraphicsUnit]::Pixel)
$g.Dispose()
$dst.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$dst.Dispose(); $src.Dispose()
Write-Output ("CROP: OK -> {0}" -f $Out)
