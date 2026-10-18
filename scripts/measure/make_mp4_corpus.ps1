# Generate the MP4 review corpus: one synthetic source, many container/codec shapes.
#
# WHY SYNTHETIC RATHER THAN A REAL CLIP. Every file here has to answer "is the
# frame on screen the frame that was asked for", and that question needs a frame
# whose identity is readable FROM THE PIXELS. A real clip cannot answer it: two
# adjacent frames of real footage are nearly identical, so a player one frame off
# looks correct. The source is therefore testsrc2 (deterministic, moving, detailed
# enough to judge scaling) with the frame number burned in two ways:
#
#   HUMAN   a large frame number and a timecode, so a wrong landing is visible
#           by eye on the panel without any harness at all.
#   MACHINE a 10-bit binary strip -- ten boxes, white = 1, MSB left -- so a
#           harness reads the frame number off a screenshot EXACTLY, with no OCR
#           and no tolerance. 10 bits covers 1024 frames; every clip here is
#           under that.
#
# The binary strip is the load-bearing half. OCR on a screenshot is a second
# source of error sitting in front of the measurement, and this project has
# already paid for instruments that accuse a correct build.
#
# THE STRIP IS DRAWN BEFORE ANY SCALING, so it is encoded at the clip's own
# resolution and survives whatever the player does to fit it to a window.
#
# Run with the full FFmpeg (gyan.dev). Trace's own bundled LGPL build has no
# libx264/libx265 encoders, which is the whole reason this is a separate tool.

param(
    # Where the clips land. Deliberately outside the repo: these are hundreds of
    # megabytes of test media, not source.
    [string]$OutDir = 'C:\Users\andre\Documents\Claude_Cowork\Trace_Testing_Assets\23_MP4_Review_Corpus',
    [string]$FFmpeg = 'C:\Users\andre\AppData\Local\Microsoft\WinGet\Packages\Gyan.FFmpeg_Microsoft.Winget.Source_8wekyb3d8bbwe\ffmpeg-9.0-full_build\bin\ffmpeg.exe',
    # Seconds per clip. 20s at 24fps is 480 frames, inside the strip's 1024.
    [int]$Seconds = 20,
    # Build only the clips whose name matches this, for re-running one case.
    [string]$Filter = '',
    [switch]$WhatIf
)

$ErrorActionPreference = 'Continue'

if (-not (Test-Path $FFmpeg)) { throw "full FFmpeg not found at $FFmpeg" }
if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Force -Path $OutDir | Out-Null }

# A font the drawtext filter can actually open. Windows paths need escaping for
# the filter parser: the colon after the drive letter would otherwise be read as
# an option separator.
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
# Build the burn-in filter chain for a given frame size.
function Get-BurnIn {
    param([int]$W, [int]$H)

    # Scale everything off the width so 1080p and 4K carry the same design.
    $k        = $W / 1920.0
    $numSize  = [int](150 * $k)
    $tcSize   = [int](60 * $k)
    $bitW     = [int](90 * $k)
    $bitH     = [int](90 * $k)
    $bitY     = [int]($H * 0.45)
    $bitX0    = [int](60 * $k)
    $gap      = [int](10 * $k)

    $parts = New-Object System.Collections.Generic.List[string]

    # The strip's own background: a mid-grey plate wide enough for ten bits plus
    # margins. The harness finds the plate, then samples ten cell centres inside
    # it -- so cell positions are derived from the plate it measured rather than
    # from a constant that would drift if this design changed.
    $plateW = $bitX0 * 2 + 10 * $bitW + 9 * $gap
    $plateH = $bitH + [int](40 * $k)
    $plateY = $bitY - [int](20 * $k)
    $parts.Add("drawbox=x=0:y=${plateY}:w=${plateW}:h=${plateH}:color=0x808080@1.0:t=fill")

    # Ten bits, LSB..MSB left to right is tempting and wrong: a human reading the
    # strip reads MSB first, so bit 9 sits leftmost and the written number and the
    # drawn number are the same number.
    for ($i = 0; $i -lt 10; $i++) {
        $bitValue = [math]::Pow(2, 9 - $i)
        $x = $bitX0 + $i * ($bitW + $gap)
        # enable is a timeline expression evaluated per frame; n is the frame
        # index. bitand returns the bit's value, non-zero meaning draw.
        $parts.Add("drawbox=x=${x}:y=${bitY}:w=${bitW}:h=${bitH}:color=white@1.0:t=fill:enable='bitand(n\,$bitValue)'")
    }

    # The human half. frame_num prints the decoder's own frame index, and pts
    # prints the presentation time, so a file whose timestamps and frame order
    # disagree says so on its own face.
    $parts.Add("drawtext=fontfile='${fontFile}':text='F %{frame_num}':x=${bitX0}:y=${bitX0}:fontsize=${numSize}:fontcolor=white:box=1:boxcolor=black@0.7:boxborderw=${gap}")
    $parts.Add("drawtext=fontfile='${fontFile}':text='%{pts\:hms}':x=${bitX0}:y=$($bitX0 + $numSize + $gap * 2):fontsize=${tcSize}:fontcolor=yellow:box=1:boxcolor=black@0.7:boxborderw=${gap}")

    return ($parts -join ',')
}

