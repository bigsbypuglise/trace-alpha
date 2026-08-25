# THE Color Transform... DIALOG (EXR/colour stage 3 step 2).
#
#   colordialog.ps1 -Mode open      # the dialog on screen, populated
#   colordialog.ps1 -Mode apply     # OK applies, the HUD names what is in force
#   colordialog.ps1 -Mode persist   # the choice survives a restart
#   colordialog.ps1 -Mode missing   # a config that has gone: bypass, say so, still open
#   colordialog.ps1 -Mode guard     # C / [ / ] must not leak into the dialog
#
# TWO RECORDED TRAPS THIS SCRIPT IS BUILT AROUND, both of which have accused a
# correct build in this project before:
#
#   Leaving Qt's menu mode can leave the MENU BAR focused, where Space is
#   silently swallowed and a bare letter opens a menu instead of reaching Trace.
#   So every leg that sends keys after touching a menu clicks the picture first
#   (item 13's click-activate) rather than assuming focus came back.
#
#   A capture taken with the pointer parked over Trace's chrome can resolve to a
#   TOOLTIP rather than the main window -- 55x19, and the run then reports
#   `frames 0` as if the build could not play. restart/capture route through
#   tracewindow.ps1, which rejects a rect under 300px; this script additionally
#   parks the cursor clear before any capture.
#
# THE DIALOG IS A SEPARATE TOP-LEVEL WINDOW, so it is captured by scanning the
# screen for it by class/title rather than by cropping Trace's own rect.

param(
    [ValidateSet('open','apply','persist','missing','guard')][string]$Mode = 'open',
    [string]$Root = 'C:\Users\andre\Documents\Claude_Cowork\Trace_Testing_Assets',
    [string]$Clip = '',
    [string]$Exe  = ''
)
$ErrorActionPreference = 'Stop'
if (-not $Clip) { $Clip = Join-Path $Root '15_Redshift_ACES_EXR\MultlayerAces\icecream_passes0000.exr' }
if (-not $Exe)  { $Exe  = Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'build\app\Release\Trace.exe' }
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms

$sp = Join-Path $env:TEMP 'trace-colordialog'
New-Item -ItemType Directory -Force -Path $sp | Out-Null

Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public class CDW {
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x,int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f,int dx,int dy,uint d,UIntPtr e);
  [DllImport("user32.dll")] public static extern void keybd_event(byte k, byte s, uint f, UIntPtr e);
  public delegate bool EnumProc(IntPtr h, IntPtr p);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
  // CharSet.Unicode on every one of these is NOT decoration: the default is
  // Ansi, so a managed string handed to a ...W function is marshalled as ANSI
  // and read back as UTF-16 -- which is how window titles once came out one
  // character long and a live window was reported NOT FOUND three times.
  public static IntPtr FindByTitle(uint pid, string want) {
    IntPtr found = IntPtr.Zero;
    EnumWindows((h,p) => {
      uint wp; GetWindowThreadProcessId(h, out wp);
      if (wp != pid || !IsWindowVisible(h)) return true;
      var sb = new StringBuilder(512); GetWindowTextW(h, sb, 512);
      if (sb.ToString() == want) { found = h; return false; }
      return true;
    }, IntPtr.Zero);
    return found;
  }
}
'@

function Park { [System.Windows.Forms.Cursor]::Position = New-Object System.Drawing.Point 40,40 }

# WINDOWS REFUSES SetForegroundWindow TO A BACKGROUND PROCESS, SILENTLY, and a
# harness that assumes it worked sends every key to its own terminal and then
# reports the feature missing. This is the recorded fix: call it, READ
# GetForegroundWindow() BACK, and on refusal tap Alt -- which releases the
# foreground lock -- and try once more. Verified rather than assumed, every time.
function Focus-Window([IntPtr]$h) {
    [void][CDW]::SetForegroundWindow($h)
    Start-Sleep -Milliseconds 250
    if ([CDW]::GetForegroundWindow() -eq $h) { return $true }
    [CDW]::keybd_event(0xA4, 0, 0, [UIntPtr]::Zero)   # VK_LMENU down
    [CDW]::keybd_event(0xA4, 0, 2, [UIntPtr]::Zero)   # up
    [void][CDW]::SetForegroundWindow($h)
    Start-Sleep -Milliseconds 250
    return [CDW]::GetForegroundWindow() -eq $h
}

