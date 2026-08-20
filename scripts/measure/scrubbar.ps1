# The scrub population sweep as a VERDICT: part 3 of the reliability phase.
#
# One command, one verdict, re-runnable after any scrub change:
#
#   scrubbar.ps1                       # sweep the pool now, check every leg against the
#                                      # committed bar, print PASS/FAIL per file and one
#                                      # overall verdict. Exit 0 = PASS, 1 = FAIL.
#   scrubbar.ps1 -Csv <results.csv>    # check an existing scrubsweep CSV instead of
#                                      # sweeping (fast re-verdict, or a control's CSV).
#   scrubbar.ps1 -DisableGopSample     # sweep with TRACE_SCRUB_GOP_SAMPLE=0 -- the
#                                      # NEGATIVE CONTROL. Expected result: exactly the
#                                      # long-GOP demand-over-supply class FAILS (WeLo,
#                                      # Universe, Jeep on leg 2) and everything else
#                                      # passes. A bar that passes this run is broken.
#   scrubbar.ps1 -Generate -Csv a.csv,b.csv
#                                      # regenerate scrub-pass-bar.csv from one or more
#                                      # fix-pass sweep CSVs. The bar is DERIVED, never
#                                      # hand-maintained -- same reason
#                                      # verify_trace_assets.py derives its set from the
#                                      # .qrc. Commit the regenerated file.
#
# What is barred, per (file, leg), and why exactly these:
#   - structural: the (file, leg) row must exist, the selftest must exit 0, the leg must
#     read PASS. A missing row means the population changed -- regenerate the bar -- or a
#     file failed to run; both are failures, on purpose.
#   - delta == 0. Absolute and non-negotiable, hard-coded here rather than stored in the
#     data file: it is the exactness contract, not a tuned expectation. 264 of 264 legs
#     read 0 in both reference sweeps.
#   - behind_end <= behind_end_max and p2p_end <= p2p_end_max. These are the class fix's
#     own headline terms (how far the picture ended behind the hand, and how stale the
#     released-on picture was), measured stable across displays (~2% spread 240Hz vs
#     60Hz on the discriminating files).
# What is deliberately NOT barred: p2p_max and behind_max (the recorded reversal-gesture
# artefact -- p2p max reads ~1.6s on healthy files across five configs on two machines),
# hitch (display- and machine-class-dependent), supply (a ratio of demand the gesture
# chose). A bar on a noisy metric produces flaky failures, which destroys trust in the
# bar faster than having no bar at all.
#
# How the bar is derived (-Generate): per (file, leg), measured = the per-cell MAX across
# every CSV given (generate from fix passes at BOTH 240Hz and 60Hz so the bar holds on
# both displays -- TheraTears leg 1 p2p_end reads ~19ms at 240Hz and ~79ms at 60Hz, the
# one large display divergence in the pool -- and give it every clean fix-config sweep you
# have: more samples is the honest way to capture a file's spread). Then:
#     behind_end_max = measured + max(4 frames, 15% of measured)
#     p2p_end_max    = measured + max(30 ms, 15% of measured, 2 decode intervals)
# where a decode interval is 1000/dec_fps at the cell's WORST (lowest) observed decode
# rate. The 30ms floor absorbs noise near zero (healthy files read 0-14ms, and a bar of 0
# fails on any blip); the decode-interval term exists because p2p_end's granularity IS the
# file's own decode step -- the end state varies by a frame or two of decode, which is
# ~11ms on a 1080p H.264 and ~110ms on the 8K XQ plate, and the first pass-proof run
# proved a flat 30ms floor flakes on the heavy plates (8K leg 4 read 51.5ms against
# reference samples of <=4ms). The 15% is deliberately tighter than the usual doubling
# because the margin must separate the boundary file: Jeep 4K60 leg 2 reads ~145-152ms
# fixed against ~200-210ms on the control, a 1.4x gap, so any generous margin makes the
# negative control pass and the bar meaningless (Jeep decodes at ~180 f/s, so the
# decode-interval term stays under the 30ms floor there and does not widen it).
#
# NOT a CI step: needs the real media pool and a real desktop, like --scrub-selftest.
# The sync pass (TRACE_PRESENT_SYNC=1) is deliberately outside the bar -- the sweep
# recorded its per-file differences as stochastic ~250ms present blocks, not classes.
param(
    [switch]$Generate,
    [string[]]$Csv = @(),
    [switch]$DisableGopSample,
    [string]$Exe = '',
    [string]$Root = 'C:\Users\andre\Documents\Claude_Cowork\Trace_Testing_Assets',
    [string]$OutDir = (Join-Path $env:TEMP 'trace-scrubsweep'),
    [string]$PassName = ''
)
$ErrorActionPreference = 'Stop'
$barPath = Join-Path $PSScriptRoot 'scrub-pass-bar.csv'
$sweep = Join-Path $PSScriptRoot 'scrubsweep.ps1'