# One source expression, so every clip carries identical content. testsrc2 moves
# and has fine detail; the frame rate is set here and inherited by the encode.
function Get-Source {
    param([int]$W, [int]$H, [string]$Rate)
    return "testsrc2=size=${W}x${H}:rate=${Rate}:duration=${Seconds}"
}

# The matrix. Each row is one hypothesis about what might make Trace behave
# differently, held against a baseline that differs in nothing else.
$cases = @(
    # --- H.264 8-bit, GOP structure swept. 1080p holds everything else equal. ---
    @{ name='h264_1080p_24_allintra';  w=1920; h=1080; rate='24';        enc='libx264'; pix='yuv420p';     extra=@('-g','1','-bf','0','-preset','medium','-crf','20') }
    @{ name='h264_1080p_24_gop12';     w=1920; h=1080; rate='24';        enc='libx264'; pix='yuv420p';     extra=@('-g','12','-bf','0','-preset','medium','-crf','20') }
    @{ name='h264_1080p_24_gop48';     w=1920; h=1080; rate='24';        enc='libx264'; pix='yuv420p';     extra=@('-g','48','-bf','0','-preset','medium','-crf','20') }
    @{ name='h264_1080p_24_gop250';    w=1920; h=1080; rate='24';        enc='libx264'; pix='yuv420p';     extra=@('-g','250','-bf','0','-preset','medium','-crf','20') }
    @{ name='h264_1080p_24_gop48_b3';  w=1920; h=1080; rate='24';        enc='libx264'; pix='yuv420p';     extra=@('-g','48','-bf','3','-preset','medium','-crf','20') }
    # Open GOP: x264 closes GOPs by default, so this needs the flag turned off
    # explicitly. An open GOP means frames after a keyframe may reference frames
    # before it, which is exactly what a seek-to-keyframe-then-walk has to cope
    # with -- so a player that lands correctly on a closed GOP can still be wrong
    # here, and only this row can say so.
    @{ name='h264_1080p_24_opengop';   w=1920; h=1080; rate='24';        enc='libx264'; pix='yuv420p';     extra=@('-g','48','-bf','3','-x264-params','open-gop=1:scenecut=0','-preset','medium','-crf','20') }

    # --- 10-bit and 4:2:2. NVDEC cannot decode 4:2:2 10-bit, so this row is the
    #     software fallback even on a machine with hardware decode available. ---
    @{ name='h264_1080p_24_422_10bit'; w=1920; h=1080; rate='24';        enc='libx264'; pix='yuv422p10le'; extra=@('-g','48','-bf','3','-preset','medium','-crf','20') }

    # --- H.265, the long-GOP worst case the pool's Seedance clip already shows. ---
    @{ name='h265_1080p_24_gop250';    w=1920; h=1080; rate='24';        enc='libx265'; pix='yuv420p';     extra=@('-g','250','-preset','medium','-crf','22','-tag:v','hvc1') }
    @{ name='h265_1080p_24_10bit';     w=1920; h=1080; rate='24';        enc='libx265'; pix='yuv420p10le'; extra=@('-g','250','-preset','medium','-crf','22','-tag:v','hvc1') }

    # --- Resolution, holding GOP 48 + 3 B-frames constant. ---
    @{ name='h264_4k_24_gop48_b3';     w=3840; h=2160; rate='24';        enc='libx264'; pix='yuv420p';     extra=@('-g','48','-bf','3','-preset','fast','-crf','22') }
    @{ name='h265_4k_24_gop250';       w=3840; h=2160; rate='24';        enc='libx265'; pix='yuv420p';     extra=@('-g','250','-preset','fast','-crf','24','-tag:v','hvc1') }

    # --- Frame rate, holding everything else constant. 23.976 is the one that
    #     has no exact double and is where a rounded rate shows up. ---
    @{ name='h264_1080p_23976_gop48';  w=1920; h=1080; rate='24000/1001'; enc='libx264'; pix='yuv420p';    extra=@('-g','48','-bf','3','-preset','medium','-crf','20') }
    @{ name='h264_1080p_2997_gop48';   w=1920; h=1080; rate='30000/1001'; enc='libx264'; pix='yuv420p';    extra=@('-g','48','-bf','3','-preset','medium','-crf','20') }
    @{ name='h264_1080p_5994_gop48';   w=1920; h=1080; rate='60000/1001'; enc='libx264'; pix='yuv420p';    extra=@('-g','48','-bf','3','-preset','medium','-crf','20') }
)

