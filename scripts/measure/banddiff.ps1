# Pixel difference between two window captures, over a band of rows.
#
# LockBits rather than GetPixel: the video band is ~450k pixels and GetPixel
# takes minutes over it, which is long enough that the check stops being run.
param(
    [Parameter(Mandatory=$true)][string]$A,
    [Parameter(Mandatory=$true)][string]$B,
    [int]$Top = 50,
    [int]$Bottom = 400,
    [int]$Left = 10,
    [int]$Right = 1286,
    # A channel difference at or below this is not counted. 8 matches the
    # threshold the phase 4-6 overlay diffs used.
    [int]$Tolerance = 8,
    [string]$Label = ""
)
Add-Type -AssemblyName System.Drawing

function LoadArgb($path) {
    $src = New-Object System.Drawing.Bitmap $path
    $bmp = New-Object System.Drawing.Bitmap $src.Width, $src.Height, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.DrawImage($src, 0, 0, $src.Width, $src.Height)
    $g.Dispose(); $src.Dispose()
    return $bmp
}

$ba = LoadArgb $A
$bb = LoadArgb $B
if ($ba.Width -ne $bb.Width -or $ba.Height -ne $bb.Height) {
    Write-Output "SIZE MISMATCH $($ba.Width)x$($ba.Height) vs $($bb.Width)x$($bb.Height)"
    exit 1
}
$rect = New-Object System.Drawing.Rectangle 0, 0, $ba.Width, $ba.Height
$la = $ba.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$lb = $bb.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$n = $la.Stride * $ba.Height
$pa = New-Object byte[] $n
$pb = New-Object byte[] $n
[System.Runtime.InteropServices.Marshal]::Copy($la.Scan0, $pa, 0, $n)
[System.Runtime.InteropServices.Marshal]::Copy($lb.Scan0, $pb, 0, $n)
$stride = $la.Stride
$ba.UnlockBits($la); $bb.UnlockBits($lb)

$diff = 0; $tot = 0; $maxd = 0
$r1 = [Math]::Min($Right, $ba.Width)
$b1 = [Math]::Min($Bottom, $ba.Height)
for ($y = $Top; $y -lt $b1; $y++) {
    $row = $y * $stride
    for ($x = $Left; $x -lt $r1; $x++) {
        $i = $row + $x * 4
        $d = [Math]::Abs($pa[$i] - $pb[$i])
        $d1 = [Math]::Abs($pa[$i+1] - $pb[$i+1]); if ($d1 -gt $d) { $d = $d1 }
        $d2 = [Math]::Abs($pa[$i+2] - $pb[$i+2]); if ($d2 -gt $d) { $d = $d2 }
        if ($d -gt $maxd) { $maxd = $d }
        if ($d -gt $Tolerance) { $diff++ }
        $tot++
    }
}
$ba.Dispose(); $bb.Dispose()
$pct = if ($tot -gt 0) { [Math]::Round(100.0 * $diff / $tot, 4) } else { 0 }
Write-Output ("{0,-18} {1} of {2} px differ (>{3})  {4}%   max channel delta {5}" -f $Label, $diff, $tot, $Tolerance, $pct, $maxd)