function Start-Trace([string]$ini, [string[]]$extra) {
    $all = @("TRACE_HUD=1", "TRACE_SETTINGS_FILE=$ini") + $extra
    & "$PSScriptRoot\restart.ps1" -Clip $Clip -Exe $Exe -Env $all -SettleSeconds 6 | Out-Null
    return (Get-Process -Name Trace -ErrorAction SilentlyContinue | Select-Object -First 1)
}

# Click the PICTURE before sending keys. Recorded lesson: after any menu
# interaction the menu bar can keep keyboard focus, and a bare letter then opens
# a menu instead of reaching Trace.
function Click-Picture([IntPtr]$hwnd) {
    $r = New-Object CDW+RECT
    [void][CDW]::GetWindowRect($hwnd, [ref]$r)
    $x = [int](($r.L + $r.R) / 2); $y = [int]($r.T + ($r.B - $r.T) * 0.35)
    [void][CDW]::SetCursorPos($x, $y)
    Start-Sleep -Milliseconds 150
    [CDW]::mouse_event(2, 0, 0, 0, [UIntPtr]::Zero)   # LEFTDOWN
    [CDW]::mouse_event(4, 0, 0, 0, [UIntPtr]::Zero)   # LEFTUP
    Start-Sleep -Milliseconds 350
}

function Open-Dialog([System.Diagnostics.Process]$proc) {
    if (-not (Focus-Window $proc.MainWindowHandle)) {
        # A denial is a HARNESS failure and must read as one. Reported as
        # "dialog not found" it looks exactly like a build with no dialog in it.
        Write-Warning "Focus-Window was refused; keys would go elsewhere"
        return [IntPtr]::Zero
    }
    # Alt+V then 'm' -- Color Transfor&m... The mnemonic, never a count of DOWN
    # arrows: counting has to know whether the first item is already highlighted
    # and has to skip separators, and a miscount silently activates the item next
    # to the one under test.
    [System.Windows.Forms.SendKeys]::SendWait("%v"); Start-Sleep -Milliseconds 600
    [System.Windows.Forms.SendKeys]::SendWait("m"); Start-Sleep -Milliseconds 1400
    return [CDW]::FindByTitle([uint32]$proc.Id, "Color Transform")
}

function Shoot-Window([IntPtr]$hwnd, [string]$out) {
    Park
    Start-Sleep -Milliseconds 200
    $r = New-Object CDW+RECT
    if (-not [CDW]::GetWindowRect($hwnd, [ref]$r)) { return $false }
    $w = $r.R - $r.L; $h = $r.B - $r.T
    if ($w -lt 200 -or $h -lt 100) { Write-Warning "window is ${w}x${h} - refusing"; return $false }
    $bmp = New-Object System.Drawing.Bitmap $w,$h
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($r.L, $r.T, 0, 0, (New-Object System.Drawing.Size $w,$h))
    $bmp.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose()
    Write-Output ("  saved {0} ({1}x{2})" -f $out, $w, $h)
    return $true
}

# Fraction of sampled picture pixels that differ. The picture band only -- the
# HUD's own per-tick counters change whatever the picture does, so a whole-window
# diff would report motion on a build where the transform did nothing.
function PictureDiff([string]$a, [string]$b) {
    $x = [System.Drawing.Bitmap]::FromFile($a); $y = [System.Drawing.Bitmap]::FromFile($b)
    if ($x.Width -ne $y.Width -or $x.Height -ne $y.Height) { $x.Dispose(); $y.Dispose(); return -1 }
    $y0 = [int]($x.Height * 0.12); $y1 = [int]($x.Height * 0.60)
    $d = 0; $n = 0
    for ($j = $y0; $j -lt $y1; $j += 5) {
        for ($i = 16; $i -lt ($x.Width - 16); $i += 5) {
            $p = $x.GetPixel($i,$j); $q = $y.GetPixel($i,$j)
            if ([math]::Abs($p.R-$q.R) -gt 2 -or [math]::Abs($p.G-$q.G) -gt 2 -or [math]::Abs($p.B-$q.B) -gt 2) { $d++ }
            $n++
        }
    }
    $x.Dispose(); $y.Dispose()
    if ($n -eq 0) { return -1 }
    return [math]::Round(100.0 * $d / $n, 3)
}

function Shoot-Hud([string]$out) { Park; & "$PSScriptRoot\capture.ps1" -Out $out | Out-Null }

$ini = Join-Path $sp "$Mode.ini"
if (Test-Path $ini) { Remove-Item $ini -Force }

