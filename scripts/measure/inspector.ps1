# The Movie Inspector harness (spec phase 13).
#
# Three of the phase's rules are only checkable by driving the window, and one
# of them is a NEGATIVE: the inspector must not hold the floating transport
# revealed while it has focus. A run that only shows the panel appearing would
# pass on a build with that fault, which is the "harness that cannot fail"
# this project keeps re-learning about.
#
# Modes:
#   show     open media, Ctrl+I, capture the inspector and the main window.
#   viewport resize the main window and check the observed row FOLLOWS -- the
#            row is measured by the paint, so this is the phase 10 trap's test.
#   hold     the transport must still fade while the inspector has focus.
#            Its control is the same wait with focus on the MAIN window, which
#            must also fade: a leg that reports "faded" on both is only evidence
#            if the panel was ever revealed in the first place.
#   media    open a second file with the panel open, and check it re-reads.
param(
    [ValidateSet("show", "viewport", "hold", "media")]
    [string]$Mode = "show",
    [string]$Clip = "C:\Users\andre\Documents\Claude_Cowork\Trace_Testing_Assets\4_4K_H264_MP4\Splash_1.mp4",
    [string]$SecondClip = "C:\Users\andre\Documents\Claude_Cowork\Trace_Testing_Assets\1_4K_ProRes_4444\TheraTears_Vial_VFX_v002.mov",
    [string]$Exe = "build\app\Release\Trace.exe",
    [string]$OutDir = "$env:TEMP\trace_inspector"
)

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public class Insp {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern bool MoveWindow(IntPtr h, int x, int y, int w, int ht, bool repaint);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);

  // Every visible top-level window of one process, with its title. The
  // inspector is a separate top-level window, so Process.MainWindowHandle
  // cannot find it -- and picking the wrong one is exactly how a leg starts
  // reporting a number about the video window instead.
  public static List<string> TopLevel(uint want) {
    var outp = new List<string>();
    EnumWindows(delegate(IntPtr h, IntPtr l) {
      uint pid; GetWindowThreadProcessId(h, out pid);
      if (pid == want && IsWindowVisible(h)) {
        var sb = new StringBuilder(512);
        GetWindowTextW(h, sb, 512);
        if (sb.Length > 0) {
          RECT r; GetWindowRect(h, out r);
          outp.Add(h.ToInt64() + "|" + sb.ToString() + "|" + r.L + "," + r.T + "," + (r.R-r.L) + "," + (r.B-r.T));
        }
      }
      return true;
    }, IntPtr.Zero);
    return outp;
  }
}
"@

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$shell = New-Object -ComObject WScript.Shell

function Windows-Of($proc) {
    return [Insp]::TopLevel([uint32]$proc.Id)
}

function Find-Window($proc, $pattern) {
    foreach ($w in (Windows-Of $proc)) {
        $parts = $w -split "\|"
        if ($parts[1] -match $pattern) { return $parts }
    }
    return $null
}

function Grab($rectCsv, $path) {
    $n = $rectCsv -split ","
    $bmp = New-Object System.Drawing.Bitmap ([int]$n[2]), ([int]$n[3])
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen([int]$n[0], [int]$n[1], 0, 0, $bmp.Size)
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose()
    return "$path ($($n[2])x$($n[3]))"
}

# THE PANEL IS LOCATED BY DIFFERENCE, NOT BY ARITHMETIC OFF THE WINDOW RECT.
#
# Phase 4 spent a phase on the other approach: overlay.ps1 predicted the panel's
# position from a fraction of the window height, aimed 1.2px outside every
# control, and printed a plausible number for twelve captures of one unchanged
# frame. A fraction of the window is the wrong coordinate space here too -- the
# window includes a menu bar and a dev HUD whose height depends on the media, so
# 74% of it lands in the HUD text rather than on the video.
#
# So: capture the same rectangle twice and count what changed. The clip is
# paused, so the picture is static and the only thing that can move in the video
# band is the transport panel fading.
function Capture-Band($rectCsv) {
    $n = $rectCsv -split ","
    $w = [int]$n[2]; $h = [int]$n[3]
    $bmp = New-Object System.Drawing.Bitmap $w, $h
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen([int]$n[0], [int]$n[1], 0, 0, $bmp.Size)
    $g.Dispose()
    return $bmp
}

