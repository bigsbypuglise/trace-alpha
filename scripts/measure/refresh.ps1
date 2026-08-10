# What is the display's ACTUAL refresh period?
#
# GATE E snaps presents to the vblank grid, so the grid's period decides what
# cadence is even achievable. 24.000fps needs an integer number of refreshes per
# frame, and whether it gets one depends on a number nobody has measured:
#
#   240.000 Hz -> 10.000 refreshes/frame, exact
#   120.000 Hz ->  5.000, exact
#    60.000 Hz ->  2.500, the 2:3 cadence every player has
#   239.760 Hz ->  9.990, a 9-refresh frame every ~100 frames
#
# 239.76 is 240000/1001 and is a very common "240Hz" mode. plan section 22.8
# recorded this panel as 239Hz, which is what an integer-rounded API reports for
# BOTH 239.76 and 240.0 -- so the nominal number cannot answer the question and
# that is the whole reason this script exists.
#
# Three numbers are printed and they are not interchangeable:
#
#   nominal      EnumDisplaySettings dmDisplayFrequency. An integer. Rounds
#                239.76 to 239 and 240.0 to 240, so it is only useful as the
#                thing to disagree with.
#   dwm period   DwmGetCompositionTimingInfo qpcRefreshPeriod, one sample. This
#                is the value GATE E's phase source would use directly.
#   measured     (cRefresh delta) / (qpcVBlank delta) over a long baseline. The
#                trustworthy one: it is an integer count of refreshes over a
#                high-resolution time span, so its error falls as the baseline
#                grows and it cannot inherit a rounding the API did.
#
# If `dwm period` and `measured` disagree by more than a few ppm, the phase
# source is reporting a rounded period and the grid snap must be built on the
# measured one instead. That disagreement is a finding, not noise.
#
# Run with the Trace window closed; nothing here needs it.

param(
    # Baseline for the measured figure. 10s at 240Hz is ~2400 refreshes, so a
    # one-refresh miscount is 400ppm; 30s is ~130ppm. Long enough to separate
    # 239.76 from 240.00 (1000ppm apart) many times over.
    [int]$Seconds = 20
)

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

public static class TraceRefresh
{
    // DWM_TIMING_INFO is declared as a raw buffer with hand-read offsets rather
    // than as a struct. The struct is 320 bytes on x64 with four internal
    // alignment pads, and cbSize must match the FULL size or the call returns
    // 0x88980090 -- which is what a first attempt at a shortened struct did.
    // Reading four fields off known offsets cannot get that wrong.
    public const int TIMING_INFO_SIZE = 320;
    public const int OFF_CBSIZE           = 0;    // UINT32
    public const int OFF_RATE_REFRESH_NUM = 4;    // UINT32
    public const int OFF_RATE_REFRESH_DEN = 8;    // UINT32
    public const int OFF_QPC_REFRESH_PERIOD = 16; // UINT64 (8-aligned after a 4-byte pad)
    public const int OFF_QPC_VBLANK       = 32;   // UINT64
    public const int OFF_CREFRESH         = 40;   // UINT64

    [DllImport("dwmapi.dll")]
    public static extern int DwmGetCompositionTimingInfo(IntPtr hwnd, IntPtr info);

    [DllImport("dwmapi.dll")]
    public static extern int DwmIsCompositionEnabled(out bool enabled);