if ($Mode -eq 'open') {
    $p = Start-Trace $ini @()
    $h = Open-Dialog $p
    if ($h -eq [IntPtr]::Zero) { Write-Warning "FAIL - the Color Transform dialog was not found"; exit 1 }
    [void](Shoot-Window $h (Join-Path $sp 'dialog.png'))
    Write-Output "PASS - dialog open; read the capture for the populated combos"
    Get-Process -Name Trace -EA SilentlyContinue | Stop-Process -Force -EA SilentlyContinue
}
elseif ($Mode -eq 'apply') {
    $p = Start-Trace $ini @()
    Shoot-Hud (Join-Path $sp 'before.png')
    $h = Open-Dialog $p
    if ($h -eq [IntPtr]::Zero) { Write-Warning "FAIL - dialog not found"; exit 1 }
    [void](Shoot-Window $h (Join-Path $sp 'dialog-apply.png'))
    [System.Windows.Forms.SendKeys]::SendWait("{ENTER}"); Start-Sleep -Milliseconds 2000
    Shoot-Hud (Join-Path $sp 'after.png')
    $diff = PictureDiff (Join-Path $sp 'before.png') (Join-Path $sp 'after.png')
    Write-Output ("  picture changed on {0}% of sampled pixels" -f $diff)
    if ($diff -lt 5) { Write-Warning "FAIL - the picture did not change; the transform did not engage" }
    else { Write-Output "PASS - transform engaged; read after.png for the HUD's config/display/view" }
    Get-Process -Name Trace -EA SilentlyContinue | Stop-Process -Force -EA SilentlyContinue
}
elseif ($Mode -eq 'persist') {
    $p = Start-Trace $ini @()
    $h = Open-Dialog $p
    if ($h -eq [IntPtr]::Zero) { Write-Warning "FAIL - dialog not found"; exit 1 }
    [System.Windows.Forms.SendKeys]::SendWait("{ENTER}"); Start-Sleep -Milliseconds 2000
    Shoot-Hud (Join-Path $sp 'persist-session1.png')
    Get-Process -Name Trace -EA SilentlyContinue | Stop-Process -Force -EA SilentlyContinue
    Start-Sleep -Milliseconds 800
    # SAME ini, so this is a restart rather than a fresh machine.
    $all = @("TRACE_HUD=1", "TRACE_SETTINGS_FILE=$ini")
    & "$PSScriptRoot\restart.ps1" -Clip $Clip -Exe $Exe -Env $all -SettleSeconds 6 | Out-Null
    Shoot-Hud (Join-Path $sp 'persist-session2.png')
    Write-Output "  compare persist-session1.png and persist-session2.png: the HUD must name the same config"
    Get-Process -Name Trace -EA SilentlyContinue | Stop-Process -Force -EA SilentlyContinue
}
elseif ($Mode -eq 'missing') {
    # A CONFIG THAT HAS GONE: bypass, say so ONCE, and never block the media.
    #
    # RUN AS A PAIR, because the failing half alone proves nothing: a build that
    # ignored the saved transform entirely would also "open the media". The
    # PRESENT half must show the transform actually restored and engaged, so the
    # ABSENT half is measuring a real fallback rather than a no-op.
    $tmpCfg = Join-Path $sp 'temp-config.ocio'
    Copy-Item (Join-Path $Root '15_Redshift_ACES_EXR\config.ocio') $tmpCfg -Force
    $lines = @(
        '[color]',
        'transformKind=displayview',
        # FORWARD SLASHES, and not for tidiness: QSettings' INI format escapes a
        # backslash, so a raw Windows path written by hand here would be read
        # back mangled -- and the run would then "prove" the fallback works when
        # what it actually proved is that a corrupt path does not load.
        ("configPath=" + ($tmpCfg.Replace('\', '/'))),
        'inputSpace=ACEScg',
        'display=sRGB',
        'view=ACES 1.0 SDR-video',
        'transformEnabled=true'
    )

    Set-Content -Path $ini -Value $lines -Encoding utf8
    & "$PSScriptRoot\restart.ps1" -Clip $Clip -Exe $Exe -Env @("TRACE_HUD=1","TRACE_SETTINGS_FILE=$ini") -SettleSeconds 7 | Out-Null
    Shoot-Hud (Join-Path $sp 'missing-present.png')
    Get-Process -Name Trace -EA SilentlyContinue | Stop-Process -Force -EA SilentlyContinue
    Start-Sleep -Milliseconds 700

    # Now take the config away and restart from the SAME settings.
    Remove-Item $tmpCfg -Force
    Set-Content -Path $ini -Value $lines -Encoding utf8
    & "$PSScriptRoot\restart.ps1" -Clip $Clip -Exe $Exe -Env @("TRACE_HUD=1","TRACE_SETTINGS_FILE=$ini") -SettleSeconds 7 | Out-Null
    Shoot-Hud (Join-Path $sp 'missing-absent.png')

    # The media must still be OPEN. Judged from the picture, not from the app
    # merely being alive: a window showing the empty-state mark would also
    # survive this.
    $d = PictureDiff (Join-Path $sp 'missing-present.png') (Join-Path $sp 'missing-absent.png')
    Write-Output ("  picture differs between the two sessions on {0}% of sampled pixels" -f $d)
    Write-Output "  read missing-present.png (transform ON, config named) and"
    Write-Output "  missing-absent.png (media OPEN, transform bypassed, reason said once)"
    Get-Process -Name Trace -EA SilentlyContinue | Stop-Process -Force -EA SilentlyContinue
}
elseif ($Mode -eq 'guard') {
    # THE TEXT-FIELD / SHORTCUT GUARD FOR A NEW MODAL WINDOW. C, [ and ] are
    # QAction shortcuts on the main window; with a modal dialog up they must not
    # fire. The negative control is the SAME keys with no dialog open, which must
    # move the picture -- without it this leg passes on a build where the keys
    # do nothing at all.
    $p = Start-Trace $ini @()
    $hwnd = $p.MainWindowHandle
    if (-not (Focus-Window $hwnd)) { Write-Warning "HARNESS - focus refused"; exit 1 }
    Shoot-Hud (Join-Path $sp 'guard-base.png')
    Click-Picture $hwnd
    # `{]}` NOT `]`. SendKeys RESERVES both brackets and swallows them unescaped
    # -- the recorded trap that once made nine of ten presses read as no-ops on a
    # build where all ten worked.
    #
    # AND TWO PRESSES, NOT ONE, WHICH IS THE SUBTLER HALF. Pass 1/9 is the root
    # layer and pass 2/9 is `Beauty` -- and stage 2 MEASURED those two as the
    # same render written twice, differing only by independent DWAA compression
    # at 0.36% mean absolute difference. So a single `]` moves to a visually
    # IDENTICAL pass, and a control built on it reads 0% on a perfectly working
    # build. Two presses reach Cryptomatte, which is Data and displays as raw
    # numeric IDs. The first version of this leg read 0% for exactly that reason.
    [System.Windows.Forms.SendKeys]::SendWait("{]}"); Start-Sleep -Milliseconds 900
    [System.Windows.Forms.SendKeys]::SendWait("{]}"); Start-Sleep -Milliseconds 1200
    Shoot-Hud (Join-Path $sp 'guard-control.png')
    $ctl = PictureDiff (Join-Path $sp 'guard-base.png') (Join-Path $sp 'guard-control.png')
    Write-Output ("  CONTROL: ']' x2 with no dialog moved the picture {0}%" -f $ctl)

    $h = Open-Dialog $p
    if ($h -eq [IntPtr]::Zero) { Write-Warning "FAIL - dialog not found"; exit 1 }
    Shoot-Hud (Join-Path $sp 'guard-dlg-before.png')
    # THE SAME GESTURE THE CONTROL JUST PROVED VISIBLE, plus `c`. Two `]` presses
    # so the brackets are as detectable here as they were there -- one press each
    # of `[` and `]` would cancel out and the leg would be sensitive to `c` only.
    foreach ($k in @(']', ']', 'c')) {
        $send = if ($k -eq '[') { '{[}' } elseif ($k -eq ']') { '{]}' } else { $k }
        [System.Windows.Forms.SendKeys]::SendWait($send); Start-Sleep -Milliseconds 500
    }
    Shoot-Hud (Join-Path $sp 'guard-dlg-after.png')
    $leak = PictureDiff (Join-Path $sp 'guard-dlg-before.png') (Join-Path $sp 'guard-dlg-after.png')
    Write-Output ("  GUARDED: ], ] and c with the dialog open moved the picture {0}%" -f $leak)
    if ($ctl -lt 5) { Write-Warning "INCONCLUSIVE - the control did not move; the comparison proves nothing" }
    elseif ($leak -gt 1) { Write-Warning "FAIL - keys leaked past the modal dialog" }
    else { Write-Output "PASS - the shortcuts do not reach past the dialog" }
    Get-Process -Name Trace -EA SilentlyContinue | Stop-Process -Force -EA SilentlyContinue
}
