# The scrub-reliability phase's population sweep: --scrub-selftest over every
# H.264 MP4 and every ProRes in the pool (plus the Seedance HEVC as a reference
# row), one pass per invocation, parsed into one CSV row per leg.
#
#   scrubsweep.ps1 -Pass 240hz
#   scripts\measure\setrefresh.ps1 -Hz 60
#   scrubsweep.ps1 -Pass 60hz
#   scrubsweep.ps1 -Pass 60hz-sync -PresentSync
#   scripts\measure\setrefresh.ps1 -Hz 240        # ALWAYS restore explicitly
#
# Method facts the results depend on (record in docs/scrub-population-sweep.md):
#  - Runs PLAIN: HUD hidden (the shipping transport, ~2.5ms/request cheaper
#    delivery than the HUD-on harness config) and a scratch TRACE_SETTINGS_FILE
#    deleted per run, so no persisted preference is an input.
#  - The cursor is parked at (10,10) first: a pointer inside the client holds
#    the chrome revealed indefinitely, which changes the paint load.
#  - All other TRACE_* variables are REMOVED from the environment first; each
#    report's own env line is the check.
#  - Start-Process + cached .Handle + WaitForExit, never bare invocation or the
#    capture form: Trace is a GUI-subsystem binary and this is the only launch
#    shape whose exit code is trustworthy in both PowerShell editions. Without
#    touching $p.Handle before exit, $p.ExitCode reads EMPTY under 5.1.
#  - The -PresentSync pass models the blocking-present machine class
#    (TRACE_PRESENT_SYNC=1). Run it AT 60Hz -- that is the recorded fault model
#    of the Threadripper class, and on this box it is far harsher than the real
#    machine (paints block ~250ms here vs ~16.7ms there; recorded 2026-08-19).
param(
    [Parameter(Mandatory)][string]$Pass,
    [switch]$PresentSync,
    # TRACE_SCRUB_GOP_SAMPLE=0: long-GOP never samples -- the pre-2026-08-20
    # behaviour and the in-binary control for the keyframe-landing change.
    [switch]$DisableGopSample,
    [string]$Exe = '',
    [string]$Root = 'C:\Users\andre\Documents\Claude_Cowork\Trace_Testing_Assets',
    [string]$OutDir = (Join-Path $env:TEMP 'trace-scrubsweep')
)
$ErrorActionPreference = 'Stop'
# $PSScriptRoot is empty when a param default is evaluated under -File, so the
# repo-relative default is resolved here in the body instead.
if (-not $Exe) {
    $Exe = Join-Path (Split-Path (Split-Path $PSScriptRoot -Parent) -Parent) 'build\app\Release\Trace.exe'
}
$repDir = Join-Path $OutDir "reports\$Pass"
New-Item -ItemType Directory -Force $repDir | Out-Null
$csv = Join-Path $OutDir "results-$Pass.csv"
if (Test-Path $csv) { Remove-Item $csv -Force -Confirm:$false }

Add-Type -AssemblyName System.Windows.Forms
[System.Windows.Forms.Cursor]::Position = New-Object System.Drawing.Point(10, 10)

Get-ChildItem env: | Where-Object { $_.Name -like 'TRACE_*' } | ForEach-Object {
    Remove-Item "env:$($_.Name)" -Confirm:$false
}
$env:TRACE_SETTINGS_FILE = Join-Path $OutDir 'scratch-settings.ini'
if ($PresentSync) { $env:TRACE_PRESENT_SYNC = '1' }
if ($DisableGopSample) { $env:TRACE_SCRUB_GOP_SAMPLE = '0' }

