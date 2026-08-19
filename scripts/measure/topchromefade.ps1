# Owner item 11 experiment: fading the NATIVE top chrome strip by layered-window
# alpha (WS_EX_LAYERED + LWA_ALPHA). The pass/fail criterion is not "does the
# opacity animate" -- it is WHAT IS REVEALED AS IT FADES. A layered child blends
# against what is beneath it in the window tree, and the video is a sibling HWND,
# so the likely failure is a strip that fades toward BLACK rather than toward the
# picture: that animates perfectly and looks worse than popping.
#
# Mode 'blend' answers it with a PINNED alpha (TRACE_TOPCHROME_ALPHA=128), which
# turns the 82ms mid-fade window into a stable state. Three captures per
# renderer over BRIGHT footage; the strip's own content is the design's flat
# solid gradient (per-row hsd exactly 0) -- since the 2026-08-19 backdrop
# removal that is the only content it has:
#   A  strip pinned 255  -> the strip's own pixels, opaque
#   V  strip hidden      -> the raw video under the strip band
#   M  strip pinned 128  -> the question
# Verdict is which prediction M matches per pixel:
#   (A+V)/2   -> blending against the VIDEO   -> item 11 closes
#   A*(128/255)-> blending against BLACK      -> the answer is no
#   A         -> alpha IGNORED (the manifest gate: layered child windows need a
#                Win8+ supportedOS declaration and Trace.exe carries none)
#   V         -> strip INVISIBLE at 255 (layered child not rendered at all)
#
# Mode 'rest' (owner item 8, option B, 2026-08-19) verifies the SHIPPING
#              resting state: a default launch with NO pin, media open, chrome
#              held. The strip rests at alpha 215 (TopChrome::kRestingAlpha),
#              so the band must read A*(215/255) + V*(40/255) on d3d11 --
#              while on cpu the layered alpha is IGNORED (native children
#              share the top-level backing store) and PASS is the band reading
#              A. THE TWO BACKENDS PASSING DIFFERENT PREDICTIONS IS THE OWNER
#              DECISION, recorded here so a future session does not read the
#              divergence as a defect. Run it once per -Renderer.
#
# Mode 'anim'  captures the strip band through a real auto-hide and prints the
#              band mean over time -- the ramp profile of the actual fade.
#              Since item 8 the ramp tops out at the resting alpha, not opaque.
# Mode 'menus' verifies hit-testing survives the layered style: a click on File
#              and an Alt+F mnemonic must both open a menu.
# Mode 'loop'  is the items-3+7 regression watch: TRACE_REVEAL_LOG=1, pointer
#              parked, count chrome SHOWN/HIDDEN transitions. The strip mapping
#              and unmapping is what fed the old blink cycle, and the layered
#              style must not re-open it.
param(
    [ValidateSet('blend','rest','anim','menus','loop')][string]$Mode = 'blend',
    [Parameter(Mandatory = $true)][string]$Clip,
    [ValidateSet('d3d11','cpu')][string]$Renderer = 'd3d11',
    [string]$Exe,
    [int]$StepFrames = 45,
    [string]$OutDir = "$env:TEMP\tracetopfade"
)

$ErrorActionPreference = 'Continue'
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class TfWin {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool f);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, IntPtr p);
  [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
}
"@

if (-not $Exe) { $Exe = Join-Path $PSScriptRoot "..\..\build\app\Release\Trace.exe" }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

function Get-TraceWindow {
    $p = Get-Process -Name Trace -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
    if (-not $p) { return $null }
    return $p.MainWindowHandle
}

function Focus-Trace($h) {
    $fg = [TfWin]::GetForegroundWindow()
    $tid = [TfWin]::GetWindowThreadProcessId($fg, [IntPtr]::Zero)
    $me  = [TfWin]::GetCurrentThreadId()
    [TfWin]::AttachThreadInput($me, $tid, $true) | Out-Null
    [TfWin]::SetForegroundWindow($h) | Out-Null
    [TfWin]::AttachThreadInput($me, $tid, $false) | Out-Null
    Start-Sleep -Milliseconds 250
    return ([TfWin]::GetForegroundWindow() -eq $h)
}

