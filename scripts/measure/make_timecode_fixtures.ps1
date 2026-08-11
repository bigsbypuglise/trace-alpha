# Generate the timecode fixtures the test asset set does not contain.
#
# WHY THIS EXISTS. Spec phase 7 added drop-frame timecode arithmetic, and
# `Trace_Testing_Assets` has no drop-frame material at all -- the whole set is
# 24, 23.976 and 60fps, and the three files that carry a timecode carry
# 00:00:01:12, 00:00:00:00 and 00:00:00:00, all non-drop. Shipping arithmetic
# that has never executed is exactly the failure section 29.2 records at length,
# so the fixtures are made rather than the case being skipped.
#
# TWO PROPERTIES MAKE THESE FIXTURES WORK, and both were wrong on the first try:
#
#   The start must not sit just before a TEN-MINUTE boundary. Drop-frame skips
#   nothing at minutes 0, 10, 20..., so a clip starting at 00:59:50 and running
#   past the hour prints IDENTICAL DIGITS in both conventions -- a fixture that
#   cannot tell them apart and passes either way. Starting at 00:00:50 puts
#   minute 1 inside the clip, which is a dropping minute.
#
#   The drop and non-drop clips must be otherwise identical, so the only thing
#   that can explain a difference is the convention. Same source, same rate,
#   same duration, same codec; only the -timecode separator differs.
#
# WHAT TO READ. Select the SMPTE readout (T) and go to frame 300 (Ctrl+G):
#
#   drop-frame      00:00:50;00  ->  00:01:00;02
#   non-drop        00:00:50:00  ->  00:01:00:00
#
# The `;02` is the two frame numbers minute 1 skips. If both read `:00`, the
# drop-frame path is not running.

param(
    [string]$OutDir = "$env:TEMP\trace_timecode_fixtures",
    # Where ffmpeg is. Not on PATH on this box; the same binary abfilter.ps1
    # builds its references with.
    [string]$FFmpeg = "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\Gyan.FFmpeg_Microsoft.Winget.Source_8wekyb3d8bbwe\ffmpeg-9.0-full_build\bin\ffmpeg.exe",
    [double]$Seconds = 25
)

if (-not (Test-Path $FFmpeg)) {
    Write-Output "FIXTURES: no ffmpeg at $FFmpeg - pass -FFmpeg"
    exit 1
}
New-Item -ItemType Directory -Force $OutDir | Out-Null

# ProRes rather than H.264: intra-only, so a Go to Timecode lands with one seek
# and no GOP walk, and a wrong landing cannot be blamed on the decoder.
$common = @("-y", "-v", "error", "-f", "lavfi",
            "-i", "testsrc=size=640x360:rate=30000/1001:duration=$Seconds",
            "-c:v", "prores_ks", "-profile:v", "0")

& $FFmpeg @common -timecode "00:00:50;00" (Join-Path $OutDir "df_2997.mov")
& $FFmpeg @common -timecode "00:00:50:00" (Join-Path $OutDir "ndf_2997.mov")

foreach ($f in @("df_2997.mov", "ndf_2997.mov")) {
    $p = Join-Path $OutDir $f
    if (Test-Path $p) { Write-Output ("FIXTURES: {0}  {1:N1} MB" -f $p, ((Get-Item $p).Length / 1MB)) }
    else { Write-Output "FIXTURES: FAILED to write $f"; exit 1 }
}
Write-Output "FIXTURES: open each with TRACE_OPEN_LOG=1 and check the timecode= column,"
Write-Output "          then press T and Ctrl+G 300 - expect 00:01:00;02 and 00:01:00:00."