$files = @(
    '3_1080p_H264_MP4\Universe_rc07_I_9x16_Online.mp4'
    '3_1080p_H264_MP4\M&M_TopGun_1080.mp4'
    '3_1080p_H264_MP4\Marinelaverse_16x9_EndTag_v007.mp4'
    '4_4K_H264_MP4\Splash_1.mp4'
    '7_4k_H264_60FPS_MP4\Jeep_Snowboard_Drone_60FPS.mp4'
    '14_720P_Comfyui_mp4\video_ComfyUI_0000Fly8.mp4'
    '17_Random_Mp4s\_model_veo31_202512171427_8auub.mp4'
    '17_Random_Mp4s\A_slow_controlled_202512171507_fbb81.mp4'
    '17_Random_Mp4s\Best_02.mp4'
    '17_Random_Mp4s\Dram_Theater_8_endcard.mp4'
    '17_Random_Mp4s\WeLo-AI-rc13-1x1_SOCIAL.mp4'
    '11_Shape_Fixtures\anamorphic-2x1-to-239.mp4'
    '11_Shape_Fixtures\anamorphic-4x3-to-16x9.mp4'
    '11_Shape_Fixtures\rotated-90.mp4'
    '11_Shape_Fixtures\rotated-180.mp4'
    '1_4K_ProRes_4444\TheraTears_Vial_VFX_v002.mov'
    '2_4K_ProRes_422HQ\Barritas_16x9_Shot_040-080_v005_ALT_1.mov'
    '9_1x1_ProRes\FY27_CX_BEER_NA_Summer_Bottomless-Cooler-Lago_PLC_PLC_OM_SP_BAU_1x1_15_product-locator_CBCX0000442H.mov'
    '10_4x5_ProRes\FY27_CX_BEER_NA_Summer_Bottomless-Cooler-Pool_PLC_PLC_OM_EN_BAU_4x5_15_product-locator_CBCX0000436H.mov'
    '12_8K_ProRes4444\Foces_8K_Lut_Dino Stomp_plate_4444XQ.mov'
    '13_4448x3096_ProRes_4444\V1-0004_A001C015_120101_ROES.mov'
    '16_4kSeedance_9x16_Comfyui_MP4\video_ComfyUI_00004_.mp4'
)

function Get-Num([string]$text, [string]$rx) {
    if ($text -match $rx) { return $Matches[1] } else { return '' }
}