function Client-Origin($h) {
    $pt = New-Object TfWin+POINT; $pt.X = 0; $pt.Y = 0
    [TfWin]::ClientToScreen($h, [ref]$pt) | Out-Null
    return $pt
}

function Capture-Client($h, $path) {
    $r = New-Object TfWin+RECT
    [TfWin]::GetClientRect($h, [ref]$r) | Out-Null
    $pt = Client-Origin $h
    $w = $r.R - $r.L; $ht = $r.B - $r.T
    $bmp = New-Object System.Drawing.Bitmap $w, $ht
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($pt.X, $pt.Y, 0, 0, $bmp.Size)
    $g.Dispose()
    if ($path) { $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png) }
    return $bmp
}

# The strip band (the retired stripbackdrop.ps1's): the right end past the menus, rows
# clear of the top edge and the 1px hairline. Returns per-pixel grayscale as a
# double[], plus hsd and channel means. Stride-aware -- only the first width*4
# bytes of each row are pixels.
function Get-Band($bmp) {
    $x0 = [int]($bmp.Width * 0.78)
    $x1 = [int]($bmp.Width * 0.98)
    $y0 = 6; $y1 = 30
    $rect = New-Object System.Drawing.Rectangle $x0, $y0, ($x1 - $x0), ($y1 - $y0)
    $data = $bmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $w = $rect.Width; $h = $rect.Height
    $buf = New-Object byte[] ($data.Stride * $h)
    [System.Runtime.InteropServices.Marshal]::Copy($data.Scan0, $buf, 0, $buf.Length)
    $bmp.UnlockBits($data)
    $gray = New-Object double[] ($w * $h)
    $rowSds = New-Object System.Collections.ArrayList
    $sumR = 0.0; $sumG = 0.0; $sumB = 0.0
    for ($y = 0; $y -lt $h; $y++) {
        $base = $y * $data.Stride
        $m = 0.0
        for ($x = 0; $x -lt $w; $x++) {
            $i = $base + $x * 4
            $b = $buf[$i]; $gg = $buf[$i+1]; $rr = $buf[$i+2]
            $v = ($rr + $gg + $b) / 3.0
            $gray[$y * $w + $x] = $v
            $m += $v
            $sumR += $rr; $sumG += $gg; $sumB += $b
        }
        $m /= $w
        $vv = 0.0
        for ($x = 0; $x -lt $w; $x++) { $d = $gray[$y * $w + $x] - $m; $vv += $d * $d }
        [void]$rowSds.Add([Math]::Sqrt($vv / $w))
    }
    $n = $w * $h
    return [pscustomobject]@{
        Gray = $gray; W = $w; H = $h
        HSd  = [Math]::Round(($rowSds | Measure-Object -Average).Average, 3)
        Mean = [Math]::Round((($gray | Measure-Object -Sum).Sum) / $n, 2)
        R = [Math]::Round($sumR / $n, 2); G = [Math]::Round($sumG / $n, 2); B = [Math]::Round($sumB / $n, 2)
    }
}

# Mean gray of an entire small bitmap (used by 'anim', where the capture IS the
# band). Stride-aware.
function Band-Mean($bmp) {
    $rect = New-Object System.Drawing.Rectangle 0, 0, $bmp.Width, $bmp.Height
    $data = $bmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $buf = New-Object byte[] ($data.Stride * $bmp.Height)
    [System.Runtime.InteropServices.Marshal]::Copy($data.Scan0, $buf, 0, $buf.Length)
    $bmp.UnlockBits($data)
    $s = 0.0
    for ($y = 0; $y -lt $bmp.Height; $y++) {
        $base = $y * $data.Stride
        for ($x = 0; $x -lt $bmp.Width; $x++) {
            $i = $base + $x * 4
            $s += ($buf[$i] + $buf[$i+1] + $buf[$i+2]) / 3.0
        }
    }
    return $s / ($bmp.Width * $bmp.Height)
}

