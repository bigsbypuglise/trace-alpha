# EXACT FRAME STEPPING AND PAUSED REVIEW ON THE IMAGE-SEQUENCE PATH, A/B.
#
# WHY. TRACE_SEQ_PREFETCH=0 removes the neighbour loads that populate the frame
# cache, so the frame a step lands on is fetched by a different route (a fresh
# loadExr rather than a cache hit). Playback rate is not the risk there --
# EXACTNESS is. A step must land on the same frame and put the same pixels on
# screen whichever route supplied them.
#
# WHAT IT ASSERTS, and the third one is what makes the first two mean anything:
#
#   1. cross-config, frame N: the two builds' pictures must be IDENTICAL
#   2. cross-config, after stepping back to 0: identical
#   3. within-config, frame N vs frame 0: MUST DIFFER
#
# Without (3) a build that showed one frozen frame forever would pass (1) and
# (2) perfectly. This project has shipped that exact class of false pass before
# -- a comparison that can only report success is not a comparison.
#
# The picture is cropped ABOVE the HUD and 16px in from each side: Windows 11's
# invisible resize border puts whatever is behind Trace into the first columns
# of a GetWindowRect capture, and the HUD's own per-tick counters would differ
# between two runs whatever the picture did.

param(
    [Parameter(Mandatory = $true)][string]$Clip,
    [int]$Steps = 7,
    [string]$OutDir = "$env:TEMP\trace_seqstep",
    # The two configurations compared. Defaults are the prefetch A/B.
    [string[]]$EnvA = @(),
    [string[]]$EnvB = @("TRACE_SEQ_PREFETCH=0"),
    [string]$LabelA = "prefetch ON",
    [string]$LabelB = "prefetch OFF"
)

Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
. "$PSScriptRoot\tracewindow.ps1"

New-Item -ItemType Directory -Force $OutDir | Out-Null
$fail = 0

if (-not ("SeqStep" -as [type])) {
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class SeqStep {
  [DllImport("user32.dll")] public static extern void mouse_event(uint f,uint x,uint y,uint d,UIntPtr e);
}
"@
}

# The picture band: everything above the HUD block, inset from the borders.
function Grab-Picture([IntPtr]$h, [string]$png) {
    $r = New-Object TraceWin+RECT
    [TraceWin]::GetWindowRect($h, [ref]$r) | Out-Null
    $w = ($r.R - $r.L) - 32
    $full = ($r.B - $r.T)
    # The sequence HUD is four lines plus the transport line; 200px clears it
    # with margin, and cropping generously is safer than predicting its height.
    $hgt = $full - 200
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
    & "$PSScriptRoot\restart.ps1" -Clip $Clip -Env (@($envSet) + @("TRACE_HUD=1")) -SettleSeconds 6 | Out-Null
    $h = Resolve-TraceWindow
    if ($h -eq [IntPtr]::Zero) { Write-Warning "seqstep[$tag]: no Trace window"; return $null }
    if (-not (Focus-TraceWindow -Handle $h)) { Write-Warning "seqstep[$tag]: foreground denied"; return $null }

    # Click-activate on the picture: the only thing that takes Qt's keyboard
    # focus off the menu bar, and a bare letter belongs to Trace only then.
    $r = New-Object TraceWin+RECT
    [TraceWin]::GetWindowRect($h, [ref]$r) | Out-Null
    [TraceWin]::SetCursorPos([int](($r.L + $r.R) / 2), [int]($r.T + ($r.B - $r.T) / 3)) | Out-Null
    Start-Sleep -Milliseconds 200
    [SeqStep]::mouse_event(2, 0, 0, 0, [UIntPtr]::Zero)
    [SeqStep]::mouse_event(4, 0, 0, 0, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 500

    $safe = $tag -replace '[^A-Za-z0-9_\-]', '_'
    for ($i = 1; $i -le $Steps; $i++) { [System.Windows.Forms.SendKeys]::SendWait("{RIGHT}"); Start-Sleep -Milliseconds 350 }
    Start-Sleep -Milliseconds 600
    $fwd = Grab-Picture $h (Join-Path $OutDir "$safe-fwd.png")
    & "$PSScriptRoot\capture.ps1" -Out (Join-Path $OutDir "$safe-fwd-full.png") | Out-Null

    for ($i = 1; $i -le $Steps; $i++) { [System.Windows.Forms.SendKeys]::SendWait("{LEFT}"); Start-Sleep -Milliseconds 350 }
    Start-Sleep -Milliseconds 600
    $back = Grab-Picture $h (Join-Path $OutDir "$safe-back.png")
    & "$PSScriptRoot\capture.ps1" -Out (Join-Path $OutDir "$safe-back-full.png") | Out-Null

    Get-Process -Name Trace -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 800
    return @{ Fwd = $fwd; Back = $back; Tag = $tag }
}

Write-Output "seqstep: $Steps steps forward then back, $LabelA vs $LabelB"
$a = Run-Config $EnvA $LabelA
$b = Run-Config $EnvB $LabelB
if ($null -eq $a -or $null -eq $b) { Write-Warning "seqstep: a config did not run; result is void"; exit 1 }

# (3) FIRST, because it is what licenses reading (1) and (2) at all.
$within = Diff-Pct $a.Fwd $a.Back
$ok3 = ($within -ge 1.0)
Write-Output ("  {0,-6} negative control  frame {1} vs frame 0 within '{2}'   {3}% differing" -f $(if ($ok3) { "PASS" } else { "FAIL" }), $Steps, $LabelA, $within)
if (-not $ok3) { $fail++; Write-Warning "seqstep: stepping produced no visible change -- the comparisons below prove nothing." }

$dFwd = Diff-Pct $a.Fwd $b.Fwd
$ok1 = ($dFwd -ge 0 -and $dFwd -le 0.05)
Write-Output ("  {0,-6} cross-config      frame {1}                            {2}% differing" -f $(if ($ok1) { "PASS" } else { "FAIL" }), $Steps, $dFwd)
if (-not $ok1) { $fail++ }

$dBack = Diff-Pct $a.Back $b.Back
$ok2 = ($dBack -ge 0 -and $dBack -le 0.05)
Write-Output ("  {0,-6} cross-config      stepped back to 0                   {1}% differing" -f $(if ($ok2) { "PASS" } else { "FAIL" }), $dBack)
if (-not $ok2) { $fail++ }

foreach ($t in @($a.Fwd, $a.Back, $b.Fwd, $b.Back)) { if ($t) { $t.Dispose() } }
Write-Output ("seqstep: {0}  (crops and full captures in {1})" -f $(if ($fail -eq 0) { "ALL PASS" } else { "$fail FAILED" }), $OutDir)
exit $(if ($fail -eq 0) { 0 } else { 1 })
