# Is Trace's downscale a real reduction, or is it undersampling?
#
# Plan step 9 is "GPU scaling", and section 9 names its honest target: the Step
# landing still converts full-resolution and lets the sampler scale it, a 6x
# downscale in the validation window. The D3D11 sampler is
# MIN_MAG_MIP_LINEAR over textures with MipLevels = 1, so it takes a 2x2 tap per
# output pixel however far it is reducing -- at 6x that reads 4 texels of every
# 36 and discards the rest. Whether that is visible is a measurement, not an
# assertion, and nothing in the repo had made it.
#
# The instrument deliberately does NOT compare Trace against Trace. Comparing the
# drag preview to the landing says whether the picture changes on release, which
# matters, but both are Trace's own output and they could be wrong together --
# and section 20.3 already spent a session on a CPU-vs-GPU difference where both
# sides were 2x2 taps. So the references come from ffmpeg, outside the app:
#
#   ref_area    swscale `area` to the exact size Trace draws at. Every source
#               pixel contributes. This is what a correct reduction looks like.
#   ref_point   swscale `neighbor` to the same size. One source pixel per output
#               pixel, everything else thrown away. This is what undersampling
#               looks like, at its worst.
#
# Trace's captured video rect is then placed between them. Near `area` means the
# scaling is fine and step 9 has no quality case; near `point` means it is
# undersampling and the picture is losing detail it was given.
#
# Two metrics, because one of them cannot see the failure:
#
#   delta       mean and max per-channel difference against each reference.
#               Says which reference the picture resembles.
#   hf          mean |Laplacian|, the high-frequency energy. This is the metric
#               that identifies aliasing rather than just difference: a correct
#               reduction has LOWER hf than an undersampled one at the same size,
#               because undersampling converts detail it cannot represent into
#               spurious local contrast. Section 9 measured "local contrast
#               within 0.7%" and concluded there was no visible defect -- local
#               contrast is exactly the statistic aliasing can preserve while
#               moving the detail around, which is why hf is measured per pixel
#               against a neighbourhood instead.
#
# `-Sensitivity` runs the reference pair alone and reports how far apart they
# are. Run it FIRST on any new material: on a source with little fine detail
# area and point agree, so the test cannot resolve anything and a pass would mean
# nothing. Smooth CG (the 4K milk splash) reads a few tenths; real texture reads
# several units.

param(
    # A PNG of the same size as the references -- normally Trace's cropped video
    # rect. Omit with -Sensitivity.
    [string]$Shot,
    [Parameter(Mandatory = $true)][string]$RefArea,
    [Parameter(Mandatory = $true)][string]$RefPoint,
    [switch]$Sensitivity
)

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;

public class Filt {
    // Luma and the three channels, unpacked once. GetPixel on a 640x360 image is
    // 230k marshalled calls per metric and made the first version of this script
    // take longer than the capture it was measuring.
    public static byte[] Load(string path, out int w, out int h) {
        using (Bitmap b = new Bitmap(path)) {
            w = b.Width; h = b.Height;
            byte[] o = new byte[w * h * 3];
            BitmapData d = b.LockBits(new Rectangle(0, 0, w, h),
                                      ImageLockMode.ReadOnly, PixelFormat.Format24bppRgb);
            for (int y = 0; y < h; y++) {
                IntPtr row = (IntPtr)(d.Scan0.ToInt64() + (long)y * d.Stride);
                Marshal.Copy(row, o, y * w * 3, w * 3);
            }
            b.UnlockBits(d);
            return o;   // BGR order, which is fine: every metric here is symmetric.
        }
    }

    public static void Delta(byte[] a, byte[] b, int n, out double mean, out int max) {
        double sum = 0; int mx = 0;
        for (int i = 0; i < n; i++) {
            int d = Math.Abs((int)a[i] - (int)b[i]);
            sum += d;
            if (d > mx) mx = d;
        }
        mean = sum / n; max = mx;
    }

