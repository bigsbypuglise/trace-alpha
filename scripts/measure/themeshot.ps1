# UI redesign roadmap step 10, typography half: capture the surfaces it changes.
#
# THE POPUP MENU IS THE ONE SURFACE THE DESIGN PACKAGE DOES NOT SHOW. Its three
# mockup screens are all of the player with no menu open, so the popup's colours,
# radius and spacing are DERIVED from the package rather than copied out of it --
# the same position the three authored transport glyphs are in. That makes
# looking at it the check, and this is what puts it on screen.
#
# A MENU IS A SEPARATE POPUP WINDOW, so a capture of the main window's rect shows
# a closed menu bar whether or not it opened. menushot.ps1 records that trap; this
# captures the whole SCREEN region around the window instead and asserts the
# capture changed, so an unopened menu cannot pass.
#
# Menus are opened BY MNEMONIC, never by counting DOWN arrows -- a miscount
# silently activates the item next to the one under test.
param(
    [Parameter(Mandatory = $true)][string]$Clip,
    [string]$Exe,
    [string]$OutDir = "$env:TEMP\tracetheme"
)

$ErrorActionPreference = 'Continue'
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class TH {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool f);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, IntPtr p);
  [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
}
"@

if (-not $Exe) { $Exe = Join-Path $PSScriptRoot "..\..\build\app\Release\Trace.exe" }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

Get-Process -Name Trace -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 800
$env:TRACE_HUD = "0"
$env:TRACE_NO_AUDIO = "1"
$env:TRACE_SETTINGS_FILE = "$OutDir\scratch.ini"
Start-Process -FilePath $Exe -ArgumentList ('"' + $Clip + '"') | Out-Null
Remove-Item env:TRACE_HUD, env:TRACE_NO_AUDIO, env:TRACE_SETTINGS_FILE -ErrorAction SilentlyContinue
Start-Sleep -Seconds 5

$p = Get-Process -Name Trace -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $p) { Write-Output "no window"; exit 1 }
$h = $p.MainWindowHandle
$fg = [TH]::GetForegroundWindow()
$tid = [TH]::GetWindowThreadProcessId($fg, [IntPtr]::Zero)
$me = [TH]::GetCurrentThreadId()
[TH]::AttachThreadInput($me, $tid, $true) | Out-Null
[TH]::SetForegroundWindow($h) | Out-Null
[TH]::AttachThreadInput($me, $tid, $false) | Out-Null
Start-Sleep -Milliseconds 400

$r = New-Object TH+RECT
[TH]::GetWindowRect($h, [ref]$r) | Out-Null
# Generous margin so a popup that extends past the window is still in frame.
$pad = 40
$x = [Math]::Max(0, $r.L - $pad); $y = [Math]::Max(0, $r.T - $pad)
$w = ($r.R - $r.L) + $pad * 2; $ht = ($r.B - $r.T) + $pad * 2

function Shot([string]$name) {
    $bmp = New-Object System.Drawing.Bitmap $w, $ht
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($x, $y, 0, 0, $bmp.Size)
    $g.Dispose()
    $path = Join-Path $OutDir "$name.png"
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    return $path
}

function Differs([string]$a, [string]$b) {
    $ia = [System.Drawing.Bitmap]::FromStream((New-Object System.IO.MemoryStream (,[System.IO.File]::ReadAllBytes($a))))
    $ib = [System.Drawing.Bitmap]::FromStream((New-Object System.IO.MemoryStream (,[System.IO.File]::ReadAllBytes($b))))
    $n = 0
    for ($yy = 0; $yy -lt $ia.Height; $yy += 3) {
        for ($xx = 0; $xx -lt $ia.Width; $xx += 3) {
            if ($ia.GetPixel($xx, $yy).ToArgb() -ne $ib.GetPixel($xx, $yy).ToArgb()) { $n++ }
        }
    }
    $ia.Dispose(); $ib.Dispose()
    return $n
}

# Reveal the chrome, which is what puts the menu bar on screen at all.
[TH]::SetCursorPos(($r.L + [int](($r.R - $r.L) / 2)), ($r.T + [int](($r.B - $r.T) / 2))) | Out-Null
Start-Sleep -Milliseconds 300
$base = Shot "00-chrome"
Write-Output "captured 00-chrome"

foreach ($m in @(@('f','File'), @('e','Edit'), @('v','View'), @('w','Window'), @('h','Help'))) {
    [System.Windows.Forms.SendKeys]::SendWait("%$($m[0])")
    Start-Sleep -Milliseconds 700
    $png = Shot ("menu-" + $m[1])
    $d = Differs $base $png
    Write-Output ("{0,-8} menu: {1} sampled px differ from the closed state{2}" -f $m[1], $d, $(if ($d -lt 200) { "   <-- SUSPECT, did it open?" } else { "" }))
    [System.Windows.Forms.SendKeys]::SendWait("{ESC}")
    Start-Sleep -Milliseconds 400
}

Write-Output ""
Write-Output "captures in $OutDir"