function Invoke-Encode {
    # NOT $Args: that is a PowerShell automatic variable holding the function's
    # own unbound arguments, so a parameter of that name is shadowed and arrives
    # empty. Start-Process then rejects it as null, which reads as an ffmpeg
    # failure rather than as a naming collision.
    param([string]$Name, [string[]]$FfArgs)
    $out = Join-Path $OutDir "$Name.mp4"
    if ($WhatIf) { Write-Output "WOULD BUILD $Name"; return }
    Write-Output "building $Name ..."
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $log = Join-Path $env:TEMP "corpus_$Name.log"

    # THE CALL OPERATOR, NOT Start-Process -ArgumentList. The filter graph
    # contains a space (inside text='F %{frame_num}'), and -ArgumentList
    # rebuilds its array into ONE command string -- so the graph was split at
    # that space and its tail arrived as an output filename, which ffmpeg
    # reported as "Unable to choose an output format". The call operator passes
    # each array element as its own argv entry and quotes it.
    #
    # Waiting and the exit code are both reliable here because ffmpeg is a
    # CONSOLE application. This file's recorded trap -- that PowerShell does not
    # wait for a bare invocation and $LASTEXITCODE is then garbage -- is about
    # Trace.exe, which is linked WIN32; it does not apply to ffmpeg.
    #
    # stderr goes to a file by plain redirection rather than through the
    # pipeline: 5.1 wraps a native command's stderr lines in ErrorRecords and
    # sets $? false even on success, and ffmpeg writes its progress there.
    & $FFmpeg @FfArgs 2> $log | Out-Null
    $code = $LASTEXITCODE
    $sw.Stop()
    if ($code -ne 0 -or -not (Test-Path $out)) {
        Write-Warning "FAILED $Name (exit $code) -- see $log"
        if (Test-Path $log) { Get-Content $log -Tail 6 | ForEach-Object { Write-Warning "    $_" } }
        return
    }
    $mb = [math]::Round((Get-Item $out).Length / 1MB, 1)
    Write-Output ("  ok {0,-30} {1,6} MB  {2,5:N1}s" -f $Name, $mb, $sw.Elapsed.TotalSeconds)
}

foreach ($c in $cases) {
    if ($Filter -and $c.name -notlike "*$Filter*") { continue }
    $burn = Get-BurnIn -W $c.w -H $c.h
    $src  = Get-Source -W $c.w -H $c.h -Rate $c.rate
    $out  = Join-Path $OutDir "$($c.name).mp4"
    # THE FILTER GRAPH GOES THROUGH A FILE, NOT THE COMMAND LINE. It contains a
    # space (inside text='F %{frame_num}'), and Start-Process -ArgumentList
    # rebuilds its array into one command string -- so the graph was being split
    # at that space and its tail arrived as an output filename. ffmpeg then
    # reported "Unable to choose an output format", which reads as a bad encode
    # setting rather than as argument quoting. -filter_script reads the graph
    # verbatim and has no quoting to get wrong.
    $a = @('-hide_banner','-y','-f','lavfi','-i',"$src",'-vf',"$burn",
           '-c:v',$c.enc,'-pix_fmt',$c.pix) + $c.extra + @('-movflags','+faststart',$out)
    Invoke-Encode -Name $c.name -FfArgs $a
}

Write-Output ''
Write-Output "corpus dir: $OutDir"
