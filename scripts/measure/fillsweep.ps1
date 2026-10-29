# Sweep TRACE_SCRUB_FILL_MS and say what the seek-walk cache fill budget buys.
#
# WHY THIS EXISTS. `scrubWalkCacheBudgetMs` shipped at 60 while its own comment
# argued for 240 off a sweep table, because a member initialiser of 240.0 was
# overwritten unconditionally at every open by `envInt("TRACE_SCRUB_FILL_MS",
# 60)`. CLAUDE.md carried it as a documentation-versus-code discrepancy and the
# MP4 review carried it as F7. Making the two agree is a one-line change; which
# value they should agree ON is a measurement, and the recorded one predates the
# async scrub batch (2026-08-14) that changed what the budget is competing with.
#
# IT DRIVES --scrub-selftest, NOT A MOUSE, AND THAT IS WHY IT IS RUNNABLE HERE.
# The selftest launches through Start-Process and resolves no window, so it is
# immune by construction to this repo's window-resolver traps -- and, more to the
# point today, the quantities it reports for THIS question are decode-side:
# rev-hit, seeks, cache inserts, decode rate. Those do not depend on the display.
#
# THE LATENCY COLUMNS IT ALSO PRINTS ARE NOT SAFE ON EVERY DISPLAY AND ARE
# LABELLED SO. `hitch`, `stalls` and the paint gap are gated by the scrub paint
# gate, whose period is max(refresh, 2 x paint cost) -- ~4.4ms at 240Hz and
# ~17.5ms at 60Hz. Quote them only against a run on the same display.
#
# Per-config repeats matter: the FIRST pass over a clip reads the file cold and
# this repo has twice mistaken a cold pass for a regression. -Reps 2 and the
# first rep discarded is the default.

param(
    # 4K H.264, long-GOP: the file class the budget exists for, and the one the
    # recorded 60-vs-240 table was taken on. On intra-only media a seek lands on
    # the target with nothing walked en route, so there is nothing to fill with.
    [string]$Clip = 'C:\Users\andre\Documents\Claude_Cowork\Trace_Testing_Assets\4_4K_H264_MP4\Splash_1.mp4',
    [string]$Exe  = 'C:\Users\andre\Documents\Claude_Cowork\Trace_Windows\build\app\Release\Trace.exe',
    [int[]]$Budgets = @(0, 30, 60, 120, 240, 480),
    [int]$Reps = 2,
    [string]$OutDir = '',
    # The leg whose numbers answer this question. Leg 2 is the reversal drag --
    # the gesture that pays for seeks, and the one the recorded 60-vs-240 table
    # was taken on. Leg 1 is a forward sweep and never leaves the walked run,
    # so the fill budget has nothing to buy there; it is reported as the control.
    [int[]]$Legs = @(1, 2, 4)
)

$ErrorActionPreference = 'Continue'
if (-not (Test-Path $Exe))  { throw "exe not found: $Exe" }
if (-not (Test-Path $Clip)) { throw "clip not found: $Clip" }
if (-not $OutDir) { $OutDir = Join-Path $env:TEMP 'fillsweep' }
if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Force -Path $OutDir | Out-Null }

function Get-Num { param($Text, $Pattern)
    if ($Text -match $Pattern) { return $Matches[1] }
    return $null
}

$rows = New-Object System.Collections.Generic.List[object]
$total = $Budgets.Count * $Reps
$i = 0

