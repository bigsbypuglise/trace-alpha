# Open Recent (spec phase 11) -- the refusals, measured.
#
# Phase 11 is mostly a set of things that must NOT happen: no probing of recent
# paths at application startup, and no blocking on disconnected LucidLink or
# network paths. A check for something that does not happen needs a negative
# control or it cannot fail, and this script's control is a real one:
#
#   AN UNREACHABLE UNC PATH COSTS 21 SECONDS TO stat().
#
# Measured on this box: [System.IO.File]::Exists('\\10.255.255.1\share\x.mov')
# returns False after 21,053ms, against 91ms for an unmapped drive letter and
# microseconds for a local miss. So a recent list holding two of them would add
# forty seconds to a launch if anything touched them -- which makes "startup
# time did not move" a measurement rather than an assertion. The script prints
# the calibration alongside the result so the two are read together.
#
# Nothing here writes to the real settings file. TRACE_SETTINGS_FILE points the
# application at a scratch INI, which is also what lets a run start from a known
# list instead of from whatever the machine happens to hold.

param(
    [Parameter(Mandatory = $true)][string]$Clip,
    [string]$Exe,
    # startup   time from process start to a visible main window, clean list vs
    #           poisoned list
    # calibrate what a stat on the poisoned paths actually costs
    # menu      drive File > Open Recent and capture it
    # dump      print the scratch INI
    # missing   choose a recent entry whose file is gone; the prompt, and the
    #           removal it offers
    # behaviour MRU order, de-duplication and the bound, driven by opening files
    # clear     Clear Recent Files, from the menu
    # home      which settings file wins: portable, per-user, and the read-only
    #           portable case that must fall back
    [ValidateSet('startup', 'calibrate', 'menu', 'missing', 'behaviour', 'clear', 'home', 'dump')][string]$Mode = 'startup',
    [int]$Repeats = 3,
    [string]$SettingsFile = "$env:TEMP\trace_recent\trace.ini",
    # Seed the scratch INI with these before launching. 'clean' = empty,
    # 'poison' = eight local paths plus two unreachable UNC hosts.
    [ValidateSet('none', 'clean', 'poison')][string]$Seed = 'none',
    # `missing` mode: press Keep instead of Remove. The negative control for the
    # removal -- an offer that removes the entry whichever button is pressed is
    # not an offer, and reading the branch is not the same as running it.
    [switch]$Keep,
    [string]$OutDir = "$env:TEMP\trace_recent"
)

if (-not $Exe) {
    $repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
    $Exe = Join-Path $repo "build\app\Release\Trace.exe"
}
if (-not (Test-Path $Exe)) { Write-Output "no exe at $Exe"; exit 1 }
New-Item -ItemType Directory -Force $OutDir | Out-Null
New-Item -ItemType Directory -Force (Split-Path -Parent $SettingsFile) | Out-Null

Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
if (-not ("RF" -as [type])) {
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class RF {
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint x, uint y, uint d, IntPtr e);
  public const uint DOWN = 0x0002, UP = 0x0004;
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }

  public delegate bool EnumProc(IntPtr h, IntPtr p);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetWindow(IntPtr h, uint cmd);

  // The modal prompt, found by owner rather than by position: it is the visible
  // top-level window of Trace's process that is NOT the main window. Sending it
  // Alt+R was tried first and the keystroke never arrived -- the dialog was
  // still on screen in the capture afterwards -- so the button is clicked.
  public static IntPtr FindDialog(uint pid, IntPtr main) {
    IntPtr found = IntPtr.Zero;
    EnumWindows((h, l) => {
      uint wpid; GetWindowThreadProcessId(h, out wpid);
      if (wpid == pid && h != main && IsWindowVisible(h)) {
        RECT r; GetWindowRect(h, out r);
        if ((r.R - r.L) > 120 && (r.B - r.T) > 60) { found = h; return false; }
      }
      return true;
    }, IntPtr.Zero);
    return found;
  }
}
"@
}

# Two DIFFERENT unreachable hosts, because Windows caches a failed UNC lookup
# for about ten seconds and reusing one host would make the second stat look
# cheap -- which would understate the control rather than the result.
$poisonPaths = @(
    '\\10.255.255.1\review\shot_0100_v004.mov',
    '\\10.255.255.2\review\shot_0100_v005.mov'
)