    // QueryDisplayConfig is the only API that reports the refresh rate as an
    // exact RATIONAL. EnumDisplaySettings rounds it to an integer, which is the
    // whole problem: 239.76 and 240.00 both report 239 or 240 and they are a
    // thousand ppm apart.
    //
    // Read off byte offsets rather than declared structs for the same reason as
    // the DWM buffer above. DISPLAYCONFIG_PATH_INFO is 72 bytes:
    //   sourceInfo.adapterId  0..8    .id 8..12   .modeInfoIdx 12..16
    //   sourceInfo.statusFlags   16..20
    //   targetInfo.adapterId 20..28   .id 28..32  .modeInfoIdx 32..36
    //   .outputTechnology    36..40   .rotation 40..44   .scaling 44..48
    //   .refreshRate.Numerator   48   .Denominator 52
    //   .scanLineOrdering    56..60   .targetAvailable 60..64  .statusFlags 64..68
    //   flags                68..72
    //
    // A first pass omitted sourceInfo.statusFlags, which put every target field
    // 4 bytes early -- it read scaling (1) as the numerator and the numerator
    // (239999) as the denominator, and it under-allocated the path buffer by
    // 4 bytes per element. The tell was a plausible-looking "1/239999".
    public const int PATH_INFO_SIZE = 72;
    public const int MODE_INFO_SIZE = 64;
    public const int OFF_TARGET_AVAILABLE = 60;
    public const int OFF_REFRESH_NUM = 48;
    public const int OFF_REFRESH_DEN = 52;
    public const uint QDC_ONLY_ACTIVE_PATHS = 2;

    [DllImport("user32.dll")]
    public static extern int GetDisplayConfigBufferSizes(uint flags, out uint numPaths, out uint numModes);

    [DllImport("user32.dll")]
    public static extern int QueryDisplayConfig(uint flags, ref uint numPaths, IntPtr paths,
                                                ref uint numModes, IntPtr modes, IntPtr topologyId);

    // Returns one "num/den" string per active path.
    public static string[] ActiveRefreshRates()
    {
        uint nPaths, nModes;
        int rc = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, out nPaths, out nModes);
        if (rc != 0) throw new Exception("GetDisplayConfigBufferSizes failed: " + rc);

        IntPtr paths = Marshal.AllocHGlobal((int)nPaths * PATH_INFO_SIZE);
        IntPtr modes = Marshal.AllocHGlobal((int)nModes * MODE_INFO_SIZE);
        try
        {
            rc = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, ref nPaths, paths, ref nModes, modes, IntPtr.Zero);
            if (rc != 0) throw new Exception("QueryDisplayConfig failed: " + rc);

            var outp = new System.Collections.Generic.List<string>();
            for (int i = 0; i < nPaths; i++)
            {
                int b = i * PATH_INFO_SIZE;
                uint num = (uint)Marshal.ReadInt32(paths, b + OFF_REFRESH_NUM);
                uint den = (uint)Marshal.ReadInt32(paths, b + OFF_REFRESH_DEN);
                if (den == 0) continue;
                outp.Add(num + "/" + den);
            }
            return outp.ToArray();
        }
        finally { Marshal.FreeHGlobal(paths); Marshal.FreeHGlobal(modes); }
    }

    [DllImport("kernel32.dll")]
    public static extern bool QueryPerformanceFrequency(out long freq);

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Auto)]
    public struct DevMode
    {
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmDeviceName;
        public ushort dmSpecVersion; public ushort dmDriverVersion; public ushort dmSize;
        public ushort dmDriverExtra; public uint dmFields;
        public int dmPositionX; public int dmPositionY;
        public uint dmDisplayOrientation; public uint dmDisplayFixedOutput;
        public short dmColor; public short dmDuplex; public short dmYResolution;
        public short dmTTOption; public short dmCollate;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmFormName;
        public ushort dmLogPixels; public uint dmBitsPerPel;
        public uint dmPelsWidth; public uint dmPelsHeight;
        public uint dmDisplayFlags; public uint dmDisplayFrequency;
        public uint dmICMMethod; public uint dmICMIntent; public uint dmMediaType;
        public uint dmDitherType; public uint dmReserved1; public uint dmReserved2;
        public uint dmPanningWidth; public uint dmPanningHeight;
    }

    [DllImport("user32.dll", CharSet = CharSet.Auto)]
    public static extern bool EnumDisplaySettings(string devName, int modeNum, ref DevMode dm);

    // Returns { rateNum, rateDen, qpcRefreshPeriod, qpcVBlank, cRefresh }.
    // The HWND is ignored on Windows 8 and later -- this is ONE composition
    // clock for the desktop, not a per-monitor one, which is the limitation
    // plan section 24.4 records against using it as GATE E's phase source.
    public static long[] Sample()
    {
        IntPtr buf = Marshal.AllocHGlobal(TIMING_INFO_SIZE);
        try
        {
            for (int i = 0; i < TIMING_INFO_SIZE; i += 4) Marshal.WriteInt32(buf, i, 0);
            Marshal.WriteInt32(buf, OFF_CBSIZE, TIMING_INFO_SIZE);
            int hr = DwmGetCompositionTimingInfo(IntPtr.Zero, buf);
            if (hr != 0) throw new Exception(String.Format("DwmGetCompositionTimingInfo failed: 0x{0:X8}", hr));
            return new long[] {
                (uint)Marshal.ReadInt32(buf, OFF_RATE_REFRESH_NUM),
                (uint)Marshal.ReadInt32(buf, OFF_RATE_REFRESH_DEN),
                Marshal.ReadInt64(buf, OFF_QPC_REFRESH_PERIOD),
                Marshal.ReadInt64(buf, OFF_QPC_VBLANK),
                Marshal.ReadInt64(buf, OFF_CREFRESH)
            };
        }
        finally { Marshal.FreeHGlobal(buf); }
    }
}
'@

