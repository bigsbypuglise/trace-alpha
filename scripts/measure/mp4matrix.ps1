# Run the accuracy check over the whole MP4 corpus and tabulate it.
#
# One command, one table, one verdict per file. The per-file detail stays in
# mp4accuracy.ps1; this exists so the review's matrix is produced by a thing
# that can be re-run rather than assembled by hand from fifteen transcripts.
#
# IT RUNS SERIALLY AND MUST. Each leg launches and kills Trace, and a recorded
# full-pool sweep in this repo was voided by a concurrent launch -- two
# processes, one window resolver, null output, a harness crash read as a
# regression. Do not parallelise this.

param(
    [string]$CorpusDir = 'C:\Users\andre\Documents\Claude_Cowork\Trace_Testing_Assets\23_MP4_Review_Corpus',
    [string]$Exe = "",
    [string]$Filter = '',
    [string]$Csv = '',
    # Where each file's full transcript lands, so a surprising leg count can be
    # accounted for rather than guessed at.
    [string]$LogDir = '',
    # Extra clips outside the corpus (real-world files with no burn-in cannot be
    # checked this way, so this is for additional burn-in clips only).
    [string[]]$Extra = @()
)

$ErrorActionPreference = 'Continue'

$clips = @()
if (Test-Path $CorpusDir) {
    $clips += Get-ChildItem -Path $CorpusDir -Filter '*.mp4' | Sort-Object Name | ForEach-Object { $_.FullName }
}
$clips += $Extra
if ($Filter) { $clips = $clips | Where-Object { [System.IO.Path]::GetFileName($_) -like "*$Filter*" } }
if ($clips.Count -eq 0) { throw "no clips found in $CorpusDir" }

Write-Output "clips: $($clips.Count)"
Write-Output ""

$results = New-Object System.Collections.Generic.List[object]

foreach ($c in $clips) {
    $name = [System.IO.Path]::GetFileNameWithoutExtension($c)
    Write-Host "=== $name ===" -ForegroundColor Cyan
    $a = @{ Clip = $c }
    if ($Exe) { $a.Exe = $Exe }
    $out = & "$PSScriptRoot\mp4accuracy.ps1" @a 2>&1
    $code = $LASTEXITCODE

    # EVERY FILE'S FULL TRANSCRIPT IS KEPT. The first version captured $out and
    # printed only the summary, so a run whose leg COUNT was lower than expected
    # could not be accounted for -- and an accuracy matrix whose legs silently
    # go missing is the "0 of 0" failure in a new costume: it reports no
    # mismatches because it made no comparisons.
    if (-not $LogDir) { $LogDir = Join-Path $env:TEMP 'mp4matrix' }
    if (-not (Test-Path $LogDir)) { New-Item -ItemType Directory -Force -Path $LogDir | Out-Null }
    $out | Out-File -FilePath (Join-Path $LogDir "$name.log") -Encoding utf8

    # Parse the harness's own summary line rather than re-deriving it, so the
    # table and the per-file verdict cannot disagree.
    $summary = ($out | Select-String -Pattern '^legs \d+' | Select-Object -Last 1).Line
    $verdict = ($out | Select-String -Pattern '^RESULT: ' | Select-Object -Last 1).Line
    $nb      = ($out | Select-String -Pattern '^nb_frames : ' | Select-Object -Last 1).Line

    $legs = $mis = $unread = $worst = $null
    if ($summary -match 'legs (\d+) \| mismatches (\d+) \| unreadable (\d+) \| worst delta (-?\d+)') {
        $legs = [int]$Matches[1]; $mis = [int]$Matches[2]
        $unread = [int]$Matches[3]; $worst = [int]$Matches[4]
    }
    $frames = $null
    if ($nb -match 'nb_frames : (\d+)') { $frames = [int]$Matches[1] }

    $row = [pscustomobject]@{
        clip        = $name
        frames      = $frames
        legs        = $legs
        mismatches  = $mis
        unreadable  = $unread
        worst_delta = $worst
        verdict     = if ($verdict) { $verdict -replace '^RESULT: ','' } else { "NO VERDICT" }
    }
    $results.Add($row)
    Write-Host ("    {0}  legs={1} mismatch={2} unreadable={3} worst={4}" -f $row.verdict, $legs, $mis, $unread, $worst)

    # Any mismatch detail is worth keeping in the transcript: the table says a
    # file failed, these lines say how.
    $out | Select-String -Pattern 'False' | Select-Object -First 6 | ForEach-Object {
        Write-Host ("      " + $_.Line.Trim())
    }
}

Write-Output ""
$results | Format-Table -AutoSize | Out-String | Write-Output

$pass = ($results | Where-Object { $_.verdict -eq 'PASS' }).Count
Write-Output ("FILES {0} | PASS {1} | FAIL {2}" -f $results.Count, $pass, ($results.Count - $pass))
if ($Csv) {
    $results | Export-Csv -NoTypeInformation -Path $Csv
    Write-Output "csv: $Csv"
}
if ($pass -eq $results.Count) { exit 0 }
exit 1
