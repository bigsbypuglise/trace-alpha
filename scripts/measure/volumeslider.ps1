# The inline volume slider (Trace_AudioSlider package, option 1b, 2026-08-20).
#
# -Mode on   : the feature. Hover the speaker -> the slider slides out (accent
#              pixels appear between Mute and Loop); wheel over the speaker
#              moves the level; a drag to the left end reads as muted (the
#              speaker cell's pixels change); a click on the speaker restores.
#              Each step captures the strip band and the HUD so the `vol N%`
#              token can be read beside the pixels.
# -Mode off  : TRACE_VOLUME_SLIDER=0, the owner-required rollback. Hovering the
#              speaker must move NOTHING (Loop stays where the collapsed layout
#              puts it) and the audio HUD line must carry no vol token.
#
# Geometry is derived from the measured strip height, the overlay.ps1 rule --
# an absolute offset is how that script aimed 1.2px off for a phase. Collapsed
# offsets (56px design units from the window's left pad):
#   mute centre  = pad(14) + 36+2+40+2+36+2 + 18 = 150
#   loop centre  = 150 + 18 + 2 + 18          = 188
# Expanded, the 74px slider plus a 2px gap sit between them:
#   slider track = 170 .. 244, loop centre = 264
param(
    [ValidateSet("on", "off", "persist")] [string]$Mode = "on",
    [string]$Clip = "C:\Users\andre\Documents\Claude_Cowork\Trace_Testing_Assets\3_1080p_H264_MP4\M&M_TopGun_1080.mp4",
    [string]$OutDir = (Join-Path $env:TEMP "trace_volumeslider"),
    # Extra NAME=VALUE pairs for the launch -- the cross-backend leg is
    # -ExtraEnv TRACE_RENDERER=cpu.
    [string[]]$ExtraEnv = @()
)
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $repo) { $repo = "C:\Users\andre\Documents\Claude_Cowork\Trace_Windows" }
$restart = Join-Path $PSScriptRoot "restart.ps1"
New-Item -ItemType Directory -Force $OutDir | Out-Null
Add-Type -AssemblyName System.Drawing

Add-Type @"
using System;
using System.Runtime.InteropServices;
public class VolWin {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [StructLayout(LayoutKind.Sequential)] public struct PT { public int X, Y; }
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref PT p);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint flags, int dx, int dy, int data, UIntPtr extra);
  public const uint WHEEL = 0x0800; public const uint LDOWN = 0x0002; public const uint LUP = 0x0004;
}
"@

# A SCRATCH SETTINGS FILE, because volume is PERSISTED (owner, 2026-08-21):
# without it every run would write its final level into the machine's real
# settings -- the Loop/cadence poisoning class -- and each run would inherit
# the previous one's level instead of starting at a known 100%.
$scratchIni = Join-Path $env:TEMP "volumeslider-scratch.ini"
if (Test-Path $scratchIni) { Remove-Item $scratchIni -Force }
# HUD HIDDEN for the interaction legs: the dev HUD is a widget BELOW the
# viewer, so with it shown the strip is not at the client bottom and every
# geometry-derived aim here lands on HUD text -- exactly what this script's
# first run did. The vol token is read from a final capture after pressing H.
$envs = @("TRACE_NO_AUDIO=0", "TRACE_HUD=0", "TRACE_SETTINGS_FILE=$scratchIni")
if ($Mode -eq "off") { $envs += "TRACE_VOLUME_SLIDER=0" }
$envs += $ExtraEnv
# Park the cursor away from where the strip will be BEFORE launching: a
# previous run leaves it on the speaker, the window opens under the cursor, and
# the collapsed baseline is then captured already-expanded.
[VolWin]::SetCursorPos(100, 100) | Out-Null
& $restart -Clip $Clip -Env $envs -SettleSeconds 5
$p = Get-Process -Name Trace -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $p) { Write-Output "no window"; exit 1 }
[VolWin]::SetForegroundWindow($p.MainWindowHandle) | Out-Null
Start-Sleep -Milliseconds 400

