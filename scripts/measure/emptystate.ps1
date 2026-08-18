# Drives and measures the polished empty state (UI redesign roadmap step 3).
#
# NO OTHER HARNESS CAN REACH THIS STATE. Every script in this directory opens a
# clip -- restart.ps1 takes a mandatory -Clip -- so "what the window looks like
# with no media" has never been measured here at all. That is also why the two
# behaviours worth checking are the two that only exist at a media BOUNDARY:
# the mark has to come back when media is closed, and it must not appear for a
# frame between two opens.
#
# THE MARK IS FOUND BY ITS COLOUR AND THE HINT BY THE ABSENCE OF COLOUR, which
# is what makes the two separable without knowing where either one is. The
# prism mark is strongly chromatic against a black stage; the hint line is
# rgba(255,255,255,0.42), i.e. exactly neutral. Neither is predicted from a
# layout constant -- overlay.ps1 predicted a panel position from a window
# fraction, was 16px stale for a whole phase, and every one of its interaction
# legs silently failed while the captures still looked right.
#
# Modes:
#   launch     no media at all. Reports the mark and hint geometry and checks
#              them against the design's own proportions.
#   transport  no media, then reveal the transport. The panel must still draw:
#              this is the empty-state half of the D3D11 quad-loop regression
#              check, since step 3 adds a case to that loop.
#   close      open -Clip, then Ctrl+W. The mark must be absent with media and
#              present after. BOTH legs are read: a check that only looks at the
#              second one passes on a build that never draws a frame.
#   swap       open -Clip, then -Clip2 through File > Open, sampling the centre
#              of the window across the change. The mark must never appear.
#
# Run `launch` on both renderers and diff the two captures for the cross-backend
# comparison; the empty window is deterministic, unlike a playing one.

param(
    [ValidateSet("launch", "transport", "close", "swap")][string]$Mode = "launch",
    [string]$Clip,
    [string]$Clip2,
    [ValidateSet("d3d11", "cpu")][string]$Renderer = "d3d11",
    [string]$OutDir = "$env:TEMP\trace_emptystate",
    # The docked transport bar, i.e. the escape hatch. The empty state is drawn
    # outside the overlay's own enabled_ gate precisely so it survives this, so
    # this switch is the negative control for that claim rather than a variant.
    [switch]$Bar,
    # The dev HUD is OFF here, unlike every other harness. Nothing in this
    # script reads a HUD figure, and the HUD's own text is chromatic clutter in
    # the middle of the window the mark detector is scanning.
    [switch]$Hud,
    [string]$Exe
)

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Add-Type @"
using System;using System.Runtime.InteropServices;
public class ES {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
  // Declared here rather than reached for from System.Drawing: Add-Type's
  // inline compile has no reference to that assembly even when PowerShell has
  // loaded it, and the error names the struct rather than the reference.
  [StructLayout(LayoutKind.Sequential)] public struct PT { public int X,Y; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref PT p);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x,int y);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, IntPtr pid);
  [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a,uint b,bool f);
  [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
}
"@

if (-not $Exe) {
    $repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
    $Exe = Join-Path $repo "build\app\Release\Trace.exe"
}
if (-not (Test-Path $Exe)) { Write-Output "no exe at $Exe"; exit 1 }
New-Item -ItemType Directory -Force $OutDir | Out-Null
Get-ChildItem $OutDir -Filter *.png -ErrorAction SilentlyContinue | Remove-Item -Force

