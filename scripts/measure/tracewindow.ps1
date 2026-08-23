# Resolving Trace's window, correctly, in one place.
#
# THE TOOLTIP TRAP, which is what this file exists for.
# `Get-Process -Name Trace | Where MainWindowHandle -ne 0` returns the process's
# first top-level visible window -- and a TOOLTIP is one. Leave the cursor
# parked over Trace's chrome, as any earlier harness leg can (a caption drag, a
# hover leg, a control tap), and that handle resolves to a 55x19 tooltip window.
# Everything downstream then targets the tooltip: SendKeys goes nowhere,
# capture.ps1 builds a 55x19 bitmap or throws on an invalid one, and the run
# reports `frames 0 | ticks 0 | presents 0`.
#
# That reads EXACTLY like a build that cannot play. Measured 2026-08-23: the
# same 4K clip that had recorded 120 frames an hour earlier read frames 0 on
# BOTH binaries and on every clip in the pool, until the cursor was moved.
# It is the third costume of a trap this project keeps paying for -- an
# instrument accusing a correct build.
#
# Three defences, and each of them has to be here rather than at a call site,
# because there are ~37 scripts carrying their own copy of the one-line lookup:
#
#   1. park the cursor clear of the window before resolving
#   2. reject a rect too small to be a main window
#   3. verify foreground was actually GRANTED, by reading it back
#
# SetForegroundWindow fails SILENTLY from a background process -- a child
# PowerShell's own terminal holds focus, keys go there, and the run reports the
# feature missing. Recorded twice already; this is the shared fix.
#
# Dot-source it:  . "$PSScriptRoot\tracewindow.ps1"

if (-not ("TraceWin" -as [type])) {
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class TraceWin {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, IntPtr pid);
  [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool f);
  [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
  [DllImport("user32.dll")] public static extern void keybd_event(byte v, byte s, uint f, UIntPtr e);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern bool GetCursorPos(out RECT p);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
}
"@
}

# Park the pointer somewhere no Trace chrome can be under it, so no tooltip is
# raised while we resolve. Bottom-left of the primary display: Trace's own
# window is centred by section 4's shaping and never reaches there at any
# opening size.
function Move-CursorClear {
    param([int]$X = 40, [int]$Y = 0)
    if ($Y -le 0) {
        Add-Type -AssemblyName System.Windows.Forms -ErrorAction SilentlyContinue
        try { $Y = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds.Height - 60 } catch { $Y = 900 }
    }
    [TraceWin]::SetCursorPos($X, $Y) | Out-Null
    # A tooltip already up does not vanish the instant the cursor moves.
    Start-Sleep -Milliseconds 700
}

# The real main window, or IntPtr::Zero.
#
# MinSize is deliberately generous rather than tuned: section 4 shapes the
# opening window to the media and the smallest it produces on this pool is the
# 460px transport minimum, so 300 rejects every tooltip and control window
# without ever rejecting a legitimate one.
function Resolve-TraceWindow {
    param(
        [string]$ProcName = "Trace",
        [int]$MinSize = 300,
        [switch]$NoCursorPark,
        [switch]$Quiet
    )
    if (-not $NoCursorPark) { Move-CursorClear }

    foreach ($p in (Get-Process -Name $ProcName -ErrorAction SilentlyContinue)) {
        $h = $p.MainWindowHandle
        if ($h -eq 0) { continue }
        if (-not [TraceWin]::IsWindowVisible($h)) { continue }
        $r = New-Object TraceWin+RECT
        if (-not [TraceWin]::GetWindowRect($h, [ref]$r)) { continue }
        $w = $r.R - $r.L; $ht = $r.B - $r.T
        if ($w -ge $MinSize -and $ht -ge $MinSize) { return $h }
        if (-not $Quiet) {
            Write-Host ("  tracewindow: rejected hwnd {0} rect {1}x{2} (tooltip or control window)" -f $h, $w, $ht)
        }
    }
    return [IntPtr]::Zero
}

# Foreground, VERIFIED. Returns $true only if the window really has it.
function Focus-TraceWindow {
    param([Parameter(Mandatory=$true)][IntPtr]$Handle, [switch]$Quiet)

    [TraceWin]::ShowWindow($Handle, 9) | Out-Null   # SW_RESTORE
    [TraceWin]::SetForegroundWindow($Handle) | Out-Null
    Start-Sleep -Milliseconds 250
    if ([TraceWin]::GetForegroundWindow() -eq $Handle) { return $true }

    # The synthetic Alt tap releases Windows' foreground lock.
    [TraceWin]::keybd_event(0xA4, 0, 0, [UIntPtr]::Zero)
    [TraceWin]::keybd_event(0xA4, 0, 2, [UIntPtr]::Zero)
    [TraceWin]::SetForegroundWindow($Handle) | Out-Null
    Start-Sleep -Milliseconds 250
    if ([TraceWin]::GetForegroundWindow() -eq $Handle) { return $true }

    # Attach our input queue to the target's so foreground becomes grantable.
    $me = [TraceWin]::GetCurrentThreadId()
    $other = [TraceWin]::GetWindowThreadProcessId($Handle, [IntPtr]::Zero)
    if ($other -ne 0 -and $other -ne $me) {
        [TraceWin]::AttachThreadInput($me, $other, $true) | Out-Null
        [TraceWin]::SetForegroundWindow($Handle) | Out-Null
        Start-Sleep -Milliseconds 250
        $ok = ([TraceWin]::GetForegroundWindow() -eq $Handle)
        [TraceWin]::AttachThreadInput($me, $other, $false) | Out-Null
        if ($ok) { return $true }
    }
    if (-not $Quiet) { Write-Host "  tracewindow: FOREGROUND DENIED for hwnd $Handle" }
    return $false
}

