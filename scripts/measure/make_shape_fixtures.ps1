# Make the media shapes the asset set does not contain -- spec phase 12.
#
# Section 4 requires the window to adopt the media's DISPLAY aspect ratio, and
# says outright: "do not assume display ratio is always encoded width divided by
# encoded height". Its validation list then names anamorphic non-square-pixel
# media and rotation metadata explicitly.
#
# EVERY FILE IN THE ASSET SET IS SQUARE-PIXEL AND UNROTATED. So shipping the
# ratio work against it would be section 29.2 again, and phase 7's drop-frame
# problem exactly: the code would compile, every existing file would look right,
# and the two branches that make the requirement a requirement would never have
# executed. Phase 7 made the fixture; so does this.
#
# THE FIXTURE IS PART OF THE MEASUREMENT, and these are chosen so that a build
# which IGNORES sample aspect and rotation produces a VISIBLY DIFFERENT answer:
#
#   anamorphic-4x3-to-16x9   1440x1080 encoded, SAR 4:3  -> DAR 16:9  (1.7778)
#                            ignoring SAR gives 1.3333 -- a different SHAPE, not
#                            a rounding difference, and the window would be
#                            visibly too tall.
#   anamorphic-2x1-to-2.39   1920x816 encoded, SAR 1.2:1 -> DAR 2.8235
#                            a widening SAR on already-wide material, so a build
#                            that quietly swaps num and den lands at 1.9608
#                            rather than agreeing to within a percent.
#   rotated-90               1920x1080 encoded, rotation 90 -> DAR 0.5625
#                            the reciprocal, which is the one case where getting
#                            it wrong cannot look like a small error.
#   rotated-180              1920x1080 encoded, rotation 180 -> DAR 1.7778
#                            THE CONTROL, and the one that matters most: 180
#                            leaves the ratio alone, so a build that transposes
#                            on ANY nonzero rotation is caught here and nowhere
#                            else. Without it, "rotation is handled" would be
#                            provable by a build that just checked for != 0.
#
# The picture in each is a frame-numbered pattern with an OFF-CENTRE marker, so
# a rotation can be read off the image rather than only off the metadata -- the
# same reason phase 10 verified rotation by the 4x5 slate's black bar and not by
# the arithmetic.
#
# Output goes beside the other fixtures under the testing assets tree by
# default. It never writes to V:\ and nothing here reads it.

param(
    [string]$FFmpeg = "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\Gyan.FFmpeg_Microsoft.Winget.Source_8wekyb3d8bbwe\ffmpeg-9.0-full_build\bin\ffmpeg.exe",
    [string]$FFprobe = "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\Gyan.FFmpeg_Microsoft.Winget.Source_8wekyb3d8bbwe\ffmpeg-9.0-full_build\bin\ffprobe.exe",
    [string]$OutDir = "C:\Users\andre\Documents\Claude_Cowork\Trace_Testing_Assets\11_Shape_Fixtures",
    [int]$Seconds = 6
)

if (-not (Test-Path $FFmpeg))  { Write-Output "FIXTURES: no ffmpeg at $FFmpeg - pass -FFmpeg"; exit 1 }
if (-not (Test-Path $FFprobe)) { Write-Output "FIXTURES: no ffprobe at $FFprobe - pass -FFprobe"; exit 1 }
New-Item -ItemType Directory -Force $OutDir | Out-Null

# A source pattern with a readable orientation. testsrc2 already carries moving
# elements and a frame counter; the drawbox puts a solid marker in ONE corner so
# "which way up is this" is answerable from a screenshot.
function Pattern([int]$w, [int]$h) {
    return "testsrc2=size=${w}x${h}:rate=24,drawbox=x=0:y=0:w=iw/6:h=ih/6:color=red@1.0:t=fill,drawbox=x=iw-iw/12:y=ih-ih/12:w=iw/12:h=ih/12:color=yellow@1.0:t=fill"
}

$made = @()