# CLIENT rect, mapped to screen -- GetWindowRect includes Windows 11's
# invisible resize border, the trap transitions.ps1 already records, and the
# strip is anchored to the CLIENT bottom.
$cr = New-Object VolWin+RECT
[VolWin]::GetClientRect($p.MainWindowHandle, [ref]$cr) | Out-Null
$origin = New-Object VolWin+PT; $origin.X = 0; $origin.Y = 0
[VolWin]::ClientToScreen($p.MainWindowHandle, [ref]$origin) | Out-Null
$r = New-Object VolWin+RECT
$r.L = $origin.X; $r.T = $origin.Y
$r.R = $origin.X + ($cr.R - $cr.L); $r.B = $origin.Y + ($cr.B - $cr.T)
$w = $r.R - $r.L; $h = $r.B - $r.T

function Shot([string]$name) {
    $bmp = New-Object System.Drawing.Bitmap $w, $h
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size)
    $g.Dispose()
    $bmp.Save((Join-Path $OutDir "$name.png"))
    $bmp.Dispose()
}

# The strip: jiggle to reveal (two points -- the same-coordinate filter), then
# measure its height from the design's 56px at the window's scale. This box
# runs dpr 1, and the strip is device-pixel-snapped, so 56 is exact here.
$stripH = 56
$iconY = $r.B - [int]($stripH / 2)
$leftPad = $r.L
$muteX  = $leftPad + [int]($stripH * (150.0 / 56.0))
$loopCollapsedX = $leftPad + [int]($stripH * (188.0 / 56.0))
$sliderLeftX  = $leftPad + [int]($stripH * (172.0 / 56.0))
$sliderRightX = $leftPad + [int]($stripH * (242.0 / 56.0))
$sliderMidY = $iconY

[VolWin]::SetCursorPos($r.L + 400, $r.B - 200) | Out-Null; Start-Sleep -Milliseconds 150
[VolWin]::SetCursorPos($r.L + 420, $r.B - 210) | Out-Null; Start-Sleep -Milliseconds 400
Shot "01-revealed-collapsed"

# Hover the speaker.
[VolWin]::SetCursorPos($muteX, $iconY) | Out-Null; Start-Sleep -Milliseconds 600
Shot "02-hover-mute"

# Accent-or-track pixels between mute and loop say whether the slider is out.
function SliderTrackPixels([string]$png) {
    $b = [System.Drawing.Bitmap]::FromFile((Join-Path $OutDir "$png.png"))
    $n = 0
    $y0 = $h - [int]($stripH / 2) - 4; $y1 = $h - [int]($stripH / 2) + 4
    for ($y = $y0; $y -le $y1; $y++) {
        for ($x = ($sliderLeftX - $r.L); $x -le ($sliderRightX - $r.L); $x++) {
            $c = $b.GetPixel($x, $y)
            # the accent fill (cyan-ish: B noticeably above R) or the white
            # thumb; the track bg alone is too close to the strip to count on.
            if (($c.B -gt ($c.R + 40)) -or ($c.R -gt 200 -and $c.G -gt 200 -and $c.B -gt 200)) { $n++ }
        }
    }
    $b.Dispose()
    return $n
}
$collapsed = SliderTrackPixels "01-revealed-collapsed"
$expanded = SliderTrackPixels "02-hover-mute"
Write-Output ("VOLUME slider pixels collapsed/hover: {0} / {1}" -f $collapsed, $expanded)

