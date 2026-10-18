# The four timing/metadata cases the plain encode matrix cannot express:
# variable frame rate, a non-zero stream start time, a real MP4 edit list, and
# rotation metadata.
#
# These are separated from make_mp4_corpus.ps1 because each is built by a
# DIFFERENT mechanism -- two are container surgery on an already-encoded file
# rather than an encode at all -- and folding them into that script's case table
# would have meant a table whose rows are not comparable with each other.
#
# WHY THEY MATTER MORE THAN THE CODEC ROWS. Trace maps a frame index to a time
# and back with ONE assumption, and these are the four files that violate it:
#
#   frame index -> time   av_rescale_q(frame, 1/fps, stream_tb)  (seekToFrame)
#   time -> frame index   av_rescale_q_rnd(pts - start, tb, 1/fps) (frameFromPts)
#
# Both directions multiply by the NOMINAL frame rate, so Trace's "frame N" means
# "the frame whose timestamp is nearest N/fps". On constant-frame-rate material
# that is the same thing as "the Nth frame" and everything agrees. On VFR it is
# not, and these clips are how that stops being a reading of the source and
# becomes a measurement.
#
# THE BURN-IN NUMBER IS STREAM POSITION, NOT SOURCE POSITION, AND ON THE VFR
# CLIP THAT DISTINCTION IS THE WHOLE POINT. drawtext runs AFTER the retiming, so
# the number burned into the Nth frame of the file is N -- which makes "the Nth
# frame of the file" readable from the pixels, and lets the measurement say
# whether Trace's index means that or something else.

param(
    [string]$OutDir = 'C:\Users\andre\Documents\Claude_Cowork\Trace_Testing_Assets\23_MP4_Review_Corpus',
    [string]$FFmpeg = 'C:\Users\andre\AppData\Local\Microsoft\WinGet\Packages\Gyan.FFmpeg_Microsoft.Winget.Source_8wekyb3d8bbwe\ffmpeg-9.0-full_build\bin\ffmpeg.exe',
    [string]$Filter = ''
)

$ErrorActionPreference = 'Continue'
if (-not (Test-Path $FFmpeg)) { throw "full FFmpeg not found at $FFmpeg" }
if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Force -Path $OutDir | Out-Null }

$fontFile = 'C\:/Windows/Fonts/consolab.ttf'
if (-not (Test-Path 'C:\Windows\Fonts\consolab.ttf')) { $fontFile = 'C\:/Windows/Fonts/arialbd.ttf' }