# Changed pixels between two captures, as a percentage, restricted to the upper
# two thirds -- the video band. The HUD below it prints counters that move on
# their own and would swamp the signal.
function Changed-Pixels($a, $b) {
    if ($a.Width -ne $b.Width -or $a.Height -ne $b.Height) { return -1 }
    $limit = [int]($a.Height * 0.66)
    $changed = 0; $total = 0
    for ($y = 0; $y -lt $limit; $y += 4) {
        for ($x = 0; $x -lt $a.Width; $x += 4) {
            $p = $a.GetPixel($x, $y); $q = $b.GetPixel($x, $y)
            $total++
            if ([math]::Abs($p.R - $q.R) + [math]::Abs($p.G - $q.G) + [math]::Abs($p.B - $q.B) -gt 12) {
                $changed++
            }
        }
    }
    if ($total -eq 0) { return -1 }
    return [math]::Round(100.0 * $changed / $total, 2)
}

# NOT named Move (a built-in alias for Move-Item, which outranks a function and
# turns every pointer move into a failed file operation -- overlay.ps1 lost a
# run to exactly that). SetCursorPos rather than
# [System.Windows.Forms.Cursor]::Position, matching overlay.ps1, which is the
# script that is known to be able to reveal this panel.
function Nudge([int]$x, [int]$y) {
    [Insp]::SetCursorPos($x, $y) | Out-Null
    Start-Sleep -Milliseconds 130
}