$rows = New-Object System.Collections.Generic.List[object]
$i = 0
foreach ($rel in $files) {
    $i++
    $full = Join-Path $Root $rel
    $name = [IO.Path]::GetFileNameWithoutExtension($rel) -replace '[^\w\-]', '_'
    $outFile = Join-Path $repDir ("{0:d2}_{1}.txt" -f $i, $name)
    $errFile = Join-Path $repDir ("{0:d2}_{1}.err.txt" -f $i, $name)
    if (Test-Path $env:TRACE_SETTINGS_FILE) { Remove-Item $env:TRACE_SETTINGS_FILE -Force -Confirm:$false }

    Write-Host ("[{0}/{1}] {2}" -f $i, $files.Count, $rel)
    $p = Start-Process -FilePath $Exe -ArgumentList "`"--scrub-selftest=$full`"" `
        -RedirectStandardOutput $outFile -RedirectStandardError $errFile -PassThru
    $null = $p.Handle   # cache the handle or ExitCode reads empty under PS 5.1
    if (-not $p.WaitForExit(300000)) {
        try { $p.Kill() } catch {}
        Write-Host "  TIMEOUT after 300s -- killed"
        $rows.Add([pscustomobject]@{ pass=$Pass; file=$rel; leg=0; exit='TIMEOUT' })
        continue
    }
    $code = $p.ExitCode
    $text = Get-Content $outFile -Raw

    $legBlocks = [regex]::Split($text, '(?=-- leg \d+:)') | Where-Object { $_ -match '^-- leg \d+:' }
    foreach ($blk in $legBlocks) {
        $leg = Get-Num $blk '-- leg (\d+):'
        $rows.Add([pscustomobject]@{
            pass        = $Pass
            file        = $rel
            leg         = $leg
            exit        = $code
            result      = $(if ($blk -match 'leg (PASS|FAIL)') { $Matches[1] } else { '?' })
            win         = Get-Num $blk 'win (\d+x\d+)'
            display     = Get-Num $blk 'display (\d+x\d+)'
            thr         = Get-Num $blk 'thr (\w+ x\d+)'
            pointer_fps = Get-Num $blk 'pointer ([\d.]+) f/s'
            presented   = Get-Num $blk 'presented (\d+) f'
            dec_fps     = Get-Num $blk 'dec ([\d.]+) f/s'
            supply      = Get-Num $blk 'supply ([\d.]+)%'
            behind_end  = Get-Num $blk 'behind (-?\d+)/'
            behind_max  = Get-Num $blk 'behind -?\d+/(-?\d+)f'
            p2p_end     = Get-Num $blk 'p2p ([\d.]+)/'
            p2p_max     = Get-Num $blk 'p2p [\d.]+/([\d.]+)ms'
            rt_avg      = Get-Num $blk 'round trip/request ([\d.]+)/'
            rt_max      = Get-Num $blk 'round trip/request [\d.]+/([\d.]+)ms'
            deliver_avg = Get-Num $blk 'deliver ([\d.]+)/'
            ovh_avg     = Get-Num $blk 'overhead ([\d.]+)/'
            req         = Get-Num $blk '\(req (\d+)\)'
            dec_avg     = Get-Num $blk 'decode/frame ([\d.]+)/'
            dec_max     = Get-Num $blk 'decode/frame [\d.]+/([\d.]+)ms'
            dec_n       = Get-Num $blk 'avg/max, n (\d+)'
            ovh_share   = Get-Num $blk 'overhead share ([\d.]+)%'
            batch_avg   = Get-Num $blk 'achieved-avg ([\d.]+)'
            batch_max   = Get-Num $blk 'achieved-avg [\d.]+ last \d+ max (\d+)'
            budget_cut  = Get-Num $blk 'budget-cut (\d+) of'
            stride      = Get-Num $blk 'stride (\d+)'
            kf_land     = Get-Num $blk 'kf-land (\d+)'
            seeks       = Get-Num $blk 'seeks (\d+)'
            ra_walk     = Get-Num $blk 'ra-walk ([\d.]+)f'
            walk_max    = Get-Num $blk 'walk max (\d+)f'
            rev_hit     = Get-Num $blk 'rev-hit ([\d.]+)%'
            cache       = Get-Num $blk 'cache (\d+/\d+)'
            cache_mb    = Get-Num $blk 'cache \d+/\d+ \(([\d.]+)/'
            hitch       = Get-Num $blk 'hitch (\d+)'
            stalls      = Get-Num $blk 'stalls (\d+) of'
            stall_n     = Get-Num $blk 'stalls \d+ of (\d+)'
            pg_avg      = Get-Num $blk 'paint gap ([\d.]+)/'
            pg_max      = Get-Num $blk 'paint gap [\d.]+/([\d.]+)ms'
            painted     = Get-Num $blk 'painted (\d+)'
            gated       = Get-Num $blk 'gated (\d+)'
            paint_cost  = Get-Num $blk 'paint cost ([\d.]+)ms'
            gate        = Get-Num $blk 'gate ([\d.]+)ms'
            uigap_avg   = Get-Num $blk 'ui gap ([\d.]+)/'
            uigap_max   = Get-Num $blk 'ui gap [\d.]+/([\d.]+)ms'
            ui_over     = Get-Num $blk 'ms: (\d+) of'
            ui_n        = Get-Num $blk 'ms: \d+ of (\d+)'
            release     = Get-Num $blk 'release ([\d.]+)ms'
            land_sup    = Get-Num $blk 'sup (\d+)'
            delta       = Get-Num $blk 'delta (-?\d+)'
        })
    }
    if ($legBlocks.Count -eq 0) {
        Write-Host "  NO LEGS PARSED (exit $code) -- see $outFile"
        $rows.Add([pscustomobject]@{ pass=$Pass; file=$rel; leg=0; exit=$code })
    }
}
$rows | Export-Csv $csv -NoTypeInformation
Write-Host "wrote $csv ($($rows.Count) rows)"
