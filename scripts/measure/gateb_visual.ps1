# The GATE B visual A/B set, in the form plan section 20.7 asks for.
#
# Every number recorded for GATE B says the right frame is at the right size.
# None of them says the picture looks right, and this project has recorded four
# times now that only the owner can answer the second question. This produces
# the material for that judgement rather than trying to make it: matched pairs
# of native-resolution captures, one per viewing condition, plus magnified crops
# of the same region so colour, black and white levels, text, diagonals and fine
# detail can be compared side by side.
#
# 4K ProRes 422 HQ is the bar (owner, 2026-08-07). It is the default clip here.
#
# The abdiff number is reported beside each pair, but it is the WEAKER evidence:
# two pictures can differ on 0% of sampled pixels and still not look alike, and
# they can differ on 4% because of a sampling phase offset nobody can see.

param(
    [Parameter(Mandatory = $true)][string]$Clip,
    [int]$Frame = 40,
    [string]$OutDir = "$env:USERPROFILE\Desktop\Trace_GateB_Visual",
    # Region of the capture to magnify, as fractions of the window.
    [double]$CropX = 0.42, [double]$CropY = 0.18,
    [int]$CropW = 190, [int]$CropH = 120, [int]$Zoom = 5,
    # Re-run one condition by name instead of the whole set.
    [string]$Only = ""
)

Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Add-Type @"
using System;using System.Runtime.InteropServices;
public class GB {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool MoveWindow(IntPtr h,int x,int y,int w,int ht,bool r);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h,int c);
}
"@

New-Item -ItemType Directory -Force $OutDir | Out-Null

# name -> how to put the window into that state
$conditions = @(
    @{ Name = "fit-window";  Scale = "1";   Mode = "size"; W = 1400; H = 1000 },
    @{ Name = "large";       Scale = "1";   Mode = "max" },
    @{ Name = "scale150";    Scale = "1.5"; Mode = "size"; W = 1400; H = 1000 },
    @{ Name = "fullscreen";  Scale = "1";   Mode = "full"; W = 1400; H = 1000 }
)
if ($Only) { $conditions = $conditions | Where-Object { $_.Name -eq $Only } }

function Capture-Condition($cond, [string]$renderer, [string]$png) {
    & "$PSScriptRoot\restart.ps1" -Clip $Clip -Env "TRACE_RENDERER=$renderer","QT_SCALE_FACTOR=$($cond.Scale)" | Out-Null
    $p = Get-Process -Name Trace -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
    if (-not $p) { Write-Output "  no window"; return $false }
    $h = $p.MainWindowHandle
    switch ($cond.Mode) {
        "size" { [GB]::MoveWindow($h, 80, 60, $cond.W, $cond.H, $true) | Out-Null }
        "max"  { [GB]::ShowWindow($h, 3) | Out-Null }   # SW_MAXIMIZE
        "full" { [GB]::MoveWindow($h, 80, 60, 1400, 1000, $true) | Out-Null }
    }
    Start-Sleep -Milliseconds 900
    [GB]::SetForegroundWindow($h) | Out-Null
    Start-Sleep -Milliseconds 400
    for ($i = 0; $i -lt $Frame; $i++) {
        [System.Windows.Forms.SendKeys]::SendWait("{RIGHT}")
        Start-Sleep -Milliseconds 45
    }
    if ($cond.Mode -eq "full") {
        # Ctrl+Return -- the app's own toggle (MainWindow.cpp:593), not a
        # window-manager resize: it is the path the user takes and the one
        # section 18.3 exercised. Assert it took, because a shortcut that does
        # nothing captures a windowed frame and calls it a fullscreen pass.
        [System.Windows.Forms.SendKeys]::SendWait("^{ENTER}")
        Start-Sleep -Milliseconds 1600
        $chk = New-Object GB+RECT
        [GB]::GetWindowRect($h, [ref]$chk) | Out-Null
        if (($chk.R - $chk.L) -le $cond.W) {
            Write-Output ("  FULLSCREEN DID NOT ENGAGE (window still {0}px wide)" -f ($chk.R - $chk.L))
            return $false
        }
    }
    Start-Sleep -Milliseconds 700
    $r = New-Object GB+RECT
    [GB]::GetWindowRect($h, [ref]$r) | Out-Null
    $bmp = New-Object System.Drawing.Bitmap ($r.R - $r.L), ($r.B - $r.T)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size)
    $bmp.Save($png, [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose()
    return $true
}

# cpu above, d3d11 below, same region, nearest-neighbour so the comparison shows
# the captured pixels rather than the resampler used to display them.
function Make-Zoom([string]$a, [string]$b, [string]$png) {
    $ia = [System.Drawing.Bitmap]::FromFile((Resolve-Path $a))
    $ib = [System.Drawing.Bitmap]::FromFile((Resolve-Path $b))
    $sx = [int]($ia.Width * $CropX); $sy = [int]($ia.Height * $CropY)
    $cw = [Math]::Min($CropW, $ia.Width - $sx); $ch = [Math]::Min($CropH, $ia.Height - $sy)
    $lbl = 26
    $dst = New-Object System.Drawing.Bitmap ($cw * $Zoom), ($ch * $Zoom * 2 + $lbl * 2)
    $g = [System.Drawing.Graphics]::FromImage($dst)
    $g.InterpolationMode = 'NearestNeighbor'; $g.PixelOffsetMode = 'Half'
    $g.Clear([System.Drawing.Color]::Black)
    $f = New-Object System.Drawing.Font('Consolas', 14, [System.Drawing.FontStyle]::Bold)
    $i = 0
    foreach ($pair in @(@($ia, 'CPU'), @($ib, 'D3D11'))) {
        $ty = $i * ($ch * $Zoom + $lbl)
        $g.DrawString($pair[1], $f, [System.Drawing.Brushes]::Yellow, 6, ($ty + 3))
        $g.DrawImage($pair[0],
            (New-Object System.Drawing.Rectangle 0, ($ty + $lbl), ($cw * $Zoom), ($ch * $Zoom)),
            (New-Object System.Drawing.Rectangle $sx, $sy, $cw, $ch), 'Pixel')
        $i++
    }
    $dst.Save($png, [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $dst.Dispose(); $ia.Dispose(); $ib.Dispose()
}

Write-Output ("clip  : {0}" -f (Split-Path -Leaf $Clip))
Write-Output ("frame : {0}" -f $Frame)
Write-Output ("out   : {0}" -f $OutDir)
Write-Output ""

foreach ($cond in $conditions) {
    $n = $cond.Name
    Write-Output ("--- {0} (scale {1}) ---" -f $n, $cond.Scale)
    $a = Join-Path $OutDir "$n`_cpu.png"
    $b = Join-Path $OutDir "$n`_d3d11.png"
    if (-not (Capture-Condition $cond "cpu" $a)) { continue }
    if (-not (Capture-Condition $cond "d3d11" $b)) { continue }
    & "$PSScriptRoot\abdiff.ps1" -A $a -B $b | ForEach-Object { Write-Output ("  " + $_) }
    Make-Zoom $a $b (Join-Path $OutDir "$n`_ZOOM.png")
    Write-Output ("  zoom -> {0}_ZOOM.png" -f $n)
}

Get-Process -Name Trace -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Write-Output ""
Write-Output "Flip between each *_cpu.png / *_d3d11.png pair at 100% zoom."
Write-Output "The *_ZOOM.png files stack the same crop, CPU above D3D11."
