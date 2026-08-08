# -HudOnly crops to the diagnostics block below the transport. The picture is
# rarely what a measurement run is reading, and a full-window shot of a 4K frame
# is mostly pixels nobody looks at.
param([string]$Out = "shot.png", [string]$ProcName = "Trace", [switch]$HudOnly)

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Win {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
}
"@

$p = Get-Process -Name $ProcName -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $p) { Write-Output "no window"; exit 1 }

$h = $p.MainWindowHandle
[Win]::SetForegroundWindow($h) | Out-Null
Start-Sleep -Milliseconds 300

$r = New-Object Win+RECT
[Win]::GetWindowRect($h, [ref]$r) | Out-Null
$w = $r.R - $r.L
$ht = $r.B - $r.T

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