function MAE($a, $b, $scaleB = 1.0, $second = $null, $secondScale = 0.0) {
    # mean |a - p| where p = b*scaleB + second*secondScale (second optional).
    # The two-source form is the layered-blend prediction: strip*wS + video*wV.
    $n = $a.Count
    $s = 0.0
    for ($i = 0; $i -lt $n; $i++) {
        $p = $b[$i] * $scaleB
        if ($null -ne $second) { $p += $second[$i] * $secondScale }
        $s += [Math]::Abs($a[$i] - $p)
    }
    return [Math]::Round($s / $n, 2)
}

function Start-TraceRun($extraEnv, $errFile = $null) {
    Get-Process -Name Trace -ErrorAction SilentlyContinue | ForEach-Object { $_.CloseMainWindow() | Out-Null }
    Start-Sleep -Milliseconds 700
    Get-Process -Name Trace -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 300
    $names = @('TRACE_RENDERER','TRACE_HUD','TRACE_NO_AUDIO','TRACE_SETTINGS_FILE','TRACE_TOPCHROME_ALPHA','TRACE_TOPCHROME_FADE','TRACE_REVEAL_LOG')
    foreach ($n in $names) { Remove-Item "env:$n" -ErrorAction SilentlyContinue }
    $env:TRACE_RENDERER = $Renderer
    $env:TRACE_HUD = "0"
    $env:TRACE_NO_AUDIO = "1"
    $env:TRACE_SETTINGS_FILE = "$OutDir\scratch.ini"
    foreach ($kv in $extraEnv.GetEnumerator()) { Set-Item "env:$($kv.Key)" $kv.Value }
    if ($errFile) {
        Start-Process -FilePath $Exe -ArgumentList ('"' + $Clip + '"') -RedirectStandardError $errFile | Out-Null
    } else {
        Start-Process -FilePath $Exe -ArgumentList ('"' + $Clip + '"') | Out-Null
    }
    foreach ($n in $names) { Remove-Item "env:$n" -ErrorAction SilentlyContinue }
    Start-Sleep -Seconds 4
    $h = Get-TraceWindow
    if (-not $h) { throw "no Trace window" }
    if (-not (Focus-Trace $h)) { throw "could not foreground Trace" }
    return $h
}

function Step-Into($h) {
    for ($i = 0; $i -lt $StepFrames; $i++) { [System.Windows.Forms.SendKeys]::SendWait("{RIGHT}"); Start-Sleep -Milliseconds 25 }
    Start-Sleep -Milliseconds 400
}

function Park-Inside($h) {
    # A stationary pointer inside the client holds the chrome up indefinitely
    # (the auto-hide holds while hover_ is a region) and generates no input.
    $r = New-Object TfWin+RECT
    [TfWin]::GetClientRect($h, [ref]$r) | Out-Null
    $pt = Client-Origin $h
    $cx = $pt.X + [int](($r.R - $r.L) / 2)
    $cy = $pt.Y + [int](($r.B - $r.T) / 2)
    [TfWin]::SetCursorPos($cx, $cy) | Out-Null
    Start-Sleep -Milliseconds 60
    [TfWin]::SetCursorPos(($cx + 5), ($cy + 3)) | Out-Null
    Start-Sleep -Milliseconds 400
}

Write-Output "mode=$Mode  renderer=$Renderer  clip=$(Split-Path -Leaf $Clip)"
Write-Output ""

