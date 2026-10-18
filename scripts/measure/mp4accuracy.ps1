# Is the frame on screen the frame that was asked for?
#
# THIS IS THE FIRST EXTERNAL CHECK OF THAT CONTRACT IN THIS PROJECT. Every
# `delta 0` recorded anywhere in this repo is the decoder's own accounting: it
# resolves the landed frame's identity from its PTS, stamps it on the frame, and
# compares that stamp with the request. That is a real check and it caught a real
# bug -- but it is SELF-ASSERTION. If the PTS-to-index mapping is wrong, the
# stamp is wrong in exactly the same direction as the comparison, and `delta 0`
# is reported for a frame that is not the one the user asked for. Precisely the
# failure the July 2026 scrub bug was (`scrub exact | delta 0` while displaying
# keyframe 30 for frames 49..55), and it survived because the telemetry asserted
# its own correctness instead of measuring it.
#
# The corpus burns its frame number into the pixels, so the picture can be read
# independently of anything Trace believes. requested is known HERE, displayed
# is read from the screenshot, and nothing in between is trusted.
#
# WHY Go to Frame AND THE ARROW KEYS, AND NOT THE SCRUB BAR. A groove click's
# target is geometric -- it depends on the slider's pixel mapping -- so a
# disagreement could be the mapping rather than the decode, and the measurement
# could not tell which. Ctrl+G takes an exact frame number, and an arrow key is
# exactly +-1. Both give a requested frame that is known without measuring
# anything, which is what makes a mismatch attributable.
#
# Nothing here measures PACING. Latency and smoothness are display-dependent and
# this box is not in a state where either can be judged; see the review doc.

param(
    [Parameter(Mandatory=$true)][string]$Clip,
    [string]$Exe = "",
    # Frames to land on via Ctrl+G. Chosen per clip by the caller, because
    # "mid-GOP" is a property of the file. -1 means "the last frame", resolved
    # from the probe rather than assumed.
    [int[]]$Targets = @(),
    [string]$OutDir = "",
    [int]$SettleMs = 900,
    # The burn-in number MINUS the frame index, for a clip whose burned numbers
    # are not its own index. The edit-list fixture is trimmed 48 frames into an
    # already-burned source, so its first frame carries 48 and `displayed ==
    # requested + 48` is CORRECT edit-list handling rather than a landing error.
    # Passed explicitly rather than inferred: a harness that guessed an offset
    # from the first frame could absorb a real 48-frame landing bug.
    [int]$BurnInOffset = 0,
    [switch]$KeepOpen,
    # Skip the step-reversibility leg (slower; 4 extra captures).
    [switch]$NoStepLeg
)

$ErrorActionPreference = 'Continue'
. "$PSScriptRoot\tracewindow.ps1"
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms

# A click of our own rather than an addition to tracewindow.ps1: ~37 scripts
# dot-source that file and it has no mouse_event, so widening its P/Invoke
# surface for one caller is a change with a blast radius out of proportion to
# the need.
if (-not ("TraceClick" -as [type])) {
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class TraceClick {
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, uint d, UIntPtr e);
}
"@
}

$py = "python"
$reader = Join-Path $PSScriptRoot "readburnin.py"
if (-not (Test-Path $reader)) { throw "readburnin.py missing beside this script" }
if (-not (Test-Path $Clip)) { throw "clip not found: $Clip" }

$clipName = [System.IO.Path]::GetFileNameWithoutExtension($Clip)
if (-not $OutDir) { $OutDir = Join-Path $env:TEMP "mp4accuracy\$clipName" }
if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Force -Path $OutDir | Out-Null }

