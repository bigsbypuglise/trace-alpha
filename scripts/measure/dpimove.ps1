# Mixed-monitor DPI validation -- plan section 20.4, the named beta gate.
#
# Everything here is the HARDWARE case. `Trace.exe --window-shape-selftest`
# already drives WindowShape.cpp across 11 shapes x 4 scale factors with `dpr`
# as an argument and no window, renderer or display behind it, and prints its
# own caveat on its last line saying so. What has never executed is a real
# WM_DPICHANGED, the D3D11 swapchain resize that must follow one, a
# monitor-to-monitor move, and fullscreen on a secondary display.
#
# THIS SCRIPT MUST BE PER-MONITOR-DPI-AWARE AND SAYS SO ON ITS FIRST LINE.
# Under system-DPI awareness -- which is what powershell.exe declares, and what
# every other harness in this directory therefore runs as -- Windows VIRTUALIZES
# any monitor whose scale factor differs from the system one. GetWindowRect on a
# window sitting on the 150% display comes back divided by 1.5, and
# Graphics.CopyFromScreen captures a stretched region. Both look like an
# application bug and neither is. The first probe written for this pass was
# system-aware and reported BOTH displays at 100% while one was at 150%.
#
# Modes:
#   -Mode enum        the displays, per-monitor DPI, work areas
#   -Mode move        window across both monitors, both directions, HUD each side
#   -Mode maximized   the same move while maximized
#   -Mode fullscreen  fullscreen on the secondary, Escape, geometry restored
#   -Mode cadence     play on a chosen monitor and read the cadence line. GATE E
#                     schedules against the source's exact rational and is
#                     refresh-independent BY DESIGN, so this should be flat --
#                     but the display's own beat differs (239.999Hz against
#                     60Hz) and 24fps against a 60Hz mode is a cadence nothing
#                     in the record was taken on. Measure it rather than
#                     assuming it.
#   -Mode resize      interactive corner drag ON A GIVEN MONITOR, checking the
#                     aspect lock holds. constrainSizingRect runs in WM_SIZING
#                     and works in PHYSICAL pixels; every recorded run of it was
#                     taken at dpr 1, where physical and logical are the same
#                     number and a missing dpr term is invisible.
#   -Mode open        open media WITH the window already on the secondary
#                     (section 4's opening size is computed against that
#                     monitor's work area, which is why the phase 12 geometry
#                     sign-off was recorded as display-dependent)