if ($Mode -eq "persist") {
    # Wheel to 75% over the speaker, close GRACEFULLY (QSettings flushes on
    # destruction), and relaunch on the same scratch INI: the stored level must
    # come back in force -- the HUD's vol token and the INI line are the two
    # observables.
    [VolWin]::SetCursorPos($muteX, $iconY) | Out-Null; Start-Sleep -Milliseconds 600
    for ($i = 0; $i -lt 5; $i++) {
        [VolWin]::mouse_event([VolWin]::WHEEL, 0, 0, -120, [UIntPtr]::Zero)
        Start-Sleep -Milliseconds 120
    }
    Start-Sleep -Milliseconds 400
    Get-Process -Name Trace -ErrorAction SilentlyContinue | ForEach-Object { $_.CloseMainWindow() | Out-Null }
    Start-Sleep -Milliseconds 1500
    Get-Process -Name Trace -ErrorAction SilentlyContinue | Stop-Process -Force
    $iniLine = (Get-Content $scratchIni -ErrorAction SilentlyContinue | Select-String "volume")
    Write-Output ("VOLUME persist: ini says '{0}'" -f $iniLine)
    & $restart -Clip $Clip -Env $envs -SettleSeconds 5
    $p2 = Get-Process -Name Trace | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
    if (-not $p2) { Write-Output "VOLUME persist: FAIL - no window on relaunch"; exit 1 }
    [VolWin]::SetForegroundWindow($p2.MainWindowHandle) | Out-Null
    Start-Sleep -Milliseconds 400
    $ws2 = New-Object -ComObject WScript.Shell
    $ws2.SendKeys("h"); Start-Sleep -Milliseconds 800
    Shot "07-persist-relaunch-hud"
    if ("$iniLine" -match "0\.75|0\.75$|=0\.75") { Write-Output "VOLUME persist: INI PASS (0.75 stored)" }
    else { Write-Output "VOLUME persist: FAIL - 0.75 not found in the scratch ini" }
    Write-Output "read the relaunch HUD's vol token off 07-persist-relaunch-hud.png (expect vol 75%)"
    Get-Process -Name Trace -ErrorAction SilentlyContinue | Stop-Process -Force
    exit 0
}

if ($Mode -eq "persist") {
    # Volume persists between sessions (owner, 2026-08-21). The pointer is
    # already hovering the speaker from the 02 shot: wheel to 75%, close
    # GRACEFULLY (QSettings flushes on destruction), check the scratch INI,
    # relaunch on the same INI, and read the vol token off the relaunch HUD.
    for ($i = 0; $i -lt 5; $i++) {
        [VolWin]::mouse_event([VolWin]::WHEEL, 0, 0, -120, [UIntPtr]::Zero)
        Start-Sleep -Milliseconds 120
    }
    Start-Sleep -Milliseconds 400
    Get-Process -Name Trace -ErrorAction SilentlyContinue | ForEach-Object { $_.CloseMainWindow() | Out-Null }
    Start-Sleep -Milliseconds 1500
    Get-Process -Name Trace -ErrorAction SilentlyContinue | Stop-Process -Force
    $iniLine = (Get-Content $scratchIni -ErrorAction SilentlyContinue | Select-String "volume")
    Write-Output ("VOLUME persist: ini says '{0}'" -f $iniLine)
    if ("$iniLine" -match "0\.75|0\.75$|=0\.75") { Write-Output "VOLUME persist: INI PASS (0.75 stored)" }
    else { Write-Output "VOLUME persist: FAIL - 0.75 not found in the scratch ini" }
    # Relaunch on the same INI -- the seed must put the stored level in force.
    & $restart -Clip $Clip -Env $envs -SettleSeconds 5
    $p2 = Get-Process -Name Trace | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
    if (-not $p2) { Write-Output "VOLUME persist: FAIL - no window on relaunch"; exit 1 }
    [VolWin]::SetForegroundWindow($p2.MainWindowHandle) | Out-Null
    Start-Sleep -Milliseconds 400
    $ws2 = New-Object -ComObject WScript.Shell
    $ws2.SendKeys("h"); Start-Sleep -Milliseconds 800
    Shot "07-persist-relaunch-hud"
    Write-Output "read the relaunch HUD's vol token off 07-persist-relaunch-hud.png (expect vol 75%)"
    Get-Process -Name Trace -ErrorAction SilentlyContinue | Stop-Process -Force
    exit 0
}

if ($Mode -eq "off") {
    # DELTA, not an absolute: bright picture pixels behind the translucent
    # strip match the white-ish predicate at a steady baseline (18 on the M&M
    # frame), and the same paused frame reads the same both times -- what says
    # "no slider" is the two counts agreeing, not either being zero.
    if (($expanded - $collapsed) -gt 5) { Write-Output "VOLUME off: FAIL - hovering the speaker changed the strip" }
    else { Write-Output "VOLUME off: PASS - mute-only button, no expansion" }
    Shot "03-off-hud"
    Get-Process -Name Trace -ErrorAction SilentlyContinue | Stop-Process -Force
    exit 0
}

