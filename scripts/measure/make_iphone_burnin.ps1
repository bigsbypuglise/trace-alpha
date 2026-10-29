# Burn the corpus frame-identity plate into REAL iPhone footage, keeping its
# timestamps bit-for-bit.
#
# WHY THIS EXISTS. The MP4 review (docs/mp4-playback-review.md section 3.6, F2)
# measured frame accuracy on eighteen SYNTHETIC files and found it exact on the
# sixteen well-formed ones. The one case it could not put on screen is the one
# the owner's complaint is most likely to live in: genuinely variable-rate phone
# footage. The pool's two iPhone clips carry no burn-in, so `vfrmap.py` could
# only PREDICT them clean -- and IMG_0516 uses 62% of the half-frame rounding
# budget the index model allows (maxoff 0.310 against 0.5), i.e. less than 2x
# margin on a prediction. Re-encoding with the plate over it is what turns that
# prediction into a measurement.
#
# THE WHOLE POINT IS THAT THE TIMESTAMPS SURVIVE, AND THAT IS NOT FREE. The
# review already paid for this lesson once: `h264_1080p_vfr` was built with a
# `setpts` jitter expression and the encoder's 1/24 time base QUANTISED THE
# JITTER AWAY, leaving duplicate and skipped timestamps instead of sub-frame
# variation -- a fixture that tested something other than its own filename
# (section 2.5). So this script does not merely ASK for passthrough timing, it
# ASSERTS the result: -Verify reads the output's whole PTS list back and compares
# it element by element with the input's. A copy whose PTS list differs is
# testing a different file and is rejected rather than reported.
#
# The three terms that make it survive:
#   -fps_mode passthrough     every input frame out with its own timestamp; no
#                             CFR conforming, no duplication, no dropping.
#   -video_track_timescale 600  the source time base. iPhone writes 1/600, and at
#                             24000/1001 a frame period is 25.0208 ticks -- so the
#                             sub-frame jitter this file is valuable for IS the
#                             1/600 quantisation. Let the muxer pick its own
#                             timescale and the ticks get rescaled and rounded,
#                             which is the same class of loss as the setpts one.
#   -enc_time_base <src tb>   keep the ENCODER's time base equal to the input's
#                             rather than letting libx265 impose its own. This
#                             term is load-bearing and was PROVEN so rather than
#                             included defensively: on a 74-frame probe of
#                             IMG_0516, with it the output pts list is identical
#                             element by element, and with it OMITTED -- every
#                             other term unchanged, muxer timescale still 600 --
#                             43 OF 74 TICKS DIFFER. The first dozen ticks match
#                             either way, so a spot check of the head of the file
#                             passes on the broken configuration. (`-enc_time_base
#                             -1`, the "copy the input" spelling, is rejected
#                             outright by FFmpeg 9: "Invalid time base: -1".)
#
# ROTATION, WHICH IS THE KNOWN SNAG (review section 3.5). IMG_0516 carries
# rotation -90. The plate's bit strip is MSB-first LEFT TO RIGHT, so a rotated
# capture cannot be read -- and worse, a 180-degree one reads as a VALID but
# WRONG number (frame 479 came back as 1006), which is why the reader refuses a
# rotated capture rather than guessing. Two copies are therefore made:
#   _norot  rotation overridden to 0 at the INPUT, so the stored frames and the
#           timing are untouched and the plate is readable. This is the copy the
#           accuracy matrix measures, and it is the one that answers F2.
#   _rot    -noautorotate with the -90 tag preserved, so the rotated path is
#           exercised end to end. Its plate is NOT expected to be machine
#           readable; it is for opening, timing counters and the eye.
# Note `-display_rotation` is an INPUT option. Written as an output option
# ffmpeg accepts it and silently does nothing -- the trap CLAUDE.md records from
# make_shape_fixtures.ps1.
#
# AUDIO AND DATA TRACKS ARE COPIED, not dropped. Both clips carry a second
# audio track (apple_apac, 4-channel spatial) beside the stereo AAC, plus five
# mebx metadata tracks ffprobe cannot identify. Keeping them means the copy
# exercises the same track-selection decision the original does.

