# Does the EXR/colour keyboard surface behave -- `[`, `]` and `C` together?
#
# Stage 2 part 2 binds three keys at once, deliberately, because they share one
# question and testing them one at a time would answer it three times badly:
#
#   [  previous EXR pass          ]  next EXR pass          C  Color Transform
#
# THE ASYMMETRY IS THE REASON THIS EXISTS. `[` and `]` are not letters, so
# QMenuBar's bare-letter mnemonic matching can never claim them -- but Qt's
# QLineEdit ShortcutOverride guard, which covers PRINTABLE keys, DOES apply.
# `C` is the other way round: it is a letter the menu bar could match, and it is
# also the key half of Ctrl+C (Copy Current Frame). Neither may be reasoned
# about; both are measured here. (CLAUDE.md, phase 7 and 2026-08-21.)
#
# All three are QActions rather than ShortcutTable rows, which is what makes the
# menu-bar case correct by construction -- Qt runs an action's shortcut before
# QMenuBar::keyPressEvent sees the key, the reason bare H was never reproducible
# in the 2026-08-21 bare-letter bug while F/S/E/T were. This script's job is to
# demonstrate that rather than assert it.
#
# Legs, and what each one can actually fail on:
#
#   passes     [ and ] cycle and WRAP on a real multilayer EXR, with the HUD's
#              `pass N/M` field as the observable. Fails if a press does not
#              change the pass, or if the ends do not wrap.
#   menubar    after Alt+F then Escape -- Qt's menu mode left, focus retained --
#              [ , ] and c must still run and must open no menu. Popups are
#              counted by WINDOW CLASS, never judged from a screenshot: a Qt
#              popup is its own top-level window and may not overlap the main one.
#   textfield  with Go to Frame open, the keys must not reach the window. Go to
#              Frame is a QInputDialog SPIN BOX, so its validator rejects letters
#              outright -- this leg proves the keys did not act, and cannot prove
#              a printable key lands as text. It carries its own negative control.
#   timecode   the real QLineEdit case, and the one that matters for `C`: Go to
#              Timecode is a text field, it needs media that HAS source timecode
#              (the ProRes 4444 clip; an EXR sequence carries none), and `C` is
#              ENABLED on video -- so a guard that leaked would toggle the LUT
#              while the user typed. Run with a LUT loaded so that leak would be
#              a 96%-of-the-picture event rather than an invisible one.
#   preserve   Ctrl+C still copies a frame and does NOT toggle the transform;
#              Ctrl+L still rotates and does NOT start a forward shuttle. The
#              second is the live masked collision warnOnShortcutCollisions()
#              reports (bare L against Ctrl+L) being shown to be harmless.
#   video      on media with no passes, [ and ] must do nothing at all -- the
#              actions are disabled and a disabled QAction declines its shortcut.
#   ctoggle    C flips the colour stage. Needs a LUT: with none loaded the HUD
#              reads `xform none` in BOTH states, so the key would be untestable
#              and a run without -Lut would report a false pass.
#
# THE HUD IS THE INSTRUMENT AND THE CROPS ARE SAVED FOR READING. Verdicts here
# are computed from whether a cropped HUD band CHANGED, which is a fact a script
# can establish; the values themselves are in the PNGs, because the HUD is
# pixels and OCR would be a second thing to debug.