# ---------------------------------------------------------------- probe -------
# The true frame count, from the container, so "the last frame" is a measured
# number rather than an assumption. nb_frames when the container states it;
# otherwise counted by decoding, which is slow but exact and only happens for
# files that do not state it.
$ffprobe = 'C:\Users\andre\AppData\Local\Microsoft\WinGet\Packages\Gyan.FFmpeg_Microsoft.Winget.Source_8wekyb3d8bbwe\ffmpeg-9.0-full_build\bin\ffprobe.exe'
$nbFrames = 0
# THE FIRST RUN OF DIGITS, NOT AN EXACT ^\d+$ MATCH. With `-of csv=p=0` a stream
# carrying SIDE DATA -- a rotated file's display matrix, for instance -- emits a
# trailing empty field, so the value arrives as "480," and an anchored match
# rejects it. That left nb_frames at 0 on the rotation fixture, which collapsed
# the target list to a single frame and reported 4 legs instead of 17.
function Get-FirstInt {
    param($Raw)
    foreach ($line in @($Raw)) {
        if ($null -eq $line) { continue }
        $m = [regex]::Match([string]$line, '\d+')
        if ($m.Success) { return [int]$m.Value }
    }
    return 0
}
if (Test-Path $ffprobe) {
    $nbFrames = Get-FirstInt (& $ffprobe -v error -select_streams v:0 -show_entries stream=nb_frames -of csv=p=0 $Clip 2>$null)
    if ($nbFrames -le 0) {
        $nbFrames = Get-FirstInt (& $ffprobe -v error -select_streams v:0 -count_frames -show_entries stream=nb_read_frames -of csv=p=0 $Clip 2>$null)
    }
}
Write-Output "clip      : $clipName"
Write-Output "nb_frames : $nbFrames"

if ($Targets.Count -eq 0) {
    # A spread that hits the cases that break differently: the first frame, a
    # frame just after a keyframe, a deliberately mid-GOP frame, something deep
    # in the file, and the last frame. Fractions rather than constants so the
    # same set works on a 96-frame and a 480-frame clip.
    # EACH TARGET IS COMPUTED INTO A VARIABLE FIRST, AND THAT IS NOT STYLE.
    # Written as one array literal with arithmetic inline, PowerShell parsed it
    # two different wrong ways at once:
    #
    #   @(a, b, c, $n-2)      -> the comma operator binds tighter than `-`, so
    #                            the whole preceding list became the LEFT operand
    #                            and it threw "[System.Object[]] does not contain
    #                            a method named op_Subtraction"
    #   @(..., [int]($n*0.5)+1, ...)
    #                         -> split into TWO elements, 240 and 1
    #
    # The throw left $Targets at its empty default, so the goto loop iterated
    # zero times and the run reported PASS on nine legs that were all step,
    # home and end. An accuracy matrix that makes no comparisons reports no
    # mismatches -- the "0 of 0" failure this project already records, reached
    # here through operator precedence.
    $n = [Math]::Max(1, $nbFrames)
    $q1 = [int]($n * 0.25)
    $mid = [int]($n * 0.5) + 1
    $q3 = [int]($n * 0.75)
    $penult = $n - 2
    $last = $n - 1
    $Targets = @(0, 1, 7, $q1, $mid, $q3, $penult, $last) |
               Where-Object { $_ -ge 0 -and $_ -lt $n } |
               Sort-Object -Unique
}

# --------------------------------------------------------------- helpers ------
function Read-Burnin {
    param([string]$Png)
    $j = & $py -I $reader $Png --json 2>$null
    if (-not $j) { return @{ ok=$false; error="reader produced no output" } }
    try { $d = ($j | ConvertFrom-Json)[0] } catch { return @{ ok=$false; error="reader json unparsable" } }
    if ($d.ok) { return @{ ok=$true; frame=([int]$d.frame - $BurnInOffset); raw=[int]$d.frame } }
    return @{ ok=$false; error=[string]$d.error }
}

function Capture-Window {
    param([IntPtr]$H, [string]$Out)
    # Park the pointer clear of the window first. Belt and braces: the burn-in
    # plate is in the picture's middle where no chrome reaches it, so a revealed
    # transport can no longer cover it -- but a pointer inside the client holds
    # BOTH strips up indefinitely, and a capture of the shipping look is the one
    # worth keeping for a human to check afterwards.
    Move-CursorClear
    $r = New-Object TraceWin+RECT
    if (-not [TraceWin]::GetWindowRect($H, [ref]$r)) { return $false }
    $w = $r.R - $r.L; $ht = $r.B - $r.T
    if ($w -le 0 -or $ht -le 0) { return $false }
    # Windows 11 puts an invisible resize border in GetWindowRect, so the first
    # and last columns are whatever is behind Trace. The recorded fix is to drop
    # 16px; the plate starts at the picture's own left edge, which is inside
    # that, so trimming cannot clip a bit cell.
    $trim = 16
    $bmp = New-Object System.Drawing.Bitmap ($w - 2*$trim), $ht
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($r.L + $trim, $r.T, 0, 0, $bmp.Size)
    $bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose()
    return $true
}