# Index names for the array Sample() returns.
$RATE_NUM = 0; $RATE_DEN = 1; $PERIOD = 2; $VBLANK = 3; $CREFRESH = 4

function Get-Timing { return [TraceRefresh]::Sample() }

$qpf = 0L
[void][TraceRefresh]::QueryPerformanceFrequency([ref]$qpf)

# --- nominal, from the display mode -------------------------------------------
$dm = New-Object TraceRefresh+DevMode
$dm.dmSize = [uint16][System.Runtime.InteropServices.Marshal]::SizeOf($dm)
$nominal = "(unavailable)"
$geom = ""
if ([TraceRefresh]::EnumDisplaySettings("\\.\DISPLAY1", -1, [ref]$dm)) {   # ENUM_CURRENT_SETTINGS
    $nominal = "{0} Hz" -f $dm.dmDisplayFrequency
    $geom = "{0}x{1}" -f $dm.dmPelsWidth, $dm.dmPelsHeight
}

# --- exact rational, from QueryDisplayConfig ----------------------------------
$exact = @()
try { $exact = [TraceRefresh]::ActiveRefreshRates() } catch { $exact = @() }

Write-Output ""
Write-Output "  display       $geom"
Write-Output "  nominal       $nominal   (integer; cannot separate 239.76 from 240.00)"
if ($exact.Count -gt 0) {
    foreach ($r in $exact) {
        $parts = $r.Split('/')
        $hz = [double]$parts[0] / [double]$parts[1]
        Write-Output ("  exact rate    {0} = {1:F6} Hz   (QueryDisplayConfig, active path)" -f $r, $hz)
    }
} else {
    Write-Output "  exact rate    (unavailable)"
}
Write-Output ("  QPC frequency {0:N0} Hz" -f $qpf)

# --- the DWM composition clock, if it exists ----------------------------------
# This is what plan section 24.4 proposed as GATE E's renderer-independent phase
# source. It is queried here rather than assumed, because the whole point of the
# script is to confirm rather than inherit.
$compositionOk = $false
$dwmHz = 0.0
$compEnabled = $false
[void][TraceRefresh]::DwmIsCompositionEnabled([ref]$compEnabled)
$a = $null
try { $a = Get-Timing; $compositionOk = $true } catch { $dwmError = $_.Exception.Message }

if ($compositionOk) {
    $dwmPeriodMs = ($a[$PERIOD] / $qpf) * 1000.0
    $dwmHz = 1000.0 / $dwmPeriodMs
    $rateHz = if ($a[$RATE_DEN] -ne 0) { $a[$RATE_NUM] / $a[$RATE_DEN] } else { 0 }
    Write-Output ("  dwm rate      {0}/{1} = {2:F4} Hz" -f $a[$RATE_NUM], $a[$RATE_DEN], $rateHz)
    Write-Output ("  dwm period    {0:F6} ms  = {1:F4} Hz   (qpcRefreshPeriod, one sample)" -f $dwmPeriodMs, $dwmHz)
} else {
    Write-Output ""
    Write-Output "  dwm timing    UNAVAILABLE on this machine"
    Write-Output ("                composition enabled = {0}" -f $compEnabled)
    Write-Output ("                {0}" -f $dwmError)
    Write-Output "                DwmGetCompositionTimingInfo is deprecated and can refuse even with"
    Write-Output "                composition on. It is therefore NOT a dependable renderer-independent"
    Write-Output "                phase source -- see plan section 24.4. Phase has to come from the"
    Write-Output "                swapchain (IDXGISwapChain::GetFrameStatistics), which is d3d11 only."
}

