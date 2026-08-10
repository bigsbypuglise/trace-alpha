# Does the CPU/GPU difference track the DOWNSCALE RATIO or the DPI?
#
# Plan section 20.3 recorded CPU and D3D11 differing on 3.9% of the video band at
# QT_SCALE_FACTOR=1.5 and on almost nothing at dpr 1, and flagged the direction
# as unexplained: the 1.5x case is a 4x downscale and the dpr 1 case a 6x one, so
# the larger difference is at the GENTLER resample, which is backwards for a
# filter-quality story.
#
# That comparison moved two things at once. The window opens at a fixed LOGICAL
# size, so raising the scale factor also makes the window -- and the video band --
# physically bigger: 640x360 device pixels at dpr 1 against 960x540 at 1.5x. DPI
# and downscale ratio are confounded.
#
# This holds the DPI at 1 and sweeps the window width instead, so the ratio is
# the only variable. If divergence rises as the ratio falls toward 4x, the ratio
# explains section 20.3 and the DPI never mattered.

param(
    [Parameter(Mandatory = $true)][string]$Clip,
    [int]$Frame = 40,
    # Physical window widths. Heights follow 16:9-ish plus chrome; the script
    # reports the video band it actually measured rather than assuming one.
    [int[]]$Widths = @(700, 900, 1100, 1300, 1500, 1700),
    [double]$ScaleFactor = 1.0,
    [string]$OutDir = "$env:TEMP\trace_abscale"
)

Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Add-Type @"
using System;using System.Runtime.InteropServices;
public class AS {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool MoveWindow(IntPtr h,int x,int y,int w,int ht,bool r);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
}
"@

New-Item -ItemType Directory -Force $OutDir | Out-Null
$root = Split-Path -Parent $PSScriptRoot

function Capture-At([string]$renderer, [int]$w, [int]$h, [string]$png) {
    & "$PSScriptRoot\restart.ps1" -Clip $Clip -Env "TRACE_RENDERER=$renderer","QT_SCALE_FACTOR=$ScaleFactor" | Out-Null
    $p = Get-Process -Name Trace -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
    if (-not $p) { return $false }
    $hwnd = $p.MainWindowHandle
    [AS]::MoveWindow($hwnd, 60, 60, $w, $h, $true) | Out-Null
    Start-Sleep -Milliseconds 900
    [AS]::SetForegroundWindow($hwnd) | Out-Null
    Start-Sleep -Milliseconds 300
    for ($i = 0; $i -lt $Frame; $i++) {
        [System.Windows.Forms.SendKeys]::SendWait("{RIGHT}")
        Start-Sleep -Milliseconds 40
    }
    Start-Sleep -Milliseconds 800
    $r = New-Object AS+RECT
    [AS]::GetWindowRect($hwnd, [ref]$r) | Out-Null
    $bw = $r.R - $r.L; $bh = $r.B - $r.T
    $bmp = New-Object System.Drawing.Bitmap $bw, $bh
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size)
    $bmp.Save($png, [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose()
    return $true
}

# Width of the non-black run on a scanline through the video band: the video
# band in device pixels, measured rather than derived from the window size,
# because the chrome height depends on the media and the scale factor.
function Band-Width([string]$png) {
    $b = [System.Drawing.Bitmap]::FromFile((Resolve-Path $png))
    $y = [int]($b.Height * 0.22)
    $first = -1; $last = -1
    for ($x = 6; $x -lt $b.Width - 6; $x++) {
        $c = $b.GetPixel($x, $y)
        if (($c.R + $c.G + $c.B) -gt 40) { if ($first -lt 0) { $first = $x }; $last = $x }
    }
    $b.Dispose()
    if ($first -lt 0) { return 0 }
    return ($last - $first + 1)
}

$rows = @()
foreach ($w in $Widths) {
    $h = [int]($w * 0.62) + 300   # chrome below the video; generous, the band is measured
    $a = Join-Path $OutDir ("w{0}_cpu.png" -f $w)
    $b = Join-Path $OutDir ("w{0}_d3d11.png" -f $w)
    if (-not (Capture-At "cpu" $w $h $a)) { Write-Output "w$w cpu: no window"; continue }
    if (-not (Capture-At "d3d11" $w $h $b)) { Write-Output "w$w d3d11: no window"; continue }

    $band = Band-Width $a
    $out = & "$PSScriptRoot\abdiff.ps1" -A $a -B $b
    $line = $out | Where-Object { $_ -match "^ABDIFF sampled" }
    $pct = 0.0; $maxd = 0
    if ($line -match "\(([\d.]+)%\), max channel delta (\d+)") { $pct = [double]$Matches[1]; $maxd = [int]$Matches[2] }
    $rows += [pscustomobject]@{
        WindowPx = $w
        BandPx   = $band
        Ratio    = if ($band -gt 0) { [Math]::Round(3840.0 / $band, 2) } else { 0 }
        DiffPct  = $pct
        MaxDelta = $maxd
    }
    Write-Output ("w {0}: band {1}px ratio {2}x -> diff {3}% max {4}" -f $w, $band, $rows[-1].Ratio, $pct, $maxd)
}

Get-Process -Name Trace -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Write-Output ""
Write-Output ("=== scale factor {0} ===" -f $ScaleFactor)
$rows | Format-Table -AutoSize | Out-String -Width 120 | Write-Output
