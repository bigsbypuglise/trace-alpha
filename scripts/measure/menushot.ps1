# Open each top-level menu in turn and capture the SCREEN, not the window.
#
# A menu is a separate top-level popup window on Windows, so a GetWindowRect
# capture of the Trace window does not contain it -- the shot comes back with a
# closed menu bar and looks exactly like a menu that failed to open. That is the
# same class of fault as phase 4's overlay aim: a plausible picture of nothing
# happening.
#
# So this captures a screen region anchored on the window and wide enough to
# hold a dropped menu, and it asserts the capture CHANGED against the same
# region with no menu open. A run that reports "no change" is a run where the
# keystroke did not arrive, which is worth knowing before reading the picture.
param(
    [string]$OutDir = "$env:TEMP\trace_menus",
    [string]$ProcName = "Trace",
    # Alt-key mnemonics, in menu-bar order.
    [string[]]$Menus = @("f", "e", "v", "w", "h")
)

Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class MW {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
}
"@

New-Item -ItemType Directory -Force $OutDir | Out-Null

$p = Get-Process -Name $ProcName -ErrorAction SilentlyContinue |
     Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $p) { Write-Output "no window"; exit 1 }
$h = $p.MainWindowHandle

$r = New-Object MW+RECT
[MW]::GetWindowRect($h, [ref]$r) | Out-Null

# A band from the title bar down far enough for the longest menu, and the full
# window width so a menu dropping from Help is inside it.
$x = $r.L
$y = $r.T
$w = [Math]::Min(1400, $r.R - $r.L)
$hgt = 620

function Grab([string]$path) {
    $bmp = New-Object System.Drawing.Bitmap $w, $hgt
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($x, $y, 0, 0, (New-Object System.Drawing.Size $w, $hgt))
    $g.Dispose()
    $bmp.Save($path)
    return $bmp
}

function MeanAbsDiff($a, $b) {
    # LockBits, because a per-pixel GetPixel over 1400x620 takes minutes and the
    # phase 10 harness already learned that once.
    $rect = New-Object System.Drawing.Rectangle 0, 0, $a.Width, $a.Height
    $fmt = [System.Drawing.Imaging.PixelFormat]::Format32bppArgb
    $da = $a.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly, $fmt)
    $db = $b.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly, $fmt)
    $n = $a.Width * $a.Height * 4
    $ba = New-Object byte[] $n
    $bb = New-Object byte[] $n
    [System.Runtime.InteropServices.Marshal]::Copy($da.Scan0, $ba, 0, $n)
    [System.Runtime.InteropServices.Marshal]::Copy($db.Scan0, $bb, 0, $n)
    $a.UnlockBits($da); $b.UnlockBits($db)
    $changed = 0
    for ($i = 0; $i -lt $n; $i += 4) {
        if ([Math]::Abs([int]$ba[$i] - [int]$bb[$i]) -gt 8) { $changed++ }
    }
    return [Math]::Round(100.0 * $changed / ($a.Width * $a.Height), 2)
}

[MW]::SetForegroundWindow($h) | Out-Null
Start-Sleep -Milliseconds 400
# Escape first: a menu left open by a previous run would poison the baseline
# into looking like the menu state, and every leg would then read "no change".
[System.Windows.Forms.SendKeys]::SendWait("{ESC}")
Start-Sleep -Milliseconds 250
$base = Grab "$OutDir\00-closed.png"

foreach ($m in $Menus) {
    [MW]::SetForegroundWindow($h) | Out-Null
    Start-Sleep -Milliseconds 200
    [System.Windows.Forms.SendKeys]::SendWait("%$m")
    Start-Sleep -Milliseconds 500
    $shot = Grab "$OutDir\menu-$m.png"
    $delta = MeanAbsDiff $base $shot
    $verdict = if ($delta -lt 1.0) { "NOT OPENED" } else { "ok" }
    Write-Output ("  alt+{0,-2} changed {1,6}%  {2}" -f $m, $delta, $verdict)
    $shot.Dispose()
    [System.Windows.Forms.SendKeys]::SendWait("{ESC}")
    Start-Sleep -Milliseconds 300
}
$base.Dispose()
Write-Output "shots in $OutDir"
