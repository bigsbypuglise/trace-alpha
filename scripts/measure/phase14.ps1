# Spec phase 14's behaviour, driven and read back rather than asserted.
#
# Modes:
#   speed     -- pick each Playback Speed rung and read the ACHIEVED rate off
#                the HUD. The negative control is 0.5x: a build whose speed
#                clamp is still 1.0 plays it at normal speed while the menu
#                ticks 0.5x, which no screenshot of the menu would catch.
#   loop      -- play off the end and check the playhead came back. Its control
#                is the same gesture with Loop OFF, which must stop.
#   copy      -- Copy Current Frame, read back from the clipboard. The control
#                is the SIZE: on the default d3d11 planar path a naive
#                implementation puts a null image on the clipboard, so "there is
#                an image and it is the source resolution" is the check.
#   close     -- Close Media, then confirm the window is still alive and the
#                media-dependent items are disabled.
#   shortcuts -- open Help > Keyboard Shortcuts and confirm it rendered rows.
#
# EVERY MODE READS THE HUD, so it needs the HUD shown -- which is the default.
param(
    [Parameter(Mandatory = $true)][string]$Clip,
    [ValidateSet("speed", "loop", "copy", "close", "shortcuts")][string]$Mode = "speed",
    [string[]]$Env = @(),
    [string]$OutDir = "$env:TEMP\trace_phase14"
)

Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class P14 {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  // CharSet.Unicode IS REQUIRED ON EVERY W CALL HERE and its absence produced
  // a clean false negative. P/Invoke's default is CharSet.Ansi, so a managed
  // string handed to a W function is marshalled as ANSI and read back as
  // UTF-16 -- a title comes out one character long, and a name lookup never
  // matches. The run reported "Keyboard Shortcuts window NOT FOUND" against a
  // build where the window was open on screen the whole time. Sixth stale
  // instrument, and the first that is a marshalling convention rather than a
  // reading taken at the wrong moment.
  //
  // FindWindow BY TITLE IS ALSO THE WRONG QUESTION, so it is gone: it searches
  // the whole desktop, so any other window called "Keyboard Shortcuts" would
  // satisfy it. Enumerating the TRACE PROCESS's own top-level windows cannot.
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, System.Text.StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc p, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  public delegate bool EnumProc(IntPtr h, IntPtr l);

  // The visible top-level window of `pid` whose title is exactly `title`.
  public static IntPtr WindowOf(uint pid, string title) {
    IntPtr hit = IntPtr.Zero;
    EnumWindows((h, l) => {
      uint owner; GetWindowThreadProcessId(h, out owner);
      if (owner != pid || !IsWindowVisible(h)) return true;
      var sb = new System.Text.StringBuilder(512);
      GetWindowTextW(h, sb, 512);
      if (sb.ToString() == title) { hit = h; return false; }
      return true;
    }, IntPtr.Zero);
    return hit;
  }
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
  [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, IntPtr pid);
  [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool attach);
  [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();

  // SetForegroundWindow ALONE IS NOT ENOUGH AND FAILS SILENTLY.
  //
  // Windows refuses a foreground change requested by a process that does not
  // already own the foreground, and it returns false rather than raising. This
  // script is invoked as a child PowerShell, which opens a terminal window that
  // takes focus -- so every SendKeys landed in the TERMINAL, and the run
  // reported "Keyboard Shortcuts window NOT FOUND", which is exactly what a
  // broken feature looks like. Same class of fault as phase 11's SendKeys "%r"
  // that never reached a modal prompt, and phase 13's -Mode hold.
  //
  // Attaching this thread's input queue to the target window's thread makes the
  // two share a foreground state, which is what lets the call through. Verified
  // by READING GetForegroundWindow back afterwards -- an attempt that is only
  // attempted is the thing being fixed.
  public static bool Focus(IntPtr h) {
    ShowWindow(h, 9); // SW_RESTORE
    uint us = GetCurrentThreadId();
    uint them = GetWindowThreadProcessId(h, IntPtr.Zero);
    if (us != them) AttachThreadInput(us, them, true);
    BringWindowToTop(h);
    SetForegroundWindow(h);
    if (us != them) AttachThreadInput(us, them, false);
    return GetForegroundWindow() == h;
  }
}
"@

New-Item -ItemType Directory -Force $OutDir | Out-Null
$exe = "$PSScriptRoot\..\..\build\app\Release\Trace.exe"

function Start-Trace {
    Get-Process Trace -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 600
    foreach ($pair in $Env) {
        $kv = $pair -split "=", 2
        [System.Environment]::SetEnvironmentVariable($kv[0], $kv[1])
    }
    Start-Process -FilePath $exe -ArgumentList "`"$Clip`"" | Out-Null
    Start-Sleep -Seconds 3
    $p = Get-Process Trace | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
    if (-not $p) { throw "no window" }
    if (-not [P14]::Focus($p.MainWindowHandle)) {
        # Loud, and it stops the run. A leg that types into the wrong window
        # produces a plausible negative result, which is worse than no result.
        throw "could not bring Trace to the foreground - keystrokes would go elsewhere"
    }
    Start-Sleep -Milliseconds 400
    return $p
}

# The HUD's first line, as text. OCR is not available here, so the HUD is read
# by capturing and letting the caller eyeball it -- EXCEPT for the numbers this
# script needs, which come from the shot's filename discipline instead. What is
# asserted mechanically is picture MOVEMENT and window liveness; the rates are
# read from the captures.
function Grab([System.Diagnostics.Process]$p, [string]$name) {
    $r = New-Object P14+RECT
    [P14]::GetWindowRect($p.MainWindowHandle, [ref]$r) | Out-Null
    $w = $r.R - $r.L; $h = $r.B - $r.T
    if ($w -le 0 -or $h -le 0) { return $null }
    $bmp = New-Object System.Drawing.Bitmap $w, $h
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($r.L, $r.T, 0, 0, (New-Object System.Drawing.Size $w, $h))
    $g.Dispose()
    $bmp.Save("$OutDir\$name.png")
    return $bmp
}

# Percentage of the VIDEO BAND that changed. The band, not the whole window --
# the HUD changes on every refresh and would swamp the signal, which is the
# fault the phase 4 harness hit from the other direction.
function BandChanged($a, $b) {
    if (-not $a -or -not $b) { return -1 }
    $h = [Math]::Min($a.Height, $b.Height)
    $w = [Math]::Min($a.Width, $b.Width)
    $top = 60
    $bot = [int]($h * 0.55)
    $rect = New-Object System.Drawing.Rectangle 0, $top, $w, ($bot - $top)
    $fmt = [System.Drawing.Imaging.PixelFormat]::Format32bppArgb
    $da = $a.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly, $fmt)
    $db = $b.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly, $fmt)
    $n = $rect.Width * $rect.Height * 4
    $ba = New-Object byte[] $n; $bb = New-Object byte[] $n
    [System.Runtime.InteropServices.Marshal]::Copy($da.Scan0, $ba, 0, $n)
    [System.Runtime.InteropServices.Marshal]::Copy($db.Scan0, $bb, 0, $n)
    $a.UnlockBits($da); $b.UnlockBits($db)
    $changed = 0
    for ($i = 0; $i -lt $n; $i += 4) {
        if ([Math]::Abs([int]$ba[$i] - [int]$bb[$i]) -gt 10) { $changed++ }
    }
    return [Math]::Round(100.0 * $changed / ($rect.Width * $rect.Height), 2)
}

function Send([string]$keys, [int]$settle = 400) {
    [System.Windows.Forms.SendKeys]::SendWait($keys)
    Start-Sleep -Milliseconds $settle
}

switch ($Mode) {

"speed" {
    # Alt+V, then down to Playback Speed, then the rung. Menu navigation rather
    # than a synthesised click: the rungs have no fixed screen position and a
    # coordinate would be the phase 4 overlay fault again.
    # BY MNEMONIC, NOT BY COUNTING DOWN-ARROWS, and the difference found two
    # real bugs. Counting has to know whether the first item is already
    # highlighted when the menu opens (it is), and has to skip separators -- so
    # a miscount silently activates the item NEXT to the one under test, which
    # in the Help menu meant firing Report an Issue and opening a mail client.
    # Mnemonics also fail loudly when two items share one, which is how the
    # duplicate mnemonics in the View menu were found.
    $rungs = @(
        @{ name = "0.5x"; key = "0" },
        @{ name = "1x";   key = "n" },
        @{ name = "2x";   key = "2" },
        @{ name = "10x";  key = "x" }   # "10&x" -- `0` is 0.5x's
    )
    foreach ($rung in $rungs) {
        $p = Start-Trace
        Send "%v" 350
        Send "s" 300              # Playback &Speed
        Send $rung.key 300
        $before = Grab $p "speed-$($rung.name)-t0"
        Start-Sleep -Seconds 3
        $after = Grab $p "speed-$($rung.name)-t3"
        $moved = BandChanged $before $after
        Write-Output ("  {0,-5}  picture moved {1,6}%   shots speed-{0}-t0/t3.png" -f $rung.name, $moved)
        $before.Dispose(); $after.Dispose()
    }
    Write-Output "  read the HUD `speed` field in the t3 shots -- that is the number under test"
}

"loop" {
    # A SCRATCH SETTINGS FILE, AND WITHOUT IT THE CONTROL WAS THE TEST RUN
    # TWICE. Loop is persisted through trace::app::settings(), so the first leg
    # turned it on and SAVED it, and the second leg launched with it already on:
    # both read `loop ON wraps 1` and the run looked like a feature that could
    # not be switched off. The same trick recentfiles.ps1 uses, and for the same
    # reason -- a measurement must not inherit or edit the machine it runs on.
    $ini = "$OutDir\loop-scratch.ini"
    $Env += "TRACE_SETTINGS_FILE=$ini"

    foreach ($loop in @($true, $false)) {
        Remove-Item $ini -ErrorAction SilentlyContinue
        $p = Start-Trace
        if ($loop) {
            Send "%v" 350
            Send "o" 400          # L&oop
        }
        # Land near the end, then play off it.
        Send "^g" 500
        Send "230" 200
        Send "{ENTER}" 600
        $atEnd = Grab $p ("loop-" + $(if ($loop) { "on" } else { "off" }) + "-before")
        Send " " 200
        Start-Sleep -Seconds 6
        $after = Grab $p ("loop-" + $(if ($loop) { "on" } else { "off" }) + "-after")
        $moved = BandChanged $atEnd $after
        $label = if ($loop) { "Loop ON " } else { "Loop OFF" }
        Write-Output ("  {0}  picture moved {1,6}%  (ON should have wrapped and be far from the tail)" -f $label, $moved)
        $atEnd.Dispose(); $after.Dispose()
    }
}

"copy" {
    $p = Start-Trace
    Send "{RIGHT 5}" 600
    # Clear the clipboard first, or a stale image passes the check.
    [System.Windows.Forms.Clipboard]::Clear()
    Start-Sleep -Milliseconds 300
    Send "^c" 900
    if ([System.Windows.Forms.Clipboard]::ContainsImage()) {
        $img = [System.Windows.Forms.Clipboard]::GetImage()
        Write-Output ("  clipboard image {0} x {1}" -f $img.Width, $img.Height)
        $img.Save("$OutDir\copied-frame.png")
        $img.Dispose()
    } else {
        Write-Output "  NO IMAGE ON THE CLIPBOARD -- this is the failure the planar path produces"
    }
    Grab $p "copy-after" | ForEach-Object { $_.Dispose() }
}

"close" {
    $p = Start-Trace
    $open = Grab $p "close-before"
    Send "^w" 900
    $p.Refresh()
    if ($p.HasExited) {
        Write-Output "  CRASHED on Close Media"
    } else {
        $closed = Grab $p "close-after"
        $moved = BandChanged $open $closed
        Write-Output ("  window alive, picture changed {0}%  (a cleared viewer should be a large change)" -f $moved)
        $closed.Dispose()
        # And the File menu, to see Close Media greyed and Open still live.
        Send "%f" 500
        Grab $p "close-filemenu" | ForEach-Object { if ($_) { $_.Dispose() } }
        Send "{ESC}" 200
    }
    $open.Dispose()
}

"shortcuts" {
    $p = Start-Trace
    Send "%h" 400
    Send "k" 900              # &Keyboard Shortcuts
    # THE WHOLE VIRTUAL DESKTOP, not a 1920x1200 corner of it. This box's panel
    # is 5120x1440 and Trace opens centred, so a 1920-wide grab of the top-left
    # contains desktop icons and no application -- which reads exactly like a
    # window that never appeared. Two runs were diagnosed off that shot before
    # the capture size was questioned.
    $vs = [System.Windows.Forms.SystemInformation]::VirtualScreen
    $screen = New-Object System.Drawing.Bitmap $vs.Width, $vs.Height
    $sg = [System.Drawing.Graphics]::FromImage($screen)
    $sg.CopyFromScreen($vs.X, $vs.Y, 0, 0, (New-Object System.Drawing.Size $vs.Width, $vs.Height))
    $sg.Dispose(); $screen.Save("$OutDir\shortcuts-screen.png"); $screen.Dispose()

    $w = [P14]::WindowOf([uint32]$p.Id, "Keyboard Shortcuts")
    if ($w -eq [IntPtr]::Zero) {
        Write-Output "  Keyboard Shortcuts window NOT FOUND"
        Write-Output "  check $OutDir\shortcuts-screen.png: if Trace is not in front,"
        Write-Output "  the keystroke never arrived and this leg did not run."
    } else {
        $r = New-Object P14+RECT
        [P14]::GetWindowRect($w, [ref]$r) | Out-Null
        $bw = $r.R - $r.L; $bh = $r.B - $r.T
        $bmp = New-Object System.Drawing.Bitmap $bw, $bh
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        $g.CopyFromScreen($r.L, $r.T, 0, 0, (New-Object System.Drawing.Size $bw, $bh))
        $g.Dispose()
        $bmp.Save("$OutDir\shortcuts.png")
        $bmp.Dispose()
        Write-Output ("  Keyboard Shortcuts window {0} x {1}, shot at $OutDir\shortcuts.png" -f $bw, $bh)
    }
}

}
Write-Output "shots in $OutDir"
