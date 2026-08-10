# The control for any CPU-vs-GPU picture comparison: the SAME renderer twice.
#
# abdiff.ps1 reports how much two captures differ. It cannot say how much two
# captures of the same thing differ, and without that number a backend
# comparison has no noise floor -- every difference looks like a backend
# difference. Restarting between the two runs makes this the same gesture the
# A/B performs, so anything that varies run to run (window placement, settling,
# the capture itself) is included exactly as it is there.

param(
    [Parameter(Mandatory = $true)][string]$Clip,
    [string]$Renderer = "cpu",
    [int]$Frame = 40,
    [int]$Width = 1300,
    [double]$ScaleFactor = 1.0,
    [string]$OutDir = "$env:TEMP\trace_abcontrol"
)

Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Add-Type @"
using System;using System.Runtime.InteropServices;
public class AC {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool MoveWindow(IntPtr h,int x,int y,int w,int ht,bool r);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
}
"@

New-Item -ItemType Directory -Force $OutDir | Out-Null
$h = [int]($Width * 0.62) + 300

function Capture-Run([string]$png) {
    & "$PSScriptRoot\restart.ps1" -Clip $Clip -Env "TRACE_RENDERER=$Renderer","QT_SCALE_FACTOR=$ScaleFactor" | Out-Null
    $p = Get-Process -Name Trace -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
    if (-not $p) { Write-Output "no window"; return $false }
    $hwnd = $p.MainWindowHandle
    [AC]::MoveWindow($hwnd, 60, 60, $Width, $h, $true) | Out-Null
    Start-Sleep -Milliseconds 900
    [AC]::SetForegroundWindow($hwnd) | Out-Null
    Start-Sleep -Milliseconds 300
    for ($i = 0; $i -lt $Frame; $i++) {
        [System.Windows.Forms.SendKeys]::SendWait("{RIGHT}")
        Start-Sleep -Milliseconds 40
    }
    Start-Sleep -Milliseconds 800
    $r = New-Object AC+RECT
    [AC]::GetWindowRect($hwnd, [ref]$r) | Out-Null
    $bmp = New-Object System.Drawing.Bitmap ($r.R - $r.L), ($r.B - $r.T)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size)
    $bmp.Save($png, [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose()
    return $true
}

$a = Join-Path $OutDir ("ctl_{0}_{1}_a.png" -f $Renderer, $ScaleFactor)
$b = Join-Path $OutDir ("ctl_{0}_{1}_b.png" -f $Renderer, $ScaleFactor)
if (-not (Capture-Run $a)) { exit 1 }
if (-not (Capture-Run $b)) { exit 1 }
Get-Process -Name Trace -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue

Write-Output ("CONTROL {0} vs {0}, scale {1}, width {2}" -f $Renderer, $ScaleFactor, $Width)
& "$PSScriptRoot\abdiff.ps1" -A $a -B $b
