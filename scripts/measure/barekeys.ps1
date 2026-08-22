# Do Trace's bare-letter commands collide with the menu bar's mnemonics?
#
# Owner report, 2026-08-21: plain F changes the readout AND opens File; plain E
# changes the readout AND opens Edit; H toggles the dev HUD and Help is &Help.
# S, T, V and W are clean today and are one binding away from the same fault.
#
# THE MECHANISM UNDER TEST: QMenuBar::keyPressEvent matches a BARE letter
# against its menus' mnemonics whenever the menu bar has keyboard focus -- the
# modifier test in Qt's own handler is `!e->modifiers() || Alt || Meta`, so no
# Alt is required. CLAUDE.md already records the state as a live hazard from the
# other side ("leaving Qt's menu mode can leave the menu bar focused, where
# Space is silently swallowed").
#
# Two states, because the fault only exists in one of them and a run that tests
# the other reports a clean build:
#   picture   the window has focus (click-activate on the picture)
#   menubar   after Alt+F then Escape -- Qt's menu mode left, focus retained
#
# Two families, because F/S/E/T are dispatched by ShortcutTable inside
# MainWindow::keyPressEvent while H is a QAction shortcut Qt runs before the
# window sees the key. That difference is the likely reason two were noticed and
# one was not, so a fix has to be verified on both.
#
# PASS is: no popup window appears for any bare key in either state, and the
# command still happens (the readout crops say which mode is in force, and the
# HUD line says whether it is showing).

param(
    [Parameter(Mandatory = $true)][string]$Clip,
    [string]$Renderer = 'd3d11',
    [string]$Exe,
    [string]$OutDir = "$env:TEMP\trace-barekeys"
)

Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Add-Type @"
using System; using System.Runtime.InteropServices; using System.Text;
public class BK {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X,Y; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x,int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f,int dx,int dy,uint d,UIntPtr e);
  [DllImport("user32.dll")] public static extern void keybd_event(byte k, byte s, uint f, UIntPtr e);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int m);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  public delegate bool EnumProc(IntPtr h, IntPtr l);
}
"@

if (-not $Exe) {
    $repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
    $Exe = Join-Path $repo "build\app\Release\Trace.exe"
}
if (-not (Test-Path $Exe))  { Write-Output "no exe at $Exe";  exit 1 }
if (-not (Test-Path $Clip)) { Write-Output "no clip at $Clip"; exit 1 }
New-Item -ItemType Directory -Force $OutDir | Out-Null
Get-ChildItem $OutDir -Filter *.png -ErrorAction SilentlyContinue | Remove-Item -Force

Get-Process -Name Trace -ErrorAction SilentlyContinue | ForEach-Object { $_.CloseMainWindow() | Out-Null }
Start-Sleep -Milliseconds 700
Get-Process -Name Trace -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 400
$env:TRACE_RENDERER = $Renderer
$env:TRACE_HUD = "1"
$proc = Start-Process -FilePath $Exe -ArgumentList ('"' + $Clip + '"') -PassThru
Remove-Item env:TRACE_RENDERER -ErrorAction SilentlyContinue
Remove-Item env:TRACE_HUD -ErrorAction SilentlyContinue
Start-Sleep -Seconds 4
if ($proc.HasExited) { Write-Output "EXITED EARLY code $($proc.ExitCode)"; exit 1 }
$p = Get-Process -Name Trace -ErrorAction SilentlyContinue |
     Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $p) { Write-Output "BAREKEYS: no window"; exit 1 }
$h = $p.MainWindowHandle
$tracePid = $p.Id

# A Qt popup menu is its own top-level window. Counting them is what separates
# "the key opened a menu" from "the key did nothing", which no screenshot of the
# main window's rect can do -- the popup may not even overlap it.
function Popup-Count {
    $script:n = 0
    $cb = [BK+EnumProc]{
        param($hw, $lp)
        $o = 0
        [BK]::GetWindowThreadProcessId($hw, [ref]$o) | Out-Null
        if ($o -eq $script:tracePid -and [BK]::IsWindowVisible($hw)) {
            $sb = New-Object System.Text.StringBuilder 256
            [BK]::GetClassName($hw, $sb, 256) | Out-Null
            if ($sb.ToString() -like '*Popup*') { $script:n++ }
        }
        return $true
    }
    [BK]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
    return $script:n
}
$script:tracePid = $tracePid

