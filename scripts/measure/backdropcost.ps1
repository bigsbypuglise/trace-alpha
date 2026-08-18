# What does the step 10 route 2 strip backdrop cost, on the files with the least
# headroom in the set? TRACE_STRIP_BACKDROP=1 against =0, same binary.
#
# THE CHROME MUST BE UP FOR THE WHOLE RUN OR THE ANSWER IS MEANINGLESS. The strip
# auto-hides after 2s, so a plain 11s run measures the effect for two seconds and
# the fallback for nine and reports a cost near zero FOR THE WRONG REASON. The
# 2026-08-18 session held it up by jiggling the pointer and jiggled in the control
# too, so both sides paid for the mouse events.
#
# THIS PARKS THE POINTER OVER THE PICTURE INSTEAD, WHICH IS STRICTLY BETTER.
# Measured at three positions on a paused clip: with the pointer resting anywhere
# INSIDE the client the chrome is still up at 6s, unchanged to three decimals,
# because the auto-hide's timeout handler holds while `hover_` is a region. Only
# a pointer parked outside the window lets it hide (~2.6s). So one move in at the
# start holds the chrome up for the entire run and generates NO further input --
# which removes the jiggle from both sides of the comparison rather than
# balancing it.
#
# AND THE RUN PROVES ITS OWN PREMISE. Every capture is measured for horizontal
# variation across a row of the strip: the design's fallback is a purely vertical
# gradient and reads exactly 0, so `strip hsd` above 0 is what says the strip was
# really revealed AND really drawing the blur while the figures beside it were
# being recorded. A flat result with `strip hsd 0.000` on the bd=1 rows would be a
# check that could only report one thing.
param(
    [Parameter(Mandatory = $true)][string]$Clip,
    [string]$Label = "",
    [int]$Seconds = 11,
    [int]$Repeats = 2,
    [string[]]$Env = @(),
    [string]$OutDir = "$env:TEMP\trace_backdropcost",
    [string]$Exe = ""
)

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class BC {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
}
"@

New-Item -ItemType Directory -Force $OutDir | Out-Null
if (-not $Label) { $Label = [System.IO.Path]::GetFileNameWithoutExtension($Clip) }
if ($Label.Length -gt 24) { $Label = $Label.Substring(0, 24) }
$safe = $Label -replace '[^\w\-]', '_'

# Scratch INI for the same reason cadence.ps1 has one: Loop is persisted, and a
# wrap re-establishes the playback timeline and zeroes every counter with it.
$scratchIni = Join-Path $OutDir "backdrop-scratch.ini"
Remove-Item $scratchIni -ErrorAction SilentlyContinue

function Strip-HSd($bmp) {
    # Horizontal stddev across the right end of the strip, rows 6..30. See
    # stripbackdrop.ps1 -- the fallback is horizontally flat, a blur is not.
    $x0 = [int]($bmp.Width * 0.78); $x1 = [int]($bmp.Width * 0.98)
    if ($bmp.Height -le 30 -or $x1 -le $x0) { return -1 }
    $rect = New-Object System.Drawing.Rectangle $x0, 6, ($x1 - $x0), 24
    $data = $bmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $buf = New-Object byte[] ($data.Stride * $rect.Height)
    [System.Runtime.InteropServices.Marshal]::Copy($data.Scan0, $buf, 0, $buf.Length)
    $bmp.UnlockBits($data)
    $acc = 0.0
    for ($y = 0; $y -lt $rect.Height; $y++) {
        $base = $y * $data.Stride
        $vals = New-Object double[] $rect.Width
        for ($x = 0; $x -lt $rect.Width; $x++) {
            $i = $base + $x * 4
            $vals[$x] = ($buf[$i] + $buf[$i+1] + $buf[$i+2]) / 3.0
        }
        $m = ($vals | Measure-Object -Average).Average
        $v = 0.0; foreach ($t in $vals) { $v += ($t - $m) * ($t - $m) }
        $acc += [Math]::Sqrt($v / $rect.Width)
    }
    return [Math]::Round($acc / $rect.Height, 3)
}