if (($expanded - $collapsed) -lt 30) {
    Write-Output "VOLUME on: FAIL - hovering the speaker did not expand the slider"
}

# Wheel down five notches over the speaker: vol 100 -> 75.
for ($i = 0; $i -lt 5; $i++) {
    [VolWin]::mouse_event([VolWin]::WHEEL, 0, 0, -120, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 120
}
Start-Sleep -Milliseconds 300
Shot "03-after-wheel-down"

# Drag the thumb to the left end: level 0, the speaker must read muted.
[VolWin]::SetCursorPos($sliderRightX, $sliderMidY) | Out-Null; Start-Sleep -Milliseconds 200
[VolWin]::mouse_event([VolWin]::LDOWN, 0, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 120
for ($x = $sliderRightX; $x -ge $sliderLeftX - 8; $x -= 12) {
    [VolWin]::SetCursorPos($x, $sliderMidY) | Out-Null; Start-Sleep -Milliseconds 30
}
[VolWin]::mouse_event([VolWin]::LUP, 0, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 300
Shot "04-dragged-to-zero"

# Click the speaker: the level must come back (the restore-from-zero default).
[VolWin]::SetCursorPos($muteX, $iconY) | Out-Null; Start-Sleep -Milliseconds 300
[VolWin]::mouse_event([VolWin]::LDOWN, 0, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 60
[VolWin]::mouse_event([VolWin]::LUP, 0, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 400
Shot "05-after-unmute-click"

# The speaker cell across the three states: 04 must differ from 03 (glyph went
# muted) and 05 must differ from 04 (glyph came back). Hovered-vs-hovered both
# times, the overlay.ps1 lesson.
function CellDelta([string]$a, [string]$b) {
    $ba = [System.Drawing.Bitmap]::FromFile((Join-Path $OutDir "$a.png"))
    $bb = [System.Drawing.Bitmap]::FromFile((Join-Path $OutDir "$b.png"))
    $mx = $muteX - $r.L; $my = $h - [int]($stripH / 2); $n = 0
    for ($dy = -12; $dy -le 12; $dy++) {
        for ($dx = -12; $dx -le 12; $dx++) {
            $ca = $ba.GetPixel($mx + $dx, $my + $dy); $cb = $bb.GetPixel($mx + $dx, $my + $dy)
            $d = [Math]::Max([Math]::Max([Math]::Abs($ca.R - $cb.R), [Math]::Abs($ca.G - $cb.G)), [Math]::Abs($ca.B - $cb.B))
            if ($d -gt 24) { $n++ }
        }
    }
    $ba.Dispose(); $bb.Dispose()
    return $n
}
$toMuted = CellDelta "03-after-wheel-down" "04-dragged-to-zero"
$restored = CellDelta "04-dragged-to-zero" "05-after-unmute-click"

# Show the HUD for the final capture so the audio line's `vol N%` can be read
# beside the pixels. After the assertions' captures, deliberately -- H re-lays
# the window and would invalidate every geometry above.
$ws = New-Object -ComObject WScript.Shell
$ws.SendKeys("h"); Start-Sleep -Milliseconds 800
Shot "06-hud-after"
Write-Output ("VOLUME glyph delta wheel->zero: {0} px, zero->restored: {1} px (of 625)" -f $toMuted, $restored)
if ($toMuted -lt 15) { Write-Output "VOLUME: FAIL - drag to zero did not read as muted" }
elseif ($restored -lt 15) { Write-Output "VOLUME: FAIL - the unmute click did not restore" }
else { Write-Output "VOLUME on: PASS" }

Get-Process -Name Trace -ErrorAction SilentlyContinue | Stop-Process -Force
Write-Output "captures -> $OutDir (read the HUD's vol token off 03/04/05)"
