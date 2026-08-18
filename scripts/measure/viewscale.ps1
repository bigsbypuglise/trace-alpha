# Spec phase 15: Actual Size, Fit to Window, Zoom In / Zoom Out, and the pan.
#
# Four modes, and only two of them can fail on a plausible build -- which is
# stated up front because this project keeps writing harnesses that cannot:
#
#   -Mode ladder   presses Ctrl+= six times and Ctrl+- six times, capturing
#                  after each. Reads the HUD's `zoom N:1` field. A build whose
#                  ladder wraps, sticks or steps by the wrong factor reads
#                  differently at every rung, so this one is a real check.
#   -Mode actual   Ctrl+0 and read `display WxH` back. THE ASSERTION IS THAT IT
#                  EQUALS THE SOURCE'S OWN PIXEL SIZE, which is what "Actual
#                  Size" means and what no screenshot can tell you by eye.
#   -Mode pan      THE ONLY LEG WITH A NEGATIVE CONTROL, and it needs one: the
#                  drag is measured as a percentage of the video band that
#                  changed, and a number alone cannot say whether the picture
#                  moved or the transport faded. So it drags twice -- once at
#                  Actual Size where the picture MUST move, and once at fit
#                  where it MUST NOT -- and prints both. A build with no pan at
#                  all reads ~0% on both legs and passes nothing.
#   -Mode filter   captures at 4:1 so the HUD's `NEAREST` / `filtered xN` can be
#                  read. The owner decision is that magnification is NEAREST;
#                  TRACE_MAG_FILTER=linear is the control and -Linear sets it.
#
# THE WINDOW MUST NOT BE RESIZED BY ANY OF THIS. Owner decision, 2026-08-11:
# Actual Size pans inside the viewport and leaves the window alone, because
# section 4's opening size, its 1280x720-equivalent area cap and its 80%
# work-area rule were signed off and a window grown to fit 4K would break all
# three. Every mode prints `win WxH` before and after for exactly that reason.
param(
    [Parameter(Mandatory=$true)][string]$Clip,
    [ValidateSet("ladder","actual","pan","filter")][string]$Mode = "ladder",
    [string]$Renderer = "d3d11",
    [string]$Tag = "",
    # The docked bar. Not optional for a cross-backend picture diff -- the
    # floating overlay is composited over the video, so its fade state lands
    # inside the band being compared (phase 10 read 9.1% of one that way).
    [switch]$Bar,
    # TRACE_MAG_FILTER=linear: the control for the magnification decision.
    [switch]$Linear
)
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Add-Type @"
using System;using System.Runtime.InteropServices;
public class VS {
  [StructLayout(LayoutKind.Sequential)] public struct RC { public int L,T,R,B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RC r);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f,uint x,uint y,uint d,IntPtr e);
}
"@
$repo = "C:\Users\andre\Documents\Claude_Cowork\Trace_Windows"
Set-Location $repo
$out = "$env:TEMP\claude"
if (-not (Test-Path $out)) { New-Item -ItemType Directory $out | Out-Null }
if ($Tag -eq "") { $Tag = $Mode }