# THE PLATE SITS IN THE VERTICAL MIDDLE, AND THAT IS A ROBUSTNESS REQUIREMENT
# RATHER THAN A LAYOUT PREFERENCE. It was at the bottom first, which is where a
# burn-in naturally goes -- and Trace's transport is an EDGE-TO-EDGE 56px strip
# across the bottom of the picture, translucent at rest, revealed for as long as
# the pointer is anywhere inside the client. The harness clicks the picture to
# take foreground, which leaves the pointer there, so the strip covered the
# lower half of the plate on every capture after the first.
#
# The reader refused to guess (its aspect guard fired: 0.068 against the design
# 0.117), so nothing was misreported -- but every leg read UNREADABLE, which on
# a less careful reader would have been a wrong frame number instead.
#
# Middle-left is the one band no chrome can reach: the top strip is 38px, the
# transport 56px, and the dev HUD is a widget BELOW the viewer rather than over
# it. Parking the cursor outside the window before each capture would also work
# and the harness does it anyway, but it depends on a 2s auto-hide elapsing,
# which is a timing assumption where this is a geometric fact.
# Same burn-in design as make_mp4_corpus.ps1, at 1080p. Kept as its own copy
# rather than dot-sourced: that script's version is a function of W and H for a
# table of resolutions, and these four clips are all 1080p, so a shared helper
# would be a parameterised abstraction over one call site.
function Get-BurnIn1080 {
    $k = 1.0
    $numSize = 150; $tcSize = 60; $bitW = 90; $bitH = 90
    $bitY = [int](1080 * 0.45); $bitX0 = 60; $gap = 10
    $parts = New-Object System.Collections.Generic.List[string]
    $plateW = $bitX0 * 2 + 10 * $bitW + 9 * $gap
    $plateH = $bitH + 40
    $plateY = $bitY - 20
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

function Run-FF {
    # Progress goes to the HOST, not the pipeline. Callers assign the return
    # value ($ok = Run-FF ...), and a Write-Output inside a function is captured
    # by that assignment along with the bool -- so the first version of this
    # printed nothing at all and every clip looked skipped. The script reported
    # only its trailing "corpus dir" line, which reads exactly like a filter
    # that matched nothing.
    param([string]$Name, [string[]]$FfArgs, [string]$Out)
    Write-Host "building $Name ..."
    $log = Join-Path $env:TEMP "corpust_$Name.log"
    & $FFmpeg @FfArgs 2> $log | Out-Null
    $code = $LASTEXITCODE
    if ($code -ne 0 -or -not (Test-Path $Out)) {
        Write-Warning "FAILED $Name (exit $code) -- see $log"
        if (Test-Path $log) { Get-Content $log -Tail 8 | ForEach-Object { Write-Warning "    $_" } }
        return $false
    }
    $mb = [math]::Round((Get-Item $Out).Length / 1MB, 1)
    Write-Host ("  ok {0,-28} {1,6} MB" -f $Name, $mb)
    return $true
}

$burn = Get-BurnIn1080

# ---------------------------------------------------------------- 1. VFR ------
# Timestamps that are monotonic but unevenly spaced, which is what a phone
# produces. setpts runs BEFORE drawtext, so the burned number is the frame's
# position in the retimed stream.
#
# N + 0.35*sin(N) steps by at least 1 - 0.7 = 0.3, so it is strictly increasing
# and no two frames can share a timestamp. The spacing swings +-35% of a frame
# period, which is far wider than real phone jitter -- deliberately, because a
# mapping that survives this survives the real thing.
#
# -fps_mode passthrough keeps the timestamps rather than resampling them to CFR,
# which is what makes the file actually VFR. Without it ffmpeg would helpfully
# undo the whole point of the clip.
if (-not $Filter -or 'vfr' -like "*$Filter*") {
    $out = Join-Path $OutDir 'h264_1080p_vfr.mp4'
    $vf = "setpts='(N + 0.35*sin(N))/24/TB'," + $burn
    $ok = Run-FF -Name 'h264_1080p_vfr' -Out $out -FfArgs @(
        '-hide_banner','-y','-f','lavfi','-i','testsrc2=size=1920x1080:rate=24:duration=20',
        '-vf',$vf,'-fps_mode','passthrough',
        '-c:v','libx264','-pix_fmt','yuv420p','-g','48','-bf','3','-preset','medium','-crf','20',
        '-movflags','+faststart',$out)
}

# --------------------------------------------- 1b. SUB-FRAME VFR --------------
# THE CASE 1 ABOVE CANNOT TEST, and the reason it cannot is worth knowing.
#
# Case 1 asks setpts for offsets of up to 0.35 of a frame period. The ENCODER's
# time base is 1/24 (it inherits the input rate), so those offsets are rounded to
# whole frames at encode and then scaled to the container's 1/12288 -- landing on
# exact 512-tick multiples with DUPLICATE and SKIPPED slots rather than smooth
# jitter. Measured: 76 of 480 frames share a timestamp with another.
#
# That is a real file shape, and case 1 is kept for it. But it is not sub-frame
# variable rate, and Trace's index mapping rounds to the nearest nominal slot --
# so the case that actually probes the mapping is one whose timestamps sit
# BETWEEN slots, which needs a time base finer than a frame.
#
# The fix is to source at 240fps and keep an irregular subset averaging ten, so
# every surviving timestamp lands on a 1/240 grid -- sub-frame at 24fps, and fine
# enough to survive the encode. select runs BEFORE drawtext, so the burned number
# is again the frame's position in the output stream.
#
# The pattern repeats every 5 output frames with strides 8,10,12,10,10 (mean 10),
# giving offsets of up to ~0.33 of a frame from the nominal 24fps slot -- inside
# the half-frame rounding budget deliberately, so a PASS here means the mapping
# survives realistic phone-class jitter. Widening the strides past +-0.5 of a
# frame is how to make it fail, and vfrmap.py predicts that from the container
# before Trace is involved.
if (-not $Filter -or 'vfrsub' -like "*$Filter*") {
    $out = Join-Path $OutDir 'h264_1080p_vfr_subframe.mp4'
    $sel = "not(mod(n\,48)-0)+not(mod(n\,48)-8)+not(mod(n\,48)-18)+not(mod(n\,48)-30)+not(mod(n\,48)-40)"
    $vf = "select='${sel}'," + $burn
    $ok = Run-FF -Name 'h264_1080p_vfr_subframe' -Out $out -FfArgs @(
        '-hide_banner','-y','-f','lavfi','-i','testsrc2=size=1920x1080:rate=240:duration=20',
        '-vf',$vf,'-fps_mode','passthrough','-video_track_timescale','240',
        '-c:v','libx264','-pix_fmt','yuv420p','-g','48','-bf','3','-preset','medium','-crf','20',
        '-movflags','+faststart',$out)
}

# ------------------------------------------------- 2. non-zero start time -----
# -output_ts_offset shifts every timestamp, so the stream's start_time is 5s
# rather than 0. frameFromPts subtracts streamStartTs, so this is the case that
# says whether that subtraction is actually in force -- without it, frame 0
# would be asked for at t=0, land 5s before the material, and the whole file
# would be offset by 120 frames.
if (-not $Filter -or 'startoffset' -like "*$Filter*") {
    $out = Join-Path $OutDir 'h264_1080p_startoffset.mp4'
    $ok = Run-FF -Name 'h264_1080p_startoffset' -Out $out -FfArgs @(
        '-hide_banner','-y','-f','lavfi','-i','testsrc2=size=1920x1080:rate=24:duration=20',
        '-vf',$burn,
        '-c:v','libx264','-pix_fmt','yuv420p','-g','48','-bf','3','-preset','medium','-crf','20',
        '-output_ts_offset','5.0','-movflags','+faststart',$out)
}

# ------------------------------------------------------------ 3. edit list ----
# A REAL MP4 edit list, which is a different thing from a timestamp offset: the
# container says "start playing 2s into the media", and the demuxer is expected
# to honour it. Built by remuxing with -ss and -copyts so the packets keep their
# original timestamps and an edit is written to describe the trim.
#
# The burned numbers therefore do NOT start at 0 in the trimmed file: the first
# surviving frame is whatever was at 2s in the source. That is the point -- it is
# how the measurement can tell whether Trace is honouring the edit (first
# displayed frame carries ~48) or ignoring it.
if (-not $Filter -or 'editlist' -like "*$Filter*") {
    $src = Join-Path $OutDir 'h264_1080p_24_gop48_b3.mp4'
    if (-not (Test-Path $src)) {
        Write-Warning "editlist needs h264_1080p_24_gop48_b3.mp4 -- run make_mp4_corpus.ps1 first"
    } else {
        $out = Join-Path $OutDir 'h264_1080p_editlist.mp4'
        $ok = Run-FF -Name 'h264_1080p_editlist' -Out $out -FfArgs @(
            '-hide_banner','-y','-ss','2.0','-i',$src,'-c','copy','-copyts',
            '-avoid_negative_ts','disabled','-movflags','+faststart',$out)
    }
}

# -------------------------------------------------------------- 4. rotation ----
# -display_rotation is an INPUT option and writing it as an output option is
# accepted silently while producing a file with no rotation in it -- the
# recorded trap from the shape fixtures. Applied with -c copy so the pixels are
# untouched and only the display matrix differs from the source: a player that
# rotates the picture and a player that ignores the matrix differ in nothing
# else.
if (-not $Filter -or 'rot90' -like "*$Filter*") {
    $src = Join-Path $OutDir 'h264_1080p_24_gop48_b3.mp4'
    if (-not (Test-Path $src)) {
        Write-Warning "rot90 needs h264_1080p_24_gop48_b3.mp4 -- run make_mp4_corpus.ps1 first"
    } else {
        $out = Join-Path $OutDir 'h264_1080p_rot90.mp4'
        $ok = Run-FF -Name 'h264_1080p_rot90' -Out $out -FfArgs @(
            '-hide_banner','-y','-display_rotation','90','-i',$src,'-c','copy',
            '-movflags','+faststart',$out)
    }
}

# ------------------------------------- 5. 59.94 reshoot, inside the strip ------
# 59.94fps for 20s is 1199 frames and the burn-in strip is 10 bits, so frames
# 1024.. wrap to 0 and would read as a catastrophic landing failure that is
# entirely the instrument's. 14s is 839 frames, comfortably inside it.
#
# Recorded rather than silently shortened: the strip's range is a property of the
# harness and any future clip longer than 1023 frames needs more bits, not a
# shorter clip.
if (-not $Filter -or '5994' -like "*$Filter*") {
    $out = Join-Path $OutDir 'h264_1080p_5994_gop48.mp4'
    $ok = Run-FF -Name 'h264_1080p_5994_gop48(14s)' -Out $out -FfArgs @(
        '-hide_banner','-y','-f','lavfi','-i','testsrc2=size=1920x1080:rate=60000/1001:duration=14',
        '-vf',$burn,
        '-c:v','libx264','-pix_fmt','yuv420p','-g','48','-bf','3','-preset','medium','-crf','20',
        '-movflags','+faststart',$out)
}

Write-Output ''
Write-Output "corpus dir: $OutDir"