# Is anything ADVANCING between two samples?
#
# This is the assertion that separates "playback ran" from "the key went
# somewhere else". It reads TWO bands and passes if either moved, because
# neither alone is trustworthy:
#
#   picture band  - the obvious signal, and it is WRONG on real material. The
#                   1x1 and 4x5 ProRes ads in the pool open on a static slate
#                   (they carry a 00:59:53:00 leader timecode), so a correct
#                   build playing them at a measured 100.0% of real time moves
#                   0.0% of picture pixels for the first several seconds. A
#                   picture-only check FAILED that build -- an instrument
#                   accusing correct code, which is the exact class of mistake
#                   this project keeps paying for.
#
#   HUD band      - the bottom of the window. Its per-frame counters (period,
#                   handler, ticks) change on EVERY tick regardless of what the
#                   picture is doing, so it catches the static-slate case. Only
#                   a few digits change, hence its own much lower threshold and
#                   a denser sampling grid.
#
# The HUD band only helps when the HUD is up -- restart.ps1 passes TRACE_HUD=1
# by default, so it is up for every harness run. A caller measuring the shipping
# HUD-hidden configuration on deliberately static material should pass
# -RequireMotion:$false rather than expect this to work.
function Test-PictureAdvancing {
    param(
        [Parameter(Mandatory=$true)][IntPtr]$Handle,
        [int]$GapMs = 350,
        [double]$MinChangedPct = 2.0,
        [double]$MinHudChangedPct = 0.30,
        [switch]$Quiet
    )
    Add-Type -AssemblyName System.Drawing
    $r = New-Object TraceWin+RECT
    if (-not [TraceWin]::GetWindowRect($Handle, [ref]$r)) { return $false }
    $w = $r.R - $r.L; $ht = $r.B - $r.T
    if ($w -lt 40 -or $ht -lt 40) { return $false }

    $grab = {
        param($L, $T, $W, $H)
        $b = New-Object System.Drawing.Bitmap $W, $H
        $g = [System.Drawing.Graphics]::FromImage($b)
        $g.CopyFromScreen($L, $T, 0, 0, $b.Size)
        $g.Dispose()
        return $b
    }
    $changed = {
        param($A, $B, $W, $H, $StepY, $StepX)
        $d = 0; $n = 0
        for ($y = 0; $y -lt $H; $y += $StepY) {
            for ($x = 8; $x -lt ($W - 8); $x += $StepX) {
                $pa = $A.GetPixel($x, $y); $pb = $B.GetPixel($x, $y); $n++
                if ([Math]::Abs($pa.R - $pb.R) + [Math]::Abs($pa.G - $pb.G) + [Math]::Abs($pa.B - $pb.B) -gt 12) { $d++ }
            }
        }
        if ($n -eq 0) { return 0.0 }
        return (100.0 * $d / $n)
    }

    # Picture: below any top chrome, above the transport and the HUD.
    $picT = $r.T + [int]($ht * 0.15); $picH = [int]($ht * 0.30)
    # HUD: the diagnostics block at the bottom of the client.
    $hudT = $r.T + [int]($ht * 0.72); $hudH = [int]($ht * 0.26)

    $p1 = & $grab $r.L $picT $w $picH
    $h1 = & $grab $r.L $hudT $w $hudH
    Start-Sleep -Milliseconds $GapMs
    $p2 = & $grab $r.L $picT $w $picH
    $h2 = & $grab $r.L $hudT $w $hudH

    $picPct = & $changed $p1 $p2 $w $picH 7 11
    $hudPct = & $changed $h1 $h2 $w $hudH 2 3
    $p1.Dispose(); $p2.Dispose(); $h1.Dispose(); $h2.Dispose()

    $ok = ($picPct -ge $MinChangedPct) -or ($hudPct -ge $MinHudChangedPct)
    if (-not $Quiet) {
        Write-Host ("  tracewindow: picture {0:N1}% / hud {1:N2}% changed -> {2}" -f $picPct, $hudPct, $(if ($ok) {'advancing'} else {'STATIC'}))
    }
    return $ok
}
