# Does audio drop out while the window is being dragged?
#
# Owner report, 2026-08-21: audio gaps while dragging the window, on audio-only
# files and on video-with-audio; other Windows players do not do it.
#
# The mechanism under test: dragging a window puts Windows into a MODAL message
# loop inside DefWindowProc between WM_ENTERSIZEMOVE and WM_EXITSIZEMOVE. If
# whatever services QAudioSink's pull lives on the main thread's event loop,
# the device starves for the length of the drag and the gap is audible.
#
# NO NEW INSTRUMENTATION: AudioPerfStats::underruns and ::silenceBytes already
# exist and are already on the HUD. `under` counts pulls the ring could not
# answer in full; `silence N B` counts the padding bytes handed to the device,
# which IS the gap, measured in bytes. Read them before and after the gesture.
#
# Four legs, because "is this a window-drag bug or a class of bug" changes the
# fix:
#   -Mode drag    the reported gesture: hold the title bar and move
#   -Mode menu    a Qt popup menu held open (nested QT event loop)
#   -Mode dialog  a modal QInputDialog held open (nested Qt event loop)
#   -Mode idle    the control: play for the same wall time, touch nothing
#
# Two cheap controls beyond the legs: -Renderer cpu (if it still gaps, the
# swapchain is not the mechanism), and running any leg against an audio-only
# file as well as a video-with-audio file.

param(
    [ValidateSet('drag','resize','menu','dialog','idle')][string]$Mode = 'drag',
    [Parameter(Mandatory = $true)][string]$Clip,
    [string]$Renderer = 'd3d11',
    [int]$HoldSeconds = 6,
    # Drive the pointer as fast as this process can rather than at ~60Hz. A
    # hand on a gaming mouse floods the queue at 1000Hz, and inside a modal
    # move loop WM_TIMER and WM_PAINT are only synthesised when the queue has
    # no input in it -- so the move RATE, not the drag's duration, is what
    # decides whether anything else on that thread gets to run.
    [switch]$Tight,
    # Horizontal travel of the drag, in pixels either side of the grab point. A
    # hand drag crosses most of the panel; a 40px jiggle repaints a fraction of
    # what a 1600px sweep of a 1269-wide window over a 5120-wide desktop does.
    [int]$Amplitude = 40,
    [string]$Exe,
    [string[]]$Env = @(),
    [string]$OutDir = "$env:TEMP\trace-audiodrag"
)

Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class AD {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, int dx, int dy, uint d, UIntPtr e);
  [DllImport("user32.dll")] public static extern void keybd_event(byte k, byte s, uint f, UIntPtr e);
}
"@
$LDOWN = 0x0002
$LUP   = 0x0004

function Focus-Window([IntPtr]$h) {
    [AD]::SetForegroundWindow($h) | Out-Null
    Start-Sleep -Milliseconds 250
    if ([AD]::GetForegroundWindow() -eq $h) { return $true }
    [AD]::keybd_event(0xA4, 0, 0, [UIntPtr]::Zero)
    [AD]::keybd_event(0xA4, 0, 2, [UIntPtr]::Zero)
    [AD]::SetForegroundWindow($h) | Out-Null
    Start-Sleep -Milliseconds 250
    return [AD]::GetForegroundWindow() -eq $h
}

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
foreach ($pair in $Env) {
    $i = $pair.IndexOf('=')
    if ($i -lt 1) { Write-Output "bad -Env entry '$pair'"; exit 1 }
    Set-Item -Path ("env:" + $pair.Substring(0, $i)) -Value $pair.Substring($i + 1)
}
$proc = Start-Process -FilePath $Exe -ArgumentList ('"' + $Clip + '"') -PassThru
Remove-Item env:TRACE_RENDERER -ErrorAction SilentlyContinue
Remove-Item env:TRACE_HUD -ErrorAction SilentlyContinue
foreach ($pair in $Env) {
    Remove-Item -Path ("env:" + $pair.Substring(0, $pair.IndexOf('='))) -ErrorAction SilentlyContinue
}