foreach ($b in $Budgets) {
    for ($r = 1; $r -le $Reps; $r++) {
        $i++
        Write-Host ("[{0}/{1}] TRACE_SCRUB_FILL_MS={2} rep {3}" -f $i, $total, $b, $r)
        # 0 means "leave the knob unset", i.e. measure the shipping default --
        # which is the control for the whole sweep and must not be faked by
        # passing the default's value explicitly. A knob set to its own default
        # and a knob unset reach the same code by different routes, and this
        # repo has a recorded case of a harness comparing a config against
        # itself under two labels.
        if ($b -eq 0) { Remove-Item Env:\TRACE_SCRUB_FILL_MS -ErrorAction SilentlyContinue }
        else          { $env:TRACE_SCRUB_FILL_MS = "$b" }

        $tag = if ($b -eq 0) { 'default' } else { "$b" }
        $outFile = Join-Path $OutDir "fill_${tag}_r$r.txt"
        $errFile = Join-Path $OutDir "fill_${tag}_r$r.err.txt"
        $p = Start-Process -FilePath $Exe -ArgumentList "`"--scrub-selftest=$Clip`"" `
            -RedirectStandardOutput $outFile -RedirectStandardError $errFile -PassThru
        $null = $p.Handle   # cache the handle or ExitCode reads empty under PS 5.1
        if (-not $p.WaitForExit(300000)) {
            try { $p.Kill() } catch {}
            Write-Host "  TIMEOUT -- killed"
            continue
        }
        $text = Get-Content $outFile -Raw
        if (-not $text) { Write-Host "  NO OUTPUT"; continue }

        $legBlocks = [regex]::Split($text, '(?=-- leg \d+:)') | Where-Object { $_ -match '^-- leg \d+:' }
        foreach ($blk in $legBlocks) {
            $leg = [int](Get-Num $blk '-- leg (\d+):')
            if ($Legs -notcontains $leg) { continue }
            $rows.Add([pscustomobject]@{
                budget   = $tag
                rep      = $r
                leg      = $leg
                result   = $(if ($blk -match 'leg (PASS|FAIL)') { $Matches[1] } else { '?' })
                dec_fps  = Get-Num $blk 'dec ([\d.]+) f/s'
                supply   = Get-Num $blk 'supply ([\d.]+)%'
                seeks    = Get-Num $blk 'seeks (\d+)'
                rev_hit  = Get-Num $blk 'rev-hit ([\d.]+)%'
                ins      = Get-Num $blk 'ins (\d+)'
                walkmax  = Get-Num $blk 'walk max (\d+)f'
                hitch    = Get-Num $blk 'hitch (\d+)'
                delta    = Get-Num $blk 'delta (-?\d+)'
            })
        }
    }
}
Remove-Item Env:\TRACE_SCRUB_FILL_MS -ErrorAction SilentlyContinue

if ($rows.Count -eq 0) {
    Write-Host "NO ROWS PARSED -- the sweep measured nothing, which is not a result." -ForegroundColor Red
    exit 2
}

Write-Host ""
Write-Host "clip: $Clip"
Write-Host "exe : $Exe"
Write-Host ""
foreach ($leg in ($rows | Select-Object -ExpandProperty leg -Unique | Sort-Object)) {
    Write-Host "---- leg $leg ----"
    $rows | Where-Object { $_.leg -eq $leg } | Sort-Object { [int]($_.budget -replace 'default','-1') }, rep |
        Format-Table budget, rep, result, dec_fps, supply, seeks, rev_hit, ins, walkmax, hitch, delta -AutoSize |
        Out-String | Write-Host
}

# EXACTNESS IS NOT A TUNING OUTCOME. Whatever the budget buys, every leg must
# still land on the frame it was asked for; a budget that changed `delta` would
# be a correctness regression and is reported as a failure of the sweep rather
# than as a row in the table.
$badDelta = @($rows | Where-Object { $_.delta -ne '0' -and $_.delta -ne $null })
if ($badDelta.Count -gt 0) {
    Write-Host "DELTA IS NOT 0 ON $($badDelta.Count) ROW(S) -- the landing moved, which no budget may do." -ForegroundColor Red
    $badDelta | Format-Table budget, rep, leg, delta -AutoSize | Out-String | Write-Host
    exit 1
}
Write-Host "delta 0 on all $($rows.Count) rows." -ForegroundColor Green
exit 0
