param([string]$Out = "shot.png", [string]$ProcName = "Trace")

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

$bmp = New-Object System.Drawing.Bitmap $w, $ht
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size)
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose()
Write-Output "saved $Out (${w}x${ht})"
