# UI redesign roadmap step 10 route 2: does the top strip's backdrop draw, is it
# FRESH, and does it go away when it should?
#
# THE DISCRIMINATOR IS HORIZONTAL VARIATION WITHIN A STRIP ROW, NOT COLOUR.
# The design package's solid fallback is a purely VERTICAL gradient (#1A1B20 to
# #14161A), so every row of it is one constant colour and its horizontal stddev
# is exactly 0. A blurred copy of the video cannot be, unless the video's own top
# band is flat. That makes "is the backdrop drawing" a question with a zero on one
# side of it rather than a threshold -- the same shape as emptystate.ps1 finding
# the prism mark by chroma and the hint by the absence of it.
#
# IT IS READ OFF THE CLIENT RECT, NEVER GetWindowRect. Windows 11's invisible
# resize border means the first captured columns are whatever is behind Trace --
# the fault that made transitions.ps1 accuse a correct build for all 25 cases.
#
# AND IT IS SAMPLED FROM THE RIGHT END OF THE STRIP. The left carries the mark,
# the wordmark and the menu items and the centre carries the filename; all three
# are text, and text is horizontal variation that has nothing to do with a
# backdrop. The right end past the menus is bare strip. The bd=0 leg reading 0.00
# is what proves the chosen band is clean rather than merely quiet.
param(
    [ValidateSet('live','revive','pausereveal','endreveal','close')][string]$Mode = 'live',
    [Parameter(Mandatory = $true)][string]$Clip,
    [string]$Exe,
    [int]$StepFrames = 45,
    [string]$OutDir = "$env:TEMP\tracebackdrop"
)

$ErrorActionPreference = 'Continue'
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class BdWin {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool f);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, IntPtr p);
  [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
}
"@

if (-not $Exe) { $Exe = Join-Path $PSScriptRoot "..\..\build\app\Release\Trace.exe" }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

function Get-TraceWindow {
    $p = Get-Process -Name Trace -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
    if (-not $p) { return $null }
    return $p.MainWindowHandle
}

function Focus-Trace($h) {
    # SetForegroundWindow fails silently from a background process -- the child
    # PowerShell's own terminal takes focus and every SendKeys goes there.
    $fg = [BdWin]::GetForegroundWindow()
    $tid = [BdWin]::GetWindowThreadProcessId($fg, [IntPtr]::Zero)
    $me  = [BdWin]::GetCurrentThreadId()
    [BdWin]::AttachThreadInput($me, $tid, $true) | Out-Null
    [BdWin]::SetForegroundWindow($h) | Out-Null
    [BdWin]::AttachThreadInput($me, $tid, $false) | Out-Null
    Start-Sleep -Milliseconds 250
    return ([BdWin]::GetForegroundWindow() -eq $h)
}

# Client-rect capture. Returns a Bitmap of the whole client area.
function Capture-Client($h, $path) {
    $r = New-Object BdWin+RECT
    [BdWin]::GetClientRect($h, [ref]$r) | Out-Null
    $pt = New-Object BdWin+POINT
    $pt.X = 0; $pt.Y = 0
    [BdWin]::ClientToScreen($h, [ref]$pt) | Out-Null
    $w = $r.R - $r.L; $ht = $r.B - $r.T
    $bmp = New-Object System.Drawing.Bitmap $w, $ht
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($pt.X, $pt.Y, 0, 0, $bmp.Size)
    $g.Dispose()
    if ($path) { $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png) }
    return $bmp
}

# Per-row horizontal stddev over the right end of the strip, plus the band mean.
# Rows 6..30 of a 38px strip: clear of the top edge and of the 1px hairline at
# the bottom.
function Measure-Strip($bmp) {
    $x0 = [int]($bmp.Width * 0.78)
    $x1 = [int]($bmp.Width * 0.98)
    $y0 = 6; $y1 = 30
    if ($bmp.Height -le $y1) { return $null }
    $rect = New-Object System.Drawing.Rectangle $x0, $y0, ($x1 - $x0), ($y1 - $y0)
    $data = $bmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $w = $rect.Width; $h = $rect.Height
    $buf = New-Object byte[] ($data.Stride * $h)
    [System.Runtime.InteropServices.Marshal]::Copy($data.Scan0, $buf, 0, $buf.Length)
    $bmp.UnlockBits($data)
    $rowSds = New-Object System.Collections.ArrayList
    $sumR = 0.0; $sumG = 0.0; $sumB = 0.0; $n = 0
    for ($y = 0; $y -lt $h; $y++) {
        # Only the first width*4 bytes of each row are pixels; the rest is
        # padding, and walking it is what once invented 399 differing pixels.
        $base = $y * $data.Stride
        $vals = New-Object double[] $w
        for ($x = 0; $x -lt $w; $x++) {
            $i = $base + $x * 4
            $b = $buf[$i]; $gg = $buf[$i+1]; $rr = $buf[$i+2]
            $vals[$x] = ($rr + $gg + $b) / 3.0
            $sumR += $rr; $sumG += $gg; $sumB += $b; $n++
        }
        $m = ($vals | Measure-Object -Average).Average
        $v = 0.0
        foreach ($t in $vals) { $v += ($t - $m) * ($t - $m) }
        [void]$rowSds.Add([Math]::Sqrt($v / $w))
    }
    return [pscustomobject]@{
        HSd  = [Math]::Round(($rowSds | Measure-Object -Average).Average, 3)
        HSdMax = [Math]::Round(($rowSds | Measure-Object -Maximum).Maximum, 3)
        R = [Math]::Round($sumR / $n, 2)
        G = [Math]::Round($sumG / $n, 2)
        B = [Math]::Round($sumB / $n, 2)
        Band = "x $x0..$x1  y $y0..$y1"
    }
}