param(
    [ValidateSet('enum', 'move', 'maximized', 'fullscreen', 'open', 'cadence', 'resize')]
    [string]$Mode = 'enum',
    [string]$Clip,
    [string]$Exe,
    [string[]]$Env = @(),
    [string]$OutDir,
    [int]$SettleMs = 1400,
    # TRACE_SHAPE_LOG=1 to a file. Section 4's sizing pass prints every term of
    # its calculation AND what the layout actually did with it, and the two
    # disagreeing is the failure mode there -- it is invisible from `win WxH`.
    [switch]$ShapeLog,
    # Which monitor the fullscreen leg runs on. 'primary' is the CONTROL.
    [ValidateSet('secondary','primary')]
    [string]$On = 'secondary',
    [int]$PlaySeconds = 11,
    # The file -Mode open opens SECOND, through File > Open in the same process.
    # A second launch would test nothing about opening on a given monitor.
    [string]$SecondClip,
    # Hide the dev HUD (H, spec phase 2) before measuring -- THE SHIPPING
    # CONFIGURATION. The HUD is ~24 text lines and reads `chrome 0x407` on a 4K
    # clip, against a secondary work area only 672 logical px tall, so with it
    # shown the chrome alone is 61% of the monitor. Phase 12 already ruled the
    # clipped HUD on narrow windows a DIAGNOSTIC limitation rather than a
    # product defect; anything measured with it on inherits that.
    [switch]$HideHud
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;

public class Dpi {
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int left, top, right, bottom; }

    [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)]
    public struct MONITORINFOEXW {
        public int cbSize;
        public RECT rcMonitor;
        public RECT rcWork;
        public uint dwFlags;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst=32)]
        public string szDevice;
    }

    public delegate bool MonitorEnumProc(IntPtr hMon, IntPtr hdc, ref RECT rc, IntPtr data);

    [DllImport("user32.dll")] public static extern bool EnumDisplayMonitors(IntPtr hdc, IntPtr clip, MonitorEnumProc cb, IntPtr data);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern bool GetMonitorInfoW(IntPtr h, ref MONITORINFOEXW mi);
    [DllImport("user32.dll")] public static extern IntPtr MonitorFromWindow(IntPtr h, uint flags);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern bool IsZoomed(IntPtr h);
    [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr h);
    [DllImport("shcore.dll")] public static extern int GetDpiForMonitor(IntPtr h, int t, out uint dx, out uint dy);
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr ctx);
    [DllImport("user32.dll")] public static extern IntPtr GetThreadDpiAwarenessContext();
    [DllImport("user32.dll")] public static extern bool AreDpiAwarenessContextsEqual(IntPtr a, IntPtr b);
    [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint from, uint to, bool attach);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, IntPtr pid);
    [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint f, int dx, int dy, uint d, UIntPtr e);
    public const uint LEFTDOWN = 0x0002, LEFTUP = 0x0004;

    public static bool MakeAware() { return SetProcessDpiAwarenessContext(new IntPtr(-4)); }
    public static bool IsPerMonitorV2() {
        return AreDpiAwarenessContextsEqual(GetThreadDpiAwarenessContext(), new IntPtr(-4));
    }

    // SetForegroundWindow fails SILENTLY from a background process -- phase 14
    // lost a whole run to this and reported a feature missing. Attach to the
    // target's input queue and READ THE RESULT BACK.
    public static bool Focus(IntPtr h) {
        uint me = GetCurrentThreadId();
        uint them = GetWindowThreadProcessId(h, IntPtr.Zero);
        AttachThreadInput(them, me, true);
        SetForegroundWindow(h);
        AttachThreadInput(them, me, false);
        return GetForegroundWindow() == h;
    }

    public static string Monitors() {
        StringBuilder sb = new StringBuilder();
        MonitorEnumProc cb = delegate(IntPtr hMon, IntPtr hdc, ref RECT rc, IntPtr data) {
            MONITORINFOEXW mi = new MONITORINFOEXW();
            mi.cbSize = Marshal.SizeOf(typeof(MONITORINFOEXW));
            GetMonitorInfoW(hMon, ref mi);
            uint dx, dy; GetDpiForMonitor(hMon, 0, out dx, out dy);
            sb.AppendLine(string.Format("{0}|{1}|{2}|{3}|{4}|{5}|{6}|{7}|{8}|{9}|{10}",
                mi.szDevice, (mi.dwFlags & 1) != 0 ? 1 : 0,
                mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right, mi.rcMonitor.bottom,
                mi.rcWork.left, mi.rcWork.top, mi.rcWork.right, mi.rcWork.bottom, dx));
            return true;
        };
        EnumDisplayMonitors(IntPtr.Zero, IntPtr.Zero, cb, IntPtr.Zero);
        return sb.ToString();
    }

    public static string WindowMonitor(IntPtr h) {
        IntPtr m = MonitorFromWindow(h, 2); // MONITOR_DEFAULTTONEAREST
        MONITORINFOEXW mi = new MONITORINFOEXW();
        mi.cbSize = Marshal.SizeOf(typeof(MONITORINFOEXW));
        GetMonitorInfoW(m, ref mi);
        uint dx, dy; GetDpiForMonitor(m, 0, out dx, out dy);
        return mi.szDevice + "|" + dx;
    }
}
'@

# ---------------------------------------------------------------- awareness
$made = [Dpi]::MakeAware()
$aware = [Dpi]::IsPerMonitorV2()
Write-Host ("  harness dpi-awareness : {0}" -f $(if ($aware) { "PER_MONITOR_AWARE_V2 (rects and captures are physical px)" } else { "NOT per-monitor -- EVERY NUMBER BELOW IS VIRTUALIZED, DO NOT QUOTE IT" }))
if (-not $aware) { Write-Host "  refusing to measure with a virtualizing instrument."; exit 2 }

if (-not $Exe) {
    $repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
    $Exe = Join-Path $repo "build\app\Release\Trace.exe"
}
if (-not $OutDir) {
    $OutDir = Join-Path ([System.IO.Path]::GetTempPath()) "dpimove"
}
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