    // Mean |Laplacian| of luma. A 4-neighbour Laplacian is the cheapest operator
    // that responds to per-pixel detail rather than to overall contrast, which is
    // the distinction this whole script turns on.
    public static double Hf(byte[] p, int w, int h) {
        double sum = 0; long n = 0;
        for (int y = 1; y < h - 1; y++) {
            for (int x = 1; x < w - 1; x++) {
                sum += Math.Abs(4.0 * L(p, w, x, y)
                                - L(p, w, x - 1, y) - L(p, w, x + 1, y)
                                - L(p, w, x, y - 1) - L(p, w, x, y + 1));
                n++;
            }
        }
        return sum / n;
    }

    static double L(byte[] p, int w, int x, int y) {
        int i = (y * w + x) * 3;
        return 0.114 * p[i] + 0.587 * p[i + 1] + 0.299 * p[i + 2];   // BGR
    }
}
"@ -ReferencedAssemblies System.Drawing

function Get-Img([string]$path) {
    $w = 0; $h = 0
    $px = [Filt]::Load((Resolve-Path $path).Path, [ref]$w, [ref]$h)
    return @{ Px = $px; W = $w; H = $h; N = $px.Length }
}

$ra = Get-Img $RefArea
$rp = Get-Img $RefPoint
if ($ra.W -ne $rp.W -or $ra.H -ne $rp.H) {
    Write-Output "FAIL: references differ in size"; exit 1
}

$m = 0.0; $mx = 0
[Filt]::Delta($ra.Px, $rp.Px, $ra.N, [ref]$m, [ref]$mx)
$hfA = [Filt]::Hf($ra.Px, $ra.W, $ra.H)
$hfP = [Filt]::Hf($rp.Px, $rp.W, $rp.H)

Write-Output ("size            {0}x{1}" -f $ra.W, $ra.H)
Write-Output ("area vs point   delta mean {0:F2} max {1}" -f $m, $mx)
Write-Output ("hf  area {0:F3}   point {1:F3}   (point/area {2:F2}x)" -f $hfA, $hfP, ($hfP / $hfA))

if ($Sensitivity) {
    # A test whose two references agree cannot report anything about a third
    # picture, and would pass silently.
    if ($m -lt 1.0) {
        Write-Output "SENSITIVITY: TOO LOW - this source has little fine detail; pick other material"
        exit 2
    }
    Write-Output "SENSITIVITY: usable"
    exit 0
}

if (-not $Shot) { Write-Output "FAIL: -Shot required without -Sensitivity"; exit 1 }
$s = Get-Img $Shot
if ($s.W -ne $ra.W -or $s.H -ne $ra.H) {
    Write-Output ("FAIL: shot is {0}x{1}, references are {2}x{3}" -f $s.W, $s.H, $ra.W, $ra.H)
    exit 1
}

$mA = 0.0; $mxA = 0; $mP = 0.0; $mxP = 0
[Filt]::Delta($s.Px, $ra.Px, $s.N, [ref]$mA, [ref]$mxA)
[Filt]::Delta($s.Px, $rp.Px, $s.N, [ref]$mP, [ref]$mxP)
$hfS = [Filt]::Hf($s.Px, $s.W, $s.H)

Write-Output ""
Write-Output ("shot vs area    delta mean {0:F2} max {1}" -f $mA, $mxA)
Write-Output ("shot vs point   delta mean {0:F2} max {1}" -f $mP, $mxP)
Write-Output ("hf  shot {0:F3}   (area {1:F3}, point {2:F3})" -f $hfS, $hfA, $hfP)

# Where the shot sits on the area..point axis, by hf. 0 = a correct reduction,
# 1 = worst-case undersampling. Reported as a position rather than a verdict
# because the honest reading of a middling number is "partly filtered", and a
# threshold here would hide that.
$span = $hfP - $hfA
if ([Math]::Abs($span) -gt 1e-6) {
    $pos = ($hfS - $hfA) / $span
    Write-Output ("position        {0:F2} on the area(0)..point(1) axis" -f $pos)
}
Write-Output ""
Write-Output "NOTE: absolute colour need not match -- the shot comes off the screen"
Write-Output "      after Trace's own matrix and range handling, the references off"
Write-Output "      ffmpeg's. The signal here is STRUCTURAL: which reference the"
Write-Output "      detail resembles, not which one the levels do."