function Jiggle($h) {
    # Reveal the chrome. The overlay reveals on pointer movement inside the
    # window, so two real moves a few px apart, not one SetCursorPos.
    $r = New-Object BdWin+RECT
    [BdWin]::GetClientRect($h, [ref]$r) | Out-Null
    $pt = New-Object BdWin+POINT; $pt.X = 0; $pt.Y = 0
    [BdWin]::ClientToScreen($h, [ref]$pt) | Out-Null
    $cx = $pt.X + [int](($r.R - $r.L) / 2)
    $cy = $pt.Y + [int](($r.B - $r.T) / 2)
    [BdWin]::SetCursorPos($cx, $cy) | Out-Null
    Start-Sleep -Milliseconds 40
    [BdWin]::SetCursorPos(($cx + 7), ($cy + 5)) | Out-Null
    Start-Sleep -Milliseconds 40
    [BdWin]::SetCursorPos($cx, $cy) | Out-Null
    Start-Sleep -Milliseconds 120
}

function Start-Trace($backdrop) {
    Get-Process -Name Trace -ErrorAction SilentlyContinue | ForEach-Object { $_.CloseMainWindow() | Out-Null }
    Start-Sleep -Milliseconds 700
    Get-Process -Name Trace -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 300
    $env:TRACE_STRIP_BACKDROP = $backdrop
    $env:TRACE_HUD = "0"
    $env:TRACE_NO_AUDIO = "1"
    $env:TRACE_SETTINGS_FILE = "$OutDir\scratch.ini"
    Start-Process -FilePath $Exe -ArgumentList ('"' + $Clip + '"') | Out-Null
    Remove-Item env:TRACE_STRIP_BACKDROP, env:TRACE_HUD, env:TRACE_NO_AUDIO, env:TRACE_SETTINGS_FILE -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 4
    $h = Get-TraceWindow
    if (-not $h) { throw "no Trace window" }
    if (-not (Focus-Trace $h)) { throw "could not foreground Trace" }
    return $h
}

Write-Output "mode=$Mode  clip=$(Split-Path -Leaf $Clip)"
Write-Output ""

if ($Mode -eq 'live') {
    foreach ($bd in @('0','1')) {
        $h = Start-Trace $bd
        Start-Sleep -Milliseconds 400
        Jiggle $h
        $bmp = Capture-Client $h "$OutDir\live-bd$bd.png"
        $m = Measure-Strip $bmp
        $bmp.Dispose()
        Write-Output ("TRACE_STRIP_BACKDROP={0}  hsd {1} (max {2})  strip mean RGB {3}/{4}/{5}   [{6}]" -f $bd, $m.HSd, $m.HSdMax, $m.R, $m.G, $m.B, $m.Band)
    }
    Write-Output ""
    Write-Output "PASS requires: bd=0 hsd 0.00 (the fallback is a purely vertical gradient)"
    Write-Output "               bd=1 hsd well above 0 (a blur of the video cannot be flat)"
}