function SeedSettings([string]$kind) {
    Remove-Item $SettingsFile -Force -ErrorAction SilentlyContinue
    if ($kind -eq 'clean') {
        # An empty but present file, so the two legs differ in the LIST and not
        # in whether the settings file had to be created.
        Set-Content -Path $SettingsFile -Value "[General]" -Encoding utf8
        return 0
    }
    $entries = @()
    for ($i = 1; $i -le 8; $i++) { $entries += "C:\NotThere\clip_{0:d3}.mov" -f $i }
    $entries += $poisonPaths
    $lines = @("[recentFiles]", ("size={0}" -f $entries.Count))
    for ($i = 0; $i -lt $entries.Count; $i++) {
        # QSettings escapes a backslash in an INI value as \\.
        $lines += ("{0}\path={1}" -f ($i + 1), ($entries[$i] -replace '\\', '\\'))
    }
    Set-Content -Path $SettingsFile -Value $lines -Encoding utf8
    return $entries.Count
}

function KillTrace {
    Get-Process -Name Trace -ErrorAction SilentlyContinue | ForEach-Object { $_.CloseMainWindow() | Out-Null }
    Start-Sleep -Milliseconds 700
    Get-Process -Name Trace -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 300
}

# Time from CreateProcess to a main window that exists and has a real rectangle.
# The recent list is loaded and its submenu built in MainWindow's constructor,
# which runs before the window is shown -- so a stat storm in either lands
# squarely inside this interval.
function LaunchAndTime([string]$clip) {
    $env:TRACE_SETTINGS_FILE = $SettingsFile
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $p = Start-Process -FilePath $Exe -ArgumentList "`"$clip`"" -PassThru
    $h = [IntPtr]::Zero
    while ($sw.Elapsed.TotalSeconds -lt 120) {
        $p.Refresh()
        if ($p.HasExited) { break }
        if ($p.MainWindowHandle -ne [IntPtr]::Zero) {
            $r = New-Object RF+RECT
            if ([RF]::GetWindowRect($p.MainWindowHandle, [ref]$r) -and ($r.R - $r.L) -gt 0) {
                $h = $p.MainWindowHandle
                break
            }
        }
        Start-Sleep -Milliseconds 5
    }
    $sw.Stop()
    Remove-Item env:TRACE_SETTINGS_FILE -ErrorAction SilentlyContinue
    return @{ Ms = $sw.Elapsed.TotalMilliseconds; H = $h; P = $p }
}

# Phase 9's lesson: Windows refuses foreground activation to a background
# process, so SetForegroundWindow returns and changes nothing, and every
# keystroke after it goes somewhere else. A real click on the title bar is what
# works. The title bar rather than the video, because a click on the video
# reveals the overlay and a second inside the double-click interval toggles
# fullscreen.
function TakeFocus([IntPtr]$h) {
    [RF]::SetForegroundWindow($h) | Out-Null
    Start-Sleep -Milliseconds 200
    if ([RF]::GetForegroundWindow() -eq $h) { return $true }
    $r = New-Object RF+RECT
    [RF]::GetWindowRect($h, [ref]$r) | Out-Null
    [RF]::SetCursorPos(($r.L + 200), ($r.T + 12)) | Out-Null
    Start-Sleep -Milliseconds 80
    [RF]::mouse_event([RF]::DOWN, 0, 0, 0, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 40
    [RF]::mouse_event([RF]::UP, 0, 0, 0, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 250
    return ([RF]::GetForegroundWindow() -eq $h)
}

function ShotFullScreen([string]$png) {
    $b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
    $bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($b.X, $b.Y, 0, 0, $bmp.Size)
    $g.Dispose()
    $bmp.Save($png, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
}

switch ($Mode) {

'calibrate' {
    Write-Output "-- what a stat on these paths costs, which is the control for `startup` --"
    foreach ($p in ($poisonPaths + @('C:\NotThere\clip_001.mov'))) {
        $sw = [Diagnostics.Stopwatch]::StartNew()
        $r = [System.IO.File]::Exists($p)
        $sw.Stop()
        Write-Output ("  {0,-46} exists={1,-6} {2,9:N0} ms" -f $p, $r, $sw.Elapsed.TotalMilliseconds)
    }
}

'dump' {
    if (-not (Test-Path $SettingsFile)) { Write-Output "no settings file at $SettingsFile"; break }
    Write-Output "-- $SettingsFile --"
    Get-Content $SettingsFile | ForEach-Object { Write-Output "  $_" }
}

'startup' {
    KillTrace
    $results = @{}
    foreach ($kind in @('clean', 'poison')) {
        $n = SeedSettings $kind
        $times = @()
        for ($r = 1; $r -le $Repeats; $r++) {
            KillTrace
            $res = LaunchAndTime $Clip
            if ($res.H -eq [IntPtr]::Zero) { Write-Output "$kind rep $r : FAIL - no window"; continue }
            $times += $res.Ms
            Write-Output ("  {0,-7} rep {1}  window up in {2,7:N0} ms   ({3} entries seeded)" -f $kind, $r, $res.Ms, $n)
            Start-Sleep -Milliseconds 400
        }
        $results[$kind] = $times
    }
    KillTrace
    if ($results['clean'].Count -and $results['poison'].Count) {
        $c = ($results['clean'] | Measure-Object -Minimum).Minimum
        $p = ($results['poison'] | Measure-Object -Minimum).Minimum
        Write-Output ""
        Write-Output ("  best clean  {0,7:N0} ms" -f $c)
        Write-Output ("  best poison {0,7:N0} ms   delta {1,7:N0} ms" -f $p, ($p - $c))
        Write-Output ("  a single stat of one poisoned entry costs ~21,000 ms; two are seeded.")
        if (($p - $c) -gt 5000) {
            Write-Output "  VERDICT: FAIL - startup pays for the recent list"
        } else {
            Write-Output "  VERDICT: PASS - nothing probed the recent paths at startup"
        }
    }
}

'behaviour' {
    # Each launch opens one file from the command line, which is the same
    # openPath() the menu and the file dialog use, so this exercises remember()
    # without having to drive a file picker.
    $assets = Split-Path -Parent (Split-Path -Parent $Clip)
    $a = $Clip
    $b = Join-Path $assets "4_4K_H264_MP4"
    $b = (Get-ChildItem $b -Filter *.mp4 | Select-Object -First 1).FullName
    if (-not $b) { Write-Output "FAIL - no second clip found"; break }

    KillTrace
    SeedSettings 'clean' | Out-Null
    $names = @()
    foreach ($f in @($a, $b, $a)) {
        KillTrace
        $r = LaunchAndTime $f
        if ($r.H -eq [IntPtr]::Zero) { Write-Output "FAIL - no window"; break }
        Start-Sleep -Seconds 2
        KillTrace
        $ini = Get-Content $SettingsFile
        $order = @()
        foreach ($line in $ini) {
            if ($line -match '^\d+\\path=(.+)$') { $order += Split-Path -Leaf ($matches[1] -replace '\\\\', '\') }
        }
        $names = $order
        Write-Output ("  after opening {0,-28} -> [{1}]" -f (Split-Path -Leaf $f), ($order -join ' | '))
    }
    if ($names.Count -eq 2 -and $names[0] -eq (Split-Path -Leaf $a)) {
        Write-Output "  VERDICT: PASS - re-opening a file moved it to the front and did NOT add a second row"
    } else {
        Write-Output "  VERDICT: FAIL - expected 2 entries with the re-opened file first"
    }

    # An entry whose file is PRESENT but will not decode must not be offered for
    # removal. Without this, "the open failed" and "the file is gone" would be
    # one condition and a transient decode failure would delete a good bookmark.
    $bad = Join-Path $OutDir "corrupt.mp4"
    [System.IO.File]::WriteAllBytes($bad, (1..4096 | ForEach-Object { [byte]($_ % 251) }))
    KillTrace
    $r = LaunchAndTime $bad
    Start-Sleep -Seconds 3
    $dlg = [RF]::FindDialog([uint32]$r.P.Id, $r.H)
    $ini = Get-Content $SettingsFile
    $listed = $ini | Where-Object { $_ -match 'corrupt' }
    KillTrace
    Write-Output ("  corrupt but present file: prompt shown = {0}, remembered = {1}" -f ($dlg -ne [IntPtr]::Zero), ($null -ne $listed))
    if ($dlg -eq [IntPtr]::Zero -and $null -eq $listed) {
        Write-Output "  VERDICT: PASS - a failed open is neither remembered nor mistaken for a missing file"
    } else {
        Write-Output "  VERDICT: FAIL"
    }
}

'clear' {
    KillTrace
    SeedSettings 'poison' | Out-Null
    $res = LaunchAndTime $Clip
    if ($res.H -eq [IntPtr]::Zero) { Write-Output "FAIL - no window"; break }
    Start-Sleep -Seconds 3
    if (-not (TakeFocus $res.H)) { Write-Output "FOCUS FAIL"; break }
    $before = (Get-Content $SettingsFile | Where-Object { $_ -match '^size=' })
    [System.Windows.Forms.SendKeys]::SendWait("%f")
    Start-Sleep -Milliseconds 400
    [System.Windows.Forms.SendKeys]::SendWait("r")
    Start-Sleep -Milliseconds 500
    [System.Windows.Forms.SendKeys]::SendWait("c")
    Start-Sleep -Milliseconds 900
    $png = Join-Path $OutDir "after_clear.png"
    ShotFullScreen $png
    KillTrace
    $after = Get-Content $SettingsFile -ErrorAction SilentlyContinue
    $left = ($after | Where-Object { $_ -match '\\path=' }).Count
    Write-Output ("  before: {0}   after: {1} path rows remain" -f $before, $left)
    Write-Output "  $png"
    if ($left -eq 0) { Write-Output "  VERDICT: PASS - the list and the stored rows are both gone" }
    else             { Write-Output "  VERDICT: FAIL - stored rows survived Clear Recent Files" }
}

'home' {
    # Which settings file wins. Read off the application's own stderr rather
    # than off a screenshot of the HUD: the answer is a path, and a path is the
    # thing a capture at 15px is worst at.
    $beside = Join-Path (Split-Path -Parent $Exe) "trace.ini"
    $log = Join-Path $OutDir "home.log"

    function RunHome([string]$label) {
        KillTrace
        $env:TRACE_SETTINGS_LOG = "1"
        Remove-Item env:TRACE_SETTINGS_FILE -ErrorAction SilentlyContinue
        $p = Start-Process -FilePath $Exe -ArgumentList "`"$Clip`"" -PassThru `
             -RedirectStandardError $log -RedirectStandardOutput "$log.out"
        Start-Sleep -Seconds 5
        KillTrace
        Remove-Item env:TRACE_SETTINGS_LOG -ErrorAction SilentlyContinue
        $line = (Get-Content $log -ErrorAction SilentlyContinue | Where-Object { $_ -match 'settings|not writable' }) -join ' ; '
        Write-Output ("  {0,-22} {1}" -f $label, $line)
    }

    Remove-Item $beside -Force -ErrorAction SilentlyContinue
    RunHome "no trace.ini:"

    Set-Content -Path $beside -Value "[General]" -Encoding utf8
    RunHome "writable trace.ini:"

    Set-ItemProperty -Path $beside -Name IsReadOnly -Value $true
    RunHome "read-only trace.ini:"

    Set-ItemProperty -Path $beside -Name IsReadOnly -Value $false
    Remove-Item $beside -Force -ErrorAction SilentlyContinue
    Write-Output "  (trace.ini removed again; the executable directory is back as it was)"
}