Write-Output ""
Write-Output "  measuring over ${Seconds}s ..."

# --- long-baseline measurement ------------------------------------------------
# cRefresh is an integer refresh counter and qpcVBlank is the QPC time of the
# vblank it names, so the pair is an exact count over an exact span. No sleep
# accuracy is involved: the endpoints come from the OS, not from the timer that
# decided when to sample them.
$measuredHz = 0.0
if ($compositionOk) {
    $start = Get-Timing
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    while ($sw.Elapsed.TotalSeconds -lt $Seconds) { Start-Sleep -Milliseconds 250 }
    $end = Get-Timing

    $dRefresh = [double]($end[$CREFRESH] - $start[$CREFRESH])
    $dTicks   = [double]($end[$VBLANK] - $start[$VBLANK])
    if ($dTicks -gt 0 -and $dRefresh -gt 0) {
        $measuredHz = $dRefresh / ($dTicks / $qpf)
        $measuredMs = 1000.0 / $measuredHz
        $ppm = if ($dwmHz -ne 0) { (($measuredHz - $dwmHz) / $dwmHz) * 1e6 } else { 0 }
        Write-Output ("  measured      {0:F6} ms  = {1:F4} Hz   ({2:N0} refreshes over {3:F2}s)" -f `
            $measuredMs, $measuredHz, $dRefresh, ($dTicks / $qpf))
        Write-Output ("  dwm vs measured   {0:F1} ppm" -f $ppm)
    } else {
        Write-Output "  measured      FAILED -- cRefresh or qpcVBlank did not advance."
    }
} else {
    Write-Output "  measured      SKIPPED -- no composition clock to count refreshes with."
}

# The ratio table below needs a rate. Prefer the measured one; fall back to the
# exact rational, which is not a phase source but is an exact PERIOD and is all
# the table needs.
if ($measuredHz -le 0 -and $exact.Count -gt 0) {
    $parts = $exact[0].Split('/')
    $measuredHz = [double]$parts[0] / [double]$parts[1]
    Write-Output ("                using the QueryDisplayConfig rational instead: {0:F6} Hz" -f $measuredHz)
}
Write-Output ""
if ($measuredHz -le 0) { exit 1 }

# --- what it means for the cadence GATE E can deliver -------------------------
Write-Output "  refreshes per frame at this rate:"
foreach ($fps in @(23.976023976, 24.0, 25.0, 29.97002997, 30.0, 60.0)) {
    $rpf = $measuredHz / $fps
    $nearest = [Math]::Round($rpf)
    $errPpm = if ($nearest -gt 0) { (($rpf - $nearest) / $nearest) * 1e6 } else { 0 }
    # How many frames before the accumulated error costs a whole refresh -- i.e.
    # the period of the residual beat the DISPLAY imposes, which GATE E cannot
    # remove and must not be blamed for.
    $beat = if ([Math]::Abs($rpf - $nearest) -gt 1e-9) { 1.0 / [Math]::Abs($rpf - $nearest) } else { 0 }
    $verdict = if ($beat -eq 0) { "exact" }
               elseif ([Math]::Abs($rpf - $nearest) -gt 0.2) { "non-integer ratio; pulldown cadence is the display's, not ours" }
               else { "{0:N0}-frame residual beat ({1:F0} ppm)" -f $beat, $errPpm }
    Write-Output ("    {0,12:F3} fps -> {1,7:F4} refreshes   {2}" -f $fps, $rpf, $verdict)
}
Write-Output ""