if ($Mode -eq 'revive') {
    # THE LEG THE GATE MADE NECESSARY, AND ITS FIRST DESIGN TESTED NOTHING.
    #
    # The first version stepped frames while the strip was hidden and asked
    # whether the backdrop that came back was STALE. It passed on every build,
    # including one with the fix deliberately removed, because `keyPressEvent`
    # calls `revealOverlay()` -- so the arrow keys that changed the frame were
    # also holding the chrome up, and it was never hidden at all. The gesture
    # under test was the gesture doing the revealing.
    #
    # Following that through: EVERY way to change the displayed frame is an
    # input, and every input reveals the chrome. So a stale backdrop is not a
    # state this application can reach. What IS reachable is a MISSING one --
    # the gate publishes null when the strip hides, and if nothing resamples on
    # the way back up, a strip revealed with no frame arriving since draws the
    # solid fallback where the design's blur should be. On a PAUSED file no
    # frame ever arrives, so it stays wrong until playback resumes.
    #
    # That is what this measures, and the discriminator is the same zero: the
    # fallback is horizontally flat and a blur is not.
    $h = Start-Trace '1'
    # Step onto a frame with something in its top band, so a fallback and a
    # backdrop are far apart rather than both being dark.
    for ($i = 0; $i -lt $StepFrames; $i++) { [System.Windows.Forms.SendKeys]::SendWait("{RIGHT}"); Start-Sleep -Milliseconds 25 }
    Start-Sleep -Milliseconds 400
    $a = Capture-Client $h (Join-Path $OutDir "revive-A-before-hide.png")
    $ma = Measure-Strip $a; $a.Dispose()
    Write-Output ("A  paused at +$StepFrames, chrome up from the keys   hsd {0}  mean {1}/{2}/{3}" -f $ma.HSd, $ma.R, $ma.G, $ma.B)

    # THE POINTER MUST BE PARKED OUTSIDE THE WINDOW OR NOTHING EVER HIDES, and
    # the first version of this leg left it at the client centre and measured a
    # chrome that was up the whole time. Probed at three positions on a paused
    # 4K clip: outside the window the strip hides at ~2.6s (the sampled row goes
    # mean 61.18 / hsd 31.5 to 86.41 / 66.3 as raw video appears); resting at the
    # client CENTRE or in the bottom CORNER it is still up at 6s, unchanged to
    # three decimals. The auto-hide's own timeout handler says why -- it holds
    # while `hover_` is a region, i.e. while the pointer is on the picture -- and
    # overlay.ps1 has always parked the pointer off the video for this reason.
    #
    # Auto-hide is 2000ms + a 165ms fade. Nothing touches the file after this,
    # so no frame arrives between the hide and the reveal below.
    #
    # THE MID-HIDE CAPTURE IS NOT DECORATION. Without it, "B looks like A" is
    # equally consistent with the backdrop being correctly resampled and with the
    # chrome never having hidden at all -- and the first version of this leg was
    # defeated by exactly that ambiguity. With the strip gone this band is raw
    # video, which is far brighter and far more varied than a scrimmed strip, so
    # H reading nothing like A is the proof the hide happened.
    [BdWin]::SetCursorPos(5, 5) | Out-Null
    Start-Sleep -Milliseconds 3400
    $hid = Capture-Client $h (Join-Path $OutDir "revive-H-hidden.png")
    $mh = Measure-Strip $hid; $hid.Dispose()
    Write-Output ("H  after 3.4s idle -- must be RAW VIDEO, not strip  hsd {0}  mean {1}/{2}/{3}" -f $mh.HSd, $mh.R, $mh.G, $mh.B)
    Jiggle $h
    $b = Capture-Client $h (Join-Path $OutDir "revive-B-after-reveal.png")
    $mb = Measure-Strip $b; $b.Dispose()
    Write-Output ("B  hidden 3.4s, revealed by pointer, NO new frame  hsd {0}  mean {1}/{2}/{3}" -f $mb.HSd, $mb.R, $mb.G, $mb.B)
    Write-Output ""
    Write-Output "PASS requires B hsd well above 0 and close to A: the strip came back drawing"
    Write-Output "the blur of the frame it is actually over."
    Write-Output "FAIL is B hsd 0.00 -- the backdrop went MISSING across a hide/reveal and the"
    Write-Output "strip is drawing the solid fallback on a build that is supposed to blur."
}

