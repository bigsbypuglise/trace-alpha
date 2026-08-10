# Why do two captures of the same frame differ? Filtering, or geometry?
#
# abdiff.ps1 answers "do the pixels match". It cannot tell a filter-quality
# difference from a destination rectangle that is offset by a pixel, and those
# two have completely different fixes. This searches whole-pixel shifts of B
# against A and reports which one minimises the difference:
#
#   best shift (0,0)  -> the rectangles line up; the difference is resampling
#   best shift (n,m)  -> the backends are not drawing to the same place, and the
#                        residual at that shift is the real filtering difference
#
# It also reports the residual AT the best shift, because a shift that lines the
# pictures up and still leaves a large difference means both causes are present.

param(
    [Parameter(Mandatory = $true)][string]$A,
    [Parameter(Mandatory = $true)][string]$B,
    [int]$Radius = 2,
    [int]$Tolerance = 2,
    # Sample stride. 1 is exact and slow; 3 matches abdiff.ps1.
    [int]$Step = 3
)

Add-Type -AssemblyName System.Drawing

function Read-Gray([string]$path) {
    $bmp = [System.Drawing.Bitmap]::FromFile((Resolve-Path $path))
    $w = $bmp.Width; $h = $bmp.Height
    $rect = New-Object System.Drawing.Rectangle 0, 0, $w, $h
    $data = $bmp.LockBits($rect, 'ReadOnly', 'Format32bppArgb')
    $bytes = New-Object byte[] ($data.Stride * $h)
    [System.Runtime.InteropServices.Marshal]::Copy($data.Scan0, $bytes, 0, $bytes.Length)
    $bmp.UnlockBits($data)
    $stride = $data.Stride
    $bmp.Dispose()
    return @{ Bytes = $bytes; Stride = $stride; W = $w; H = $h }
}

$ia = Read-Gray $A
$ib = Read-Gray $B
if ($ia.W -ne $ib.W -or $ia.H -ne $ib.H) {
    Write-Output ("ABSHIFT: FAIL - different capture sizes {0}x{1} vs {2}x{3}" -f $ia.W, $ia.H, $ib.W, $ib.H)
    exit 1
}

# Same band and inset as abdiff.ps1: the video region only, away from the resize
# border where the desktop legitimately shows through.
$margin = 8 + $Radius
$y0 = [int]($ia.H * 0.06) + $Radius
$y1 = [int]($ia.H * 0.46) - $Radius
$x0 = $margin
$x1 = $ia.W - $margin

$results = @()
foreach ($dy in -$Radius..$Radius) {
  foreach ($dx in -$Radius..$Radius) {
    $diff = 0; $n = 0; $maxd = 0; $sum = 0.0
    for ($y = $y0; $y -lt $y1; $y += $Step) {
      $ra = $y * $ia.Stride
      $rb = ($y + $dy) * $ib.Stride
      for ($x = $x0; $x -lt $x1; $x += $Step) {
        $pa = $ra + $x * 4
        $pb = $rb + ($x + $dx) * 4
        $d = [Math]::Abs($ia.Bytes[$pa] - $ib.Bytes[$pb])
        $g = [Math]::Abs($ia.Bytes[$pa + 1] - $ib.Bytes[$pb + 1])
        if ($g -gt $d) { $d = $g }
        $r = [Math]::Abs($ia.Bytes[$pa + 2] - $ib.Bytes[$pb + 2])
        if ($r -gt $d) { $d = $r }
        if ($d -gt $maxd) { $maxd = $d }
        if ($d -gt $Tolerance) { $diff++ }
        $sum += $d
        $n++
      }
    }
    $results += [pscustomobject]@{
        dx = $dx; dy = $dy
        pct = [Math]::Round(100.0 * $diff / $n, 3)
        mean = [Math]::Round($sum / $n, 3)
        max = $maxd
    }
  }
}

$results | Sort-Object mean | Format-Table -AutoSize | Out-String -Width 120 | Write-Output

$best = $results | Sort-Object mean | Select-Object -First 1
$zero = $results | Where-Object { $_.dx -eq 0 -and $_.dy -eq 0 }
Write-Output ("ABSHIFT best ({0},{1}) mean {2} pct {3} max {4} | at (0,0) mean {5} pct {6} max {7}" -f `
    $best.dx, $best.dy, $best.mean, $best.pct, $best.max, $zero.mean, $zero.pct, $zero.max)

if ($best.dx -eq 0 -and $best.dy -eq 0) {
    Write-Output "ABSHIFT: ALIGNED - the rectangles coincide, so the difference is resampling, not geometry"
} else {
    Write-Output ("ABSHIFT: OFFSET - B is displaced by ({0},{1}); the backends are not drawing to the same rectangle" -f $best.dx, $best.dy)
}