param(
    [string]$Src    = "C:\Users\andre\Documents\Claude_Cowork\Trace_Testing_Assets\19_IphonePro17max",
    [string]$OutDir = "C:\Users\andre\Documents\Claude_Cowork\Trace_Testing_Assets\24_iPhone_BurnIn",
    [string]$FFmpeg = 'C:\Users\andre\AppData\Local\Microsoft\WinGet\Packages\Gyan.FFmpeg_Microsoft.Winget.Source_8wekyb3d8bbwe\ffmpeg-9.0-full_build\bin\ffmpeg.exe',
    [string]$FFprobe = 'C:\Users\andre\AppData\Local\Microsoft\WinGet\Packages\Gyan.FFmpeg_Microsoft.Winget.Source_8wekyb3d8bbwe\ffmpeg-9.0-full_build\bin\ffprobe.exe',
    [string]$Preset = "fast",
    [int]$Crf       = 20,
    # Keyframe interval. The originals measure max GOP 24 / mean 20 (0516) and
    # max 20 (0563), so 24 keeps the shape without pretending to reproduce the
    # camera's scene-cut decisions.
    [int]$Gop       = 24,
    [switch]$VerifyOnly
)

$ErrorActionPreference = 'Continue'
if (-not (Test-Path $FFmpeg))  { throw "full FFmpeg not found at $FFmpeg" }
if (-not (Test-Path $FFprobe)) { throw "full ffprobe not found at $FFprobe" }
if (-not (Test-Path $OutDir))  { New-Item -ItemType Directory -Force -Path $OutDir | Out-Null }

# ------------------------------------------------------- the plate ------------
# Byte-for-byte the design in make_mp4_corpus.ps1's Get-BurnIn. Duplicated
# rather than dot-sourced DELIBERATELY: that script is a generator with its own
# encode matrix and side effects at load, and readburnin.py reads the plate by
# measuring it (plate aspect asserted against the design's 130/1110) rather than
# from constants -- so the thing that must agree between the two generators is
# the GEOMETRY, and the reader checks that itself on every capture.
$fontFile = 'C\:/Windows/Fonts/consolab.ttf'
if (-not (Test-Path 'C:\Windows\Fonts\consolab.ttf')) { $fontFile = 'C\:/Windows/Fonts/arialbd.ttf' }

function Get-BurnIn {
    param([int]$W, [int]$H)
    $k        = $W / 1920.0
    $numSize  = [int](150 * $k)
    $tcSize   = [int](60 * $k)
    $bitW     = [int](90 * $k)
    $bitH     = [int](90 * $k)
    $bitY     = [int]($H * 0.45)
    $bitX0    = [int](60 * $k)
    $gap      = [int](10 * $k)

    $parts = New-Object System.Collections.Generic.List[string]
    $plateW = $bitX0 * 2 + 10 * $bitW + 9 * $gap
    $plateH = $bitH + [int](40 * $k)
    $plateY = $bitY - [int](20 * $k)
    $parts.Add("drawbox=x=0:y=${plateY}:w=${plateW}:h=${plateH}:color=0x808080@1.0:t=fill")
    for ($i = 0; $i -lt 10; $i++) {
        $bitValue = [math]::Pow(2, 9 - $i)
        $x = $bitX0 + $i * ($bitW + $gap)
        $parts.Add("drawbox=x=${x}:y=${bitY}:w=${bitW}:h=${bitH}:color=white@1.0:t=fill:enable='bitand(n\,$bitValue)'")
    }
    $parts.Add("drawtext=fontfile='${fontFile}':text='F %{frame_num}':x=${bitX0}:y=${bitX0}:fontsize=${numSize}:fontcolor=white:box=1:boxcolor=black@0.7:boxborderw=${gap}")
    $parts.Add("drawtext=fontfile='${fontFile}':text='%{pts\:hms}':x=${bitX0}:y=$($bitX0 + $numSize + $gap * 2):fontsize=${tcSize}:fontcolor=yellow:box=1:boxcolor=black@0.7:boxborderw=${gap}")
    return ($parts -join ',')
}