param(
    [string]$ExrDir = 'C:\Users\andre\Documents\Claude_Cowork\Trace_Testing_Assets\15_Redshift_ACES_EXR\MultlayerAces',
    [string]$Video  = 'C:\Users\andre\Documents\Claude_Cowork\Trace_Testing_Assets\4_4K_H264_MP4\Splash_1.mp4',
    # Go to Timecode is DISABLED on media with no source timecode, so the text-
    # field leg needs a clip that has one. The ProRes 4444 clip starts at
    # 00:00:01:12; an EXR sequence carries no container timecode at all.
    [string]$Timecoded = 'C:\Users\andre\Documents\Claude_Cowork\Trace_Testing_Assets\1_4K_ProRes_4444\TheraTears_Vial_VFX_v002.mov',
    [string]$Lut    = 'C:\Users\andre\Documents\Claude_Cowork\Trace_Testing_Assets\20_Alexa_ProRes_Lut\ARRI_LogC4-to-Gamma24_Rec709-D65_v1-65.cube',
    [string]$Renderer = 'd3d11',
    [string]$Exe,
    [string]$OutDir = "$env:TEMP\trace-passkeys",
    [string[]]$Modes = @('passes','menubar','textfield','timecode','preserve','video','ctoggle')
)

Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Add-Type @"
using System; using System.Runtime.InteropServices; using System.Text;
public class PK {
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
if (-not (Test-Path $Exe)) { Write-Output "no exe at $Exe"; exit 1 }
New-Item -ItemType Directory -Force $OutDir | Out-Null
Get-ChildItem $OutDir -Filter *.png -ErrorAction SilentlyContinue | Remove-Item -Force

$script:h = [IntPtr]::Zero
$script:tracePid = 0
$script:fail = 0
$script:notes = @()

function Kill-Trace {
    Get-Process -Name Trace -ErrorAction SilentlyContinue | ForEach-Object { $_.CloseMainWindow() | Out-Null }
    Start-Sleep -Milliseconds 600
    Get-Process -Name Trace -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 400
}

# A scratch settings file, for the reason cadence.ps1 records: a persisted
# preference is an input to a measurement. The colour transform's enabled state
# and its LUT path are BOTH persisted, so a run that left the stage on would
# hand the next run a different starting point than the one it reports.
function Start-Trace([string]$path, [hashtable]$extraEnv) {
    Kill-Trace
    $ini = Join-Path $OutDir 'passkeys-scratch.ini'
    Remove-Item $ini -ErrorAction SilentlyContinue
    $env:TRACE_RENDERER = $Renderer
    $env:TRACE_HUD = "1"
    $env:TRACE_SETTINGS_FILE = $ini
    if ($extraEnv) { foreach ($k in $extraEnv.Keys) { Set-Item -Path ("env:" + $k) -Value $extraEnv[$k] } }
    $proc = Start-Process -FilePath $Exe -ArgumentList ('"' + $path + '"') -PassThru
    Remove-Item env:TRACE_RENDERER, env:TRACE_HUD, env:TRACE_SETTINGS_FILE -ErrorAction SilentlyContinue
    if ($extraEnv) { foreach ($k in $extraEnv.Keys) { Remove-Item -Path ("env:" + $k) -ErrorAction SilentlyContinue } }
    Start-Sleep -Seconds 5
    if ($proc.HasExited) { Write-Output "  EXITED EARLY code $($proc.ExitCode)"; return $false }
    $p = Get-Process -Name Trace -ErrorAction SilentlyContinue |
         Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
    if (-not $p) { Write-Output "  no window"; return $false }
    $script:h = $p.MainWindowHandle
    $script:tracePid = $p.Id
    return $true
}

# Counting popups by CLASS is what separates "the key opened a menu" from "the
# key did nothing" -- barekeys.ps1's mechanism, reused because it is the only
# one that works: a Qt popup is its own top-level window.
function Popup-Count {
    $script:n = 0
    $cb = [PK+EnumProc]{
        param($hw, $lp)
        $o = 0
        [PK]::GetWindowThreadProcessId($hw, [ref]$o) | Out-Null
        if ($o -eq $script:tracePid -and [PK]::IsWindowVisible($hw)) {
            $sb = New-Object System.Text.StringBuilder 256
            [PK]::GetClassName($hw, $sb, 256) | Out-Null
            if ($sb.ToString() -like '*Popup*') { $script:n++ }
        }
        return $true
    }
    [PK]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
    return $script:n
}

function Focus-Window {
    [PK]::SetForegroundWindow($script:h) | Out-Null
    Start-Sleep -Milliseconds 250
    if ([PK]::GetForegroundWindow() -eq $script:h) { return $true }
    [PK]::keybd_event(0xA4, 0, 0, [UIntPtr]::Zero)
    [PK]::keybd_event(0xA4, 0, 2, [UIntPtr]::Zero)
    [PK]::SetForegroundWindow($script:h) | Out-Null
    Start-Sleep -Milliseconds 250
    return [PK]::GetForegroundWindow() -eq $script:h
}

# Click-activate on the picture (feedback item 13), and the only thing that takes
# Qt's keyboard focus back off the menu bar.
function Click-Picture {
    Focus-Window | Out-Null
    $cr = New-Object PK+RECT; [PK]::GetClientRect($script:h, [ref]$cr) | Out-Null
    $o  = New-Object PK+POINT; [PK]::ClientToScreen($script:h, [ref]$o) | Out-Null
    [PK]::SetCursorPos([int]($o.X + $cr.R / 2), [int]($o.Y + $cr.B / 3)) | Out-Null
    Start-Sleep -Milliseconds 150
    [PK]::mouse_event(2, 0, 0, 0, [UIntPtr]::Zero)
    [PK]::mouse_event(4, 0, 0, 0, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 350
}

# TWO OBSERVABLES, EACH CHOSEN SO IT CANNOT SEE THIS FEATURE'S OWN OUTPUT.
#
# Grab-Hud takes the LAST HUD LINE ONLY -- the media line, which is what carries
# `pass N/M <name> <class> [rawnames]` on an EXR sequence. It deliberately
# excludes the line above it, and that exclusion is the whole reason this
# function is shaped the way it is: the transport line prints refreshHud()'s
# own action label, so it reads `Open file`, then `next pass`, then
# `previous pass`. A band that included it reported the pass cycle as never
# wrapping and `[` as not undoing `]` -- on a build where the media line was
# byte-identical after nine presses of a nine-pass file. THE HARNESS WAS READING
# ITS OWN STIMULUS.
#
# Grab-Pic takes the RIGHT HALF of the picture, for the video legs. The colour
# stage's state is on the video HUD's third line from the top, whose distance
# from the bottom depends on how many lines that HUD has -- fragile. The picture
# is the stronger observable anyway: a LUT going on or off is the whole frame
# changing. The right half, because the transient toast is drawn top-LEFT, and
# Copy Current Frame raises one -- so a full-width picture band would report
# "the picture changed" for a command that only changed the clipboard.
#
# Both are anchored to a window EDGE rather than to a fraction of its height:
# the HUD grows upward from the bottom, so a fraction lands on the picture, and
# that is the trap barekeys.ps1 already records.
$kExrHudRows = 22
function Grab-Hud([string]$tag, [int]$rows) {
    $r = New-Object PK+RECT; [PK]::GetWindowRect($script:h, [ref]$r) | Out-Null
    # 16px in from each side: Windows 11's invisible resize border puts whatever
    # is BEHIND Trace in the first columns of a GetWindowRect capture -- the
    # recorded transitions.ps1 trap, which part 1 hit again in a new harness.
    $w = ($r.R - $r.L) - 32
    $bmp = New-Object System.Drawing.Bitmap $w, $rows
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($r.L + 16, $r.B - $rows - 10, 0, 0, $bmp.Size)
    $g.Dispose()
    if ($tag) { $bmp.Save((Join-Path $OutDir ($tag + ".png")), [System.Drawing.Imaging.ImageFormat]::Png) }
    return $bmp
}

function Grab-Pic([string]$tag) {
    $r = New-Object PK+RECT; [PK]::GetWindowRect($script:h, [ref]$r) | Out-Null
    $w = [int](($r.R - $r.L) * 0.42)
    $h = [int](($r.B - $r.T) * 0.34)
    $bmp = New-Object System.Drawing.Bitmap $w, $h
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($r.R - $w - 16, $r.T + [int](($r.B - $r.T) * 0.14), 0, 0, $bmp.Size)
    $g.Dispose()
    if ($tag) { $bmp.Save((Join-Path $OutDir ($tag + ".png")), [System.Drawing.Imaging.ImageFormat]::Png) }
    return $bmp
}

# Percentage of sampled pixels that differ. Used only to answer "did the HUD
# move", which is a fact; the VALUES are read off the saved crops.
function Hud-Diff($a, $b) {
    if ($a.Width -ne $b.Width -or $a.Height -ne $b.Height) { return 100.0 }
    $diff = 0; $tot = 0
    for ($y = 0; $y -lt $a.Height; $y += 2) {
        for ($x = 0; $x -lt $a.Width; $x += 2) {
            $tot++
            $ca = $a.GetPixel($x, $y); $cb = $b.GetPixel($x, $y)
            if ([Math]::Abs($ca.R - $cb.R) -gt 24 -or [Math]::Abs($ca.G - $cb.G) -gt 24 -or [Math]::Abs($ca.B - $cb.B) -gt 24) { $diff++ }
        }
    }
    if ($tot -eq 0) { return 0.0 }
    return [Math]::Round(100.0 * $diff / $tot, 3)
}

# SAME AND MOVED ARE TWO THRESHOLDS WITH A GAP BETWEEN THEM, NOT ONE CUTOFF.
#
# The measured populations are far apart: a real change reads 10-96% and an
# unchanged capture reads 0.000-0.086%. That 0.086% is text antialiasing across a
# repaint -- six sampled pixels of a 22-row band -- and a single 0.05% cutoff
# called it a difference on a pair of crops whose every field was identical.
#
# Two thresholds with a deliberate gap is the honest shape: a reading BETWEEN
# them means the instrument cannot tell, and Same/Moved both return false for it,
# so the verdict fails and says so. Rounding an ambiguous number toward the
# expected answer is how a harness starts agreeing with whatever it is pointed
# at -- and this file has already produced two false verdicts today from bands
# that were looking at the wrong thing.
$kSameMax  = 0.50
$kMovedMin = 2.00
function Same([double]$pct)  { return $pct -le $kSameMax }
function Moved([double]$pct) { return $pct -ge $kMovedMin }

function Send([string]$keys, [int]$settle = 500) {
    [System.Windows.Forms.SendKeys]::SendWait($keys)
    Start-Sleep -Milliseconds $settle
}

# SendKeys RESERVES [ and ] and they must be brace-escaped. Unescaped they are
# silently swallowed, which the first run of this script proved by reporting
# nine of ten presses as no-ops on a build where all ten worked -- a harness
# fault that looked exactly like the feature being broken.
function SendKey([string]$k, [int]$settle = 700) {
    $esc = switch ($k) { '[' { '{[}' } ']' { '{]}' } default { $k } }
    Send $esc $settle
}

# PARK THE POINTER OUTSIDE THE WINDOW AND LET THE STRIP GO.
#
# The transport strip holds itself up for as long as a stationary pointer is
# anywhere inside the client (measured: still up at 6s), and hides ~2.6s after
# the pointer leaves. Every capture in a leg must agree about whether it is on
# screen, or a diff reports the strip fading rather than the thing under test.
# Parking outside and waiting once per leg makes that a property of the run.
function Park-Cursor {
    $r = New-Object PK+RECT; [PK]::GetWindowRect($script:h, [ref]$r) | Out-Null
    [PK]::SetCursorPos([int]($r.R + 60), [int]($r.T + 40)) | Out-Null
    Start-Sleep -Milliseconds 3000
}

function Verdict([string]$name, [bool]$ok, [string]$detail) {
    if (-not $ok) { $script:fail++ }
    Write-Output ("  {0,-9} {1,-34} {2}" -f $(if ($ok) { "PASS" } else { "FAIL" }), $name, $detail)
}

# ---------------------------------------------------------------- passes ----
if ($Modes -contains 'passes') {
    Write-Output "== passes: [ and ] cycle and wrap on a multilayer EXR"
    $first = Get-ChildItem $ExrDir -Filter *.exr | Select-Object -First 1
    if (-not $first) { Write-Output "  no EXR in $ExrDir"; $script:fail++ }
    elseif (Start-Trace $first.FullName $null) {
        Click-Picture
        Park-Cursor
        $shots = @()
        $shots += ,(Grab-Hud "passes-00-open" $kExrHudRows)
        for ($i = 1; $i -le 10; $i++) {
            SendKey "]" 800
            $shots += ,(Grab-Hud ("passes-{0:d2}-next" -f $i) $kExrHudRows)
        }
        # Every consecutive pair must differ: a press that changed nothing is
        # the failure this leg exists to catch.
        $stuck = 0
        for ($i = 1; $i -lt $shots.Count; $i++) {
            if (-not (Moved (Hud-Diff $shots[$i-1] $shots[$i]))) { $stuck++ }
        }
        Verdict "] advances" ($stuck -eq 0) ("$stuck of " + ($shots.Count - 1) + " presses changed nothing")

        # Wrap: somewhere in ten presses of a 9-pass file the list must return to
        # where it started. Compared against the opening crop.
        # THE INDEX IS REPORTED, NOT JUST THE FACT. A build where nothing moved
        # would trivially "wrap" at index 1, so the index is what has to be read:
        # on a 9-pass file it must be 9.
        $wrapAt = -1
        for ($i = 1; $i -lt $shots.Count; $i++) {
            if (Same (Hud-Diff $shots[0] $shots[$i])) { $wrapAt = $i; break }
        }
        Verdict "] wraps" ($wrapAt -gt 1) ("returned to the opening pass after $wrapAt presses")

        # And back the other way: [ must undo ].
        $a = Grab-Hud "passes-20-before-prev" $kExrHudRows
        SendKey "]" 800
        $b = Grab-Hud "passes-21-after-next" $kExrHudRows
        SendKey "[" 800
        $c = Grab-Hud "passes-22-after-prev" $kExrHudRows
        $fwd = Hud-Diff $a $b
        $rt  = Hud-Diff $a $c
        Verdict "[ undoes ]" ((Moved $fwd) -and (Same $rt)) ("next moved {0}%   next-then-prev {1}% from start" -f $fwd, $rt)
        foreach ($s in $shots) { $s.Dispose() }
        $a.Dispose(); $b.Dispose(); $c.Dispose()
    }
}

# --------------------------------------------------------------- menubar ----
if ($Modes -contains 'menubar') {
    Write-Output "== menubar: the keys with the menu bar holding focus"
    $first = Get-ChildItem $ExrDir -Filter *.exr | Select-Object -First 1
    if (Start-Trace $first.FullName @{ TRACE_COLOR_LUT = $Lut }) {
        foreach ($k in @('[', ']', 'c')) {
            Click-Picture
            # Leave Qt's menu mode the way a user does. The menu bar keeps
            # keyboard focus afterwards, which is the state under test.
            Send "%f" 600
            Send "{ESC}" 600
            Park-Cursor
            $before = Popup-Count
            $hudBefore = Grab-Hud $null $kExrHudRows
            # SendKeys treats [ ] { } ( ) + ^ % ~ as syntax; braces escape them.
            SendKey $k 900
            $after = Popup-Count
            $tag = switch ($k) { '[' { 'prev' } ']' { 'next' } default { 'c' } }
            $hudAfter = Grab-Hud ("menubar-" + $tag) $kExrHudRows
            $moved = Hud-Diff $hudBefore $hudAfter
            $noMenu = ($after -le $before)
            # `c` has no HUD observable on an EXR line, so it is judged on the
            # menu half only; the brackets are judged on both.
            $ran = if ($k -eq 'c') { $true } else { Moved $moved }
            Verdict ("menubar " + $k) ($noMenu -and $ran) ("popups {0}->{1}   hud moved {2}%" -f $before, $after, $moved)
            $hudBefore.Dispose(); $hudAfter.Dispose()
            Send "{ESC}" 300
        }
    }
}

# ------------------------------------------------------------- textfield ----
if ($Modes -contains 'textfield') {
    Write-Output "== textfield: the phase 7 guard -- typing into Go to Frame"
    $first = Get-ChildItem $ExrDir -Filter *.exr | Select-Object -First 1
    if (Start-Trace $first.FullName $null) {
        Click-Picture
        # Move off the opening pass first, so "unchanged" is a real observation
        # rather than the state the file happened to open in.
        SendKey "]" 800
        Park-Cursor
        $before = Grab-Hud "textfield-00-before" $kExrHudRows
        Send "^g" 900                      # Go to Frame
        # Every key of the new surface, plus the letters phase 7 already proved.
        SendKey "c" 300
        SendKey "[" 300
        SendKey "]" 300
        Send "hjkltefsm" 400
        # The dialog itself, saved so the typed text can be read.
        $r = New-Object PK+RECT; [PK]::GetWindowRect($script:h, [ref]$r) | Out-Null
        $dlg = New-Object System.Drawing.Bitmap ($r.R - $r.L), ($r.B - $r.T)
        $gg = [System.Drawing.Graphics]::FromImage($dlg)
        $gg.CopyFromScreen($r.L, $r.T, 0, 0, $dlg.Size)
        $gg.Dispose()
        $dlg.Save((Join-Path $OutDir "textfield-01-dialog.png"), [System.Drawing.Imaging.ImageFormat]::Png)
        $dlg.Dispose()
        Send "{ESC}" 800
        Click-Picture
        Park-Cursor
        $after = Grab-Hud "textfield-02-after" $kExrHudRows
        $moved = Hud-Diff $before $after
        Verdict "keys stay in the field" (Same $moved) ("hud moved {0}% across c [ ] hjkltefsm" -f $moved)
        $before.Dispose(); $after.Dispose()

        # THE NEGATIVE CONTROL. Without it, a build where the dialog never
        # opened would pass the leg above by doing nothing at all.
        $ctl0 = Grab-Hud $null $kExrHudRows
        SendKey "[" 800
        $ctl1 = Grab-Hud "textfield-03-control" $kExrHudRows
        $ctlMoved = Hud-Diff $ctl0 $ctl1
        Verdict "control: [ works outside" (Moved $ctlMoved) ("hud moved {0}% with no dialog open" -f $ctlMoved)
        $ctl0.Dispose(); $ctl1.Dispose()
    }
}

# -------------------------------------------------------------- timecode ----
if ($Modes -contains 'timecode') {
    Write-Output "== timecode: the guard on a real text field, with C live"
    if (Start-Trace $Timecoded @{ TRACE_COLOR_LUT = $Lut }) {
        Click-Picture
        Park-Cursor
        $before = Grab-Pic "timecode-00-before"
        Send "^+g" 1200                    # Go to Timecode -- a QLineEdit
        SendKey "c" 300
        SendKey "[" 300
        SendKey "]" 300
        Send "hjkltefsm" 500
        $r = New-Object PK+RECT; [PK]::GetWindowRect($script:h, [ref]$r) | Out-Null
        $dlg = New-Object System.Drawing.Bitmap ($r.R - $r.L), ($r.B - $r.T)
        $gg = [System.Drawing.Graphics]::FromImage($dlg)
        $gg.CopyFromScreen($r.L, $r.T, 0, 0, $dlg.Size)
        $gg.Dispose()
        $dlg.Save((Join-Path $OutDir "timecode-01-dialog.png"), [System.Drawing.Imaging.ImageFormat]::Png)
        $dlg.Dispose()
        Send "{ESC}" 900
        Click-Picture
        Park-Cursor
        $after = Grab-Pic "timecode-02-after"
        $moved = Hud-Diff $before $after
        # THE PICTURE IS THE OBSERVABLE AND THE LUT IS WHAT GIVES IT TEETH. With
        # the transform loaded, a `c` that reached the window is a 96% change --
        # the ctoggle leg measures exactly that. 0% here is the guard holding.
        Verdict "C blocked while typing" (Same $moved) ("picture moved {0}% across c [ ] hjkltefsm" -f $moved)
        $before.Dispose(); $after.Dispose()

        # NEGATIVE CONTROL: the same key, same build, no dialog open. Without it
        # a run where SendKeys went nowhere would pass the leg above by doing
        # nothing at all -- which is how the first version of the textfield leg
        # passed on a build whose brackets were disabled.
        $c0 = Grab-Pic $null
        SendKey "c" 1100
        $c1 = Grab-Pic "timecode-03-control"
        $cm = Hud-Diff $c0 $c1
        Verdict "control: C works outside" (Moved $cm) ("picture moved {0}% with no dialog open" -f $cm)
        $c0.Dispose(); $c1.Dispose()
    }
}

# -------------------------------------------------------------- preserve ----
if ($Modes -contains 'preserve') {
    Write-Output "== preserve: Ctrl+C and Ctrl+L are untouched"
    if (Start-Trace $Video @{ TRACE_COLOR_LUT = $Lut }) {
        Click-Picture
        Park-Cursor
        # Ctrl+C must copy a frame and must NOT toggle the colour stage. The
        # clipboard answers the first; the HUD's xform field answers the second.
        Set-Clipboard -Value "sentinel"
        $b0 = Grab-Pic "preserve-00-before-ctrlc"
        Send "^c" 1200
        $b1 = Grab-Pic "preserve-01-after-ctrlc"
        $img = $null
        try { $img = Get-Clipboard -Format Image } catch { $img = $null }
        $copied = ($img -ne $null)
        # xform sits on the video HUD line, so a toggle WOULD show. Frame
        # counters do not move while paused, so any change is the stage.
        $xformMoved = Hud-Diff $b0 $b1
        Verdict "Ctrl+C copies" $copied ("clipboard image: " + $copied)
        Verdict "Ctrl+C leaves the picture" (Same $xformMoved) ("picture moved {0}%" -f $xformMoved)
        if ($img) { $img.Dispose() }
        $b0.Dispose(); $b1.Dispose()

        # Ctrl+L rotates. It must NOT start a forward shuttle -- this is the
        # masked collision warnOnShortcutCollisions() reports (bare L is the
        # shuttle and the table's dispatcher ignores modifiers) being shown to
        # be harmless in practice.
        Click-Picture
        Park-Cursor
        $r0 = Grab-Pic "preserve-10-before-ctrll"
        Send "^l" 1200
        $r1 = Grab-Pic "preserve-11-after-ctrll"
        $rotMoved = Hud-Diff $r0 $r1
        Verdict "Ctrl+L still rotates" (Moved $rotMoved) ("picture moved {0}%" -f $rotMoved)
        # THE OTHER HALF OF THE CLAIM, and the one the picture cannot answer:
        # Ctrl+L must not ALSO start a forward shuttle. Bare L is the shuttle and
        # ShortcutTable's dispatcher ignores modifiers, so this is the masked
        # collision warnOnShortcutCollisions() reports being shown to be harmless
        # in practice. `speed` lives on the transport line, so the line is saved
        # and the value is READ rather than diffed.
        $rr = New-Object PK+RECT; [PK]::GetWindowRect($script:h, [ref]$rr) | Out-Null
        $full = New-Object System.Drawing.Bitmap ($rr.R - $rr.L - 32), 26
        $fg = [System.Drawing.Graphics]::FromImage($full)
        $fg.CopyFromScreen($rr.L + 16, $rr.B - 400, 0, 0, $full.Size)
        $fg.Dispose()
        $full.Save((Join-Path $OutDir "preserve-12-speed-after-ctrll.png"), [System.Drawing.Imaging.ImageFormat]::Png)
        $full.Dispose()
        Write-Output "            read preserve-12 for speed: 0.00x means no shuttle started"
        $r0.Dispose(); $r1.Dispose()
    }
}

# ----------------------------------------------------------------- video ----
if ($Modes -contains 'video') {
    Write-Output "== video: [ and ] are inert on media with no passes"
    if (Start-Trace $Video $null) {
        Click-Picture
        Park-Cursor
        $v0 = Grab-Pic "video-00-before"
        SendKey "[" 700
        SendKey "]" 700
        SendKey "[" 700
        $v1 = Grab-Pic "video-01-after"
        $moved = Hud-Diff $v0 $v1
        Verdict "[ ] inert on video" (Same $moved) ("picture moved {0}% over three presses" -f $moved)
        $v0.Dispose(); $v1.Dispose()
    }
}

# --------------------------------------------------------------- ctoggle ----
if ($Modes -contains 'ctoggle') {
    Write-Output "== ctoggle: C flips the colour stage (needs a LUT to be visible)"
    if (-not (Test-Path $Lut)) {
        Write-Output "  no LUT at $Lut -- SKIPPED, and a run without one would report a false pass"
        $script:fail++
    } elseif (Start-Trace $Video @{ TRACE_COLOR_LUT = $Lut }) {
        Click-Picture
        Park-Cursor
        $c0 = Grab-Pic "ctoggle-00-on"
        SendKey "c" 1100
        $c1 = Grab-Pic "ctoggle-01-bypass"
        SendKey "c" 1100
        $c2 = Grab-Pic "ctoggle-02-on-again"
        $off = Hud-Diff $c0 $c1
        $back = Hud-Diff $c0 $c2
        Verdict "C toggles" (Moved $off) ("picture moved {0}% on the first press" -f $off)
        Verdict "C toggles back" (Same $back) ("{0}% from the starting state after two presses" -f $back)
        $c0.Dispose(); $c1.Dispose(); $c2.Dispose()
    }
}

Kill-Trace
Write-Output ""
Write-Output ("PASSKEYS: {0}" -f $(if ($script:fail -eq 0) { "PASS" } else { "FAIL - $($script:fail) checks" }))
Write-Output ("crops in " + $OutDir)
exit $(if ($script:fail -eq 0) { 0 } else { 1 })