# ---------------------------------------------------------------- monitors
$mons = @()
foreach ($r in ([Dpi]::Monitors() -split "`r?`n" | Where-Object { $_.Trim() -ne '' })) {
    $p = $r -split '\|'
    $mons += [pscustomobject]@{
        Name    = $p[0]
        Primary = ($p[1] -eq '1')
        X = [int]$p[2]; Y = [int]$p[3]
        W = ([int]$p[4] - [int]$p[2]); H = ([int]$p[5] - [int]$p[3])
        WorkX = [int]$p[6]; WorkY = [int]$p[7]
        WorkW = ([int]$p[8] - [int]$p[6]); WorkH = ([int]$p[9] - [int]$p[7])
        Dpi = [int]$p[10]
    }
}

function Show-Monitors {
    foreach ($m in $mons) {
        Write-Host ("  {0} {1,-12} {2,5}x{3,-5} at {4},{5}   work {6}x{7}   dpi {8} = {9}% (dpr {10})" -f `
            $(if ($m.Primary) { '*' } else { ' ' }), $m.Name, $m.W, $m.H, $m.X, $m.Y, $m.WorkW, $m.WorkH, $m.Dpi,
            [math]::Round(100.0 * $m.Dpi / 96.0, 0), [math]::Round($m.Dpi / 96.0, 2))
    }
    $distinct = ($mons | Select-Object -ExpandProperty Dpi -Unique).Count
    if ($mons.Count -lt 2) {
        Write-Host "  ONLY ONE DISPLAY -- this is the configuration section 20.4 was tabled for. Nothing here is testable."
    } elseif ($distinct -lt 2) {
        Write-Host "  NOTE: both displays share a scale factor, so NO WM_DPICHANGED will fire on a move."
        Write-Host "        Monitor-to-monitor moves, fullscreen-on-secondary and the work-area"
        Write-Host "        difference are still real; the DPI-change path is not exercised."
    }
}

Write-Host ""
Show-Monitors
Write-Host ""
if ($Mode -eq 'enum') { exit 0 }

# ---------------------------------------------------------------- helpers
$primary   = $mons | Where-Object { $_.Primary } | Select-Object -First 1
$secondary = $mons | Where-Object { -not $_.Primary } | Select-Object -First 1
if (-not $secondary) { Write-Host "  no secondary display; nothing to do."; exit 1 }

function Get-Trace {
    Get-Process -Name Trace -ErrorAction SilentlyContinue |
        Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
}

function Start-Trace([string]$clip, [string[]]$envPairs) {
    # The dev HUD ships hidden (UI roadmap step 2). Launch with it forced ON so
    # the HUD-reading legs keep working and so -HideHud's `h` press still means
    # "hide" rather than toggling a hidden HUD back on. An explicit TRACE_HUD in
    # $Env wins.
    $hasHud = $false
    foreach ($pair in $envPairs) { if ($pair -like 'TRACE_HUD=*') { $hasHud = $true } }
    if (-not $hasHud) { $envPairs = @($envPairs) + @('TRACE_HUD=1') }
    Get-Process -Name Trace -ErrorAction SilentlyContinue | ForEach-Object { $_.CloseMainWindow() | Out-Null }
    Start-Sleep -Milliseconds 700
    Get-Process -Name Trace -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 300
    foreach ($pair in $envPairs) {
        $i = $pair.IndexOf('='); Set-Item -Path ("env:" + $pair.Substring(0, $i)) -Value $pair.Substring($i + 1)
    }
    if ($ShapeLog) { Set-Item -Path "env:TRACE_SHAPE_LOG" -Value "1" }
    $script:ShapeLogPath = Join-Path $OutDir "shapelog.txt"
    if ($clip) {
        if ($ShapeLog) { Start-Process -FilePath $Exe -ArgumentList ('"' + $clip + '"') -RedirectStandardError $script:ShapeLogPath | Out-Null }
        else           { Start-Process -FilePath $Exe -ArgumentList ('"' + $clip + '"') | Out-Null }
    } else {
        if ($ShapeLog) { Start-Process -FilePath $Exe -RedirectStandardError $script:ShapeLogPath | Out-Null }
        else           { Start-Process -FilePath $Exe | Out-Null }
    }
    if ($ShapeLog) { Remove-Item -Path "env:TRACE_SHAPE_LOG" -ErrorAction SilentlyContinue }
    foreach ($pair in $envPairs) {
        Remove-Item -Path ("env:" + $pair.Substring(0, $pair.IndexOf('='))) -ErrorAction SilentlyContinue
    }
    Start-Sleep -Seconds 4
    return Get-Trace
}

# Where the window is, in PHYSICAL pixels, plus which monitor and that
# monitor's DPI -- read from the OS rather than inferred from the rect, because
# on a mixed-DPI desktop a rect alone cannot say which monitor it is on.
function Get-State([IntPtr]$h) {
    $r = New-Object Dpi+RECT
    [Dpi]::GetWindowRect($h, [ref]$r) | Out-Null
    $wm = ([Dpi]::WindowMonitor($h) -split '\|')
    [pscustomobject]@{
        X = $r.left; Y = $r.top
        W = ($r.right - $r.left); H = ($r.bottom - $r.top)
        Monitor = $wm[0]
        MonDpi  = [int]$wm[1]
        WinDpi  = [int][Dpi]::GetDpiForWindow($h)
        Zoomed  = [Dpi]::IsZoomed($h)
    }
}

function Show-State([string]$label, $s) {
    Write-Host ("  {0,-22} rect {1},{2} {3}x{4} phys  |  {5} dpi {6} (dpr {7})  |  GetDpiForWindow {8}{9}" -f `
        $label, $s.X, $s.Y, $s.W, $s.H, $s.Monitor, $s.MonDpi,
        [math]::Round($s.MonDpi / 96.0, 2), $s.WinDpi,
        $(if ($s.Zoomed) { "  [maximized]" } else { "" }))
}

function Save-Shot([IntPtr]$h, [string]$name) {
    $r = New-Object Dpi+RECT
    [Dpi]::GetWindowRect($h, [ref]$r) | Out-Null
    $w = $r.right - $r.left; $ht = $r.bottom - $r.top
    if ($w -le 0 -or $ht -le 0) { return $null }
    $bmp = New-Object System.Drawing.Bitmap $w, $ht
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($r.left, $r.top, 0, 0, $bmp.Size)
    $path = Join-Path $OutDir ($name + ".png")
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose()
    return $path
}

# Put the window's TOP-LEFT inside a monitor's work area. SWP_NOSIZE, because
# the whole question is what the application does to its own size when the
# scale factor under it changes -- resizing it here would answer it for it.
function Move-ToMonitor([IntPtr]$h, $mon) {
    [Dpi]::SetWindowPos($h, [IntPtr]::Zero, ($mon.WorkX + 40), ($mon.WorkY + 40), 0, 0, 0x0001 -bor 0x0004) | Out-Null
    Start-Sleep -Milliseconds $SettleMs
}

# THE HUD DOES NOT REFRESH ON resizeEvent AND NEVER HAS -- phase 12 found it and
# the owner ruled it a diagnostic limitation rather than a defect, because
# `display` is measured BY the paint and building the HUD string on 123 events
# per drag would put the instrument inside the path. A paused window that has
# just been resized therefore redraws the HUD at the NEW size with the OLD
# string in it: `win`, `display`, and now `dpr`/`scr`/`dpiChg` too.
#
# Stepping one frame forces a decode, a paint and a refreshHud(). Without this
# every reading below is from before the move, which looks exactly like a move
# that did not register.
function Step-Frame([IntPtr]$h) {
    [Dpi]::Focus($h) | Out-Null
    Start-Sleep -Milliseconds 250
    [System.Windows.Forms.SendKeys]::SendWait("{RIGHT}")
    Start-Sleep -Milliseconds 500
}

function Send-Keys([string]$keys) {
    $h = (Get-Trace).MainWindowHandle
    [Dpi]::Focus($h) | Out-Null
    Start-Sleep -Milliseconds 200
    [System.Windows.Forms.SendKeys]::SendWait($keys)
    Start-Sleep -Milliseconds $SettleMs
}

Add-Type -AssemblyName System.Windows.Forms

# ---------------------------------------------------------------- run
if (-not $Clip) { Write-Host "  -Clip is required for mode '$Mode'"; exit 1 }
if (-not (Test-Path $Clip)) { Write-Host "  no clip at $Clip"; exit 1 }

$proc = Start-Trace $Clip $Env
if (-not $proc) { Write-Host "  Trace did not open a window"; exit 1 }
$h = $proc.MainWindowHandle
if ($HideHud) {
    [Dpi]::Focus($h) | Out-Null
    Start-Sleep -Milliseconds 300
    [System.Windows.Forms.SendKeys]::SendWait("h")
    Start-Sleep -Milliseconds 700
    Write-Host "  dev HUD hidden (shipping configuration)"
}
Write-Host ("  clip   {0}" -f (Split-Path -Leaf $Clip))
Write-Host ("  env    {0}" -f $(if ($Env.Count) { $Env -join ' ' } else { '(none)' }))
Write-Host ""

# Which monitor the single-monitor legs (fullscreen, cadence, open) run on.
# 'primary' is the CONTROL for each of them.
$target = if ($On -eq 'primary') { $primary } else { $secondary }

switch ($Mode) {

    'move' {
        Step-Frame $h
        $a = Get-State $h; Show-State "01 opened (primary)" $a
        Save-Shot $h "move-01-primary" | Out-Null

        Move-ToMonitor $h $secondary
        Step-Frame $h
        $b = Get-State $h; Show-State "02 -> secondary" $b
        Save-Shot $h "move-02-secondary" | Out-Null

        Move-ToMonitor $h $primary
        Step-Frame $h
        $c = Get-State $h; Show-State "03 -> primary" $c
        Save-Shot $h "move-03-primary" | Out-Null

        Write-Host ""
        Write-Host "  --- what to read ---"
        Write-Host ("  monitor changed on the way out : {0}" -f ($a.Monitor -ne $b.Monitor))
        Write-Host ("  monitor changed on the way back: {0}" -f ($b.Monitor -ne $c.Monitor))
        Write-Host ("  dpi  {0} -> {1} -> {2}" -f $a.MonDpi, $b.MonDpi, $c.MonDpi)
        Write-Host ("  size {0}x{1} -> {2}x{3} -> {4}x{5}  (physical px)" -f $a.W, $a.H, $b.W, $b.H, $c.W, $c.H)
        # A round trip that does not come back to the same PHYSICAL size on the
        # same monitor is the fault this pass is looking for.
        $roundTrip = ($a.W -eq $c.W -and $a.H -eq $c.H)
        Write-Host ("  round trip returns to the opening physical size: {0}" -f $roundTrip)
        if (-not $roundTrip) {
            Write-Host "  ^^ IF THE ASPECT IS ALSO WRONG, THAT IS THE BUG. Compare the captures."
        }
    }

    'maximized' {
        # ShowWindow, not Alt+Space+x. The system-menu route depends on the menu
        # opening, on focus, and on the accelerator being 'x' in the UI language
        # -- the first cut used it and silently never maximized at all, which the
        # work-area assertion caught only because it was there to catch it.
        # SW_MAXIMIZE = 3, SW_RESTORE = 9.
        [Dpi]::ShowWindow($h, 3) | Out-Null; Start-Sleep -Milliseconds $SettleMs
        Step-Frame $h
        $a = Get-State $h; Show-State "01 maximized primary" $a
        Save-Shot $h "max-01-primary" | Out-Null

        # A maximized window cannot be moved by SetWindowPos, so restore, move,
        # re-maximize -- which is also what a user does.
        [Dpi]::ShowWindow($h, 9) | Out-Null; Start-Sleep -Milliseconds 600
        Move-ToMonitor $h $secondary
        [Dpi]::ShowWindow($h, 3) | Out-Null; Start-Sleep -Milliseconds $SettleMs
        Step-Frame $h
        $b = Get-State $h; Show-State "02 maximized secondary" $b
        Save-Shot $h "max-02-secondary" | Out-Null

        [Dpi]::ShowWindow($h, 9) | Out-Null; Start-Sleep -Milliseconds 600
        Move-ToMonitor $h $primary
        [Dpi]::ShowWindow($h, 3) | Out-Null; Start-Sleep -Milliseconds $SettleMs
        Step-Frame $h
        $c = Get-State $h; Show-State "03 maximized primary" $c
        Save-Shot $h "max-03-primary" | Out-Null

        [Dpi]::ShowWindow($h, 9) | Out-Null; Start-Sleep -Milliseconds 800
        Step-Frame $h
        $d = Get-State $h; Show-State "04 restored to normal" $d

        Write-Host ""
        Write-Host "  --- what to read ---"
        Write-Host ("  actually maximized on each leg    : {0} / {1} / {2}" -f $a.Zoomed, $b.Zoomed, $c.Zoomed)
        Write-Host ("  secondary maximize fills its work area {0}x{1} : {2}  (got {3}x{4})" -f `
            $secondary.WorkW, $secondary.WorkH,
            ($b.W -ge $secondary.WorkW - 24 -and $b.H -ge $secondary.WorkH - 24), $b.W, $b.H)
        Write-Host ("  primary maximize fills its work area {0}x{1}   : {2}  (got {3}x{4})" -f `
            $primary.WorkW, $primary.WorkH,
            ($c.W -ge $primary.WorkW - 24 -and $c.H -ge $primary.WorkH - 24), $c.W, $c.H)
        Write-Host ("  each maximize stayed on its own monitor        : {0} / {1}" -f `
            ($b.Monitor -eq $secondary.Name), ($c.Monitor -eq $primary.Name))
        Write-Host ""
        Write-Host '  Section 4 does not shape a maximized window -- windowGeometryIsOurs'
        Write-Host '  declines while maximized, and changeEvent reshapes on the way back to'
        Write-Host '  normal. Leg 04 is that return: it must be the media ratio again.'
    }

    'fullscreen' {
        # -On primary is the CONTROL. Phase 12 recorded a -7px vertical offset
        # from setGeometry positioning the CLIENT rect, at dpr 1; if the same
        # offset appears on the primary it is not a DPI fault and this pass must
        # not report it as one.
        Move-ToMonitor $h $target
        Step-Frame $h
        $pre = Get-State $h; Show-State ("01 windowed " + $(if ($On -eq 'primary') { 'primary' } else { 'secondary' })) $pre
        Save-Shot $h "fs-01-windowed-secondary" | Out-Null

        Send-Keys "{F11}"
        $fs = Get-State $h; Show-State "02 fullscreen" $fs
        Save-Shot $h "fs-02-fullscreen" | Out-Null

        Send-Keys "{ESC}"
        $post = Get-State $h; Show-State "03 after Escape" $post
        Save-Shot $h "fs-03-restored" | Out-Null

        Write-Host ""
        Write-Host "  --- what to read ---"
        # Phase 6's rule: enter fullscreen on the monitor containing the active
        # window. It has only ever run with one monitor to contain it.
        $onRight = ($fs.Monitor -eq $target.Name)
        Write-Host ("  fullscreen went to the monitor the window was on : {0}  ({1}, wanted {2})" -f `
            $onRight, $fs.Monitor, $target.Name)
        Write-Host ("  fullscreen covers that monitor exactly {0}x{1}     : {2}  (got {3}x{4})" -f `
            $target.W, $target.H, ($fs.W -eq $target.W -and $fs.H -eq $target.H), $fs.W, $fs.H)
        Write-Host ("  Escape restored the pre-fullscreen rect           : {0}" -f `
            ($post.X -eq $pre.X -and $post.Y -eq $pre.Y -and $post.W -eq $pre.W -and $post.H -eq $pre.H))
        Write-Host ("     pre  {0},{1} {2}x{3}" -f $pre.X,$pre.Y,$pre.W,$pre.H)
        Write-Host ("     post {0},{1} {2}x{3}" -f $post.X,$post.Y,$post.W,$post.H)
        Write-Host ("  restored onto the RIGHT monitor                   : {0}  ({1})" -f `
            ($post.Monitor -eq $pre.Monitor), $post.Monitor)
    }

    'cadence' {
        Move-ToMonitor $h $target
        Step-Frame $h
        Show-State ("00 before play, " + $On) (Get-State $h)
        # Space plays. Long enough for the histogram to be worth reading, and
        # captured BEFORE stopping -- the cumulative counters survive a stop but
        # `presented` does not.
        [Dpi]::Focus($h) | Out-Null
        Start-Sleep -Milliseconds 250
        [System.Windows.Forms.SendKeys]::SendWait(" ")
        Start-Sleep -Seconds $PlaySeconds
        Save-Shot $h ("cadence-" + $On) | Out-Null
        Show-State ("01 during play, " + $On) (Get-State $h)
        [System.Windows.Forms.SendKeys]::SendWait(" ")
        Write-Host ""
        Write-Host '  Read the presented %, the cadence histogram, handler>budget and hitch'
        Write-Host '  off the capture. QUOTE hitch, NEVER stalls: stalls is 2 x refresh, so it'
        Write-Host '  is 8.3ms on the 239.999Hz primary and 33.3ms on the 60Hz secondary, and'
        Write-Host '  the SAME run reads differently on each. That is why hitch exists.' 
    }

    'resize' {
        Move-ToMonitor $h $target
        Step-Frame $h
        $a = Get-State $h; Show-State ("01 before drag, " + $On) $a

        # Grab the bottom-right corner and drag it in. Coordinates are PHYSICAL,
        # which is only correct because this process is per-monitor-aware; a
        # system-aware harness would aim at virtualized coordinates on the 150%
        # monitor and miss the resize border entirely, which reads as "the drag
        # did nothing" rather than as a broken instrument.
        $gx = $a.X + $a.W - 4
        $gy = $a.Y + $a.H - 4
        [Dpi]::SetCursorPos($gx, $gy) | Out-Null; Start-Sleep -Milliseconds 300
        [Dpi]::mouse_event([Dpi]::LEFTDOWN, 0, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 200
        for ($i = 1; $i -le 12; $i++) {
            [Dpi]::SetCursorPos(($gx - $i * 14), ($gy - $i * 3)) | Out-Null
            Start-Sleep -Milliseconds 60
        }
        [Dpi]::mouse_event([Dpi]::LEFTUP, 0, 0, 0, [UIntPtr]::Zero)
        Start-Sleep -Milliseconds $SettleMs
        Step-Frame $h
        $b = Get-State $h; Show-State "02 after drag" $b
        Save-Shot $h ("resize-" + $On) | Out-Null

        Write-Host ""
        Write-Host "  --- what to read ---"
        Write-Host ("  window changed size          : {0}  ({1}x{2} -> {3}x{4})" -f `
            (($a.W -ne $b.W) -or ($a.H -ne $b.H)), $a.W, $a.H, $b.W, $b.H)
        Write-Host ("  width followed the drag      : {0}" -f ($b.W -lt $a.W))
        Write-Host ('  READ `wm N/N/N` OFF THE CAPTURE. WM_SIZING must be NON-ZERO: if it is 0')
        Write-Host ('  the drag missed the resize border and nothing below means anything.')
        Write-Host ('  Then read `display WxH` and check it is still the media ratio -- that is')
        Write-Host ('  constrainSizingRect having done its job at this scale factor.')
    }

    'open' {
        # Section 4's opening size is computed against the work area of the
        # monitor the window is on, which is why the phase 12 geometry sign-off
        # was recorded as display-dependent. Move first, THEN open, so the
        # shaping pass runs with the secondary's work area as its input.
        Move-ToMonitor $h $target
        Step-Frame $h
        $before = Get-State $h; Show-State ("01 on " + $On + ", first file") $before
        Save-Shot $h "open-01-before" | Out-Null

        # THE PATH GOES VIA THE CLIPBOARD, NOT SendKeys. A path typed with
        # SendKeys is mangled by every ( ) + ^ % ~ { } in it, and the asset set
        # has parentheses in it -- a dialog that silently opens the wrong file,
        # or no file, looks exactly like a feature that does not work.
        [Dpi]::Focus($h) | Out-Null
        Start-Sleep -Milliseconds 300
        Set-Clipboard -Value $SecondClip
        [System.Windows.Forms.SendKeys]::SendWait("^o")
        Start-Sleep -Milliseconds 2000
        [System.Windows.Forms.SendKeys]::SendWait("^v")
        Start-Sleep -Milliseconds 600
        [System.Windows.Forms.SendKeys]::SendWait("{ENTER}")
        Start-Sleep -Seconds 5
        Step-Frame $h

        $after = Get-State $h; Show-State ("02 opened second file") $after
        Save-Shot $h "open-02-after" | Out-Null

        $dpr = $target.Dpi / 96.0
        Write-Host ""
        Write-Host ("  opened on          : {0}  (wanted {1})" -f $after.Monitor, $target.Name)
        Write-Host ("  work area          : {0}x{1} physical = {2}x{3} logical at dpr {4}" -f `
            $target.WorkW, $target.WorkH,
            [int]($target.WorkW / $dpr), [int]($target.WorkH / $dpr), $dpr)
        Write-Host ("  section 4 80% rule : {0}x{1} logical" -f `
            [int](0.8 * $target.WorkW / $dpr), [int](0.8 * $target.WorkH / $dpr))
        Write-Host ("  window is          : {0}x{1} logical" -f `
            [int]($after.W / $dpr), [int]($after.H / $dpr))
        Write-Host ""
        Write-Host '  The window must not have moved to the other monitor (section 4), and'
        Write-Host '  the picture must touch all four viewport edges. Check the capture.'
    }

}

Write-Host ""
Write-Host ("  captures in {0}" -f $OutDir)
Write-Host '  QUOTE dpr, scr and dpiChg from the HUD beside every figure above.' 
