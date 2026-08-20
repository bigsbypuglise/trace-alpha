# Track the transport strip's thumb/played-track position per captured frame
# through a hard reversal drag, and classify spike-and-revert excursions.
#
# Built for the beta.4 owner report: during a drag the thumb showed
# single-frame excursions of 100-350px that immediately reverted, clustered at
# direction changes, with the excursion side matching the decoder's lag
# direction. The mechanism (found in code, verified by this harness): the
# overlay's positionFraction read playback_.state().currentFrame, which during
# an async drag is written alternately by the pointer (queueVideoScrubFrame)
# and by every delivered chain frame (onScrubResult) -- and a delivered frame
# on the sampled/keyframe-landing path differs from the pointer by up to a
# GOP, so any paint landing between the delivery write and the next pointer
# write drew the thumb at the decoder's position instead of the hand's.
#
# Method: press on the overlay strip's track, sweep hard reversals with one
# strip-band capture per pointer step (CopyFromScreen + LockBits, stride-aware
# -- only the first width*4 bytes of each row are read), find the rightmost
# strongly-accent pixel per capture (the played track / thumb ring is the ONLY
# accent on the strip while Loop is off), and count excursions: a step of
# >= ThresholdPx immediately followed by a reversion of at least half its size
# in the opposite direction. The pointer moves monotonically inside a leg, so
# a genuine thumb cannot do that -- every excursion is a second position
# source leaking into the paint.
#
# Fault model (the recorded Threadripper class): run at 60Hz with
# TRACE_PRESENT_SYNC=1 --
#   scripts\measure\setrefresh.ps1 -Hz 60
#   scripts\measure\thumbtrack.ps1 -Env TRACE_PRESENT_SYNC=1
#   scripts\measure\setrefresh.ps1 -Hz 240      # ALWAYS restore explicitly
# It also reproduces (smaller px, same signature) at the healthy 240Hz default
# on the long-GOP demand-over-supply files (WeLo, Universe).
#
# Runs PLAIN (HUD hidden, scratch settings, all other TRACE_* cleared): the
# observable is strip pixels, and the HUD would change the viewer geometry.

param(
    [string]$Clip = 'C:\Users\andre\Documents\Claude_Cowork\Trace_Testing_Assets\17_Random_Mp4s\WeLo-AI-rc13-1x1_SOCIAL.mp4',
    [string]$Exe = '',
    [string[]]$Env = @(),
    [int]$Legs = 4,
    [int]$StepPx = 12,
    [int]$ThresholdPx = 60,
    [string]$OutCsv = (Join-Path $env:TEMP 'trace-thumbtrack.csv')
)
$ErrorActionPreference = 'Stop'

if (-not $Exe) {
    $Exe = Join-Path (Split-Path (Split-Path $PSScriptRoot -Parent) -Parent) 'build\app\Release\Trace.exe'
}
if (-not (Test-Path $Exe)) { Write-Output "no exe at $Exe"; exit 1 }
if (-not (Test-Path $Clip)) { Write-Output "no clip at $Clip"; exit 1 }

Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Drawing @"
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
public class TT {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint x, uint y, uint d, IntPtr e);
  public const uint DOWN = 0x0002, UP = 0x0004;

  Bitmap bmp; Graphics g; int bx, by, bw, bh; byte[] buf;
  public TT(int x, int y, int w, int h) {
    bx = x; by = y; bw = w; bh = h;
    bmp = new Bitmap(w, h, PixelFormat.Format32bppArgb);
    g = Graphics.FromImage(bmp);
  }
  // Rightmost accent-coloured pixel in the band (band-relative x), or -1.
  // Accent is #5AC8E8; through the strip's resting alpha over video it stays
  // strongly blue-over-red (B-R >= 55) and bright (B >= 150), while the
  // neutral track/readouts/thumb-dot read B-R ~ 0 and raw video through the
  // a215 scrim tops out near B = 62. leftOut gets the leftmost accent pixel
  // (the track's left end), so the caller can sanity-check the press landed.
  public int Sample(out int leftOut) {
    g.CopyFromScreen(bx, by, 0, 0, bmp.Size);
    Rectangle rc = new Rectangle(0, 0, bw, bh);
    BitmapData d = bmp.LockBits(rc, ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
    int len = d.Stride * bh;
    if (buf == null || buf.Length < len) buf = new byte[len];
    Marshal.Copy(d.Scan0, buf, 0, len);
    bmp.UnlockBits(d);
    int right = -1, left = -1;
    for (int y = 0; y < bh; y++) {
      int row = y * d.Stride;
      // Stride-aware: read only the first bw*4 bytes of the row.
      for (int x = 0; x < bw; x++) {
        int i = row + x * 4;
        int B = buf[i], G = buf[i + 1], R = buf[i + 2];
        if (B >= 150 && (B - R) >= 55 && (G - R) >= 40) {
          if (left < 0 || x < left) left = x;
          if (x > right) right = x;
        }
      }
    }
    leftOut = left;
    return right;
  }
}
"@

