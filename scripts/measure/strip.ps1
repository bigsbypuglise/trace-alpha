# Stitch the same HUD band out of several captures into one labelled strip.
#
# Bitmap::FromFile holds a lock on the file for the life of the object, so a run
# that errors before Dispose leaves every source locked and the NEXT run loads
# nothing -- $imgs comes back empty and the canvas constructor fails with
# "Parameter is not valid", which reads like a size bug and is not. Load through
# a MemoryStream so nothing is ever locked.
param(
    [Parameter(Mandatory = $true)][string[]]$Png,
    [Parameter(Mandatory = $true)][string[]]$Label,
    [Parameter(Mandatory = $true)][string]$Out,
    [int]$FromBottom = 350,
    [int]$BandH = 120
)
Add-Type -AssemblyName System.Drawing

$imgs = New-Object System.Collections.ArrayList
foreach ($f in $Png) {
    if (-not (Test-Path $f)) { throw "missing capture: $f" }
    $bytes = [System.IO.File]::ReadAllBytes($f)
    $ms = New-Object System.IO.MemoryStream (,$bytes)
    [void]$imgs.Add([System.Drawing.Image]::FromStream($ms))
}
if ($imgs.Count -eq 0) { throw "no images loaded" }

$w = 0
foreach ($im in $imgs) { if ($im.Width -gt $w) { $w = $im.Width } }
$rowH = $BandH + 18
Write-Output ("stitching {0} images, canvas {1}x{2}" -f $imgs.Count, $w, ($rowH * $imgs.Count))

$canvas = New-Object System.Drawing.Bitmap([int]$w, [int]($rowH * $imgs.Count))
$g = [System.Drawing.Graphics]::FromImage($canvas)
$g.Clear([System.Drawing.Color]::Black)
$font = New-Object System.Drawing.Font("Consolas", 11, [System.Drawing.FontStyle]::Bold)

for ($i = 0; $i -lt $imgs.Count; $i++) {
    $src = $imgs[$i]
    $y = [Math]::Max(0, $src.Height - $FromBottom)
    $h = [Math]::Min($BandH, $src.Height - $y)
    $g.FillRectangle([System.Drawing.Brushes]::DarkSlateBlue, 0, ($i * $rowH), $w, 17)
    $g.DrawString($Label[$i], $font, [System.Drawing.Brushes]::Yellow, 4, ($i * $rowH))
    $dst = New-Object System.Drawing.Rectangle 0, ($i * $rowH + 18), $src.Width, $h
    $srcR = New-Object System.Drawing.Rectangle 0, $y, $src.Width, $h
    $g.DrawImage($src, $dst, $srcR, [System.Drawing.GraphicsUnit]::Pixel)
}
$g.Dispose()
$canvas.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
foreach ($im in $imgs) { $im.Dispose() }
$canvas.Dispose()
Write-Output $Out
