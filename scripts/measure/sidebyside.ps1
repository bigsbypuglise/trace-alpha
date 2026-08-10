# Two Trace instances side by side: CPU renderer left, D3D11 right.
#
# For judging the two backends by eye on the same media at the same moment,
# which is the only way the remaining difference between them has ever been
# settled -- every A/B number the plan records was taken sequentially, and
# sequential runs cannot answer "does this one look better".
#
# It VERIFIES which backend each window actually adopted rather than assuming.
# A GPU path that quietly falls back to CPU while the app looks fine is the
# failure mode the whole renderer boundary is designed against (plan section
# 12), and it would make this comparison a CPU-versus-CPU one without saying so.

param(
    # Defaults to the file the cadence work was signed off on. Any other clip
    # can be dropped into either window afterwards -- both accept drag and drop.
    [string]$Clip = "C:\Users\andre\Documents\Claude_Cowork\Trace_Testing_Assets\1_4K_ProRes_4444\TheraTears_Vial_VFX_v002.mov",
    [string]$Exe,
    # Skip the screenshot check. The check costs a couple of seconds and is the
    # only thing that proves d3d11 engaged, so it is on by default.
    [switch]$NoVerify
)

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class W {
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after,
      int x, int y, int cx, int cy, uint flags);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  // NOT named SHOWWINDOW: CLR member lookup is case-insensitive, so a constant
  // spelled that way shadows the ShowWindow METHOD above and PowerShell reports
  // "does not contain a method named 'ShowWindow'" while everything else in the
  // class works fine.
  public const uint SWP_SHOWWINDOW = 0x0040, SWP_NOZORDER = 0x0004;
  public const int SW_RESTORE = 9;
}
"@

if (-not $Exe) {
    $repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
    $Exe = Join-Path $repo "build\app\Release\Trace.exe"
}
if (-not (Test-Path $Exe))  { Write-Output "no exe at $Exe";  exit 1 }
if (-not (Test-Path $Clip)) { Write-Output "no clip at $Clip"; exit 1 }

# Start from nothing, so the two windows below are the only ones and there is no
# ambiguity about which process is which.
Get-Process -Name Trace -ErrorAction SilentlyContinue | ForEach-Object { $_.CloseMainWindow() | Out-Null }
Start-Sleep -Milliseconds 800
Get-Process -Name Trace -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 400

$area = [System.Windows.Forms.Screen]::PrimaryScreen.WorkingArea
$halfW = [int]($area.Width / 2)

function Start-Trace([string]$renderer) {
    # Set on THIS process so the child inherits it, then cleared -- a leaked
    # TRACE_RENDERER would silently apply to every later launch from this shell.
    $env:TRACE_RENDERER = $renderer
    $p = Start-Process -FilePath $Exe -ArgumentList "`"$Clip`"" -PassThru
    Remove-Item -Path "env:TRACE_RENDERER" -ErrorAction SilentlyContinue

    # Wait for the window rather than sleeping a fixed amount: the d3d11 window
    # has a device and a swapchain to create first and is reliably slower.
    $deadline = (Get-Date).AddSeconds(20)
    while ((Get-Date) -lt $deadline) {
        $p.Refresh()
        if ($p.HasExited) { Write-Output "  $renderer EXITED EARLY code $($p.ExitCode)"; return $null }
        if ($p.MainWindowHandle -ne 0) { return $p }
        Start-Sleep -Milliseconds 200
    }
    Write-Output "  $renderer never showed a window"
    return $null
}

function Place($p, [int]$x, [string]$label) {
    [W]::ShowWindow($p.MainWindowHandle, [W]::SW_RESTORE) | Out-Null
    [W]::SetWindowPos($p.MainWindowHandle, [IntPtr]::Zero,
        $x, $area.Y, $halfW, $area.Height, [W]::SWP_SHOWWINDOW) | Out-Null
    Start-Sleep -Milliseconds 500
    $r = New-Object W+RECT
    [W]::GetWindowRect($p.MainWindowHandle, [ref]$r) | Out-Null
    Write-Output ("  {0,-6} pid {1,-6} at {2},{3} {4}x{5}" -f `
        $label, $p.Id, $r.L, $r.T, ($r.R - $r.L), ($r.B - $r.T))
}

