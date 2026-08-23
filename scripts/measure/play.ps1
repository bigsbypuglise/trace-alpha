# Press Space and let it play.
#
# Three things this now does that the one-line version did not, each of which
# has silently produced a `frames 0 | ticks 0 | presents 0` run that reads
# exactly like a build which cannot play:
#
#   1. resolves the window through tracewindow.ps1, so a TOOLTIP raised under a
#      parked cursor cannot be mistaken for the main window
#   2. VERIFIES foreground was granted -- SetForegroundWindow fails silently
#      from a background process, and the keys then go to the harness's own
#      console
#   3. asserts the PICTURE ACTUALLY ADVANCED before returning success
#
# (3) is the one that turns a silent wrong number into a loud failure. A
# cadence figure from a run that never ticked is not a slow number, it is no
# number. Callers pointed at a still image or a deliberately paused file pass
# -RequireMotion:$false; everything measuring playback should leave it on.
param(
    [int]$Seconds = 9,
    [int]$Attempts = 3,
    [bool]$RequireMotion = $true,
    [switch]$NoCursorPark
)

. "$PSScriptRoot\tracewindow.ps1"

for ($attempt = 1; $attempt -le $Attempts; $attempt++) {
    $h = Resolve-TraceWindow -NoCursorPark:$NoCursorPark
    if ($h -eq [IntPtr]::Zero) {
        Write-Output "play: no window (attempt $attempt of $Attempts)"
        Start-Sleep -Milliseconds 500
        continue
    }

    if (-not (Focus-TraceWindow -Handle $h)) {
        Write-Output "play: foreground denied (attempt $attempt of $Attempts)"
        Start-Sleep -Milliseconds 500
        continue
    }

    $sh = New-Object -ComObject WScript.Shell
    $sh.SendKeys(" ")

    if (-not $RequireMotion) {
        Write-Output "space sent; playing for $Seconds s (motion not checked)"
        Start-Sleep -Seconds $Seconds
        exit 0
    }

    Start-Sleep -Milliseconds 700
    if (Test-PictureAdvancing -Handle $h) {
        Write-Output "space sent; playing for $Seconds s"
        Start-Sleep -Seconds $Seconds
        exit 0
    }

    Write-Output "play: SPACE DID NOT START PLAYBACK (attempt $attempt of $Attempts)"
    # Space toggles, so a press that DID land would leave it paused. Press again
    # to return to the state we started from before retrying, or the retry is
    # fighting its own previous attempt.
    $sh.SendKeys(" ")
    Start-Sleep -Milliseconds 400
}

# Write-WARNING, not Write-Output, and this matters: every caller pipes this
# script to Out-Null, which swallows the success stream and would hide the one
# message that says the run is void. The warning stream survives it.
Write-Warning "play.ps1: FAILED to start playback after $Attempts attempts -- any figure from this run is void"
exit 1
