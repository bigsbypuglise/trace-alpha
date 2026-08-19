# Set the PRIMARY display's refresh rate, keeping the current resolution unless
# told otherwise. Built for the refresh-cap investigation (2026-08-19,
# docs/mp4-scrub-threadripper.md): the scrub present chain is throttled by the
# display draining the flip queue, so the one experiment that confirms or
# refutes it on the dev box is running the same selftest on the same clip at
# 60Hz and at the panel's native 239.999Hz.
#
# -List prints every mode the display advertises and changes nothing.
# The change is DYNAMIC (no CDS_UPDATEREGISTRY): it does not persist across a
# reboot, and the caller is expected to restore the original rate explicitly --
# this script prints the rate that was in force so the restore value is always
# in the transcript. It refuses a mode the display does not advertise rather
# than letting Windows pick a near miss.
#
# NOTE the printed rate is EnumDisplaySettings' integer (240, 60), not the
# QueryDisplayConfig rational (239.999) -- refresh.ps1 is the instrument for the
# exact rate; this one only needs the integer to name a mode.
#
# All logic lives in C#: a DEVMODE is a struct, and PowerShell's boxing loses
# field writes on structs (the first version passed dmSize=0 and read 0x0@0Hz).
param(
    [int]$Hz = 0,
    [int]$Width = 0,
    [int]$Height = 0,
    [switch]$List
)

Add-Type -TypeDefinition @"
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

public static class RefreshSetter {
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Ansi)]
    public struct DEVMODE {
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmDeviceName;
        public short dmSpecVersion, dmDriverVersion, dmSize, dmDriverExtra;
        public int dmFields;
        public int dmPositionX, dmPositionY;
        public int dmDisplayOrientation, dmDisplayFixedOutput;
        public short dmColor, dmDuplex, dmYResolution, dmTTOption, dmCollate;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmFormName;
        public short dmLogPixels;
        public int dmBitsPerPel, dmPelsWidth, dmPelsHeight, dmDisplayFlags, dmDisplayFrequency;
        public int dmICMMethod, dmICMIntent, dmMediaType, dmDitherType;
        public int dmReserved1, dmReserved2, dmPanningWidth, dmPanningHeight;
    }

    [DllImport("user32.dll", CharSet = CharSet.Ansi)]
    static extern bool EnumDisplaySettings(string dev, int mode, ref DEVMODE dm);
    [DllImport("user32.dll", CharSet = CharSet.Ansi)]
    static extern int ChangeDisplaySettings(ref DEVMODE dm, uint flags);

    const int ENUM_CURRENT_SETTINGS = -1;
    const int DM_BITSPERPEL = 0x40000, DM_PELSWIDTH = 0x80000,
              DM_PELSHEIGHT = 0x100000, DM_DISPLAYFREQUENCY = 0x400000;

    static DEVMODE Fresh() {
        DEVMODE dm = new DEVMODE();
        dm.dmSize = (short)Marshal.SizeOf(typeof(DEVMODE));
        return dm;
    }

    public static string Current() {
        DEVMODE dm = Fresh();
        if (!EnumDisplaySettings(null, ENUM_CURRENT_SETTINGS, ref dm)) return "FAIL";
        return dm.dmPelsWidth + "x" + dm.dmPelsHeight + " @ " + dm.dmDisplayFrequency + "Hz";
    }

    public static string[] Modes() {
        var seen = new List<string>();
        DEVMODE dm = Fresh();
        for (int i = 0; EnumDisplaySettings(null, i, ref dm); i++) {
            if (dm.dmBitsPerPel != 32) continue;
            string key = dm.dmPelsWidth + "x" + dm.dmPelsHeight + " @ " + dm.dmDisplayFrequency + "Hz";
            if (!seen.Contains(key)) seen.Add(key);
        }
        return seen.ToArray();
    }

    // 0 = ok; 1 = mode not advertised; otherwise the ChangeDisplaySettings code.
    public static int Set(int width, int height, int hz) {
        DEVMODE cur = Fresh();
        if (!EnumDisplaySettings(null, ENUM_CURRENT_SETTINGS, ref cur)) return -100;
        int wantW = width > 0 ? width : cur.dmPelsWidth;
        int wantH = height > 0 ? height : cur.dmPelsHeight;

        // Find the advertised mode exactly. A made-up frequency can silently
        // succeed at something else, which would put a wrong denominator under
        // every figure measured after it.
        DEVMODE dm = Fresh();
        bool found = false;
        for (int i = 0; EnumDisplaySettings(null, i, ref dm); i++) {
            if (dm.dmBitsPerPel == 32 && dm.dmPelsWidth == wantW
                && dm.dmPelsHeight == wantH && dm.dmDisplayFrequency == hz) { found = true; break; }
        }
        if (!found) return 1;

        dm.dmFields = DM_BITSPERPEL | DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY;
        int rc = ChangeDisplaySettings(ref dm, 0);
        return rc == 0 ? 0 : (rc == 1 ? -101 : rc);
    }
}
"@

Write-Output ("current  {0}" -f [RefreshSetter]::Current())

if ($List) {
    [RefreshSetter]::Modes() | ForEach-Object { Write-Output ("  mode {0}" -f $_) }
    exit 0
}

if ($Hz -le 0) { Write-Output "nothing to do (pass -Hz N, or -List)"; exit 0 }

$rc = [RefreshSetter]::Set($Width, $Height, $Hz)
if ($rc -eq 1) { Write-Output "FAIL: the display does not advertise that mode (use -List)"; exit 1 }
if ($rc -ne 0) { Write-Output "FAIL: ChangeDisplaySettings returned $rc"; exit 1 }

Start-Sleep -Milliseconds 1500
Write-Output ("now      {0}" -f [RefreshSetter]::Current())