if ($Mode -eq 'blend') {
    # A: strip pinned fully opaque -- the strip's own flat solid gradient.
    $h = Start-TraceRun @{ TRACE_TOPCHROME_ALPHA = '255' }
    Step-Into $h
    Park-Inside $h
    $bmpA = Capture-Client $h "$OutDir\$Renderer-A-alpha255.png"
    $A = Get-Band $bmpA; $bmpA.Dispose()
    # V: same launch, pointer parked outside -> strip hides -> raw video.
    [TfWin]::SetCursorPos(5, 5) | Out-Null
    Start-Sleep -Milliseconds 3400
    $bmpV = Capture-Client $h "$OutDir\$Renderer-V-hidden.png"
    $V = Get-Band $bmpV; $bmpV.Dispose()

    # M: strip pinned to the midpoint.
    $h = Start-TraceRun @{ TRACE_TOPCHROME_ALPHA = '128' }
    Step-Into $h
    Park-Inside $h
    $bmpM = Capture-Client $h "$OutDir\$Renderer-M-alpha128.png"
    $M = Get-Band $bmpM; $bmpM.Dispose()

    Write-Output ("A  strip @255      hsd {0}  mean {1}  RGB {2}/{3}/{4}" -f $A.HSd, $A.Mean, $A.R, $A.G, $A.B)
    Write-Output ("V  strip hidden    hsd {0}  mean {1}  RGB {2}/{3}/{4}" -f $V.HSd, $V.Mean, $V.R, $V.G, $V.B)
    Write-Output ("M  strip @128      hsd {0}  mean {1}  RGB {2}/{3}/{4}" -f $M.HSd, $M.Mean, $M.R, $M.G, $M.B)
    Write-Output ""
    if ($A.W -ne $V.W -or $A.W -ne $M.W) { Write-Output "band sizes differ -- captures not comparable"; exit 1 }

    $errVideo   = MAE $M.Gray $A.Gray 0.5 $V.Gray 0.5      # M ~ (A+V)/2
    $errBlack   = MAE $M.Gray $A.Gray (128.0/255.0)        # M ~ A*0.502
    $errNoAlpha = MAE $M.Gray $A.Gray 1.0                  # M ~ A (alpha ignored)
    $errNoStrip = MAE $M.Gray $V.Gray 1.0                  # M ~ V (strip invisible)
    Write-Output ("M vs (A+V)/2  [fades toward VIDEO]      MAE {0}" -f $errVideo)
    Write-Output ("M vs A*0.50   [fades toward BLACK]      MAE {0}" -f $errBlack)
    Write-Output ("M vs A        [alpha IGNORED]           MAE {0}" -f $errNoAlpha)
    Write-Output ("M vs V        [strip INVISIBLE at 255]  MAE {0}" -f $errNoStrip)
    Write-Output ""
    $best = @(
        @{ n = 'VIDEO -- item 11 closes';            e = $errVideo },
        @{ n = 'BLACK -- the answer is no';          e = $errBlack },
        @{ n = 'ALPHA IGNORED (manifest gate?)';     e = $errNoAlpha },
        @{ n = 'STRIP INVISIBLE (layered not drawn)'; e = $errNoStrip }
    ) | Sort-Object { $_.e } | Select-Object -First 1
    Write-Output ("VERDICT: blends toward {0}  (MAE {1})" -f $best.n, $best.e)
    Write-Output "Captures in $OutDir -- confirm by eye before believing the arithmetic."
}