# --- Environment: plain run, scratch settings, caller's -Env on top ---------
Get-ChildItem env: | Where-Object { $_.Name -like 'TRACE_*' } | ForEach-Object {
    Remove-Item "env:$($_.Name)" -Confirm:$false -ErrorAction SilentlyContinue
}
$scratch = Join-Path $env:TEMP 'trace-thumbtrack-settings.ini'
if (Test-Path $scratch) { Remove-Item $scratch -Force -Confirm:$false }
$env:TRACE_SETTINGS_FILE = $scratch
$applied = @()
foreach ($pair in $Env) {
    $i = $pair.IndexOf('=')
    if ($i -lt 1) { Write-Output "bad -Env entry '$pair', expected NAME=VALUE"; exit 1 }
    Set-Item -Path "env:$($pair.Substring(0, $i))" -Value $pair.Substring($i + 1)
    $applied += $pair
}

# --- Launch --------------------------------------------------------------
Get-Process -Name Trace -ErrorAction SilentlyContinue | ForEach-Object { $_.CloseMainWindow() | Out-Null }
Start-Sleep -Milliseconds 800
Get-Process -Name Trace -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 400
$proc = Start-Process -FilePath $Exe -ArgumentList "`"$Clip`"" -PassThru
Start-Sleep -Seconds 4
if ($proc.HasExited) { Write-Output "EXITED EARLY code $($proc.ExitCode)"; exit 1 }

$p = Get-Process -Name Trace -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $p) { Write-Output 'no window'; exit 1 }
$h = $p.MainWindowHandle
[TT]::SetForegroundWindow($h) | Out-Null
Start-Sleep -Milliseconds 400
if ([TT]::GetForegroundWindow() -ne $h) { Write-Output 'FOREGROUND NOT TAKEN - run void'; exit 1 }

$r = New-Object TT+RECT
[TT]::GetWindowRect($h, [ref]$r) | Out-Null
# GetWindowRect includes Windows 11's invisible resize border (~8px each side
# except the top) -- the transitions.ps1 lesson.
$border = 8
$cl = $r.L + $border; $cr = $r.R - $border; $cb = $r.B - $border
$W = $cr - $cl
if ($W -lt 400) { Write-Output "window too narrow ($W px) for a meaningful sweep"; exit 1 }

# The strip is 56 logical px tall at the client bottom (dpr 1 on this box);
# the track sits at its vertical centre.
$trackY = $cb - 28
$bandY = $cb - 56; $bandH = 52

$tt = New-Object TT($cl, $bandY, $W, $bandH)

# --- Reveal the chrome (two points -- the same-coordinate reveal trap) ------
$midX = [int](($cl + $cr) / 2); $midY = [int](($r.T + $cb) / 2)
[TT]::SetCursorPos($midX, $midY) | Out-Null; Start-Sleep -Milliseconds 120
[TT]::SetCursorPos($midX + 60, $midY + 40) | Out-Null; Start-Sleep -Milliseconds 250

# --- Press on the track and sweep hard reversals ----------------------------
$xHi = $cl + [int](0.66 * $W)   # inside the track span on every pool shape
$xLo = $cl + [int](0.34 * $W)
[TT]::SetCursorPos($xHi, $trackY) | Out-Null; Start-Sleep -Milliseconds 200
[TT]::mouse_event([TT]::DOWN, 0, 0, 0, [IntPtr]::Zero)
Start-Sleep -Milliseconds 60

$L = 0
$first = $tt.Sample([ref]$L)
if ($first -lt 0) {
    [TT]::mouse_event([TT]::UP, 0, 0, 0, [IntPtr]::Zero)
    Write-Output 'NO ACCENT after press - the press did not land on the track; aborting'
    exit 1
}

$sw = [System.Diagnostics.Stopwatch]::StartNew()
$rows = New-Object System.Collections.Generic.List[string]
$rows.Add('i,t_ms,pointerX,accentR,accentL')
$idx = 0
$cur = $xHi
$targets = @()
for ($leg = 0; $leg -lt $Legs; $leg++) { $targets += @(if ($leg % 2 -eq 0) { $xLo } else { $xHi }) }
foreach ($tgt in $targets) {
    $dir = if ($tgt -gt $cur) { 1 } else { -1 }
    while (($dir -gt 0 -and $cur -lt $tgt) -or ($dir -lt 0 -and $cur -gt $tgt)) {
        $cur += $dir * $StepPx
        if (($dir -gt 0 -and $cur -gt $tgt) -or ($dir -lt 0 -and $cur -lt $tgt)) { $cur = $tgt }
        [TT]::SetCursorPos($cur, $trackY) | Out-Null
        $al = 0
        $ar = $tt.Sample([ref]$al)
        $rows.Add(('{0},{1},{2},{3},{4}' -f $idx, [int]$sw.ElapsedMilliseconds, ($cur - $cl), $ar, $al))
        $idx++
    }
}
[TT]::mouse_event([TT]::UP, 0, 0, 0, [IntPtr]::Zero)
# Watch the landing settle too.
for ($k = 0; $k -lt 40; $k++) {
    Start-Sleep -Milliseconds 12
    $al = 0
    $ar = $tt.Sample([ref]$al)
    $rows.Add(('{0},{1},{2},{3},{4}' -f $idx, [int]$sw.ElapsedMilliseconds, ($cur - $cl), $ar, $al))
    $idx++
}
$rows | Set-Content -Encoding utf8 $OutCsv

foreach ($pair in $Env) {
    Remove-Item -Path "env:$($pair.Substring(0, $pair.IndexOf('=')))" -ErrorAction SilentlyContinue
}

# --- Classify: spike-and-revert excursions ----------------------------------
# The pointer moves monotonically inside a leg, so a real thumb cannot step
# >= ThresholdPx one way and immediately step back. Excursion = |step| >=
# ThresholdPx AND the next step reverses by at least half of it.
$data = Import-Csv $OutCsv | Where-Object { [int]$_.accentR -ge 0 }
$xs = @($data | ForEach-Object { [int]$_.accentR })
$ptr = @($data | ForEach-Object { [int]$_.pointerX })
$n = $xs.Count
$excursions = 0; $maxSpike = 0; $spikeRows = @()
$maxLag = 0
for ($i = 1; $i -lt $n - 1; $i++) {
    $a = $xs[$i] - $xs[$i - 1]
    $b = $xs[$i + 1] - $xs[$i]
    $lag = [Math]::Abs($xs[$i] - $ptr[$i]); if ($lag -gt $maxLag) { $maxLag = $lag }
    if ([Math]::Abs($a) -ge $ThresholdPx -and (($a -gt 0) -ne ($b -gt 0)) -and [Math]::Abs($b) -ge ([Math]::Abs($a) / 2)) {
        $excursions++
        if ([Math]::Abs($a) -gt $maxSpike) { $maxSpike = [Math]::Abs($a) }
        $spikeRows += ('  i={0} t={1}ms accentR {2} -> {3} -> {4} (ptr {5})' -f $i, $data[$i].t_ms, $xs[$i-1], $xs[$i], $xs[$i+1], $ptr[$i])
    }
}
Write-Output ("clip {0}" -f (Split-Path $Clip -Leaf))
Write-Output ("env  {0}" -f ($(if ($applied.Count) { $applied -join ' ' } else { '(none)' })))
Write-Output ("samples {0} | win {1}px band | legs {2} step {3}px" -f $n, $W, $Legs, $StepPx)
Write-Output ("thumb-vs-pointer max lag {0}px" -f $maxLag)
Write-Output ("EXCURSIONS {0} (>= {1}px spike with immediate reversion), max {2}px" -f $excursions, $ThresholdPx, $maxSpike)
$spikeRows | Select-Object -First 12 | ForEach-Object { Write-Output $_ }
Write-Output ("csv {0}" -f $OutCsv)

Get-Process -Name Trace -ErrorAction SilentlyContinue | ForEach-Object { $_.CloseMainWindow() | Out-Null }
Start-Sleep -Milliseconds 500
Get-Process -Name Trace -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue

if ($excursions -gt 0) { exit 2 } else { exit 0 }