function Launch($clip) {
    Get-Process Trace -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 400
    $p = Start-Process -FilePath $Exe -ArgumentList "`"$clip`"" -PassThru
    Start-Sleep -Milliseconds 2500
    return $p
}

function Open-Inspector($proc) {
    $main = Find-Window $proc "Trace"
    if (-not $main) { Write-Output "FAIL: no main window"; return $null }
    [Insp]::SetForegroundWindow([IntPtr][int64]$main[0]) | Out-Null
    Start-Sleep -Milliseconds 400
    $shell.SendKeys("^i")
    Start-Sleep -Milliseconds 900
    $insp = Find-Window $proc "Movie Inspector"
    if (-not $insp) {
        # The phase 11 lesson: a keystroke that never arrived reports as a
        # broken feature. Say which it was.
        Write-Output "FAIL: Ctrl+I produced no inspector window (foreground was $([Insp]::GetForegroundWindow()))"
    }
    return $insp
}

$p = Launch $Clip
Write-Output "clip     $([System.IO.Path]::GetFileName($Clip))"

switch ($Mode) {

  "show" {
    $insp = Open-Inspector $p
    if ($insp) {
      Write-Output "title    $($insp[1])"
      Write-Output "geom     $($insp[2])"
      Write-Output (Grab $insp[2] "$OutDir\inspector.png")
    }
    $main = Find-Window $p "Trace"
    if ($main) { Write-Output (Grab $main[2] "$OutDir\main.png") }
    Write-Output "windows  $((Windows-Of $p).Count)"
  }

  "viewport" {
    $insp = Open-Inspector $p
    if ($insp) {
      Write-Output (Grab $insp[2] "$OutDir\viewport-before.png")
      $main = Find-Window $p "Trace"
      $mn = $main[2] -split ","
      # A real programmatic resize. It does not send WM_SIZING (phase 12), but
      # it does send WM_SIZE and produces a resizeEvent, which is what arms the
      # refresh -- and it changes the drawn size, which is what the row reports.
      [Insp]::MoveWindow([IntPtr][int64]$main[0], [int]$mn[0], [int]$mn[1],
                         [int]([int]$mn[2] * 0.72), [int]([int]$mn[3] * 0.72), $true) | Out-Null
      # Well past the 150ms coalescing window, so the panel has settled.
      Start-Sleep -Milliseconds 1200
      # THE HUD DOES NOT REFRESH ON A RESIZE, deliberately (phase 12: `display`
      # is measured by the paint and building the string on ~123 events per drag
      # would put the instrument inside the path). So `display` and `win` are
      # STALE in the capture unless something makes the window refresh them --
      # a frame step is the cheapest thing that does. Without this the two
      # numbers being compared are from different moments and the comparison
      # means nothing, which is the fault this project keeps re-finding.
      [Insp]::SetForegroundWindow([IntPtr][int64]$main[0]) | Out-Null
      Start-Sleep -Milliseconds 250
      $shell.SendKeys("{RIGHT}")
      Start-Sleep -Milliseconds 600
      $insp2 = Find-Window $p "Movie Inspector"
      Write-Output (Grab $insp2[2] "$OutDir\viewport-after.png")
      $main2 = Find-Window $p "Trace"
      Write-Output (Grab $main2[2] "$OutDir\viewport-hud.png")
      Write-Output "main     $($mn[2])x$($mn[3])  ->  $(($main2[2] -split ',')[2])x$(($main2[2] -split ',')[3])"
    }
  }

  "hold" {
    # THE INSPECTOR MUST NOT HOLD THE TRANSPORT REVEALED WHILE IT HAS FOCUS.
    #
    # OverlayHooks::holdVisible vetoes the auto-hide for a popup, a tooltip, a
    # modal dialog or a focused child control. QApplication::focusWidget() is
    # application-wide, so a modeless window satisfies the child-focus branch for
    # as long as it is focused -- and the panel would then sit over the video
    # forever. This leg is the negative control on that decision, and it is the
    # only one of the four modes that can fail on a plausible build.
    $main = Find-Window $p "Trace"
    # INTS, NOT THE STRINGS -split HANDS BACK. `$mn[0] + 30` on a string is
    # CONCATENATION in PowerShell -- "1920" + 30 is "192030" -- so every pointer
    # coordinate below was a four- or five-digit number off the screen entirely.
    # The run then reported 0% changed with the HUD reading `+overlay`, which
    # reads exactly like a transport that never appears. It accused the app for
    # three runs. Cast once, here.
    $mn = @($main[2] -split "," | ForEach-Object { [int]$_ })
    $centreX = $mn[0] + [int]($mn[2] / 2)
    $centre = New-Object System.Drawing.Point -ArgumentList $centreX, ($mn[1] + [int]($mn[3] * 0.42))

    # THE INSPECTOR OPENS CENTRED ON ITS PARENT, DIRECTLY OVER THE TRANSPORT.
    #
    # The first version of this leg took its "revealed" baseline before Ctrl+I
    # and its "faded" capture after, and read 39% changed -- which was the
    # inspector WINDOW appearing over the video, not the panel fading. It would
    # have reported a pass on a build that held the transport up forever. So the
    # window is moved clear first and every capture below is taken with the
    # screen in the same arrangement.
    # THE BASELINE IS TAKEN BEFORE THE INSPECTOR EXISTS, AND THAT ORDER IS
    # FORCED BY WINDOWS RATHER THAN CHOSEN. Windows refuses SetForegroundWindow
    # to a background process (the fault that made lifecycle.ps1 accuse two
    # healthy binaries), so once the inspector has taken focus this script cannot
    # give it back to the video window -- and the pointer reveal then never
    # reaches the D3D11 child HWND. Measured: every capture read 0% changed with
    # the HUD showing `+overlay`, i.e. a panel that was never up, which is
    # indistinguishable from a panel that never faded if you read one number.
    #
    # ESTABLISH HIDDEN, THEN REVEAL -- overlay.ps1's sequence, and the reason is
    # the same: a diff needs a known state on both sides.
    Nudge ([int]($mn[0] + 30)) ([int]($mn[1] + [int]$mn[3] - 30))
    Start-Sleep -Milliseconds 2800
    $hidden = Capture-Band $main[2]
    $hidden.Save("$OutDir\hold-01-hidden.png", [System.Drawing.Imaging.ImageFormat]::Png)

    Nudge ([int]$centre.X) ([int]($mn[1] + 200))
    Nudge ([int]($centre.X + 4)) ([int]($mn[1] + 204))
    Start-Sleep -Milliseconds 500
    $revealed = Capture-Band $main[2]
    $revealed.Save("$OutDir\hold-02-revealed.png", [System.Drawing.Imaging.ImageFormat]::Png)
    $revealDelta = Changed-Pixels $hidden $revealed
    Write-Output ("guard  hidden -> revealed  changed {0}%   (must be LARGE, or nothing below means anything)" -f $revealDelta)
    if ($revealDelta -lt 1.0) {
        Write-Output "FAIL: the transport never appeared; no fade figure below is evidence"
    }

    $insp = Open-Inspector $p
    if ($insp) {
      # Clear of the video, so a capture of the main window is the main window.
      # The first version of this leg left it centred over the transport and read
      # 39% changed -- which was the inspector WINDOW appearing, not the panel
      # fading, and would have passed a build that held the transport up forever.
      $ip = $insp[2] -split ","
      [Insp]::MoveWindow([IntPtr][int64]$insp[0],
                         [int]([int]$mn[0] + [int]$mn[2] + 40), [int]$mn[1],
                         [int]$ip[2], [int]$ip[3], $true) | Out-Null
      Start-Sleep -Milliseconds 500

      # Leg 1 -- give the inspector focus and DO NOT touch the pointer. This is
      # the case OverlayHooks::holdVisible had to be told about: a modeless
      # window satisfies its child-focus branch application-wide.
      [Insp]::SetForegroundWindow([IntPtr][int64]$insp[0]) | Out-Null
      Start-Sleep -Milliseconds 4500
      $afterInsp = Capture-Band $main[2]
      $afterInsp.Save("$OutDir\hold-03-inspector-focused.png", [System.Drawing.Imaging.ImageFormat]::Png)
      # Read against HIDDEN, not against revealed. "Differs from revealed" only
      # says something changed; "matches hidden" says the panel is GONE.
      Write-Output ("leg1   inspector focused   vs hidden {0}%  vs revealed {1}%   (must be ~0 and LARGE)" -f `
          (Changed-Pixels $hidden $afterInsp), (Changed-Pixels $revealed $afterInsp))

      # Leg 2 -- the control, and it has to be run by hand rather than scripted.
      # Giving focus back to the video window is the thing Windows will not let
      # this process do, so the re-reveal cannot be driven from here; it is
      # reported as NOT RUN rather than printed as a number nothing produced.
      # What leg 1 rests on instead is the guard above: the panel was measurably
      # up, and it went back to the hidden state while the inspector held focus.
      Write-Output "leg2   main focused        NOT RUN - SetForegroundWindow is refused to a background process"
    }
  }

  "media" {
    # ONE PROCESS, TWO FILES. "Update when active media changes" is about a
    # panel that is already open when the media changes underneath it, so a
    # second launch would not test it at all -- a fresh window builds a fresh
    # snapshot whatever the refresh path does.
    $insp = Open-Inspector $p
    if ($insp) {
      Write-Output "title1   $($insp[1])"
      Write-Output (Grab $insp[2] "$OutDir\media-first.png")

      # Move the panel clear so the file dialog does not land on top of it.
      $main = Find-Window $p "Trace"
      $mn = @($main[2] -split "," | ForEach-Object { [int]$_ })
      $ip = @($insp[2] -split "," | ForEach-Object { [int]$_ })
      [Insp]::MoveWindow([IntPtr][int64]$insp[0], ($mn[0] + $mn[2] + 40), $mn[1],
                         $ip[2], $ip[3], $true) | Out-Null
      Start-Sleep -Milliseconds 400

      # Open the second file through the app's own File > Open, with the panel
      # still on screen.
      [Insp]::SetForegroundWindow([IntPtr][int64]$main[0]) | Out-Null
      Start-Sleep -Milliseconds 300
      $shell.SendKeys("^o")
      Start-Sleep -Milliseconds 1500
      $shell.SendKeys($SecondClip.Replace("(", "{(}").Replace(")", "{)}"))
      Start-Sleep -Milliseconds 500
      $shell.SendKeys("{ENTER}")
      # Past the 150ms coalescing window and the open itself.
      Start-Sleep -Milliseconds 2500

      $insp2 = Find-Window $p "Movie Inspector"
      if ($insp2) {
        Write-Output "title2   $($insp2[1])"
        Write-Output (Grab $insp2[2] "$OutDir\media-second.png")
        if ($insp2[1] -eq $insp[1]) {
          Write-Output "FAIL: the panel still names the first file"
        }
      } else {
        Write-Output "FAIL: inspector window gone after the second open"
      }
    }
  }
}

Start-Sleep -Milliseconds 300
Get-Process Trace -ErrorAction SilentlyContinue | Stop-Process -Force