function Start-Trace([string]$mediaPath) {
    Get-Process -Name Trace -ErrorAction SilentlyContinue |
        ForEach-Object { $_.CloseMainWindow() | Out-Null }
    Start-Sleep -Milliseconds 700
    Get-Process -Name Trace -ErrorAction SilentlyContinue |
        Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 400

    $env:TRACE_RENDERER = $Renderer
    $env:TRACE_HUD = $(if ($Hud) { "1" } else { "0" })
    if ($Bar) { $env:TRACE_TRANSPORT_BAR = "1" }
    $proc = if ($mediaPath) { Start-Process -FilePath $Exe -ArgumentList "`"$mediaPath`"" -PassThru }
            else            { Start-Process -FilePath $Exe -PassThru }
    Remove-Item env:TRACE_RENDERER -ErrorAction SilentlyContinue
    Remove-Item env:TRACE_HUD -ErrorAction SilentlyContinue
    Remove-Item env:TRACE_TRANSPORT_BAR -ErrorAction SilentlyContinue

    Start-Sleep -Seconds 4
    if ($proc.HasExited) { Write-Output "EXITED EARLY code $($proc.ExitCode)"; exit 1 }
    $p = Get-Process -Name Trace -ErrorAction SilentlyContinue |
         Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
    if (-not $p) { Write-Output "EMPTYSTATE: no window"; exit 1 }
    return $p
}

# The CLIENT rect in screen coordinates, never GetWindowRect.
#
# GetWindowRect includes Windows 11's invisible resize border, so its first
# columns are whatever is behind Trace -- which is how transitions.ps1 came to
# report "controls not located" on all 25 cases against a correct build,
# depending only on what was on the desktop behind it.
function Get-ClientBox($h) {
    $c = New-Object ES+RECT
    [ES]::GetClientRect($h, [ref]$c) | Out-Null
    $origin = New-Object ES+PT
    $origin.X = 0; $origin.Y = 0
    [ES]::ClientToScreen($h, [ref]$origin) | Out-Null
    return @{ x = $origin.X; y = $origin.Y; w = $c.R - $c.L; h = $c.B - $c.T }
}

function Grab($box, [string]$path) {
    $b = New-Object System.Drawing.Bitmap $box.w, $box.h
    $g = [System.Drawing.Graphics]::FromImage($b)
    $g.CopyFromScreen($box.x, $box.y, 0, 0, $b.Size)
    $g.Dispose()
    if ($path) { $b.Save($path, [System.Drawing.Imaging.ImageFormat]::Png) }
    return $b
}

# WHERE THE STAGE IS, FOUND RATHER THAN ASSUMED.
#
# The client rect includes the menu bar, and the menu bar is the one thing on
# screen that defeats both detectors at once: its labels are drawn with
# subpixel antialiasing, so "File Edit View Window Help" is a row of strongly
# CHROMATIC pixels, and its background is a lit neutral. The first run of this
# script reported a 670x398 "mark" that was the menu bar and a 1280x744 "hint"
# that was the whole window.
#
# The stage is located by where the MENU BAR STOPS, and the menu bar is found by
# being a flat fill rather than by a height anyone wrote down. Row 2 is sampled
# across the RIGHT of the window, past where the labels reach, so what is
# compared is the bar's background against whatever is under it; the first row
# that differs from it is the top of the stage. That tracks the chrome rather
# than encoding it, which is the difference between this and the layout
# constant overlay.ps1 was 16px stale on for a phase.
#
# THE FIRST VERSION LOOKED FOR A FULLY BLACK ROW AND COULD NOT RUN WITH MEDIA
# OPEN. Section 4 shapes the window to the media, so a frame reaches all four
# edges of the stage and there is no black row anywhere -- it returned -1 and
# the scan below indexed off the bitmap. That is worth keeping as a note rather
# than just a fix: "the stage is the black part" is true of exactly the state
# this script was first pointed at, and false of the one it has to compare
# against.
function Find-Stage([System.Drawing.Bitmap]$b) {
    $rowMean = {
        param($y)
        $sum = 0.0; $n = 0
        for ($x = [int]($b.Width * 0.55); $x -lt ($b.Width - 8); $x += 7) {
            $c = $b.GetPixel($x, $y)
            $sum += ($c.R + $c.G + $c.B) / 3.0; $n++
        }
        return $sum / [Math]::Max(1, $n)
    }
    $bar = & $rowMean 2
    $top = 0
    for ($y = 3; $y -lt [int]($b.Height / 3); $y++) {
        if ([Math]::Abs((& $rowMean $y) - $bar) -gt 6) { $top = $y; break }
    }
    # THE BOTTOM SCAN IS BACK, NARROWER, AND THE REASON THE OLD NOTE GAVE FOR
    # DROPPING IT WAS ONLY EVER TRUE OF THE HINT.
    #
    # The note below said the chrome down there is "entirely NEUTRAL, so it
    # cannot enter the chromatic mark scan". That is false in bar mode, and
    # measurably so: the status bar's "Ready" is subpixel-antialiased, so its
    # pixels are chromatic at a MEDIAN hi-lo of 115 against the prism mark's own
    # 58 -- more chromatic than the thing being looked for, so no threshold can
    # separate them. Unbounded, the mark's bounding box ran from the mark down to
    # the status bar and read 1154x470 against the design's 59x68, on a build
    # whose empty state is visibly correct. That is the SAME trap this script
    # already records for the menu bar at the top, arriving from the other end.
    #
    # It is found rather than encoded, symmetrically with the top: walk up from
    # the last row to the lowest row that is essentially black across the sampled
    # columns, which is the bottom of the stage in bar mode and the bottom of the
    # client in overlay mode. And the old failure cannot come back, because the
    # hint's band is bounded by the MARK's own bounding box rather than by this:
    # a bound that lands above the docked bar cannot sweep a lit border row into
    # anything.
    $bottom = $b.Height - 1
    for ($y = $b.Height - 1; $y -gt $top; $y--) {
        if ((& $rowMean $y) -lt 12) { $bottom = $y; break }
    }
    return @{ top = $top; bottom = $bottom }
}

# Chromatic pixels: the mark. Neutral-but-lit pixels below it: the hint.
#
# TWO PASSES, AND THE HINT'S IS BOUNDED BY THE MARK RATHER THAN BY THE STAGE.
# The design puts the hint on one line 22px under the mark, so that is where it
# is looked for -- which means no other neutral-lit thing in the window (the
# docked bar's groove and glyphs, a window border, a toast) can join its
# bounding box no matter where the stage is judged to end.
#
# Scanned at a stride of 1, not 3: the mark is ~59px wide and the design's
# optical offset is 9.5px, so a coarse stride would move the measured bbox by
# the same order as the thing being checked.
function Measure-Empty([System.Drawing.Bitmap]$b, [int]$topSkip = 0, [int]$bottomStop = -1) {
    if ($bottomStop -lt 0) { $bottomStop = $b.Height - 1 }
    $mx0 = [int]::MaxValue; $my0 = [int]::MaxValue; $mx1 = -1; $my1 = -1; $mn = 0
    for ($y = $topSkip; $y -le $bottomStop; $y++) {
        for ($x = 0; $x -lt $b.Width; $x++) {
            $c = $b.GetPixel($x, $y)
            $hi = [Math]::Max($c.R, [Math]::Max($c.G, $c.B))
            $lo = [Math]::Min($c.R, [Math]::Min($c.G, $c.B))
            if ($hi -lt 22) { continue }
            if (($hi - $lo) -gt 18) {
                $mn++
                if ($x -lt $mx0) { $mx0 = $x }; if ($x -gt $mx1) { $mx1 = $x }
                if ($y -lt $my0) { $my0 = $y }; if ($y -gt $my1) { $my1 = $y }
            }
        }
    }

    $hx0 = [int]::MaxValue; $hy0 = [int]::MaxValue; $hx1 = -1; $hy1 = -1; $hn = 0
    if ($my1 -ge 0) {
        $bandTop = $my1 + 1
        $bandBottom = [Math]::Min($bottomStop, $my1 + 120)
        for ($y = $bandTop; $y -le $bandBottom; $y++) {
            for ($x = 0; $x -lt $b.Width; $x++) {
                $c = $b.GetPixel($x, $y)
                $hi = [Math]::Max($c.R, [Math]::Max($c.G, $c.B))
                $lo = [Math]::Min($c.R, [Math]::Min($c.G, $c.B))
                if (($hi - $lo) -le 6 -and $hi -ge 55 -and $hi -le 170) {
                    $hn++
                    if ($x -lt $hx0) { $hx0 = $x }; if ($x -gt $hx1) { $hx1 = $x }
                    if ($y -lt $hy0) { $hy0 = $y }; if ($y -gt $hy1) { $hy1 = $y }
                }
            }
        }
    }
    return @{
        markN = $mn; markX = $mx0; markY = $my0
        markW = $(if ($mx1 -ge 0) { $mx1 - $mx0 + 1 } else { 0 })
        markH = $(if ($my1 -ge 0) { $my1 - $my0 + 1 } else { 0 })
        hintN = $hn; hintX = $hx0; hintY = $hy0
        hintW = $(if ($hx1 -ge 0) { $hx1 - $hx0 + 1 } else { 0 })
        hintH = $(if ($hy1 -ge 0) { $hy1 - $hy0 + 1 } else { 0 })
    }
}

function Report-Empty($m, $box, $stage, [string]$tag) {
    Write-Output ("{0,-10} client {1}x{2}   stage rows {3}..{4} ({5} tall)" -f
        $tag, $box.w, $box.h, $stage.top, $stage.bottom, ($stage.bottom - $stage.top + 1))
    if ($m.markN -eq 0) {
        Write-Output ("{0,-10} mark   ABSENT" -f "")
    } else {
        $cx = $m.markX + $m.markW / 2.0
        $cy = $m.markY + $m.markH / 2.0
        $stageCy = ($stage.top + $stage.bottom) / 2.0
        Write-Output ("{0,-10} mark   {1}x{2} at {3},{4}  px {5}  offset x {6:+0.0;-0.0;0.0} y {7:+0.0;-0.0;0.0}" -f
            "", $m.markW, $m.markH, $m.markX, $m.markY, $m.markN,
            ($cx - $box.w / 2.0), ($cy - $stageCy))
    }
    if ($m.hintN -eq 0) {
        Write-Output ("{0,-10} hint   ABSENT" -f "")
    } else {
        $hx = $m.hintX + $m.hintW / 2.0
        Write-Output ("{0,-10} hint   {1}x{2} at {3},{4}  px {5}  centre offset {6:+0.0;-0.0;0.0}" -f
            "", $m.hintW, $m.hintH, $m.hintX, $m.hintY, $m.hintN, ($hx - $box.w / 2.0))
    }
    if ($m.markN -gt 0 -and $m.hintN -gt 0) {
        Write-Output ("{0,-10} gap    {1} px between mark ink and hint ink" -f
            "", ($m.hintY - ($m.markY + $m.markH)))
    }
}

$fail = 0
switch ($Mode) {

"launch" {
    $p = Start-Trace $null
    $box = Get-ClientBox $p.MainWindowHandle
    Start-Sleep -Seconds 3          # past the 2s auto-hide, so no panel is up
    $b = Grab $box (Join-Path $OutDir "launch-$Renderer.png")
    $stage = Find-Stage $b
    $m = Measure-Empty $b $stage.top $stage.bottom
    $b.Dispose()
    Report-Empty $m $box $stage "launch"

    # The design's own proportions, from Trace-App-Mockups.html screen-3 and
    # confirmed against the delivered mockup PNG: a 104px canvas whose ink is
    # 59x68, sitting 9.5px right of the canvas centre because a right-pointing
    # triangle is balanced by eye rather than by its bounding box. The hint is
    # centred exactly.
    if ($m.markN -eq 0) { Write-Output "FAIL: no mark"; $fail++ }
    elseif ($m.markH -lt 58 -or $m.markH -gt 80) {
        Write-Output "FAIL: mark ink height $($m.markH) is not the design's 68 +/- dpr"; $fail++
    }
    if ($m.hintN -eq 0) { Write-Output "FAIL: no hint line"; $fail++ }
    elseif ([Math]::Abs(($m.hintX + $m.hintW / 2.0) - $box.w / 2.0) -gt 3) {
        Write-Output "FAIL: hint is not centred"; $fail++
    }
    if ($m.markN -gt 0) {
        $off = ($m.markX + $m.markW / 2.0) - $box.w / 2.0
        if ($off -lt 4 -or $off -gt 18) {
            Write-Output "FAIL: mark optical offset $off is not the design's ~+9.5"; $fail++
        }
    }
}

"transport" {
    # The panel is located by DIFFERENCE, exactly as overlay.ps1 does it, so a
    # layout change breaks this loudly instead of aiming it at the wallpaper.
    $p = Start-Trace $null
    $h = $p.MainWindowHandle
    $box = Get-ClientBox $h
    [ES]::SetForegroundWindow($h) | Out-Null
    Start-Sleep -Seconds 3
    $hidden = Grab $box (Join-Path $OutDir "transport-hidden-$Renderer.png")
    [ES]::SetCursorPos(($box.x + [int]($box.w / 2)), ($box.y + [int]($box.h * 0.55))) | Out-Null
    Start-Sleep -Milliseconds 700
    $shown = Grab $box (Join-Path $OutDir "transport-shown-$Renderer.png")

    $x0 = [int]::MaxValue; $y0 = [int]::MaxValue; $x1 = -1; $y1 = -1
    for ($y = 0; $y -lt $box.h; $y += 2) {
        for ($x = 0; $x -lt $box.w; $x += 2) {
            $ca = $hidden.GetPixel($x, $y); $cb = $shown.GetPixel($x, $y)
            if (([Math]::Abs($ca.R - $cb.R) + [Math]::Abs($ca.G - $cb.G) +
                 [Math]::Abs($ca.B - $cb.B)) -gt 24) {
                if ($x -lt $x0) { $x0 = $x }; if ($x -gt $x1) { $x1 = $x }
                if ($y -lt $y0) { $y0 = $y }; if ($y -gt $y1) { $y1 = $y }
            }
        }
    }
    $hidden.Dispose(); $shown.Dispose()
    if ($x1 -lt 0) {
        Write-Output "transport  NOTHING CHANGED on reveal"
        Write-Output "FAIL: the transport did not draw over the empty state"; $fail++
    } else {
        $w = $x1 - $x0 + 1; $hgt = $y1 - $y0 + 1
        Write-Output ("transport  revealed region {0}x{1} at {2},{3} (client {4}x{5})" -f
            $w, $hgt, $x0, $y0, $box.w, $box.h)
        # The settled 460x84 panel, allowing for the 2px scan stride and the
        # panel's own soft edge. A pass here on a build whose quad loop draws
        # nothing is impossible, which is the point.
        if ($w -lt 380 -or $w -gt 540 -or $hgt -lt 60 -or $hgt -gt 120) {
            Write-Output "FAIL: revealed region is not the 460x84 transport panel"; $fail++
        }
    }
}

"close" {
    if (-not $Clip) { Write-Output "close needs -Clip"; exit 1 }
    $p = Start-Trace $Clip
    $h = $p.MainWindowHandle
    $box = Get-ClientBox $h
    [ES]::SetForegroundWindow($h) | Out-Null
    Start-Sleep -Seconds 3
    $b1 = Grab $box (Join-Path $OutDir "close-01-media.png")
    $stage1 = Find-Stage $b1
    $m1 = Measure-Empty $b1 $stage1.top $stage1.bottom
    $b1.Dispose()
    Report-Empty $m1 $box $stage1 "with media"
    # BOTH LEGS ARE READ. Checking only that the mark returns would pass on a
    # build that draws it over the picture as well, or on one that never shows
    # a frame at all -- so the first leg asserts the mark is NOT there, over a
    # clip whose own picture is chromatic and therefore easy to mistake for it.
    # The discriminator is coverage, not colour: a video frame fills the rect.
    $coverage = 100.0 * $m1.markN / ($box.w * $box.h)
    Write-Output ("with media chromatic coverage {0:0.00}% of client" -f $coverage)
    if ($coverage -lt 2.0) {
        Write-Output "FAIL: no picture on screen, so the second leg proves nothing"; $fail++
    }

    [System.Windows.Forms.SendKeys]::SendWait("^w")
    Start-Sleep -Milliseconds 1500
    $box2 = Get-ClientBox $h
    $b2 = Grab $box2 (Join-Path $OutDir "close-02-closed.png")
    $stage2 = Find-Stage $b2
    $m2 = Measure-Empty $b2 $stage2.top $stage2.bottom
    $b2.Dispose()
    Report-Empty $m2 $box2 $stage2 "closed"
    if ($m2.markN -eq 0 -or $m2.hintN -eq 0) {
        Write-Output "FAIL: the empty state did not come back after Close Media"; $fail++
    } else {
        $cov2 = 100.0 * $m2.markN / ($box2.w * $box2.h)
        Write-Output ("closed     chromatic coverage {0:0.00}% of client" -f $cov2)
        if ($cov2 -gt 2.0) { Write-Output "FAIL: still showing a picture"; $fail++ }
    }
}

"swap" {
    if (-not $Clip -or -not $Clip2) { Write-Output "swap needs -Clip and -Clip2"; exit 1 }
    $p = Start-Trace $Clip
    $h = $p.MainWindowHandle
    $box = Get-ClientBox $h
    [ES]::SetForegroundWindow($h) | Out-Null
    Start-Sleep -Milliseconds 600
    if ([ES]::GetForegroundWindow() -ne $h) {
        # SetForegroundWindow fails SILENTLY from a background process, and a
        # run where every keystroke went to the console reports the feature
        # missing rather than reporting itself broken.
        Write-Output "FAIL: could not focus Trace, so no key reached it"; exit 1
    }

    # A centre strip, tall enough to hold the whole mark-and-hint group and
    # narrow enough that a grab costs about a millisecond. Sampling the whole
    # client would be slower than the transition being sampled.
    $strip = @{ x = $box.x + [int]($box.w / 2) - 130
                y = $box.y + [int]($box.h / 2) - 90
                w = 260; h = 180 }

    $shell = New-Object -ComObject WScript.Shell
    $shell.SendKeys("^o")
    Start-Sleep -Milliseconds 1500
    $shell.SendKeys($Clip2.Replace("(", "{(}").Replace(")", "{)}"))
    Start-Sleep -Milliseconds 400
    $shell.SendKeys("{ENTER}")

    # THE DETECTOR IS THE MARK'S SHAPE, NOT A COVERAGE BAND.
    #
    # A coverage threshold cannot tell a 59x68 mark from a mostly-dark video
    # frame with something colourful in one corner, and both clips here are
    # real footage. What is unmistakable is the BOUNDING BOX of the chromatic
    # pixels: the empty state confines them to a mark-sized object near the
    # centre, while any frame that fills the video rect spreads them across the
    # whole strip. So the flag is "a mark-sized chromatic object appeared",
    # which is the thing being ruled out rather than a proxy for it.
    $hits = 0; $firstAt = -1; $n = 0; $sawPicture = 0
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    while ($sw.ElapsedMilliseconds -lt 4000) {
        $b = Grab $strip $null
        $x0 = [int]::MaxValue; $y0 = [int]::MaxValue; $x1 = -1; $y1 = -1; $c1 = 0
        for ($y = 0; $y -lt $b.Height; $y += 2) {
            for ($x = 0; $x -lt $b.Width; $x += 2) {
                $c = $b.GetPixel($x, $y)
                $hi = [Math]::Max($c.R, [Math]::Max($c.G, $c.B))
                $lo = [Math]::Min($c.R, [Math]::Min($c.G, $c.B))
                if ($hi -ge 22 -and ($hi - $lo) -gt 18) {
                    $c1++
                    if ($x -lt $x0) { $x0 = $x }; if ($x -gt $x1) { $x1 = $x }
                    if ($y -lt $y0) { $y0 = $y }; if ($y -gt $y1) { $y1 = $y }
                }
            }
        }
        $n++
        if ($x1 -ge 0) {
            $bw = $x1 - $x0 + 1; $bh = $y1 - $y0 + 1
            if ($bw -gt 150 -or $bh -gt 130) { $sawPicture++ }
            elseif ($c1 -gt 200 -and $bw -ge 40 -and $bw -le 100 -and $bh -ge 45 -and $bh -le 115) {
                $hits++
                if ($firstAt -lt 0) {
                    $firstAt = $sw.ElapsedMilliseconds
                    $b.Save((Join-Path $OutDir "swap-mark-seen-$firstAt.png"),
                            [System.Drawing.Imaging.ImageFormat]::Png)
                }
            }
        }
        $b.Dispose()
    }
    $sw.Stop()
    Write-Output ("swap       {0} samples over 4.0s, {1} of them showing a picture" -f $n, $sawPicture)

    # The second file has to have actually opened, or the whole run is "nothing
    # happened, and nothing looked like the empty state" -- which would pass on
    # a build where Ctrl+O did not work at all.
    $after = Grab $box (Join-Path $OutDir "swap-after.png")
    $stageA = Find-Stage $after
    $mA = Measure-Empty $after $stageA.top $stageA.bottom
    $after.Dispose()
    $covA = 100.0 * $mA.markN / ($box.w * $box.h)
    Write-Output ("swap       after: chromatic coverage {0:0.00}% of client" -f $covA)
    if ($covA -lt 2.0) { Write-Output "FAIL: the second file is not on screen"; $fail++ }
    if ($sawPicture -eq 0) { Write-Output "FAIL: never saw a picture, so nothing was sampled"; $fail++ }

    if ($hits -gt 0) {
        Write-Output ("FAIL: a mark-sized object appeared {0} times, first at {1}ms" -f $hits, $firstAt)
        $fail++
    } else {
        Write-Output "swap       the empty state never appeared between the two opens"
    }
}
}

Start-Sleep -Milliseconds 300
Get-Process Trace -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Write-Output ""
if ($fail -gt 0) { Write-Output "EMPTYSTATE $Mode ($Renderer): $fail FAIL"; exit 1 }
Write-Output "EMPTYSTATE $Mode ($Renderer): PASS"
