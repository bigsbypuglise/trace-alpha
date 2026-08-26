# Crop the dev HUD out of a window capture and magnify it.
#
# The HUD is unreadable in a downsampled screenshot on the 5120x1440 panel, and
# a capture of the whole window is mostly picture. This takes the bottom N rows
# at native resolution and doubles them with nearest-neighbour, which is the
# only enlargement that adds no pixels of its own.
param(
    [Parameter(Mandatory = $true)][string]$In,
    [Parameter(Mandatory = $true)][string]$Out,
    [int]$Rows = 44,
    [int]$Zoom = 2
)
Add-Type -AssemblyName System.Drawing
$b = [System.Drawing.Bitmap]::FromFile((Resolve-Path $In))
$rows = [Math]::Min($Rows, $b.Height)
$src = New-Object System.Drawing.Rectangle 0, ($b.Height - $rows), $b.Width, $rows
$crop = New-Object System.Drawing.Bitmap $b.Width, $rows
$g = [System.Drawing.Graphics]::FromImage($crop)
$g.DrawImage($b, (New-Object System.Drawing.Rectangle 0, 0, $b.Width, $rows), $src, [System.Drawing.GraphicsUnit]::Pixel)
$big = New-Object System.Drawing.Bitmap ($b.Width * $Zoom), ($rows * $Zoom)
$g2 = [System.Drawing.Graphics]::FromImage($big)
$g2.InterpolationMode = 'NearestNeighbor'
$g2.PixelOffsetMode = 'Half'
$g2.DrawImage($crop, 0, 0, ($b.Width * $Zoom), ($rows * $Zoom))
$big.Save($Out)
$b.Dispose(); $crop.Dispose(); $big.Dispose()
Write-Output "hud $Out ($($big.Width)x$($big.Height)) from $In"