if ($Mode -eq 'rest') {
    # A: strip pinned fully opaque -- the strip's own flat solid gradient.
    $h = Start-TraceRun @{ TRACE_TOPCHROME_ALPHA = '255' }
    Step-Into $h
    Park-Inside $h
    $bmpA = Capture-Client $h "$OutDir\$Renderer-rest-A-alpha255.png"
    $A = Get-Band $bmpA; $bmpA.Dispose()
    # V: same launch, pointer parked outside -> strip hides -> raw video.
    [TfWin]::SetCursorPos(5, 5) | Out-Null
    Start-Sleep -Milliseconds 3400
    $bmpV = Capture-Client $h "$OutDir\$Renderer-rest-V-hidden.png"
    $V = Get-Band $bmpV; $bmpV.Dispose()

    # R: the SHIPPING configuration -- no pin, no override. What the settled
    # strip actually reads at rest. NOTE this assumes Windows' transparency
    # setting is ON; with it off the strip correctly rests opaque and this
    # mode would report OPAQUE on a correct build -- read the HUD's `strip`
    # field (a215 vs opaque (windows)) before believing a FAIL.
    $h = Start-TraceRun @{ }
    Step-Into $h
    Park-Inside $h
    $bmpR = Capture-Client $h "$OutDir\$Renderer-rest-R-default.png"
    $R = Get-Band $bmpR; $bmpR.Dispose()

    Write-Output ("A  strip @255      hsd {0}  mean {1}  RGB {2}/{3}/{4}" -f $A.HSd, $A.Mean, $A.R, $A.G, $A.B)
    Write-Output ("V  strip hidden    hsd {0}  mean {1}  RGB {2}/{3}/{4}" -f $V.HSd, $V.Mean, $V.R, $V.G, $V.B)
    Write-Output ("R  default rest    hsd {0}  mean {1}  RGB {2}/{3}/{4}" -f $R.HSd, $R.Mean, $R.R, $R.G, $R.B)
    Write-Output ""
    if ($A.W -ne $V.W -or $A.W -ne $R.W) { Write-Output "band sizes differ -- captures not comparable"; exit 1 }

    $wS = 215.0 / 255.0; $wV = 40.0 / 255.0
    $errResting = MAE $R.Gray $A.Gray $wS $V.Gray $wV      # R ~ A*(215/255)+V*(40/255)
    $errOpaque  = MAE $R.Gray $A.Gray 1.0                  # R ~ A (opaque at rest)
    $errNoStrip = MAE $R.Gray $V.Gray 1.0                  # R ~ V (strip invisible)
    Write-Output ("R vs A*0.843+V*0.157  [RESTING TRANSLUCENCY]  MAE {0}" -f $errResting)
    Write-Output ("R vs A                [OPAQUE at rest]        MAE {0}" -f $errOpaque)
    Write-Output ("R vs V                [strip INVISIBLE]       MAE {0}" -f $errNoStrip)
    Write-Output ""
    $best = @(
        @{ n = 'RESTING TRANSLUCENCY (alpha 215)'; e = $errResting },
        @{ n = 'OPAQUE at rest';                   e = $errOpaque },
        @{ n = 'STRIP INVISIBLE';                  e = $errNoStrip }
    ) | Sort-Object { $_.e } | Select-Object -First 1
    Write-Output ("VERDICT: {0}  (MAE {1})" -f $best.n, $best.e)
    # The owner decision of 2026-08-19 (item 8, option B): the two backends
    # deliberately differ at rest. d3d11 must read the translucent blend; cpu
    # ignores the layered alpha (native children share the top-level backing
    # store) and must read OPAQUE. Neither result is a defect on its own
    # renderer, and a cpu run reading translucent would be the surprise.
    $want = if ($Renderer -eq 'd3d11') { 'RESTING TRANSLUCENCY (alpha 215)' } else { 'OPAQUE at rest' }
    if ($best.n -eq $want) { Write-Output ("PASS for renderer={0}" -f $Renderer) }
    else { Write-Output ("FAIL for renderer={0} -- expected {1}" -f $Renderer, $want) }
    Write-Output "Captures in $OutDir -- confirm by eye before believing the arithmetic."
}