# --------------------------------------------------- pts list helper ----------
# The RAW integer pts in the stream's own time base, not pts_time seconds.
# Seconds are a float rendering of the tick and two different ticks can print
# the same string; the tick is the thing the container stores and the thing
# Trace's index arithmetic consumes.
function Get-PtsTicks {
    param([string]$Path)
    $raw = & $FFprobe -v error -select_streams v:0 -show_entries frame=pts -of csv=p=0 $Path 2>$null
    $out = New-Object System.Collections.Generic.List[string]
    foreach ($line in $raw) {
        $t = ("$line").Trim().TrimEnd(',')
        if ($t -match '^-?\d+$') { $out.Add($t) }
    }
    return $out
}

function Get-TimeBase {
    param([string]$Path)
    $v = & $FFprobe -v error -select_streams v:0 -show_entries stream=time_base -of csv=p=0 $Path 2>$null
    return ("$v").Trim().TrimEnd(',')
}

function Get-Rotation {
    param([string]$Path)
    $v = & $FFprobe -v error -select_streams v:0 -show_entries stream_side_data=rotation -of csv=p=0 $Path 2>$null
    $t = ("$v" -join '').Trim().TrimEnd(',')
    if ($t -eq '') { return 'none' }
    return $t
}

# ------------------------------------------------------- the copies -----------
# RotMode: 'strip' overrides the input rotation to 0 so the stored frames are
# decoded and emitted as stored; 'keep' passes -noautorotate and leaves the
# display matrix alone.
$jobs = @(
    @{ in='IMG_0516.MOV'; out='IMG_0516_burnin_norot.MOV'; rot='strip' },
    @{ in='IMG_0563.MOV'; out='IMG_0563_burnin.MOV';       rot='strip' },
    @{ in='IMG_0516.MOV'; out='IMG_0516_burnin_rot.MOV';   rot='keep'  }
)

$results = New-Object System.Collections.Generic.List[object]

