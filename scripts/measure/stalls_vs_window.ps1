# Is the scrub stall count a property of the build, or of the window size?
#
# Section 17.4 recorded `stalls 2 of 394` on 4K H.264 reversals. The same gesture
# on the same file later measured ~44 of ~375, on both renderers and on a control
# build of the preceding commit -- so not a code regression. The suspect is
# geometry, and the mechanism is specific:
#
#   window size -> viewer size -> scrub previews convert to the size they will be
#   DRAWN at (b5a56af) -> bytes per cache entry -> how many entries fit the 192MB
#   budget -> hit rate -> how often a miss forces a seek plus a GOP walk -> stalls
#
# Cache depth is a function of window size. If the two measurements were taken at
# different sizes, a large stall difference is fully explained and there is no
# regression to hunt.
#
# THE REAL DEFECT IS THAT NOBODY RECORDED THE SIZE. A stall count without the
# window it was measured in is not comparable across sessions, which is why this
# script prints the geometry on every row and why the HUD now carries it too.

param(
    [Parameter(Mandatory = $true)][string]$Clip,
    # Physical window widths to sweep. Heights follow a 16:9-ish window plus chrome.
    [int[]]$Widths = @(900, 1300, 1700, 2100),
    [int]$Repeats = 2,
    [string]$Renderer = "cpu",
    [string]$OutDir = "$env:TEMP\trace_stallwin"
)

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;using System.Runtime.InteropServices;
public class SW {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool MoveWindow(IntPtr h,int x,int y,int w,int ht,bool r);
}
"@

New-Item -ItemType Directory -Force $OutDir | Out-Null
$strips = @()

foreach ($w in $Widths) {
    $h = [int]($w * 0.62) + 300
    for ($rep = 1; $rep -le $Repeats; $rep++) {
        & "$PSScriptRoot\restart.ps1" -Clip $Clip -Env "TRACE_RENDERER=$Renderer" -SettleSeconds 5 | Out-Null
        $p = Get-Process -Name Trace -ErrorAction SilentlyContinue |
             Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
        if (-not $p) { Write-Output "w $w rep $rep : no window"; continue }
        [SW]::MoveWindow($p.MainWindowHandle, 60, 60, $w, $h, $true) | Out-Null
        Start-Sleep -Milliseconds 1000

        & "$PSScriptRoot\scrub.ps1" -Reversals | Out-Null

        $png = Join-Path $OutDir ("w{0}_r{1}.png" -f $w, $rep)
        & "$PSScriptRoot\capture.ps1" -Out $png | Out-Null

        $r = New-Object SW+RECT
        [SW]::GetWindowRect($p.MainWindowHandle, [ref]$r) | Out-Null
        $strips += @{ Png = $png; Label = ("window {0}x{1}  rep {2}  [{3}]" -f ($r.R-$r.L), ($r.B-$r.T), $rep, $Renderer) }
        Write-Output ("captured window {0}x{1} rep {2}" -f ($r.R-$r.L), ($r.B-$r.T), $rep)
    }
}

Get-Process -Name Trace -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
if ($strips.Count -eq 0) { Write-Output "no captures"; exit 1 }

# The HUD lines that answer this: the display size, the smooth/stalls line and
# the cache line. Stacked so the whole sweep is read in one look.
$probe = [System.Drawing.Bitmap]::FromFile($strips[0].Png)
$top = [int]($probe.Height * 0.60)
$bandH = 175
$w0 = [int]($probe.Width * 0.68)
$probe.Dispose()

$lbl = 22
$dst = New-Object System.Drawing.Bitmap ($w0 * 2), ($strips.Count * ($bandH * 2 + $lbl))
$g = [System.Drawing.Graphics]::FromImage($dst)
$g.InterpolationMode = 'NearestNeighbor'
$g.Clear([System.Drawing.Color]::FromArgb(30, 0, 50))
$f = New-Object System.Drawing.Font('Consolas', 14, [System.Drawing.FontStyle]::Bold)
$i = 0
foreach ($s in $strips) {
    $src = [System.Drawing.Bitmap]::FromFile($s.Png)
    $ty = $i * ($bandH * 2 + $lbl)
    $g.DrawString($s.Label, $f, [System.Drawing.Brushes]::Yellow, 4, ($ty + 2))
    $g.DrawImage($src,
        (New-Object System.Drawing.Rectangle 0, ($ty + $lbl), ($w0 * 2), ($bandH * 2)),
        (New-Object System.Drawing.Rectangle 0, $top, $w0, $bandH), 'Pixel')
    $src.Dispose(); $i++
}
$summary = Join-Path $OutDir "summary.png"
$dst.Save($summary, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $dst.Dispose()
Write-Output $summary