function Make-Anamorphic([string]$name, [int]$w, [int]$h, [int]$sarN, [int]$sarD) {
    $out = Join-Path $OutDir "$name.mp4"
    # -vf setsar writes the SAR into the stream; the mp4 muxer emits it as a
    # `pasp` atom, which is what av_guess_sample_aspect_ratio then reads.
    & $FFmpeg -y -hide_banner -loglevel error `
        -f lavfi -i (Pattern $w $h) -t $Seconds `
        -vf "setsar=$sarN/$sarD" `
        -c:v libx264 -preset veryfast -crf 20 -pix_fmt yuv420p $out 2>&1 | Out-Null
    $script:made += $out
}

function Make-Rotated([string]$name, [int]$w, [int]$h, [int]$deg) {
    $out = Join-Path $OutDir "$name.mp4"
    $tmp = Join-Path $OutDir "$name.tmp.mp4"
    & $FFmpeg -y -hide_banner -loglevel error `
        -f lavfi -i (Pattern $w $h) -t $Seconds `
        -c:v libx264 -preset veryfast -crf 20 -pix_fmt yuv420p $tmp 2>&1 | Out-Null
    # TWO PASSES, AND THE SECOND IS -c copy, BECAUSE -display_rotation IS AN
    # INPUT OPTION. Written as an output option it is silently accepted and the
    # file is never produced -- which is how the first version of this script
    # failed: the anamorphic pair appeared, the rotated pair did not, and the
    # only symptom was ffprobe reporting "No such file or directory" at the end.
    #
    # A stream copy is the right second pass anyway: it writes the display matrix
    # WITHOUT re-encoding, so the fixture is a file whose pixels are one way up
    # and whose container asks for another, which is exactly the case under test.
    # Re-encoding with an autorotate filter would bake the rotation into the
    # pixels and test nothing.
    & $FFmpeg -y -hide_banner -loglevel error `
        -display_rotation $deg -i $tmp -c copy $out 2>&1 | Out-Null
    Remove-Item $tmp -ErrorAction SilentlyContinue
    $script:made += $out
}

Make-Anamorphic "anamorphic-4x3-to-16x9"  1440 1080 4 3
Make-Anamorphic "anamorphic-2x1-to-239"   1920  816 6 5
# ffmpeg's -display_rotation takes the ANTICLOCKWISE angle, the same convention
# av_display_rotation_get reports in. 90 anticlockwise here is what Trace must
# read back as 270 clockwise; the point of the fixture is that SOME nonzero
# rotation is present and the ratio is transposed, not which sign convention
# either tool prefers -- so the assertion below reads the ratio, not the angle.
Make-Rotated "rotated-90"  1920 1080 90
Make-Rotated "rotated-180" 1920 1080 180

Write-Output "FIXTURES in $OutDir"
Write-Output ""
Write-Output "ffprobe says (this is the INDEPENDENT check -- Trace must agree with it):"
foreach ($f in $made) {
    $j = & $FFprobe -v error -select_streams v:0 `
        -show_entries "stream=width,height,sample_aspect_ratio,display_aspect_ratio" `
        -show_entries "stream_side_data=rotation" `
        -of default=noprint_wrappers=1 $f
    Write-Output ("  " + (Split-Path -Leaf $f))
    foreach ($line in $j) { Write-Output ("      " + $line) }
}
Write-Output ""
Write-Output "Expected display aspect, for Trace's `dar` field on the HUD's media line:"
Write-Output "  anamorphic-4x3-to-16x9  1.7778   (1440*4/3 / 1080)  -- 1.3333 means SAR was ignored"
Write-Output "  anamorphic-2x1-to-239   2.8235   (1920*6/5 /  816)  -- 1.9608 means num/den were swapped"
Write-Output "  rotated-90              0.5625   (the reciprocal of 1.7778)"
Write-Output "  rotated-180             1.7778   THE CONTROL: unchanged, and the only fixture that"
Write-Output "                                   fails a build which transposes on any nonzero rotation"
