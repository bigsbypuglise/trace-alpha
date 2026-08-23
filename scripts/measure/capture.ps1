# -HudOnly crops to the diagnostics block below the transport. The picture is
# rarely what a measurement run is reading, and a full-window shot of a 4K frame
# is mostly pixels nobody looks at.
#
# The window is resolved through tracewindow.ps1 rather than by the bare
# `Get-Process | Where MainWindowHandle -ne 0` this used to carry: that lookup
# returns a TOOLTIP when one is up under a parked cursor, and this script then
# built a 55x19 bitmap -- or threw on an invalid one and left no file at all,
# which downstream reads as a build that cannot draw. See tracewindow.ps1.
param([string]$Out = "shot.png", [string]$ProcName = "Trace", [switch]$HudOnly,
      [switch]$NoCursorPark)

Add-Type -AssemblyName System.Drawing
. "$PSScriptRoot\tracewindow.ps1"

$h = Resolve-TraceWindow -ProcName $ProcName -NoCursorPark:$NoCursorPark
if ($h -eq [IntPtr]::Zero) { Write-Output "no window"; exit 1 }

[TraceWin]::SetForegroundWindow($h) | Out-Null
Start-Sleep -Milliseconds 300

$r = New-Object TraceWin+RECT
[TraceWin]::GetWindowRect($h, [ref]$r) | Out-Null
$w = $r.R - $r.L
$ht = $r.B - $r.T

# Resolve-TraceWindow already rejects anything under 300px, so this cannot fire
# on a tooltip. It stays as the last line of defence against a zero-area rect
# from a window that was destroyed between the resolve and here -- New-Object
# Bitmap 0,0 throws, and the throw is what used to bury the real cause.
if ($w -le 0 -or $ht -le 0) { Write-Output "degenerate window rect ${w}x${ht}"; exit 1 }

$srcY = $r.T
$capH = $ht
if ($HudOnly) {
    # The HUD is the run of text lines at the bottom; its height varies with the
    # media, so take a generous slice from just below the transport rather than
    # a fixed offset.
    $off = [int]($ht * 0.56)
    $srcY = $r.T + $off
    $capH = $ht - $off
}

$bmp = New-Object System.Drawing.Bitmap $w, $capH
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($r.L, $srcY, 0, 0, $bmp.Size)
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose()
Write-Output "saved $Out (${w}x${capH})"
