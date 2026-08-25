# REVERSE PLAYBACK ON THE IMAGE-SEQUENCE PATH, A/B ACROSS TWO CONFIGURATIONS.
#
# WHY. The stride-aware prefetch gate counts SIGNED unit steps, so reverse at 1x
# is meant to run the counter with dir -1 and predict backwards with no second
# branch. That is a claim about construction and it had never been executed --
# every figure taken for the policy was forward playback. This closes it.
#
# THE GESTURE: End (go to the last frame), then J (reverse at 1x), hold, capture.
# Then K to stop, and step +3 / -3 afterwards, because the risk on a policy that
# changes WHICH frames are cached is not rate -- it is whether the frame you stop
# on and the frames you step to are still exact.
#
# Reverse is silent by the standing audio rule, so no TRACE_NO_AUDIO control is
# needed: both configurations are already on the same scheduler.

param(
    [Parameter(Mandatory = $true)][string]$Clip,
    [int]$Seconds = 12,
    [int]$Steps = 3,
    [string]$OutDir = "$env:TEMP\trace_seqreverse",
    [string[]]$EnvA = @("TRACE_SEQ_PROFILE=1"),
    [string[]]$EnvB = @("TRACE_SEQ_PROFILE=1", "TRACE_SEQ_PREFETCH_STRIDE=1"),
    [string]$LabelA = "fixed",
    [string]$LabelB = "gate4"
)

Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
. "$PSScriptRoot\tracewindow.ps1"
New-Item -ItemType Directory -Force $OutDir | Out-Null
$fail = 0

if (-not ("SeqRev" -as [type])) {
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class SeqRev {
  [DllImport("user32.dll")] public static extern void mouse_event(uint f,uint x,uint y,uint d,UIntPtr e);
}
"@
}

function Grab-Picture([IntPtr]$h, [string]$png) {
    $r = New-Object TraceWin+RECT
    [TraceWin]::GetWindowRect($h, [ref]$r) | Out-Null
    $w = ($r.R - $r.L) - 32
    $hgt = ($r.B - $r.T) - 200
    if ($w -le 0 -or $hgt -le 0) { return $null }
    $bmp = New-Object System.Drawing.Bitmap $w, $hgt
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($r.L + 16, $r.T + 40, 0, 0, $bmp.Size)
    $g.Dispose()
    if ($png) { $bmp.Save($png, [System.Drawing.Imaging.ImageFormat]::Png) }
    return $bmp
}

function Diff-Pct($a, $b) {
    if ($null -eq $a -or $null -eq $b) { return -1.0 }
    if ($a.Width -ne $b.Width -or $a.Height -ne $b.Height) { return 100.0 }
    $d = 0; $t = 0
    for ($y = 0; $y -lt $a.Height; $y += 3) {
        for ($x = 0; $x -lt $a.Width; $x += 3) {
            $t++
            $ca = $a.GetPixel($x, $y); $cb = $b.GetPixel($x, $y)
            if ([Math]::Abs($ca.R - $cb.R) -gt 6 -or
                [Math]::Abs($ca.G - $cb.G) -gt 6 -or
                [Math]::Abs($ca.B - $cb.B) -gt 6) { $d++ }
        }
    }
    if ($t -eq 0) { return 0.0 }
    return [Math]::Round(100.0 * $d / $t, 4)
}