if ($Mode -eq 'anim') {
    # The real gesture: chrome up and settled, pointer leaves, auto-hide (2s)
    # then the 165ms fade. Capture just the strip band every ~30ms and print
    # the mean over time -- a fade is a ramp through intermediate values, a pop
    # is one step.
    $h = Start-TraceRun @{ }
    Step-Into $h
    Park-Inside $h
    $r = New-Object TfWin+RECT
    [TfWin]::GetClientRect($h, [ref]$r) | Out-Null
    $pt = Client-Origin $h
    $w = $r.R - $r.L
    $x0 = $pt.X + [int]($w * 0.78); $y0 = $pt.Y + 6
    $bw = [int]($w * 0.20); $bh = 24
    [TfWin]::SetCursorPos(5, 5) | Out-Null
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $samples = New-Object System.Collections.ArrayList
    $keep = @{}
    while ($sw.ElapsedMilliseconds -lt 3600) {
        $bmp = New-Object System.Drawing.Bitmap $bw, $bh
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        $g.CopyFromScreen($x0, $y0, 0, 0, $bmp.Size)
        $g.Dispose()
        $band = Band-Mean $bmp
        [void]$samples.Add([pscustomobject]@{ T = $sw.ElapsedMilliseconds; Mean = $band })
        if ($samples.Count % 1 -eq 0) { $keep[$sw.ElapsedMilliseconds] = $bmp } else { $bmp.Dispose() }
        Start-Sleep -Milliseconds 25
    }
    # Print the transition: every sample whose mean moved >2 from the previous,
    # plus first and last.
    $prev = $null
    foreach ($s in $samples) {
        if ($null -eq $prev -or [Math]::Abs($s.Mean - $prev) -gt 2 -or $s -eq $samples[0] -or $s -eq $samples[$samples.Count-1]) {
            Write-Output ("t={0,5}ms  band mean {1}" -f $s.T, [Math]::Round($s.Mean,1))
            $prev = $s.Mean
        }
    }
    # Save the frames closest to the transition midpoint for eyeballing.
    $lo = ($samples | Select-Object -First 1).Mean
    $hi = ($samples | Select-Object -Last 1).Mean
    $mid = ($lo + $hi) / 2
    $closest = $samples | Sort-Object { [Math]::Abs($_.Mean - $mid) } | Select-Object -First 3
    foreach ($c in $closest) {
        if ($keep.ContainsKey($c.T)) { $keep[$c.T].Save("$OutDir\$Renderer-anim-t$($c.T).png", [System.Drawing.Imaging.ImageFormat]::Png) }
    }
    foreach ($b in $keep.Values) { $b.Dispose() }
    Write-Output ""
    Write-Output "A fade shows >=3 intermediate means between the strip's and the video's;"
    Write-Output "a pop shows one step. Mid-transition frames saved to $OutDir."
}