function Send-Keys { param([string]$K) [System.Windows.Forms.SendKeys]::SendWait($K) }

# Click the picture before sending keys. Recorded trap: leaving Qt's menu mode
# can leave the menu bar focused, where Space and other bare keys are silently
# swallowed -- and Go to Frame is reached by a shortcut, not the menu, so the
# dialog closing must not leave focus anywhere surprising either. Item 13's
# click-activate makes a picture click take foreground without moving the
# playhead.
function Click-Picture {
    param([IntPtr]$H)
    $r = New-Object TraceWin+RECT
    [TraceWin]::GetWindowRect($H, [ref]$r) | Out-Null
    # Upper-middle of the picture: clear of the transport strip at the bottom and
    # of the top chrome.
    $x = [int](($r.L + $r.R) / 2)
    $y = [int]($r.T + ($r.B - $r.T) * 0.35)
    [TraceWin]::SetCursorPos($x, $y) | Out-Null
    Start-Sleep -Milliseconds 80
    [TraceClick]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero)
    [TraceClick]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 150
}

function Go-ToFrame {
    param([IntPtr]$H, [int]$Frame)
    Send-Keys "^g"
    Start-Sleep -Milliseconds 450
    # The dialog is a QInputDialog SPIN BOX. Select-all then type, so a
    # pre-filled value is replaced rather than appended to -- an appended digit
    # would land on a plausible wrong frame and read as a landing bug.
    Send-Keys "^a"
    Send-Keys "$Frame"
    Start-Sleep -Milliseconds 120
    Send-Keys "{ENTER}"
    Start-Sleep -Milliseconds $SettleMs
}

# ----------------------------------------------------------------- launch -----
Get-Process -Name Trace -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 400

$restartArgs = @{ Clip = $Clip; Env = @("TRACE_HUD=1","TRACE_NO_AUDIO=1") }
if ($Exe) { $restartArgs.Exe = $Exe }
& "$PSScriptRoot\restart.ps1" @restartArgs | Out-Null

$h = Resolve-TraceWindow
if ($h -eq [IntPtr]::Zero) { Write-Output "RESULT: FAIL -- no Trace window"; exit 1 }
if (-not (Focus-TraceWindow -Handle $h)) { Write-Output "RESULT: FAIL -- foreground denied"; exit 1 }
Click-Picture -H $h

$rows = New-Object System.Collections.Generic.List[object]
$unreadable = 0

# ------------------------------------------------------------ leg: on open ----
# The opening frame, before any navigation. A file whose first displayed frame
# is not frame 0 is wrong before any seek is involved -- which is the shape an
# edit list or a non-zero start time would take.
$p0 = Join-Path $OutDir "open.png"
if (Capture-Window -H $h -Out $p0) {
    $r0 = Read-Burnin -Png $p0
    if ($r0.ok) {
        $rows.Add([pscustomobject]@{ leg="open"; requested=0; displayed=$r0.frame; match=($r0.frame -eq 0) })
    } else { $unreadable++; $rows.Add([pscustomobject]@{ leg="open"; requested=0; displayed=$null; match=$false; note=$r0.error }) }
}

# -------------------------------------------------------- leg: Go to Frame ----
foreach ($t in $Targets) {
    Go-ToFrame -H $h -Frame $t
    $p = Join-Path $OutDir ("goto_{0}.png" -f $t)
    if (-not (Capture-Window -H $h -Out $p)) { continue }
    $r = Read-Burnin -Png $p
    if ($r.ok) {
        $rows.Add([pscustomobject]@{ leg="goto"; requested=$t; displayed=$r.frame; match=($r.frame -eq $t) })
    } else {
        $unreadable++
        $rows.Add([pscustomobject]@{ leg="goto"; requested=$t; displayed=$null; match=$false; note=$r.error })
    }
}