function Run-Config([string[]]$envSet, [string]$tag) {
    $safe = $tag -replace '[^A-Za-z0-9_\-]', '_'
    & "$PSScriptRoot\restart.ps1" -Clip $Clip -Env (@($envSet) + @("TRACE_HUD=1")) -SettleSeconds 6 | Out-Null
    $h = Resolve-TraceWindow
    if ($h -eq [IntPtr]::Zero) { Write-Warning "seqreverse[$tag]: no Trace window"; return $null }
    if (-not (Focus-TraceWindow -Handle $h)) { Write-Warning "seqreverse[$tag]: foreground denied"; return $null }

    $r = New-Object TraceWin+RECT
    [TraceWin]::GetWindowRect($h, [ref]$r) | Out-Null
    [TraceWin]::SetCursorPos([int](($r.L + $r.R) / 2), [int]($r.T + ($r.B - $r.T) / 3)) | Out-Null
    Start-Sleep -Milliseconds 200
    [SeqRev]::mouse_event(2, 0, 0, 0, [UIntPtr]::Zero)
    [SeqRev]::mouse_event(4, 0, 0, 0, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 500

    # To the tail, then reverse at 1x.
    [System.Windows.Forms.SendKeys]::SendWait("{END}")
    Start-Sleep -Milliseconds 1200
    [System.Windows.Forms.SendKeys]::SendWait("j")
    Start-Sleep -Seconds $Seconds

    # Captured BEFORE K: the cumulative counters survive the stop but `speed`
    # does not, and a reverse run has to be read while it is a reverse run.
    & "$PSScriptRoot\capture.ps1" -Out (Join-Path $OutDir "$safe-reverse.png") | Out-Null

    [System.Windows.Forms.SendKeys]::SendWait("k")
    Start-Sleep -Milliseconds 900
    $stopped = Grab-Picture $h (Join-Path $OutDir "$safe-stopped.png")
    & "$PSScriptRoot\capture.ps1" -Out (Join-Path $OutDir "$safe-stopped-full.png") | Out-Null

    for ($i = 1; $i -le $Steps; $i++) { [System.Windows.Forms.SendKeys]::SendWait("{RIGHT}"); Start-Sleep -Milliseconds 350 }
    Start-Sleep -Milliseconds 500
    $fwd = Grab-Picture $h (Join-Path $OutDir "$safe-step-fwd.png")
    & "$PSScriptRoot\capture.ps1" -Out (Join-Path $OutDir "$safe-step-fwd-full.png") | Out-Null

    for ($i = 1; $i -le $Steps; $i++) { [System.Windows.Forms.SendKeys]::SendWait("{LEFT}"); Start-Sleep -Milliseconds 350 }
    Start-Sleep -Milliseconds 500
    $back = Grab-Picture $h (Join-Path $OutDir "$safe-step-back.png")
    & "$PSScriptRoot\capture.ps1" -Out (Join-Path $OutDir "$safe-step-back-full.png") | Out-Null

    Copy-Item "$env:TEMP\trace_seqprofile.txt" (Join-Path $OutDir "$safe-profile.txt") -Force -ErrorAction SilentlyContinue
    Get-Process -Name Trace -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 800
    return @{ Stopped = $stopped; Fwd = $fwd; Back = $back; Tag = $tag }
}

Write-Output "seqreverse: End, J, ${Seconds}s reverse, K, +${Steps}/-${Steps}  --  $LabelA vs $LabelB"
$a = Run-Config $EnvA $LabelA
$b = Run-Config $EnvB $LabelB
if ($null -eq $a -or $null -eq $b) { Write-Warning "seqreverse: a config did not run; result is void"; exit 1 }

# NEGATIVE CONTROL FIRST. Stepping after the stop must visibly change the
# picture, or the two cross-config comparisons below are comparing a frozen
# frame with itself and cannot fail.
$within = Diff-Pct $a.Stopped $a.Fwd
$ok0 = ($within -ge 1.0)
Write-Output ("  {0,-6} negative control  stop vs +{1} within '{2}'   {3}% differing" -f $(if ($ok0) { "PASS" } else { "FAIL" }), $Steps, $LabelA, $within)
if (-not $ok0) { $fail++; Write-Warning "seqreverse: stepping after the stop changed nothing -- the results below prove nothing." }

foreach ($leg in @(
    @{ n = "frame stopped on"; x = $a.Stopped; y = $b.Stopped },
    @{ n = "after +$Steps";    x = $a.Fwd;     y = $b.Fwd },
    @{ n = "back to stop";     x = $a.Back;    y = $b.Back })) {
    $d = Diff-Pct $leg.x $leg.y
    $ok = ($d -ge 0 -and $d -le 0.05)
    Write-Output ("  {0,-6} cross-config      {1,-18} {2}% differing" -f $(if ($ok) { "PASS" } else { "FAIL" }), $leg.n, $d)
    if (-not $ok) { $fail++ }
}

foreach ($t in @($a.Stopped, $a.Fwd, $a.Back, $b.Stopped, $b.Fwd, $b.Back)) { if ($t) { $t.Dispose() } }
Write-Output ("seqreverse: {0}  (captures in {1})" -f $(if ($fail -eq 0) { "ALL PASS" } else { "$fail FAILED" }), $OutDir)
exit $(if ($fail -eq 0) { 0 } else { 1 })