function Focus-Window {
    [BK]::SetForegroundWindow($h) | Out-Null
    Start-Sleep -Milliseconds 250
    if ([BK]::GetForegroundWindow() -eq $h) { return $true }
    [BK]::keybd_event(0xA4, 0, 0, [UIntPtr]::Zero)
    [BK]::keybd_event(0xA4, 0, 2, [UIntPtr]::Zero)
    [BK]::SetForegroundWindow($h) | Out-Null
    Start-Sleep -Milliseconds 250
    return [BK]::GetForegroundWindow() -eq $h
}

# Click the picture. This is click-activate (feedback item 13) and is also the
# only thing that takes Qt's focus back off the menu bar.
function Click-Picture {
    Focus-Window | Out-Null
    $cr = New-Object BK+RECT; [BK]::GetClientRect($h, [ref]$cr) | Out-Null
    $o  = New-Object BK+POINT; [BK]::ClientToScreen($h, [ref]$o) | Out-Null
    [BK]::SetCursorPos([int]($o.X + $cr.R / 2), [int]($o.Y + $cr.B / 2)) | Out-Null
    Start-Sleep -Milliseconds 150
    [BK]::mouse_event(2, 0, 0, 0, [UIntPtr]::Zero)
    [BK]::mouse_event(4, 0, 0, 0, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 350
}

# The first HUD line, which carries `Readout: <mode>` -- the observable for
# F/S/E/T. Cropped and saved rather than parsed: the HUD is pixels.
#
# Measured UP FROM THE WINDOW'S BOTTOM, never as a fraction of its height. The
# HUD widget sits below the viewer and grows upward from the bottom edge, so its
# first line is a fixed number of rows above the bottom for a given media class
# and a fraction lands on the picture instead -- which is exactly what the first
# version of this did. kHudRows is the video HUD's line count; an audio-only
# file prints two lines and would need a different offset.
$kHudFirstLineUp = 375
function Save-Readout([string]$tag) {
    $r = New-Object BK+RECT; [BK]::GetWindowRect($h, [ref]$r) | Out-Null
    $w = [Math]::Min(620, $r.R - $r.L)
    $y = $r.B - $kHudFirstLineUp
    $bmp = New-Object System.Drawing.Bitmap $w, 20
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($r.L, $y, 0, 0, $bmp.Size)
    $bmp.Save((Join-Path $OutDir ($tag + ".png")), [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose()
}

function Escape-Out {
    [System.Windows.Forms.SendKeys]::SendWait("{ESC}"); Start-Sleep -Milliseconds 250
    [System.Windows.Forms.SendKeys]::SendWait("{ESC}"); Start-Sleep -Milliseconds 250
}

# Leave Qt's menu mode the way a user does: open a menu, then Escape. The menu
# bar keeps keyboard focus afterwards, which is the state under test.
function Enter-MenuBarFocus {
    [System.Windows.Forms.SendKeys]::SendWait("%f"); Start-Sleep -Milliseconds 500
    [System.Windows.Forms.SendKeys]::SendWait("{ESC}"); Start-Sleep -Milliseconds 500
}

$fail = 0
foreach ($state in @('picture', 'menubar')) {
    foreach ($k in @('f', 's', 'e', 't', 'h', 'v', 'w')) {
        Click-Picture
        # Put the readout somewhere none of the keys under test would leave it,
        # so "the command ran" is legible rather than inferred.
        [System.Windows.Forms.SendKeys]::SendWait("t"); Start-Sleep -Milliseconds 300
        if ($state -eq 'menubar') { Enter-MenuBarFocus }
        $before = Popup-Count
        [System.Windows.Forms.SendKeys]::SendWait($k); Start-Sleep -Milliseconds 600
        $after = Popup-Count
        $verdict = if ($after -gt $before) { "MENU OPENED"; } else { "no menu" }
        if ($after -gt $before) { $fail++ }
        Write-Output ("{0,-8} {1}  popups {2}->{3}  {4}" -f $state, $k, $before, $after, $verdict)
        Escape-Out
        Click-Picture
        Save-Readout ("{0}-{1}" -f $state, $k)
        # H is a toggle: put the HUD back or every later crop is blank.
        if ($k -eq 'h') { [System.Windows.Forms.SendKeys]::SendWait("h"); Start-Sleep -Milliseconds 400 }
    }
}
Write-Output ""
Write-Output ("BAREKEYS: {0}" -f $(if ($fail -eq 0) { "PASS - no bare key opened a menu in either state" }
                                  else { "FAIL - $fail bare keys opened a menu" }))
Write-Output ("readout crops in " + $OutDir)