'missing' {
    # Entry 2 is C:\NotThere\clip_001.mov -- a local path that is definitely
    # gone, so the answer arrives instantly and the check is about the PROMPT
    # rather than about a timeout.
    KillTrace
    SeedSettings 'poison' | Out-Null
    $res = LaunchAndTime $Clip
    if ($res.H -eq [IntPtr]::Zero) { Write-Output "FAIL - no window"; break }
    Start-Sleep -Seconds 3
    if (-not (TakeFocus $res.H)) { Write-Output "FOCUS FAIL"; break }

    # File > Open Recent > entry 2, by mnemonic throughout.
    [System.Windows.Forms.SendKeys]::SendWait("%f")
    Start-Sleep -Milliseconds 400
    [System.Windows.Forms.SendKeys]::SendWait("r")
    Start-Sleep -Milliseconds 500
    $sw = [Diagnostics.Stopwatch]::StartNew()
    [System.Windows.Forms.SendKeys]::SendWait("2")
    Start-Sleep -Milliseconds 900
    $sw.Stop()
    $png1 = Join-Path $OutDir "missing_prompt.png"
    ShotFullScreen $png1
    Write-Output ("  prompt up {0,6:N0} ms after activating the entry" -f $sw.Elapsed.TotalMilliseconds)
    Write-Output "  $png1"

    # Remove from Recent is the LEFT button; Keep is the default and sits right
    # of it, so a stray Return here would keep the entry -- which is the point of
    # Keep being the default and is worth not defeating in the harness.
    $dlg = [RF]::FindDialog([uint32]$res.P.Id, $res.H)
    if ($dlg -eq [IntPtr]::Zero) { Write-Output "  FAIL - no prompt window found"; KillTrace; break }
    $dr = New-Object RF+RECT
    [RF]::GetWindowRect($dlg, [ref]$dr) | Out-Null
    $bx = $dr.L + [int](($dr.R - $dr.L) * $(if ($Keep) { 0.78 } else { 0.27 }))
    $by = $dr.B - 30
    Write-Output ("  prompt at {0},{1} {2}x{3}; clicking {4} at {5},{6}" -f $dr.L, $dr.T, ($dr.R-$dr.L), ($dr.B-$dr.T), $(if ($Keep) { "Keep" } else { "Remove" }), $bx, $by)
    [RF]::SetCursorPos($bx, $by) | Out-Null
    Start-Sleep -Milliseconds 120
    [RF]::mouse_event([RF]::DOWN, 0, 0, 0, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 60
    [RF]::mouse_event([RF]::UP, 0, 0, 0, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 900
    $png2 = Join-Path $OutDir "missing_removed.png"
    ShotFullScreen $png2
    Write-Output "  $png2"
    KillTrace

    $ini = Get-Content $SettingsFile -ErrorAction SilentlyContinue
    $still = $ini | Where-Object { $_ -match 'clip_001' }
    if ($Keep) {
        if ($still) { Write-Output "  VERDICT: PASS - Keep left the entry in place" }
        else        { Write-Output "  VERDICT: FAIL - Keep removed the entry anyway" }
    } elseif ($still) { Write-Output "  VERDICT: FAIL - the entry is still in the settings file" }
    else              { Write-Output "  VERDICT: PASS - clip_001.mov is gone from the settings file" }
    $count = ($ini | Where-Object { $_ -match '^size=' }) -replace 'size=', ''
    Write-Output ("  list is now {0} entries" -f $count)
}

'menu' {
    KillTrace
    if ($Seed -ne 'none') { SeedSettings $Seed | Out-Null }
    $res = LaunchAndTime $Clip
    if ($res.H -eq [IntPtr]::Zero) { Write-Output "FAIL - no window"; break }
    Start-Sleep -Seconds 3
    if (-not (TakeFocus $res.H)) { Write-Output "FOCUS FAIL"; break }

    # Alt+F opens File, then the mnemonic 'r' (Open &Recent) opens the submenu
    # directly. Arrow navigation was tried first and does not work: {RIGHT} on a
    # highlighted item moves to the NEXT TOP-LEVEL MENU rather than opening a
    # submenu, so the capture showed the Edit menu and looked like an Open Recent
    # that had failed to open. A leg aimed at the wrong menu is the phase 4
    # overlay fault in another costume.
    #
    # Timed, because on an implementation that probed on aboutToShow, this is
    # where the 21 seconds would land instead of at startup.
    $sw = [Diagnostics.Stopwatch]::StartNew()
    [System.Windows.Forms.SendKeys]::SendWait("%f")
    Start-Sleep -Milliseconds 400
    [System.Windows.Forms.SendKeys]::SendWait("r")
    Start-Sleep -Milliseconds 650
    $sw.Stop()
    $png = Join-Path $OutDir "recent_menu.png"
    ShotFullScreen $png
    [System.Windows.Forms.SendKeys]::SendWait("{ESC}{ESC}{ESC}")
    Write-Output ("  submenu open after {0,6:N0} ms of a gesture whose own sleeps are 1,050 ms" -f $sw.Elapsed.TotalMilliseconds)
    Write-Output "  (a stat per entry would put 21,000 ms per unreachable path inside that)"
    Write-Output "  $png"
}

}