if ($Mode -eq 'pausereveal') {
    # THE ONE ORDERING THE GATE CAN ACTUALLY BREAK, arrived at by elimination
    # rather than by guessing, after two earlier designs passed on every build.
    #
    # The gate publishes null when it runs and finds the strip hidden -- but it
    # only RUNS when a frame arrives. So on a paused file nothing clears the
    # retained image and a hide/reveal is accidentally correct. During PLAYBACK
    # frames keep arriving, so a hidden strip does get its backdrop cleared to
    # null; and if the file is then PAUSED, no further frame arrives to put one
    # back. Pausing is a key press, which reveals the chrome -- so the strip
    # comes back up over a picture with the solid fallback behind it and stays
    # that way, because a paused file never delivers another frame.
    #
    # Play, hide, pause, look.
    $h = Start-Trace '1'
    [System.Windows.Forms.SendKeys]::SendWait(" ")           # play
    Start-Sleep -Milliseconds 600
    [BdWin]::SetCursorPos(5, 5) | Out-Null                   # off the picture, or it never hides
    Start-Sleep -Milliseconds 3400                           # chrome hides WHILE frames arrive
    $hid = Capture-Client $h (Join-Path $OutDir "pause-H-hidden.png")
    $mh = Measure-Strip $hid; $hid.Dispose()
    Write-Output ("H  playing, chrome hidden -- must be RAW VIDEO   hsd {0}  mean {1}/{2}/{3}" -f $mh.HSd, $mh.R, $mh.G, $mh.B)
    [System.Windows.Forms.SendKeys]::SendWait(" ")           # pause: reveals the chrome, delivers no frame
    Start-Sleep -Milliseconds 700
    $b = Capture-Client $h (Join-Path $OutDir "pause-B-revealed.png")
    $mb = Measure-Strip $b; $b.Dispose()
    Write-Output ("B  paused -> chrome revealed, no frame since    hsd {0}  mean {1}/{2}/{3}" -f $mb.HSd, $mb.R, $mb.G, $mb.B)
    Write-Output ""
    Write-Output "PASS requires B hsd well above 0: the strip is drawing the blur."
    Write-Output "FAIL is B hsd 0.00 -- the solid fallback, on a build that is meant to blur,"
    Write-Output "and it stays that way until playback resumes."
}

if ($Mode -eq 'endreveal') {
    # THE ONLY REVEAL THAT DELIVERS NO FRAME, and every other candidate turned
    # out to deliver one. Key presses reveal the chrome AND run a command that
    # re-presents the current frame; a paused file that never had its backdrop
    # cleared still holds a valid one. What is left is a reveal by POINTER
    # MOVEMENT ALONE, on a file that has stopped delivering frames by itself.
    #
    # Playback running off the end is exactly that: it stops on the last frame
    # with no key pressed. So -- play, park the pointer off the picture, let the
    # chrome hide WHILE frames are still arriving (which is what clears the
    # backdrop to null), let the file run out, then move the pointer back in.
    # Nothing between the hide and the reveal delivers a frame.
    $h = Start-Trace '1'
    [System.Windows.Forms.SendKeys]::SendWait(" ")
    Start-Sleep -Milliseconds 400
    [BdWin]::SetCursorPos(5, 5) | Out-Null
    # Long enough for: the chrome to hide (~2.2s) while frames are still
    # arriving, and then for playback to reach the end and stop.
    Start-Sleep -Milliseconds 9000
    $hid = Capture-Client $h (Join-Path $OutDir "end-H-hidden.png")
    $mh = Measure-Strip $hid; $hid.Dispose()
    Write-Output ("H  ended, chrome hidden -- must be RAW VIDEO   hsd {0}  mean {1}/{2}/{3}" -f $mh.HSd, $mh.R, $mh.G, $mh.B)
    Jiggle $h
    $b = Capture-Client $h (Join-Path $OutDir "end-B-revealed.png")
    $mb = Measure-Strip $b; $b.Dispose()
    Write-Output ("B  revealed by POINTER only, no frame since    hsd {0}  mean {1}/{2}/{3}" -f $mb.HSd, $mb.R, $mb.G, $mb.B)
    Write-Output ""
    Write-Output "PASS requires B hsd well above 0: the strip is drawing the blur."
    Write-Output "FAIL is B hsd 0.00 -- the solid fallback on a build meant to blur, and it"
    Write-Output "stays that way for as long as the file sits at its last frame."
}

if ($Mode -eq 'close') {
    $h = Start-Trace '1'
    Jiggle $h
    Start-Sleep -Milliseconds 400
    $a = Capture-Client $h "$OutDir\close-before.png"
    $ma = Measure-Strip $a; $a.Dispose()
    Write-Output ("media open, chrome revealed   hsd {0}  mean {1}/{2}/{3}" -f $ma.HSd, $ma.R, $ma.G, $ma.B)
    [System.Windows.Forms.SendKeys]::SendWait("^w")
    Start-Sleep -Milliseconds 1200
    $b = Capture-Client $h "$OutDir\close-after.png"
    $mb = Measure-Strip $b; $b.Dispose()
    Write-Output ("after Ctrl+W (strip held up)  hsd {0}  mean {1}/{2}/{3}" -f $mb.HSd, $mb.R, $mb.G, $mb.B)
    Write-Output ""
    Write-Output "PASS requires hsd 0.00 after close: the strip is back to the design's solid"
    Write-Output "fallback and is NOT still blurring the file that just closed."
}