function Num([string]$s) {
    if ($null -eq $s -or $s -eq '') { return $null }
    return [double]$s
}

if ($Generate) {
    if ($Csv.Count -lt 1) { throw "-Generate needs at least one scrubsweep results CSV (-Csv a.csv,b.csv). Generate from a 240Hz fix pass and a 60Hz fix pass." }
    $cells = @{}
    $sources = @()
    foreach ($c in $Csv) {
        if (-not (Test-Path $c)) { throw "CSV not found: $c" }
        $rows = Import-Csv $c
        $sources += [IO.Path]::GetFileName($c)
        foreach ($r in $rows) {
            if ([int]$r.leg -lt 1) { throw "row with leg $($r.leg) for $($r.file) in $c -- a reference sweep must be clean before a bar is generated from it" }
            if ($r.result -ne 'PASS' -or $r.delta -ne '0') { throw "reference leg not structurally clean ($($r.file) leg $($r.leg): result=$($r.result) delta=$($r.delta)) in $c -- fix the sweep before deriving a bar from it" }
            $key = "$($r.file)|$($r.leg)"
            $be = Num $r.behind_end; $pe = Num $r.p2p_end; $df = Num $r.dec_fps
            if ($null -eq $be -or $null -eq $pe) { throw "missing behind_end/p2p_end for $key in $c" }
            if ($null -eq $df -or $df -le 0) { throw "missing dec_fps for $key in $c -- the p2p floor is scaled by the decode interval" }
            if (-not $cells.ContainsKey($key)) {
                $cells[$key] = [pscustomobject]@{ file=$r.file; leg=[int]$r.leg; behind_end=$be; p2p_end=$pe; dec_fps=$df }
            } else {
                if ($be -gt $cells[$key].behind_end) { $cells[$key].behind_end = $be }
                if ($pe -gt $cells[$key].p2p_end)    { $cells[$key].p2p_end   = $pe }
                if ($df -lt $cells[$key].dec_fps)    { $cells[$key].dec_fps   = $df }
            }
        }
    }
    $out = $cells.Values | Sort-Object file, leg | ForEach-Object {
        $beBar = [int]($_.behind_end + [math]::Max(4, [math]::Ceiling(0.15 * $_.behind_end)))
        $decInterval2 = 2000.0 / $_.dec_fps
        $peBar = [int][math]::Ceiling($_.p2p_end + [math]::Max([math]::Max(30, 0.15 * $_.p2p_end), $decInterval2))
        [pscustomobject]@{
            file           = $_.file
            leg            = $_.leg
            behind_end_max = $beBar
            p2p_end_max    = $peBar
            src_behind_end = $_.behind_end
            src_p2p_end    = $_.p2p_end
            src_dec_fps    = $_.dec_fps
            sources        = ($sources -join ';')
            generated      = (Get-Date -Format 'yyyy-MM-dd')
        }
    }
    $out | Export-Csv $barPath -NoTypeInformation
    Write-Host "wrote $barPath ($($out.Count) rows from $($sources -join ' + '))"
    exit 0
}

# ---- check mode ----
if (-not (Test-Path $barPath)) { throw "no pass bar at $barPath -- run -Generate first" }
$bar = Import-Csv $barPath
if ($bar.Count -lt 1) { throw "empty pass bar at $barPath" }

