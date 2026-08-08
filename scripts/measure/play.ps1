param([int]$Seconds = 9)

Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Fg { [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h); }
"@

$p = Get-Process -Name Trace -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $p) { Write-Output "no window"; exit 1 }
[Fg]::SetForegroundWindow($p.MainWindowHandle) | Out-Null
Start-Sleep -Milliseconds 400

$sh = New-Object -ComObject WScript.Shell
$sh.SendKeys(" ")
Write-Output "space sent; playing for $Seconds s"
Start-Sleep -Seconds $Seconds
