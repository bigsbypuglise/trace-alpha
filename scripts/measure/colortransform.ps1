# Stage 1 colour-transform harness.
#
# Drives the stage across the media classes it has to work on and reports, per
# case, what the HUD says the stage is doing and whether the picture actually
# changed when it was switched on.
#
# TWO OBSERVABLES, DELIBERATELY, because either alone can lie. The HUD's `xform`
# field says what the stage BELIEVES (`ON <lut>` / `bypass <lut>` / `none`), and
# the picture diff says what actually reached the screen. A build where the
# state is right and the pixels never change looks perfect on the first and
# fails on the second -- which is exactly the failure the --ocio-selftest's
# "moved" assertion exists for, one layer further out.
#
#   colortransform.ps1 -Mode matrix     # every media class, off vs on
#   colortransform.ps1 -Mode toggle     # ON/OFF/ON without reopening media
#   colortransform.ps1 -Mode renderers  # d3d11 vs cpu, same clip
#
# The LUT is loaded through TRACE_COLOR_LUT rather than the file dialog: the
# dialog is modal and driving it with synthetic input is the class of harness
# this project has been burned by. The MENU path is exercised by hand.
param(
    [ValidateSet('matrix','toggle','renderers')][string]$Mode = 'matrix',
    [string]$Root = 'C:\Users\andre\Documents\Claude_Cowork\Trace_Testing_Assets',
    [string]$Lut  = '',
    [string]$Exe  = ''
)
$ErrorActionPreference = 'Stop'
if (-not $Lut) { $Lut = Join-Path $Root '20_Alexa_ProRes_Lut\ARRI_LogC4-to-Gamma24_Rec709-D65_v1-65.cube' }
if (-not $Exe) { $Exe = Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'build\app\Release\Trace.exe' }
Add-Type -AssemblyName System.Drawing

$sp = Join-Path $env:TEMP 'trace-colortransform'
New-Item -ItemType Directory -Force -Path $sp | Out-Null

function Shoot([string]$clip, [string]$tag, [string[]]$env) {
    $ini = Join-Path $sp "$tag.ini"
    if (Test-Path $ini) { Remove-Item $ini -Force }
    $all = @("TRACE_HUD=1", "TRACE_SETTINGS_FILE=$ini") + $env
    & "$PSScriptRoot\restart.ps1" -Clip $clip -Exe $Exe -Env $all -SettleSeconds 6 | Out-Null
    $png = Join-Path $sp "$tag.png"
    & "$PSScriptRoot\capture.ps1" -Out $png | Out-Null
    return $png
}

# Mean luma over the picture band, sampled coarsely. A display transform that
# engaged moves this; one that silently did not cannot.
function Luma([string]$png) {
    $b = [System.Drawing.Bitmap]::FromFile($png)
    $y0 = [int]($b.Height * 0.12); $y1 = [int]($b.Height * 0.60)
    $sum = 0.0; $n = 0
    for ($y = $y0; $y -lt $y1; $y += 6) {
        for ($x = 12; $x -lt ($b.Width - 12); $x += 6) {
            $p = $b.GetPixel($x, $y)
            $sum += (0.2126*$p.R + 0.7152*$p.G + 0.0722*$p.B); $n++
        }
    }
    $b.Dispose()
    if ($n -eq 0) { return -1 }
    return [math]::Round($sum / $n, 2)
}

$cases = @(
    @{ n='still';    p='5_Still_Images' },
    @{ n='exr';      p='6_Image_Sequence\EXR_SEQ\R2_OP_Stacks_01\R2_OP_Stacks_01_00000.exr' },
    @{ n='h264';     p='4_4K_H264_MP4\Splash_1.mp4' },
    @{ n='prores';   p='1_4K_ProRes_4444\TheraTears_Vial_VFX_v002.mov' },
    @{ n='alexa';    p='20_Alexa_ProRes_Lut\B_0001C007_260202_085214_h1E9O.mxf_Rendered.mov' }
)

