# Static characterisation of every MP4/MOV in the asset pool: codec, shape,
# fps, frame count, audio, and GOP structure (keyframe count plus the gap
# distribution, from a decode-order packet scan -- no decoding).
#
# Built for the scrub-reliability phase part 1 (2026-08-20,
# docs/scrub-population-sweep.md): GOP structure is the file-side half of that
# table, and ffprobe is the independent witness for it.
#
# TRAP, paid for on the first run: PowerShell's -match is CASE-INSENSITIVE, so
# matching the keyframe flag with -match 'K' matches the lowercase k in the
# word "packet" on EVERY line and reports every frame as a keyframe -- which on
# this pool is indistinguishable from ProRes being ProRes. The match below is
# -cmatch against the flags field specifically.
param(
    [string]$Root = 'C:\Users\andre\Documents\Claude_Cowork\Trace_Testing_Assets',
    [string]$Ffprobe = 'C:\Users\andre\AppData\Local\Microsoft\WinGet\Packages\Gyan.FFmpeg_Microsoft.Winget.Source_8wekyb3d8bbwe\ffmpeg-9.0-full_build\bin\ffprobe.exe',
    [string]$OutCsv = ''
)
# NOTE this ffprobe is the winget GPL/GCC build -- fine here, because packet
# flags and stream metadata are container facts, not decoder behaviour. Do not
# use it for decode-speed claims (the recorded 8K "20.5 fps" mistake).

$files = Get-ChildItem $Root -Recurse -Include *.mp4,*.mov -File | Sort-Object FullName
$rows = foreach ($f in $files) {
    $vj = & $Ffprobe -v error -select_streams v:0 -show_entries stream=codec_name,profile,width,height,pix_fmt,r_frame_rate,nb_frames,duration -of json $f.FullName | ConvertFrom-Json
    $v = $vj.streams[0]
    $aj = & $Ffprobe -v error -select_streams a -show_entries stream=codec_name -of json $f.FullName | ConvertFrom-Json
    $audio = if ($aj.streams.Count -gt 0) { $aj.streams[0].codec_name } else { 'none' }

    $flags = & $Ffprobe -v error -select_streams v:0 -show_entries packet=flags -of csv $f.FullName
    $n = 0; $kf = @()
    foreach ($line in $flags) {
        if ($line -match '^packet') {
            if ($line -cmatch ',K') { $kf += $n }
            $n++
        }
    }
    $gaps = @()
    for ($i = 1; $i -lt $kf.Count; $i++) { $gaps += ($kf[$i] - $kf[$i-1]) }
    if ($kf.Count -gt 0) { $gaps += ($n - $kf[-1]) }  # tail gap
    $gapStr = if ($kf.Count -eq $n) { 'all-intra' }
              elseif ($gaps.Count -eq 0) { 'no-keyframes?' }
              else {
                $g = $gaps | Sort-Object
                $u = $gaps | Group-Object | Sort-Object {[int]$_.Name} | ForEach-Object { "$($_.Name)x$($_.Count)" }
                if ($u.Count -le 6) { $u -join ' ' } else { "min $($g[0]) med $($g[[int]($g.Count/2)]) max $($g[-1])" }
              }
    [pscustomobject]@{
        file      = $f.FullName.Substring($Root.Length + 1)
        codec     = $v.codec_name
        profile   = $v.profile
        pix_fmt   = $v.pix_fmt
        res       = "$($v.width)x$($v.height)"
        fps       = $v.r_frame_rate
        frames    = $v.nb_frames
        dur_s     = if ($v.duration) { [math]::Round([double]$v.duration, 2) } else { '' }
        pkts      = $n
        keyframes = $kf.Count
        gops      = $gapStr
        audio     = $audio
        MB        = [math]::Round($f.Length/1MB,1)
    }
}
if ($OutCsv) { $rows | Export-Csv $OutCsv -NoTypeInformation }
$rows | Format-Table -AutoSize | Out-String -Width 300