foreach ($j in $jobs) {
    $inPath  = Join-Path $Src    $j.in
    $outPath = Join-Path $OutDir $j.out
    if (-not (Test-Path $inPath)) { Write-Warning "missing source: $inPath"; continue }

    $srcTb  = Get-TimeBase $inPath
    $srcRot = Get-Rotation $inPath
    $srcPts = Get-PtsTicks $inPath

    # Stored frame size, which is what the plate is scaled against. With the
    # rotation stripped this is also the displayed size; with it kept the
    # display is transposed and the plate comes out rotated on screen, which is
    # expected and is why that copy is not the measurement one.
    $wh = & $FFprobe -v error -select_streams v:0 -show_entries stream=width,height -of csv=p=0 $inPath 2>$null
    $parts = ("$wh").Trim().TrimEnd(',') -split ','
    $W = [int]$parts[0]; $H = [int]$parts[1]

    Write-Host ""
    Write-Host "=== $($j.in) -> $($j.out)" -ForegroundColor Cyan
    Write-Host "    stored ${W}x${H}  time_base $srcTb  rotation $srcRot  frames $($srcPts.Count)  rot-mode $($j.rot)"

    if (-not $VerifyOnly) {
        $burn = Get-BurnIn -W $W -H $H

        # Input options come BEFORE -i and the rotation decision is one of them.
        $a = New-Object System.Collections.Generic.List[string]
        $a.AddRange([string[]]@('-hide_banner','-loglevel','warning','-y'))
        if ($j.rot -eq 'strip') {
            $a.AddRange([string[]]@('-display_rotation','0'))
        } else {
            $a.Add('-noautorotate')
        }
        $a.AddRange([string[]]@('-i', $inPath))
        $a.AddRange([string[]]@('-map','0'))
        $a.AddRange([string[]]@('-c','copy'))
        $a.AddRange([string[]]@('-c:v','libx265'))
        $a.AddRange([string[]]@('-vf', $burn))
        $a.AddRange([string[]]@('-preset', $Preset, '-crf', "$Crf"))
        $a.AddRange([string[]]@('-x265-params', "keyint=$Gop`:min-keyint=1:log-level=error"))
        $a.AddRange([string[]]@('-pix_fmt','yuv420p'))
        $a.AddRange([string[]]@('-tag:v','hvc1'))
        # Colour tags restated so the copy is not silently untagged; the
        # originals are tv/bt709/bt709/bt709 and Trace reads these.
        $a.AddRange([string[]]@('-color_range','tv','-colorspace','bt709','-color_trc','bt709','-color_primaries','bt709'))
        # The three timing terms. See the header.
        $a.AddRange([string[]]@('-fps_mode','passthrough'))
        $a.AddRange([string[]]@('-enc_time_base', $srcTb))
        $a.AddRange([string[]]@('-video_track_timescale', ($srcTb -split '/')[1]))
        $a.Add($outPath)

        $sw = [System.Diagnostics.Stopwatch]::StartNew()
        & $FFmpeg @($a.ToArray()) 2>&1 | ForEach-Object { if ("$_" -match 'rror|arning') { Write-Host "    ffmpeg: $_" -ForegroundColor DarkYellow } }
        $sw.Stop()
        Write-Host ("    encoded in {0:n1}s" -f $sw.Elapsed.TotalSeconds)
    }

    if (-not (Test-Path $outPath)) { Write-Host "    NO OUTPUT" -ForegroundColor Red; continue }

    # ------------------------------------------------- the assertion ----------
    $dstTb  = Get-TimeBase $outPath
    $dstRot = Get-Rotation $outPath
    $dstPts = Get-PtsTicks $outPath

    $tbOk    = ($dstTb -eq $srcTb)
    $countOk = ($dstPts.Count -eq $srcPts.Count)
    $firstBad = -1
    $nDiff = 0
    if ($countOk) {
        for ($i = 0; $i -lt $srcPts.Count; $i++) {
            if ($srcPts[$i] -ne $dstPts[$i]) { $nDiff++; if ($firstBad -lt 0) { $firstBad = $i } }
        }
    }
    $ptsOk = ($countOk -and $nDiff -eq 0)

    # Rotation expectation is per copy, so a surviving tag on the 'keep' copy
    # and an absent one on the 'strip' copy are both PASSES and a swap is a FAIL.
    if ($j.rot -eq 'strip') { $rotOk = ($dstRot -eq 'none') } else { $rotOk = ($dstRot -eq $srcRot) }

    Write-Host "    time_base  $dstTb   $(if($tbOk){'ok'}else{"MISMATCH (src $srcTb)"})"
    Write-Host "    frames     $($dstPts.Count)   $(if($countOk){'ok'}else{"MISMATCH (src $($srcPts.Count))"})"
    if ($countOk) {
        if ($ptsOk) { Write-Host "    pts ticks  IDENTICAL element by element, all $($srcPts.Count)" -ForegroundColor Green }
        else        { Write-Host "    pts ticks  $nDiff OF $($srcPts.Count) DIFFER, first at index $firstBad (src $($srcPts[$firstBad]) dst $($dstPts[$firstBad]))" -ForegroundColor Red }
    }
    Write-Host "    rotation   $dstRot   $(if($rotOk){'as intended'}else{'NOT AS INTENDED'})"

    $verdict = if ($tbOk -and $ptsOk -and $rotOk) { 'PASS' } else { 'FAIL' }
    Write-Host "    verdict    $verdict" -ForegroundColor $(if($verdict -eq 'PASS'){'Green'}else{'Red'})

    $results.Add([pscustomobject]@{
        File = $j.out; Frames = $dstPts.Count; TimeBase = $dstTb
        PtsDiff = $nDiff; Rotation = $dstRot; Verdict = $verdict
    })
}

Write-Host ""
Write-Host "================ summary ================"
$results | Format-Table -AutoSize
$bad = @($results | Where-Object { $_.Verdict -ne 'PASS' })
if ($bad.Count -gt 0) {
    Write-Host "$($bad.Count) copy/copies did not preserve what they claim -- do NOT measure with them." -ForegroundColor Red
    exit 1
}
Write-Host "every copy preserves its source time base, frame count and pts ticks exactly." -ForegroundColor Green
exit 0