Start-Sleep -Seconds 4
if ($proc.HasExited) { Write-Output "EXITED EARLY code $($proc.ExitCode)"; exit 1 }
$p = Get-Process -Name Trace -ErrorAction SilentlyContinue |
     Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $p) { Write-Output "AUDIODRAG: no window"; exit 1 }
$h = $p.MainWindowHandle
if (-not (Focus-Window $h)) { Write-Output "AUDIODRAG: could not take foreground"; exit 1 }

$r = New-Object AD+RECT
[AD]::GetWindowRect($h, [ref]$r) | Out-Null
$cr = New-Object AD+RECT
[AD]::GetClientRect($h, [ref]$cr) | Out-Null
$org = New-Object AD+POINT
[AD]::ClientToScreen($h, [ref]$org) | Out-Null
# Caption band: between the window's top edge and the client area's. Never a
# fixed offset -- Windows 11's invisible resize border puts GetWindowRect's top
# several pixels above anything that is drawn.
$capY = [int](($r.T + $org.Y) / 2)
$capX = [int]($r.L + ($r.R - $r.L) * 0.30)
Write-Output ("window {0},{1} {2}x{3} | client top {4} | caption aim {5},{6}" -f `
    $r.L, $r.T, ($r.R - $r.L), ($r.B - $r.T), $org.Y, $capX, $capY)

# Click the picture to activate (feedback item 13's click-activate), then play.
[AD]::SetCursorPos([int]($org.X + $cr.R / 2), [int]($org.Y + $cr.B / 2)) | Out-Null
Start-Sleep -Milliseconds 150
[AD]::mouse_event($LDOWN, 0, 0, 0, [UIntPtr]::Zero)
[AD]::mouse_event($LUP,   0, 0, 0, [UIntPtr]::Zero)
Start-Sleep -Milliseconds 300
[System.Windows.Forms.SendKeys]::SendWait(" ")
Start-Sleep -Seconds 3

& "$PSScriptRoot\capture.ps1" -Out (Join-Path $OutDir "01-before.png") -HudOnly | Out-Null

$sw = [System.Diagnostics.Stopwatch]::StartNew()
switch ($Mode) {
    'drag' {
        [AD]::SetCursorPos($capX, $capY) | Out-Null
        Start-Sleep -Milliseconds 150
        [AD]::mouse_event($LDOWN, 0, 0, 0, [UIntPtr]::Zero)
        Start-Sleep -Milliseconds 100
        # Keep moving for the whole hold. A drag that teleports once and then
        # sits still is not the reported gesture, and a stationary pointer is a
        # weaker test of a modal move loop than one generating input.
        #
        # Sample the window rect while the button is down, because "the modal
        # move loop ran" and "the window moved" are different claims and the
        # wm enter/exit counters only make the first. A run where every sample
        # reads the same origin is a gesture that grabbed nothing.
        $samples = @()
        $mid = $false
        $moves = 0
        while ($sw.Elapsed.TotalSeconds -lt $HoldSeconds) {
            $t = $sw.Elapsed.TotalSeconds
            $dx = [int]($Amplitude * [Math]::Sin($t * 6.0))
            $dy = [int](18 * [Math]::Cos($t * 4.0))
            [AD]::SetCursorPos($capX + $dx, $capY + $dy) | Out-Null
            if ($samples.Count -lt [int]($t * 2) + 1) {
                $wr = New-Object AD+RECT
                [AD]::GetWindowRect($h, [ref]$wr) | Out-Null
                $samples += ("{0:F1}s {1},{2}" -f $t, $wr.L, $wr.T)
            }
            if (-not $mid -and $t -ge $HoldSeconds / 2.0) {
                $mid = $true
                # Inline, never capture.ps1: that helper calls
                # SetForegroundWindow, and asking for the foreground in the
                # middle of a modal move loop can cancel the drag being
                # measured.
                $mr = New-Object AD+RECT
                [AD]::GetWindowRect($h, [ref]$mr) | Out-Null
                $mw = $mr.R - $mr.L
                $mh = $mr.B - $mr.T
                $off = [int]($mh * 0.56)
                $bmp = New-Object System.Drawing.Bitmap $mw, ($mh - $off)
                $g = [System.Drawing.Graphics]::FromImage($bmp)
                $g.CopyFromScreen($mr.L, $mr.T + $off, 0, 0, $bmp.Size)
                $bmp.Save((Join-Path $OutDir "01b-mid.png"),
                          [System.Drawing.Imaging.ImageFormat]::Png)
                $g.Dispose(); $bmp.Dispose()
            }
            $moves++
            if (-not $Tight) { Start-Sleep -Milliseconds 16 }
        }
        Write-Output ("pointer moves {0} over {1:F2}s = {2:F0}/s" -f `
            $moves, $sw.Elapsed.TotalSeconds, ($moves / $sw.Elapsed.TotalSeconds))
        [AD]::SetCursorPos($capX, $capY) | Out-Null
        Start-Sleep -Milliseconds 60
        [AD]::mouse_event($LUP, 0, 0, 0, [UIntPtr]::Zero)
        Write-Output ("window while held: " + ($samples -join " | "))
    }
    'resize' {
        # The other modal size/move gesture, and the one with real UI-thread
        # work inside the loop: WM_SIZING fires ~120 times across a drag, each
        # running the section 4 aspect lock, each resizeEvent re-syncing the
        # scrub preview size. A move loop does none of that.
        $rx = $r.R - 6
        $ry = $r.B - 6
        [AD]::SetCursorPos($rx, $ry) | Out-Null
        Start-Sleep -Milliseconds 150
        [AD]::mouse_event($LDOWN, 0, 0, 0, [UIntPtr]::Zero)
        Start-Sleep -Milliseconds 100
        while ($sw.Elapsed.TotalSeconds -lt $HoldSeconds) {
            $t = $sw.Elapsed.TotalSeconds
            $d = [int](120 * [Math]::Sin($t * 3.0))
            [AD]::SetCursorPos($rx + $d, $ry + $d) | Out-Null
            Start-Sleep -Milliseconds 16
        }
        [AD]::SetCursorPos($rx, $ry) | Out-Null
        Start-Sleep -Milliseconds 60
        [AD]::mouse_event($LUP, 0, 0, 0, [UIntPtr]::Zero)
    }
    'menu' {
        # Alt+F opens File from hidden chrome, and a Qt popup runs its own
        # nested QT event loop -- a different mechanism from the Win32 modal
        # move loop, which is the whole reason this leg exists.
        [System.Windows.Forms.SendKeys]::SendWait("%f")
        Start-Sleep -Seconds $HoldSeconds
        [System.Windows.Forms.SendKeys]::SendWait("{ESC}")
        Start-Sleep -Milliseconds 250
        [System.Windows.Forms.SendKeys]::SendWait("{ESC}")
    }
    'dialog' {
        [System.Windows.Forms.SendKeys]::SendWait("^g")
        Start-Sleep -Seconds $HoldSeconds
        [System.Windows.Forms.SendKeys]::SendWait("{ESC}")
    }
    'idle' {
        Start-Sleep -Seconds $HoldSeconds
    }
}
$sw.Stop()
Write-Output ("{0} held {1:F2}s" -f $Mode, $sw.Elapsed.TotalSeconds)

Start-Sleep -Milliseconds 900
& "$PSScriptRoot\capture.ps1" -Out (Join-Path $OutDir "02-after.png") -HudOnly | Out-Null
Write-Output ("read the audio line's under/silence on both captures in " + $OutDir)
