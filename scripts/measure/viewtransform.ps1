# Drives the spec phase 10 view-transform actions and captures the result.
#
# Rotate Left and Rotate Right are Ctrl+L / Ctrl+R; the flips and Reset are menu
# items with no shortcut, so they are reached as `%e;h`, `%e;v` and `%e;t`
# (Alt+E then the mnemonic). Read the `display WxH ... view ...` field out of the
# capture: it carries the post-transform fit, the reduction taps and the
# transform in force, which is the whole of what this phase changed.
#
# TAKE CROSS-BACKEND DIFFS IN BAR MODE (-Bar). The floating overlay is composited
# over the video, so its fade state lands inside the band being compared and read
# 9.1% of it on the first attempt -- nothing to do with the transform. Pair this
# with scripts/measure/banddiff.ps1.
#
# Takes focus with a CLICK, not SetForegroundWindow -- Windows refuses foreground
# activation to a background process and the failure is silent, which is what
# made the phase 9 lifecycle leg unable to pass.
param(
    [Parameter(Mandatory=$true)][string]$Clip,
    [Parameter(Mandatory=$true)][string]$Tag,
    # Semicolon-separated, because an ARRAY parameter does not bind through
    # `powershell -File` -- it arrives as a positional argument and the script
    # rejects it. One string, split here.
    [string]$Keys = "",
    [string]$Renderer = "d3d11",
    # Seconds to idle before capturing. The floating transport fades after 2s,
    # and a capture taken while it is mid-fade differs between two runs for a
    # reason that has nothing to do with what is being compared -- it was 9.1%
    # of the video band on the first backend A/B here.
    [double]$SettleSeconds = 0.7,
    # Use the docked bar. For a cross-backend diff this is not optional: the
    # floating overlay is composited over the video, so its fade state lands
    # inside the band being compared and dominated the first measurement here
    # at 9.1%. The overlay's own backend agreement was settled at phases 4-6.
    [switch]$Bar
)
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Add-Type @"
using System;using System.Runtime.InteropServices;
public class VT {
  [StructLayout(LayoutKind.Sequential)] public struct RC { public int L,T,R,B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RC r);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f,uint x,uint y,uint d,IntPtr e);
}
"@
$repo = "C:\Users\andre\Documents\Claude_Cowork\Trace_Windows"
Set-Location $repo
Get-Process Trace -EA SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 700
$env:TRACE_RENDERER = $Renderer
if ($Bar) { $env:TRACE_TRANSPORT_BAR = "1" }
Start-Process -FilePath "build\app\Release\Trace.exe" -ArgumentList "`"$Clip`""
$env:TRACE_RENDERER = ""
$env:TRACE_TRANSPORT_BAR = ""
Start-Sleep -Seconds 6

$p = Get-Process Trace | Select-Object -First 1
$rc = New-Object VT+RC; [VT]::GetWindowRect($p.MainWindowHandle,[ref]$rc) | Out-Null
[VT]::SetForegroundWindow($p.MainWindowHandle) | Out-Null
Start-Sleep -Milliseconds 300
if ([VT]::GetForegroundWindow() -ne $p.MainWindowHandle) {
    [System.Windows.Forms.Cursor]::Position = New-Object System.Drawing.Point (($rc.L+400),($rc.T+14))
    Start-Sleep -Milliseconds 200
    [VT]::mouse_event(0x0002,0,0,0,[IntPtr]::Zero); Start-Sleep -Milliseconds 70
    [VT]::mouse_event(0x0004,0,0,0,[IntPtr]::Zero); Start-Sleep -Milliseconds 400
}
if ([VT]::GetForegroundWindow() -ne $p.MainWindowHandle) { Write-Output "FOCUS FAIL"; exit 1 }

foreach ($k in ($Keys -split ';' | Where-Object { $_ -ne '' })) { [System.Windows.Forms.SendKeys]::SendWait($k); Start-Sleep -Milliseconds 600 }
Start-Sleep -Seconds $SettleSeconds

$rc2 = New-Object VT+RC; [VT]::GetWindowRect($p.MainWindowHandle,[ref]$rc2) | Out-Null
$w = $rc2.R-$rc2.L; $h = $rc2.B-$rc2.T
$b = New-Object System.Drawing.Bitmap $w, $h
$g = [System.Drawing.Graphics]::FromImage($b)
$g.CopyFromScreen($rc2.L,$rc2.T,0,0,$b.Size); $g.Dispose()
$b.Save("$env:TEMP\claude\vt-$Tag.png"); $b.Dispose()
Write-Output "saved vt-$Tag.png  win ${w}x${h}  renderer=$Renderer  keys='$Keys'"