# ------------------------------------------------------- leg: step +1 / -1 ----
# Stepping is where B-frame reordering and open GOPs show up: a decoder that
# reorders wrongly can still land correctly on a seek and be wrong on a walk.
# Run from a mid-file frame so both directions involve real work.
if (-not $NoStepLeg -and $nbFrames -gt 20) {
    $base = [int]($nbFrames * 0.5)
    Go-ToFrame -H $h -Frame $base
    $seq = @()
    foreach ($i in 1..3) {
        Send-Keys "{RIGHT}"; Start-Sleep -Milliseconds $SettleMs
        $p = Join-Path $OutDir ("step_fwd{0}.png" -f $i)
        if (Capture-Window -H $h -Out $p) {
            $r = Read-Burnin -Png $p
            $want = $base + $i
            if ($r.ok) { $rows.Add([pscustomobject]@{ leg="step+"; requested=$want; displayed=$r.frame; match=($r.frame -eq $want) }) }
            else { $unreadable++; $rows.Add([pscustomobject]@{ leg="step+"; requested=$want; displayed=$null; match=$false; note=$r.error }) }
        }
    }
    foreach ($i in 1..3) {
        Send-Keys "{LEFT}"; Start-Sleep -Milliseconds $SettleMs
        $p = Join-Path $OutDir ("step_back{0}.png" -f $i)
        if (Capture-Window -H $h -Out $p) {
            $r = Read-Burnin -Png $p
            $want = $base + 3 - $i
            if ($r.ok) { $rows.Add([pscustomobject]@{ leg="step-"; requested=$want; displayed=$r.frame; match=($r.frame -eq $want) }) }
            else { $unreadable++; $rows.Add([pscustomobject]@{ leg="step-"; requested=$want; displayed=$null; match=$false; note=$r.error }) }
        }
    }
}

# ------------------------------------------------------------ leg: Home/End ---
Send-Keys "{HOME}"; Start-Sleep -Milliseconds $SettleMs
$p = Join-Path $OutDir "home.png"
if (Capture-Window -H $h -Out $p) {
    $r = Read-Burnin -Png $p
    if ($r.ok) { $rows.Add([pscustomobject]@{ leg="home"; requested=0; displayed=$r.frame; match=($r.frame -eq 0) }) }
    else { $unreadable++; $rows.Add([pscustomobject]@{ leg="home"; requested=0; displayed=$null; match=$false; note=$r.error }) }
}
Send-Keys "{END}"; Start-Sleep -Milliseconds $SettleMs
$p = Join-Path $OutDir "end.png"
if (Capture-Window -H $h -Out $p) {
    $r = Read-Burnin -Png $p
    # END's correct answer is the LAST frame, and what that is is exactly the
    # question a frame-count disagreement raises -- so it is reported against
    # the container's count rather than asserted, and a mismatch here is
    # labelled as the count question rather than as a landing failure.
    $want = $nbFrames - 1
    if ($r.ok) { $rows.Add([pscustomobject]@{ leg="end"; requested=$want; displayed=$r.frame; match=($r.frame -eq $want) }) }
    else { $unreadable++; $rows.Add([pscustomobject]@{ leg="end"; requested=$want; displayed=$null; match=$false; note=$r.error }) }
}

if (-not $KeepOpen) {
    Get-Process -Name Trace -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
}

# ----------------------------------------------------------------- verdict ----
Write-Output ""
$rows | Format-Table -AutoSize leg, requested, displayed, match, note | Out-String | Write-Output

$total = $rows.Count
# @() around the pipeline: Where-Object returns a SCALAR when exactly one
# object matches, and .Count on it came back empty -- so a run with precisely
# one mismatch printed "mismatches " with nothing after it, and the matrix
# driver that parses this line would have failed to match at all.
$bad = @($rows | Where-Object { -not $_.match }).Count
$worstDelta = 0
foreach ($r in $rows) {
    if ($null -ne $r.displayed) {
        $d = [Math]::Abs([int]$r.displayed - [int]$r.requested)
        if ($d -gt $worstDelta) { $worstDelta = $d }
    }
}
Write-Output ("legs {0} | mismatches {1} | unreadable {2} | worst delta {3}" -f $total, $bad, $unreadable, $worstDelta)
# A RUN WITH NO GOTO LEGS CANNOT PASS. The precedence fault above produced
# exactly that and called it PASS; asserting the leg is present makes the same
# class of failure loud instead of flattering.
$gotoLegs = @($rows | Where-Object { $_.leg -eq 'goto' }).Count
if ($gotoLegs -eq 0) {
    Write-Output "RESULT: FAIL -- no goto legs ran (targets empty); the matrix made no absolute-landing comparisons"
    exit 1
}
if ($bad -eq 0 -and $unreadable -eq 0) { Write-Output "RESULT: PASS"; exit 0 }
Write-Output "RESULT: FAIL"
exit 1