Write-Output ("clip   {0}" -f (Split-Path -Leaf $Clip))
Write-Output ("screen {0}x{1} work area, half = {2}px" -f $area.Width, $area.Height, $halfW)
# The refresh rate belongs with any smoothness judgement made from this layout.
# 24fps on a 60Hz panel is a 2:3 cadence imposed by the DISPLAY on every player,
# and it looks like judder that neither backend causes and neither can remove;
# on this box's 240Hz mode a 24fps frame is exactly 10 refreshes. Comparing the
# two backends is still valid at any rate -- they share the display -- but
# comparing today's feel against a note taken at another rate is not.
& "$PSScriptRoot\refresh.ps1" -Seconds 0 2>&1 | Where-Object { $_ -match 'exact rate|nominal|display ' }

$cpu = Start-Trace "cpu"
if (-not $cpu) { exit 1 }
Place $cpu $area.X "cpu"

$gpu = Start-Trace "d3d11"
if (-not $gpu) { exit 1 }
Place $gpu ($area.X + $halfW) "d3d11"

if ($NoVerify) { exit 0 }

# --- confirm each window adopted the backend it was asked for -----------------
# The HUD names what is ACTUALLY presenting, so this reads it back off the pixels
# rather than trusting the environment variable that requested it.
Start-Sleep -Seconds 2
$outDir = "$env:TEMP\trace_sidebyside"
New-Item -ItemType Directory -Force $outDir | Out-Null

$strips = @()
foreach ($pair in @(@{P = $cpu; L = "left  (asked for cpu)" }, @{P = $gpu; L = "right (asked for d3d11)" })) {
    $r = New-Object W+RECT
    [W]::GetWindowRect($pair.P.MainWindowHandle, [ref]$r) | Out-Null
    $w = $r.R - $r.L; $h = $r.B - $r.T
    $bmp = New-Object System.Drawing.Bitmap $w, $h
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size)
    $g.Dispose()
    $png = Join-Path $outDir ("{0}.png" -f $pair.P.Id)
    $bmp.Save($png, [System.Drawing.Imaging.ImageFormat]::Png)
    $strips += @{ Png = $png; Label = $pair.L; W = $w; H = $h }
    $bmp.Dispose()
}

# The `renderer` field sits on the colour line. Crop a band around it from each
# window and stack the two, so one image answers the question.
$bandH = 34
$fromBottom = 395
$dst = New-Object System.Drawing.Bitmap 1700, ($strips.Count * ($bandH * 2 + 22))
$g = [System.Drawing.Graphics]::FromImage($dst)
$g.InterpolationMode = 'NearestNeighbor'
$g.Clear([System.Drawing.Color]::FromArgb(20, 0, 40))
$f = New-Object System.Drawing.Font('Consolas', 12, [System.Drawing.FontStyle]::Bold)
$i = 0
foreach ($s in $strips) {
    $src = [System.Drawing.Bitmap]::FromFile($s.Png)
    $top = [Math]::Max(0, $src.Height - $fromBottom)
    $ty = $i * ($bandH * 2 + 22)
    $g.DrawString($s.Label, $f, [System.Drawing.Brushes]::Yellow, 4, ($ty + 2))
    $g.DrawImage($src,
        (New-Object System.Drawing.Rectangle 0, ($ty + 22), 1700, ($bandH * 2)),
        (New-Object System.Drawing.Rectangle 0, $top, 850, $bandH), 'Pixel')
    $src.Dispose(); $i++
}
$out = Join-Path $outDir "renderer_check.png"
$dst.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $dst.Dispose()
Write-Output $out
