# Does a continuously visible composited overlay cost playback anything?
#
# The requirement it checks is "do not upload an unchanged overlay texture every
# video frame". If the atlas were being re-rasterised or re-uploaded per frame,
# a 4K playback run would show it, because the overlay would be doing QPainter
# work inside the presentation path.
#
# The overlay auto-hides when the pointer stops, so keeping it visible for a
# whole run means jiggling the pointer over the video. That is itself input
# work, which makes this a conservative test rather than a flattering one: the
# comparison run gets the identical jiggle with the overlay switched off.

param(
    [Parameter(Mandatory = $true)][string]$Clip,
    [int]$Seconds = 9,
    [string]$OutDir = "$env:TEMP\trace_overlay_cost"
)

Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Add-Type @"
using System;using System.Runtime.InteropServices;
public class C {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x,int y);
}
"@

New-Item -ItemType Directory -Force $OutDir | Out-Null

foreach ($mode in @(@("off",""), @("on","1"))) {
    $tag = $mode[0]
    $envs = @("TRACE_RENDERER=d3d11")
    if ($mode[1] -ne "") { $envs += "TRACE_OVERLAY_COMPOSITED=1" }
    & "$PSScriptRoot\restart.ps1" -Clip $Clip -Env $envs | Out-Null

    $p = Get-Process -Name Trace | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
    $h = $p.MainWindowHandle
    [C]::SetForegroundWindow($h) | Out-Null
    Start-Sleep -Milliseconds 600
    $r = New-Object C+RECT
    [C]::GetWindowRect($h, [ref]$r) | Out-Null
    $cx = $r.L + [int](($r.R - $r.L) / 2)

    # Park the pointer over the video BEFORE starting, so the first jiggle does
    # not have to travel and the reveal is already up when playback begins.
    [C]::SetCursorPos($cx, $r.T + 200) | Out-Null
    Start-Sleep -Milliseconds 300
    [System.Windows.Forms.SendKeys]::SendWait(" ")

    $sw = [Diagnostics.Stopwatch]::StartNew()
    $i = 0
    while ($sw.Elapsed.TotalSeconds -lt $Seconds) {
        [C]::SetCursorPos($cx + ($i % 7), $r.T + 200 + ($i % 5)) | Out-Null
        Start-Sleep -Milliseconds 200
        $i++
    }

    # Capture WHILE playing. Sending space here would stop the run and the
    # counters would describe a run that had already ended.
    $rr = New-Object C+RECT
    [C]::GetWindowRect($h, [ref]$rr) | Out-Null
    $b = New-Object System.Drawing.Bitmap ($rr.R-$rr.L), ($rr.B-$rr.T)
    $g = [System.Drawing.Graphics]::FromImage($b)
    $g.CopyFromScreen($rr.L, $rr.T, 0, 0, $b.Size)
    $b.Save((Join-Path $OutDir "playback_overlay_$tag.png"), [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $b.Dispose()
    Write-Output "captured overlay=$tag -> playback_overlay_$tag.png"
    Get-Process -Name Trace -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 400
}