if ($Mode -eq 'menus') {
    $h = Start-TraceRun @{ }
    Step-Into $h
    Park-Inside $h
    Start-Sleep -Milliseconds 300
    $before = Capture-Client $h "$OutDir\$Renderer-menus-before.png"
    # Click File: the menus start at the edge pad (x=12), vertically centred in
    # the 38px strip.
    $pt = Client-Origin $h
    [TfWin]::SetCursorPos(($pt.X + 30), ($pt.Y + 19)) | Out-Null
    Start-Sleep -Milliseconds 120
    $sig = New-Object byte[] 0
    Add-Type -AssemblyName System.Windows.Forms
    [System.Windows.Forms.Cursor]::Position = New-Object System.Drawing.Point (($pt.X + 30), ($pt.Y + 19))
    # A real click through SendInput-equivalent: mouse_event via .NET is gone;
    # use the tried route -- SendKeys cannot click, so use user32 mouse_event.
    Add-Type @"
using System;
using System.Runtime.InteropServices;
public class TfClick {
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint x, uint y, uint d, UIntPtr e);
}
"@
    [TfClick]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero)  # LEFTDOWN
    Start-Sleep -Milliseconds 60
    [TfClick]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero)  # LEFTUP
    Start-Sleep -Milliseconds 500
    $after = Capture-Client $h "$OutDir\$Renderer-menus-click.png"
    # A menu is a popup below the strip: compare rows 38..300.
    $diff = 0
    $hLim = [Math]::Min(300, [Math]::Min($before.Height, $after.Height))
    for ($y = 40; $y -lt $hLim; $y += 4) {
        for ($x = 0; $x -lt [Math]::Min(400, $before.Width); $x += 4) {
            $c1 = $before.GetPixel($x, $y); $c2 = $after.GetPixel($x, $y)
            if ([Math]::Abs($c1.R - $c2.R) -gt 12 -or [Math]::Abs($c1.G - $c2.G) -gt 12) { $diff++ }
        }
    }
    $before.Dispose(); $after.Dispose()
    Write-Output ("click on File: {0} sampled pixels changed below the strip (menu open = well above 0)" -f $diff)
    [System.Windows.Forms.SendKeys]::SendWait("{ESC}")
    Start-Sleep -Milliseconds 400
    # Alt+F from a hidden strip: park outside, wait for the hide, then mnemonic.
    [TfWin]::SetCursorPos(5, 5) | Out-Null
    Start-Sleep -Milliseconds 3400
    $hidden = Capture-Client $h "$OutDir\$Renderer-menus-hidden.png"
    [System.Windows.Forms.SendKeys]::SendWait("%f")
    Start-Sleep -Milliseconds 600
    $alt = Capture-Client $h "$OutDir\$Renderer-menus-altf.png"
    $diff2 = 0
    for ($y = 0; $y -lt $hLim; $y += 4) {
        for ($x = 0; $x -lt [Math]::Min(400, $hidden.Width); $x += 4) {
            $c1 = $hidden.GetPixel($x, $y); $c2 = $alt.GetPixel($x, $y)
            if ([Math]::Abs($c1.R - $c2.R) -gt 12 -or [Math]::Abs($c1.G - $c2.G) -gt 12) { $diff2++ }
        }
    }
    $hidden.Dispose(); $alt.Dispose()
    Write-Output ("Alt+F from hidden strip: {0} sampled pixels changed (reveal + File open = well above 0)" -f $diff2)
    [System.Windows.Forms.SendKeys]::SendWait("{ESC}")
}

if ($Mode -eq 'loop') {
    $errFile = "$OutDir\$Renderer-reveal.log"
    Remove-Item $errFile -ErrorAction SilentlyContinue
    $h = Start-TraceRun @{ TRACE_REVEAL_LOG = '1' } $errFile
    Step-Into $h
    Park-Inside $h
    $markStart = (Get-Content $errFile -ErrorAction SilentlyContinue | Measure-Object -Line).Lines
    Start-Sleep -Seconds 10
    $lines = Get-Content $errFile -ErrorAction SilentlyContinue | Select-Object -Skip $markStart
    $shown  = ($lines | Where-Object { $_ -match 'chrome SHOWN' }).Count
    $hidden = ($lines | Where-Object { $_ -match 'chrome HIDDEN' }).Count
    $synth  = ($lines | Where-Object { $_ -match 'SYNTHETIC' }).Count
    Write-Output ("pointer parked INSIDE for 10s: chrome SHOWN {0}  HIDDEN {1}  synthetic-filtered {2}" -f $shown, $hidden, $synth)
    Write-Output "PASS requires SHOWN 0 / HIDDEN 0 -- the chrome stays up under a parked pointer."
    # And one clean hide when the pointer leaves.
    $mark2 = (Get-Content $errFile | Measure-Object -Line).Lines
    [TfWin]::SetCursorPos(5, 5) | Out-Null
    Start-Sleep -Seconds 6
    $lines2 = Get-Content $errFile | Select-Object -Skip $mark2
    $shown2  = ($lines2 | Where-Object { $_ -match 'chrome SHOWN' }).Count
    $hidden2 = ($lines2 | Where-Object { $_ -match 'chrome HIDDEN' }).Count
    Write-Output ("pointer parked OUTSIDE for 6s:  chrome SHOWN {0}  HIDDEN {1}" -f $shown2, $hidden2)
    Write-Output "PASS requires exactly HIDDEN 1 and SHOWN 0 -- one fade-out, no blink cycle."
}