Get-Process Trace -EA SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 700
$env:TRACE_RENDERER = $Renderer
if ($Bar) { $env:TRACE_TRANSPORT_BAR = "1" }
if ($Linear) { $env:TRACE_MAG_FILTER = "linear" }
$env:TRACE_HUD = "1"  # this script reads its figures off the HUD; hidden by default since roadmap step 2
Start-Process -FilePath "build\app\Release\Trace.exe" -ArgumentList "`"$Clip`""
$env:TRACE_RENDERER = ""; $env:TRACE_TRANSPORT_BAR = ""; $env:TRACE_MAG_FILTER = ""; $env:TRACE_HUD = ""
Start-Sleep -Seconds 6

$p = Get-Process Trace | Select-Object -First 1
$hwnd = $p.MainWindowHandle
$rc = New-Object VS+RC; [VS]::GetWindowRect($hwnd,[ref]$rc) | Out-Null

# A CLICK, not SetForegroundWindow. Windows refuses foreground activation to a
# background process and the refusal is SILENT -- phase 14 printed "window NOT
# FOUND" three times against a build where it was on screen. Read it back.
[VS]::SetForegroundWindow($hwnd) | Out-Null
Start-Sleep -Milliseconds 300
if ([VS]::GetForegroundWindow() -ne $hwnd) {
    [System.Windows.Forms.Cursor]::Position = New-Object System.Drawing.Point (($rc.L+400),($rc.T+14))
    Start-Sleep -Milliseconds 200
    [VS]::mouse_event(0x0002,0,0,0,[IntPtr]::Zero); Start-Sleep -Milliseconds 70
    [VS]::mouse_event(0x0004,0,0,0,[IntPtr]::Zero); Start-Sleep -Milliseconds 400
}
if ([VS]::GetForegroundWindow() -ne $hwnd) { Write-Output "FOCUS FAIL"; exit 1 }

function Grab($name) {
    $r = New-Object VS+RC; [VS]::GetWindowRect($hwnd,[ref]$r) | Out-Null
    $w = $r.R-$r.L; $h = $r.B-$r.T
    $b = New-Object System.Drawing.Bitmap $w, $h
    $g = [System.Drawing.Graphics]::FromImage($b)
    $g.CopyFromScreen($r.L,$r.T,0,0,$b.Size); $g.Dispose()
    $path = "$out\vs-$Tag-$name.png"
    $b.Save($path); $b.Dispose()
    # Write-Host, not Write-Output: the callers that only want the path pipe the
    # result to Out-Null, and Write-Output would go down that pipe with it --
    # the run would then report nothing at all while doing everything right.
    Write-Host ("  {0,-16} win {1}x{2}  -> {3}" -f $name, $w, $h, (Split-Path $path -Leaf))
    return $path
}

# The video rect's centre, in screen coordinates. Used as the pan drag's anchor
# and deliberately taken from the CURRENT window rect each time rather than
# cached: a mode that resized the window would then drag somewhere else, which
# is a fault worth having show up rather than be compensated for.
function VideoCentre() {
    $r = New-Object VS+RC; [VS]::GetWindowRect($hwnd,[ref]$r) | Out-Null
    return New-Object System.Drawing.Point ((($r.L+$r.R)/2), (($r.T+$r.B)/2 - 40))
}

function Drag($fromX, $fromY, $dx, $dy) {
    [System.Windows.Forms.Cursor]::Position = New-Object System.Drawing.Point $fromX, $fromY
    Start-Sleep -Milliseconds 150
    [VS]::mouse_event(0x0002,0,0,0,[IntPtr]::Zero)
    Start-Sleep -Milliseconds 80
    # Stepped, not teleported. A synthetic drag that jumps in one move produces
    # exactly one mouse-move event, which would pass a build that only panned on
    # the press -- and would tell you nothing about a gesture made of samples.
    for ($i = 1; $i -le 12; $i++) {
        $x = [int]($fromX + $dx * $i / 12.0)
        $y = [int]($fromY + $dy * $i / 12.0)
        [System.Windows.Forms.Cursor]::Position = New-Object System.Drawing.Point $x, $y
        Start-Sleep -Milliseconds 25
    }
    [VS]::mouse_event(0x0004,0,0,0,[IntPtr]::Zero)
    Start-Sleep -Milliseconds 400
}

# THE LADDER LEG IS THIRTEEN CAPTURES AND ITS RESULT IS ONE HUD FIELD PER
# CAPTURE, so it gets stitched into a single strip of just that line. Without
# this the check exists but nobody runs it: reading `zoom N:1` off thirteen
# full-window screenshots is thirteen separate readings, and a ladder is only
# meaningful as a sequence.
#
# The row is found by offset from the BOTTOM of the capture, not the top: the
# HUD is anchored there and the window height changes with the media's shape.
function HudStrip($names, $stripName) {
    $rowH = 18; $w = 1040
    $strip = New-Object System.Drawing.Bitmap $w, ($rowH * $names.Count)
    $g = [System.Drawing.Graphics]::FromImage($strip)
    $g.Clear([System.Drawing.Color]::Black)
    $i = 0
    foreach ($n in $names) {
        $path = "$out\vs-$Tag-$n.png"
        if (-not (Test-Path $path)) { $i++; continue }
        $src = New-Object System.Drawing.Bitmap $path
        # The `color ... | display ... | win ...` line, 358px up from the bottom.
        $r = New-Object System.Drawing.Rectangle 10, ($src.Height - 358), $w, $rowH
        $g.DrawImage($src, (New-Object System.Drawing.Rectangle 0, ($i * $rowH), $w, $rowH),
                     $r, [System.Drawing.GraphicsUnit]::Pixel)
        $src.Dispose(); $i++
    }
    $g.Dispose()
    $strip.Save("$out\vs-$Tag-$stripName.png"); $strip.Dispose()
    Write-Host "  strip -> vs-$Tag-$stripName.png  (rows: $($names -join ', '))"
}

Write-Output "mode=$Mode renderer=$Renderer bar=$($Bar.IsPresent) linear=$($Linear.IsPresent)"
Write-Output "clip=$(Split-Path $Clip -Leaf)"

switch ($Mode) {
  "ladder" {
    Grab "00-fit" | Out-Null
    for ($i = 1; $i -le 6; $i++) {
        [System.Windows.Forms.SendKeys]::SendWait("^{=}")
        Start-Sleep -Milliseconds 450
        Grab ("in-{0}" -f $i) | Out-Null
    }
    for ($i = 1; $i -le 6; $i++) {
        [System.Windows.Forms.SendKeys]::SendWait("^-")
        Start-Sleep -Milliseconds 450
        Grab ("out-{0}" -f $i) | Out-Null
    }
    HudStrip @("00-fit","in-1","in-2","in-3","in-4","in-5","in-6",
               "out-1","out-2","out-3","out-4","out-5","out-6") "strip"
    Write-Output "READ the strip. Expect the doubling ladder anchored on 1.00 in both"
    Write-Output "directions, the top rung pinned by maxViewScale, and NO change in win WxH."
  }
  "actual" {
    Grab "00-fit" | Out-Null
    [System.Windows.Forms.SendKeys]::SendWait("^0")
    Start-Sleep -Milliseconds 600
    Grab "01-actual" | Out-Null
    HudStrip @("00-fit","01-actual") "strip"
    Write-Output "ASSERT: `display WxH` equals the source's own pixel size, `zoom 1.00:1`,"
    Write-Output "and `win WxH` is identical to the fit capture (the window must not move)."
  }
  "pan" {
    # THE NEGATIVE CONTROL FIRST, so a run that dies half way still leaves the
    # leg that proves the instrument works.
    $c = VideoCentre
    $a = Grab "fit-before"
    Drag $c.X $c.Y 260 0
    $b = Grab "fit-after"
    Write-Output "  -- control: drag at FIT, the picture must NOT move --"
    & "$repo\scripts\measure\banddiff.ps1" -A $a -B $b -Label "fit-drag"

    [System.Windows.Forms.SendKeys]::SendWait("^0")
    Start-Sleep -Milliseconds 600
    $c = VideoCentre
    $a2 = Grab "actual-before"
    Drag $c.X $c.Y 260 0
    $b2 = Grab "actual-after"
    Write-Output "  -- result: drag at ACTUAL SIZE, the picture MUST move --"
    & "$repo\scripts\measure\banddiff.ps1" -A $a2 -B $b2 -Label "actual-drag"
  }
  "filter" {
    Grab "00-fit" | Out-Null
    # 1:1 then two rungs up. Ctrl+0 first so the starting point is a known rung
    # rather than whatever ratio this window happens to fit at.
    [System.Windows.Forms.SendKeys]::SendWait("^0")
    Start-Sleep -Milliseconds 500
    Grab "01-actual" | Out-Null
    [System.Windows.Forms.SendKeys]::SendWait("^{=}")
    Start-Sleep -Milliseconds 500
    [System.Windows.Forms.SendKeys]::SendWait("^{=}")
    Start-Sleep -Milliseconds 500
    Grab "02-four-to-one" | Out-Null
    HudStrip @("00-fit","01-actual","02-four-to-one") "strip"
    Write-Output "ASSERT: the fit capture reads `filtered xN`, and 1:1 and 4:1 read"
    Write-Output "`NEAREST` -- unless -Linear, which is the control and reads filtered."
  }
}