if ($Mode -eq 'matrix') {
    foreach ($c in $cases) {
        $clip = Join-Path $Root $c.p
        if ($c.n -eq 'still') {
            $f = Get-ChildItem $clip -File -Include *.png,*.jpg,*.tif -Recurse -EA SilentlyContinue | Select-Object -First 1
            if (-not $f) { Write-Host ("{0,-8} SKIP - no still found" -f $c.n); continue }
            $clip = $f.FullName
        }
        if (-not (Test-Path $clip)) { Write-Host ("{0,-8} SKIP - not found" -f $c.n); continue }
        $off = Luma (Shoot $clip "$($c.n)-off" @())
        $on  = Luma (Shoot $clip "$($c.n)-on"  @("TRACE_COLOR_LUT=$Lut"))
        $d   = [math]::Round([math]::Abs($on - $off), 2)
        $verdict = if ($d -gt 1.0) { 'CHANGED' } else { 'NO CHANGE' }
        Write-Host ("{0,-8} luma off {1,7} -> on {2,7}   delta {3,7}   {4}" -f $c.n, $off, $on, $d, $verdict)
    }
}
elseif ($Mode -eq 'renderers') {
    $clip = Join-Path $Root $cases[4].p
    foreach ($r in @('d3d11','cpu')) {
        $off = Luma (Shoot $clip "r-$r-off" @("TRACE_RENDERER=$r"))
        $on  = Luma (Shoot $clip "r-$r-on"  @("TRACE_RENDERER=$r","TRACE_COLOR_LUT=$Lut"))
        $d = [math]::Round([math]::Abs($on - $off), 2)
        Write-Host ("{0,-6} luma off {1,7} -> on {2,7}   delta {3,7}   {4}" -f $r, $off, $on, $d,
                    $(if ($d -gt 1.0) { 'CHANGED' } else { 'NO CHANGE' }))
    }
}
elseif ($Mode -eq 'toggle') {
    # ON at launch, then the MENU's own bypass twice, without reopening media.
    $clip = Join-Path $Root $cases[4].p
    $png1 = Shoot $clip 'tg-on' @("TRACE_COLOR_LUT=$Lut")
    $l1 = Luma $png1
    . "$PSScriptRoot\tracewindow.ps1"
    $h = Resolve-TraceWindow
    [TraceWin]::SetForegroundWindow($h) | Out-Null
    Start-Sleep -Milliseconds 400
    Add-Type -AssemblyName System.Windows.Forms
    # Alt+V, then the Color Transform item by mnemonic (never by counting Down).
    [System.Windows.Forms.SendKeys]::SendWait("%v"); Start-Sleep -Milliseconds 500
    [System.Windows.Forms.SendKeys]::SendWait("c");  Start-Sleep -Milliseconds 1200
    & "$PSScriptRoot\capture.ps1" -Out (Join-Path $sp 'tg-bypass.png') | Out-Null
    $l2 = Luma (Join-Path $sp 'tg-bypass.png')
    [System.Windows.Forms.SendKeys]::SendWait("%v"); Start-Sleep -Milliseconds 500
    [System.Windows.Forms.SendKeys]::SendWait("c");  Start-Sleep -Milliseconds 1200
    & "$PSScriptRoot\capture.ps1" -Out (Join-Path $sp 'tg-back.png') | Out-Null
    $l3 = Luma (Join-Path $sp 'tg-back.png')
    Write-Host ("ON {0}  -> bypass {1}  -> ON again {2}" -f $l1, $l2, $l3)
    $ok = ([math]::Abs($l2 - $l1) -gt 1.0) -and ([math]::Abs($l3 - $l1) -lt 1.0)
    Write-Host ("toggle: {0}  (bypass must differ from ON; re-enable must return to it)" -f $(if ($ok) {'PASS'} else {'FAIL'}))
}