$resultCsv = ''
if ($Csv.Count -gt 1) { throw "check mode takes one -Csv (or none, to sweep now)" }
if ($Csv.Count -eq 1) {
    $resultCsv = $Csv[0]
    if (-not (Test-Path $resultCsv)) { throw "CSV not found: $resultCsv" }
} else {
    if (-not $PassName) {
        $PassName = 'barcheck'
        if ($DisableGopSample) { $PassName = 'barcheck-ctrl' }
    }
    $sweepArgs = @{ Pass = $PassName; OutDir = $OutDir; Root = $Root }
    if ($Exe) { $sweepArgs.Exe = $Exe }
    if ($DisableGopSample) { $sweepArgs.DisableGopSample = $true }
    Write-Host "sweeping the pool (pass '$PassName') -- ~20 min ..."
    & $sweep @sweepArgs
    $resultCsv = Join-Path $OutDir "results-$PassName.csv"
    if (-not (Test-Path $resultCsv)) { throw "sweep produced no CSV at $resultCsv" }
}

$rows = Import-Csv $resultCsv
$byKey = @{}
foreach ($r in $rows) { $byKey["$($r.file)|$($r.leg)"] = $r }

# Two-way population check: every barred (file,leg) must be in the results, and every
# result leg must be in the bar. A file added to scrubsweep.ps1's pool turns this red
# until the bar is regenerated -- deriving, not duplicating.
$fails = New-Object System.Collections.Generic.List[string]
$perFile = @{}
foreach ($b in $bar) {
    $key = "$($b.file)|$($b.leg)"
    if (-not $perFile.ContainsKey($b.file)) { $perFile[$b.file] = New-Object System.Collections.Generic.List[string] }
    if (-not $byKey.ContainsKey($key)) {
        $perFile[$b.file].Add("leg $($b.leg): MISSING from results (file failed to run, or population changed -- regenerate the bar)")
        continue
    }
    $r = $byKey[$key]
    $legFails = New-Object System.Collections.Generic.List[string]
    if ($r.exit -ne '0')       { $legFails.Add("exit $($r.exit)") }
    if ($r.result -ne 'PASS')  { $legFails.Add("leg result $($r.result)") }
    if ($r.delta -ne '0')      { $legFails.Add("delta $($r.delta) (exactness contract)") }
    $be = Num $r.behind_end; $pe = Num $r.p2p_end
    if ($null -eq $be) { $legFails.Add('behind_end unparsed') }
    elseif ($be -gt [double]$b.behind_end_max) { $legFails.Add("behind_end $be > $($b.behind_end_max)f") }
    if ($null -eq $pe) { $legFails.Add('p2p_end unparsed') }
    elseif ($pe -gt [double]$b.p2p_end_max) { $legFails.Add("p2p_end ${pe} > $($b.p2p_end_max)ms") }
    if ($legFails.Count -gt 0) { $perFile[$b.file].Add("leg $($b.leg): " + ($legFails -join ', ')) }
}
foreach ($r in $rows) {
    $key = "$($r.file)|$($r.leg)"
    $inBar = $false
    foreach ($b in $bar) { if ("$($b.file)|$($b.leg)" -eq $key) { $inBar = $true; break } }
    if (-not $inBar) {
        if (-not $perFile.ContainsKey($r.file)) { $perFile[$r.file] = New-Object System.Collections.Generic.List[string] }
        $perFile[$r.file].Add("leg $($r.leg): NOT IN BAR (population changed -- regenerate the bar)")
    }
}

$failedFiles = 0
$barFiles = $bar | Select-Object -ExpandProperty file -Unique
foreach ($f in ($perFile.Keys | Sort-Object)) {
    if ($perFile[$f].Count -eq 0) {
        Write-Host ("PASS  {0}" -f $f)
    } else {
        $failedFiles++
        Write-Host ("FAIL  {0}" -f $f)
        foreach ($m in $perFile[$f]) { Write-Host ("        {0}" -f $m) }
        $fails.Add($f)
    }
}
Write-Host ""
if ($failedFiles -eq 0) {
    Write-Host ("SCRUB BAR: PASS -- {0} files, {1} legs, delta 0 throughout ({2})" -f $barFiles.Count, $bar.Count, [IO.Path]::GetFileName($resultCsv))
    exit 0
} else {
    Write-Host ("SCRUB BAR: FAIL -- {0} of {1} files below the bar ({2})" -f $failedFiles, $barFiles.Count, [IO.Path]::GetFileName($resultCsv))
    exit 1
}