$shots = @()
foreach ($bd in @('0','1')) {
    for ($r = 1; $r -le $Repeats; $r++) {
        $runEnv = @($Env) + @("TRACE_STRIP_BACKDROP=$bd", "TRACE_NO_AUDIO=1", "TRACE_SETTINGS_FILE=$scratchIni")
        $restartArgs = @{ Clip = $Clip; Env = $runEnv; SettleSeconds = 5 }
        if ($Exe) { $restartArgs.Exe = $Exe }
        & "$PSScriptRoot\restart.ps1" @restartArgs | Out-Null

        $p = Get-Process -Name Trace -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
        if (-not $p) { Write-Output "no window"; exit 1 }
        $h = $p.MainWindowHandle
        [BC]::SetForegroundWindow($h) | Out-Null
        Start-Sleep -Milliseconds 400

        # Park the pointer over the picture, clear of both strips, and leave it
        # there. This is what holds the chrome revealed for the whole run.
        $cr = New-Object BC+RECT; [BC]::GetClientRect($h, [ref]$cr) | Out-Null
        $o = New-Object BC+POINT; $o.X = 0; $o.Y = 0; [BC]::ClientToScreen($h, [ref]$o) | Out-Null
        [BC]::SetCursorPos(($o.X + [int]($cr.R / 2)), ($o.Y + [int]($cr.B / 2))) | Out-Null
        Start-Sleep -Milliseconds 200
        [BC]::SetCursorPos(($o.X + [int]($cr.R / 2) + 5), ($o.Y + [int]($cr.B / 2) + 3)) | Out-Null
        Start-Sleep -Milliseconds 300

        $sh = New-Object -ComObject WScript.Shell
        $sh.SendKeys(" ")
        Start-Sleep -Seconds $Seconds

        # ONE CLIENT-RECT CAPTURE SERVES BOTH READINGS, deliberately. capture.ps1
        # shoots the WINDOW rect, which starts at the title bar -- rows 6..30 of
        # that are chrome Windows drew, not Trace's strip. Capturing the client
        # puts the strip at row 0 where the detector expects it, and taking the
        # HUD out of the same bitmap means the premise and the figures cannot be
        # from different moments.
        $png = Join-Path $OutDir ("{0}_bd{1}_r{2}.png" -f $safe, $bd, $r)
        $cr2 = New-Object BC+RECT; [BC]::GetClientRect($h, [ref]$cr2) | Out-Null
        $o2 = New-Object BC+POINT; $o2.X = 0; $o2.Y = 0; [BC]::ClientToScreen($h, [ref]$o2) | Out-Null
        $cap = New-Object System.Drawing.Bitmap ($cr2.R - $cr2.L), ($cr2.B - $cr2.T)
        $cg = [System.Drawing.Graphics]::FromImage($cap)
        $cg.CopyFromScreen($o2.X, $o2.Y, 0, 0, $cap.Size)
        $cg.Dispose()
        $hsd = Strip-HSd $cap
        $cap.Save($png, [System.Drawing.Imaging.ImageFormat]::Png)
        $cap.Dispose()

        $shots += @{ Png = $png; Bd = $bd; Rep = $r; HSd = $hsd }
        Write-Output ("captured bd={0} rep {1}   strip hsd {2}" -f $bd, $r, $hsd)
    }
}
Get-Process -Name Trace -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue

# Stitch the presented/sched/cadence block from each run, 2x, for reading.
$probe = [System.Drawing.Bitmap]::FromFile($shots[0].Png)
$fromBottom = 340
$bandH = 96
$w0 = [int]($probe.Width * 0.88)
$probe.Dispose()

$lbl = 22
$dst = New-Object System.Drawing.Bitmap ($w0 * 2), ($shots.Count * ($bandH * 2 + $lbl))
$g = [System.Drawing.Graphics]::FromImage($dst)
$g.InterpolationMode = 'NearestNeighbor'
$g.Clear([System.Drawing.Color]::FromArgb(30, 0, 50))
$f = New-Object System.Drawing.Font('Consolas', 14, [System.Drawing.FontStyle]::Bold)
$i = 0
foreach ($s in $shots) {
    $src = [System.Drawing.Bitmap]::FromFile($s.Png)
    $top = [Math]::Max(0, $src.Height - $fromBottom)
    $ty = $i * ($bandH * 2 + $lbl)
    $text = "{0}  TRACE_STRIP_BACKDROP={1}  rep{2}   strip hsd {3}" -f $Label, $s.Bd, $s.Rep, $s.HSd
    $g.DrawString($text, $f, [System.Drawing.Brushes]::Yellow, 4, ($ty + 2))
    $g.DrawImage($src,
        (New-Object System.Drawing.Rectangle 0, ($ty + $lbl), ($w0 * 2), ($bandH * 2)),
        (New-Object System.Drawing.Rectangle 0, $top, $w0, $bandH), 'Pixel')
    $src.Dispose(); $i++
}
$out = Join-Path $OutDir ("summary_{0}.png" -f $safe)
$dst.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $dst.Dispose()
Write-Output ""
Write-Output "strip hsd must be 0 on every bd=0 row and ABOVE 0 on every bd=1 row."
Write-Output "If a bd=1 row reads 0 the strip was not drawing the blur and that run is void."
Write-Output $out
