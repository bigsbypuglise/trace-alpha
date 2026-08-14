# Build the minimal MinGW/GCC LGPL FFmpeg that Trace ships.
#
# WHY THIS EXISTS. The vcpkg dependency is built `--toolchain=msvc`, and on the
# 7680x4320 ProRes 4444 XQ plate a GCC build of the SAME avcodec version
# (62.28.102) decodes 18% faster -- 29.3 fps of pure decode against 24.8. The
# gain is the toolchain and nothing else: FFmpeg master (avcodec 63.8.100)
# measures the same as n8.1 to within noise.
#
# WHY NOT A PREBUILT. BtbN's `win64-lgpl-shared` reproduces the gain and is
# bit-identical in Trace, but it statically links libaom, dav1d, vulkan/shaderc
# and much else Trace never calls: the DLL set goes 17.3 MB -> 104.3 MB, which is
# ~87 MB on a portable ZIP and a large set of third-party notices for code that
# never executes. It is retained as the validated performance CONTROL, not as the
# shipping artifact.
#
# WHAT "MINIMAL" MEANS HERE. Not a smaller codec set -- that would remove formats
# Trace supports today. It means **vcpkg's exact feature set, built with GCC**,
# minus the two libraries Trace demonstrably does not use. Every native FFmpeg
# decoder stays in, every external library stays out, so the set of openable files
# is unchanged by construction rather than by audit.
#
# EVERYTHING IS PINNED. Compiler, assembler and FFmpeg source are fixed versions
# with recorded SHA256s, so this is reproducible on a CI runner that has none of
# them installed.

[CmdletBinding()]
param(
    [string]$Root = "C:\tw_ff",
    [string]$OutDir = "",
    # Skip the downloads when the working tree already has them.
    [switch]$NoFetch,
    [int]$Jobs = 0
)

$ErrorActionPreference = "Stop"

# ---- pinned inputs ---------------------------------------------------------
# Bump deliberately, never implicitly: a different compiler is a different
# binary, and the whole reason this file exists is that the compiler matters.
$PIN = @{
    GccUrl    = "https://github.com/brechtsanders/winlibs_mingw/releases/download/16.2.0posix-14.0.0-ucrt-r1/winlibs-x86_64-posix-seh-gcc-16.2.0-mingw-w64ucrt-14.0.0-r1.zip"
    GccSha    = "C1F52294597C0B73786B2A78EB5D176D"   # first 32 hex of SHA256
    NasmUrl   = "https://www.nasm.us/pub/nasm/releasebuilds/2.16.03/win64/nasm-2.16.03-win64.zip"
    NasmSha   = "3EE4782247BCB874378D02F7EAB4E294"
    FFmpegUrl = "https://github.com/FFmpeg/FFmpeg/archive/refs/tags/n8.1.2.zip"
    FFmpegSha = "C6660EEE2507EF9644C7DFF3B91DF97C"
    FFmpegDir = "FFmpeg-n8.1.2"
    # MSYS2 IS REQUIRED AND IS NOT INTERCHANGEABLE WITH GIT FOR WINDOWS' BASH.
    # FFmpeg's `makedef` receives every object file of a library as a single argv,
    # and libavcodec's list overflows Git bash's command-line limit: the build
    # dies with "Object does not exist: libavcodec/h261_parser." -- a filename
    # truncated mid-word. Disabling encoders and muxers halves the list and it
    # still overflows, so this is a hard limit rather than something to trim under.
    #
    # A DATED tag, not `nightly-x86_64`/`latest`: that asset is rewritten in place,
    # so a hash pin against it would turn every upstream refresh into a red build
    # for a dependency nobody changed.
    Msys2Url  = "https://github.com/msys2/msys2-installer/releases/download/2026-06-11/msys2-base-x86_64-20260611.sfx.exe"
    Msys2Sha  = "C105946E64E08F099AC0E4647461CE76"
}

# n8.1.x is chosen so the sonames match what vcpkg produces -- avcodec-62,
# avformat-62, avutil-60, swresample-6, swscale-9. That makes the replacement a
# DLL swap rather than a Trace rebuild, which is what lets it be A/B'd against the
# current dependency without changing anything else at the same time.

if (-not $OutDir) { $OutDir = Join-Path $Root "out" }
$dl = Join-Path $Root "dl"
New-Item -ItemType Directory -Force $Root, $dl | Out-Null
if ($Jobs -le 0) { $Jobs = [int]$env:NUMBER_OF_PROCESSORS }

function Fetch($url, $name, $shaPrefix) {
    $path = Join-Path $dl $name
    if (-not (Test-Path $path)) {
        Write-Host "fetching $name"
        Invoke-WebRequest -Uri $url -OutFile $path -UseBasicParsing -TimeoutSec 1800
    }
    $h = (Get-FileHash $path -Algorithm SHA256).Hash
    if ($shaPrefix) {
        if ($h.Substring(0, 32) -ne $shaPrefix) {
            throw "$name SHA256 mismatch: got $($h.Substring(0,32)) expected $shaPrefix"
        }
        Write-Host ("  {0,-12} ok  sha256 {1}..." -f $name, $h.Substring(0, 32))
    } else {
        # An unpinned input is a hole in reproducibility, so say so loudly and
        # print the value to paste back into $PIN rather than quietly proceeding.
        Write-Host ("  {0,-12} UNPINNED - record this: {1}" -f $name, $h.Substring(0, 32))
    }
    return $path
}

if (-not $NoFetch) {
    $g = Fetch $PIN.GccUrl    "gcc.zip"    $PIN.GccSha
    $n = Fetch $PIN.NasmUrl   "nasm.zip"   $PIN.NasmSha
    $f = Fetch $PIN.FFmpegUrl "ffmpeg.zip" $PIN.FFmpegSha
    foreach ($p in @(@{z=$g;d="gcc"}, @{z=$n;d="nasm"}, @{z=$f;d="ffmpeg"})) {
        $target = Join-Path $Root $p.d
        if (-not (Test-Path $target)) { Expand-Archive -Path $p.z -DestinationPath $target -Force }
    }
    # msys2 ships a self-extracting archive, which is the one form that needs no
    # tar/xz/zstd on a machine that may have none of them.
    $msysDir = Join-Path $Root "msys2"
    if (-not (Test-Path (Join-Path $msysDir "msys64\usr\bin\bash.exe"))) {
        $m = Fetch $PIN.Msys2Url "msys2.sfx.exe" $PIN.Msys2Sha
        New-Item -ItemType Directory -Force $msysDir | Out-Null
        & $m -y "-o$msysDir" | Out-Null
        if (-not (Test-Path (Join-Path $msysDir "msys64\usr\bin\bash.exe"))) {
            throw "msys2 self-extract produced no bash at $msysDir\msys64\usr\bin"
        }
    }
}

$gccBin  = (Get-ChildItem (Join-Path $Root "gcc")  -Recurse -Filter gcc.exe  | Select-Object -First 1).DirectoryName
$nasmBin = (Get-ChildItem (Join-Path $Root "nasm") -Recurse -Filter nasm.exe | Select-Object -First 1).DirectoryName
$src     = Join-Path $Root "ffmpeg\$($PIN.FFmpegDir)"
if (-not (Test-Path $src)) { throw "FFmpeg source not found at $src" }

# The POSIX shell FFmpeg's configure needs, and it MUST be msys2.
#
# Git for Windows' bash is not a substitute, and this was learned the expensive
# way: FFmpeg's `makedef` receives every object file of a library as one argv, and
# libavcodec's list overflows Git bash's command-line limit. The build dies with
# "Object does not exist: libavcodec/h261_parser." -- a filename truncated
# mid-word, which reads like a corrupt tree rather than a shell limit. Disabling
# encoders and muxers halves the object list and it STILL overflows.
#
# Order: the pinned msys2 this script downloads, then any msys2 vcpkg has already
# fetched (avoids a 50 MB download on a box that has one). Git bash is
# deliberately absent from the list.
$bashCandidates = @(Join-Path $Root "msys2\msys64\usr\bin\bash.exe")
if ($env:VCPKG_ROOT) {
    $bashCandidates += (Get-ChildItem (Join-Path $env:VCPKG_ROOT "downloads\tools\msys2") -Directory -ErrorAction SilentlyContinue |
        ForEach-Object { Join-Path $_.FullName "usr\bin\bash.exe" })
}
$bashCandidates += (Get-ChildItem "C:\vcpkg\downloads\tools\msys2" -Directory -ErrorAction SilentlyContinue |
    ForEach-Object { Join-Path $_.FullName "usr\bin\bash.exe" })
$bashCandidates = @($bashCandidates | Where-Object { $_ -and (Test-Path $_) })
if (-not $bashCandidates) {
    throw "no msys2 bash found. Run without -NoFetch so the pinned msys2 is downloaded; " +
          "Git for Windows' bash cannot build libavcodec (argv limit) and is not used."
}
# PICK BY CAPABILITY, NOT BY ORDER: prefer an msys2 that already has `make`.
# msys2-base does not ship one and vcpkg's msys2 does, so this uses whichever
# install is actually complete and only falls back to installing make when no
# candidate has it. It also removes a network round trip on any box or runner
# where vcpkg has already been provisioned.
$bash = $bashCandidates | Where-Object { Test-Path (Join-Path (Split-Path $_ -Parent) "make.exe") } | Select-Object -First 1
if (-not $bash) { $bash = $bashCandidates[0] }
$shellBin = Split-Path $bash -Parent

# `-c`, NOT `-lc`. A login shell sources /etc/profile, which REBUILDS PATH from
# msys2's own defaults and throws away the gcc/nasm/make entries set just below --
# configure then fails with "gcc is unable to create an executable file", which
# reads like a broken compiler rather than a lost PATH. MSYS2_PATH_TYPE=inherit
# says the same thing to the msys2 runtime for any child that does start a login
# shell.
$env:MSYS2_PATH_TYPE = 'inherit'
$env:PATH = "$gccBin;$nasmBin;$shellBin;$env:PATH"

# MSYS2 RUNS A ONE-TIME SETUP ON ITS FIRST LAUNCH and announces it on stderr. With
# $ErrorActionPreference = 'Stop', PowerShell 5.1 promotes any native stderr line
# to a terminating error, so that banner alone aborted the build the first time a
# freshly extracted msys2 was used. Get it out of the way here, deliberately, with
# errors demoted -- and twice, because the first pass exits before setup finishes.
foreach ($i in 1..2) {
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    & $bash -c "exit 0" 2>&1 | Out-Null
    $ErrorActionPreference = $prev
}

# `make`, AND IT MUST BE MSYS2's, NOT winlibs' mingw32-make.
#
# This is what the "Object does not exist: libavcodec/h261_parser." failure
# actually was, and it took three wrong diagnoses to find: it is not bash, and it
# is not the size of the object list. mingw32-make is a NATIVE Windows make, so it
# spawns each recipe through cmd.exe and inherits its ~32K command-line limit;
# FFmpeg hands `makedef` every object file of a library as one argv and libavcodec
# blows straight through it, arriving truncated mid-filename. msys2's make spawns
# through the MSYS runtime, which passes long argv between MSYS processes without
# going near cmd.exe -- which is why the very first build of this dependency, run
# against vcpkg's msys2, worked.
#
# msys2-base does not include it, so install it. Deliberately NOT pinned: make is
# a build driver, not an input to the compiled output, and the things that DO
# determine the binary -- compiler, assembler, source -- are all pinned above.
$msysMake = Join-Path $shellBin "make.exe"
if (-not (Test-Path $msysMake)) {
    Write-Host "installing make into the pinned msys2"
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $pacLog = Join-Path $Root "logs\pacman.log"
    New-Item -ItemType Directory -Force (Split-Path $pacLog -Parent) | Out-Null
    & $bash -c "pacman -Sy --noconfirm --needed make" *> $pacLog
    $ErrorActionPreference = $prev
    if (-not (Test-Path $msysMake)) {
        # Print it rather than swallow it. The first CI run failed here with the
        # output discarded, which made a one-line diagnosis into a round trip.
        if (Test-Path $pacLog) { Get-Content $pacLog -Tail 25 | ForEach-Object { Write-Host "  $_" } }
        throw "pacman did not produce make.exe at $msysMake (log: $pacLog)"
    }
}

Write-Host "gcc   : $gccBin"
Write-Host "nasm  : $nasmBin"
Write-Host "bash  : $bash"
Write-Host "src   : $src"
Write-Host "out   : $OutDir"

# ---- configure -------------------------------------------------------------
# vcpkg's own flags, with three deliberate differences and no others:
#   * the toolchain is GCC rather than MSVC -- the entire point;
#   * avdevice and avfilter are OFF, because Trace includes neither header and
#     ships neither DLL;
#   * --disable-programs, because Trace ships no ffmpeg/ffplay/ffprobe.
# Everything else -- every native decoder, every muxer, no external libraries,
# runtime CPU detection, w32threads, the Windows hwaccels, schannel -- matches
# what ships today, so the set of files Trace can open cannot change.
$flags = @(
    "--prefix=$($OutDir -replace '\\','/')"
    "--target-os=mingw32"
    "--arch=x86_64"
    "--enable-pic"
    "--disable-doc"
    "--disable-debug"
    "--enable-runtime-cpudetect"   # keeps SIMD dispatch at runtime; do not remove
    "--disable-autodetect"         # nothing from the host may leak into the build
    "--enable-w32threads"          # NOT pthreads: avoids a libwinpthread DLL in the ZIP
    "--enable-d3d11va"
    "--enable-d3d12va"
    "--enable-dxva2"
    "--enable-mediafoundation"
    "--enable-schannel"
    "--disable-programs"
    "--disable-avdevice"
    "--disable-avfilter"
    # TRACE NEVER WRITES A FRAME OR A FILE. It decodes and demuxes; there is no
    # encode path anywhere in src/ (Copy Current Frame goes through swscale and
    # QImage, not an encoder). Dropping encoders and muxers removes nothing Trace
    # can OPEN -- decoders, demuxers, parsers and bitstream filters all stay --
    # so "every currently supported format" is preserved exactly.
    #
    # It also fixes a real build failure. FFmpeg's `makedef` receives every object
    # file as one argv, and libavcodec's list overflows the command-line limit
    # under Git for Windows' bash: the build dies with "Object does not exist:
    # libavcodec/faan", which is `faandct.o` truncated mid-word. Halving the
    # object count keeps the build working under either shell rather than only
    # under an msys2 that a CI runner is not entitled to have.
    "--disable-encoders"
    "--disable-muxers"
    "--enable-avcodec"
    "--enable-avformat"
    "--enable-swresample"
    "--enable-swscale"
    "--disable-static"
    "--enable-shared"
    "--enable-optimizations"
    "--enable-asm"
    "--enable-x86asm"
    # Static libgcc keeps the portable ZIP to av*/sw* DLLs only. Without it the
    # build depends on libgcc_s_seh-1.dll, which would have to be shipped and
    # would be a new redistributable with its own notice.
    "--extra-ldflags=-static-libgcc"
)


$srcU = ($src -replace '\\','/') -replace '^([A-Za-z]):', '/$1'

# EVERY STEP REDIRECTS INSIDE BASH, and that is not cosmetic. PowerShell 5.1
# wraps a native command's stderr in an ErrorRecord and sets $? false even when
# the process exited 0 -- so a build that merely emits warnings (FFmpeg emits
# plenty) aborts the script and looks exactly like a compile failure. It did:
# the first run of this script "failed" in dashenc.c on a build that had in fact
# completed. Let bash own the redirection and read $LASTEXITCODE, which is the
# process's real answer.
$logDir = Join-Path $Root "logs"
New-Item -ItemType Directory -Force $logDir | Out-Null
function Run($label, $cmd) {
    $log = Join-Path $logDir "$label.log"
    $logU = ($log -replace '\\','/') -replace '^([A-Za-z]):', '/$1'
    Write-Host "`n--- $label ---"
    # Errors demoted around the native call for the same reason as the msys2
    # first-run banner above: stderr is not failure, $LASTEXITCODE is.
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    & $bash -c "$cmd > '$logU' 2>&1"
    $code = $LASTEXITCODE
    $ErrorActionPreference = $prev
    if ($code -ne 0) {
        Write-Host "--- last 25 lines of $log ---"
        Get-Content $log -Tail 25 | ForEach-Object { Write-Host "  $_" }
        throw "$label failed ($code); full log at $log"
    }
    $errs = (Get-Content $log | Select-String -Pattern 'error:').Count
    Write-Host ("  ok  ({0} lines, {1} 'error:' matches)" -f (Get-Content $log | Measure-Object -Line).Lines, $errs)
}

Run "configure" ("cd '$srcU' && ./configure " + ($flags -join ' '))
Run "make"      "cd '$srcU' && make -j$Jobs"
Run "install"   "cd '$srcU' && make install"

# ---- MSVC import libraries -------------------------------------------------
# mingw emits `libavcodec.dll.a`, which link.exe cannot consume. FFmpeg also
# emits a .def per shared library on Windows; lib.exe turns that into a real
# MSVC import library. Generated from the .def rather than from dumpbin output
# so the export list comes from the build rather than from a parse of it.
# lib.exe comes from whichever Visual Studio edition is installed. The first
# version hard-coded the Community path, which is right on this box and wrong on
# a runner (Enterprise). vswhere is the supported way to ask.
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vcvars = $null
if (Test-Path $vswhere) {
    $vsRoot = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($vsRoot) { $vcvars = Join-Path $vsRoot "VC\Auxiliary\Build\vcvars64.bat" }
}
if (-not $vcvars -or -not (Test-Path $vcvars)) {
    $vcvars = @("Enterprise", "Professional", "Community", "BuildTools") |
        ForEach-Object { "$env:ProgramFiles\Microsoft Visual Studio\2022\$_\VC\Auxiliary\Build\vcvars64.bat" } |
        Where-Object { Test-Path $_ } | Select-Object -First 1
}
if (-not $vcvars) { throw "vcvars64.bat not found; cannot build MSVC import libraries" }
Write-Host "vcvars: $vcvars"
$binOut = Join-Path $OutDir "bin"
$libOut = Join-Path $OutDir "lib"
$defs = Get-ChildItem $binOut -Filter *.def -ErrorAction SilentlyContinue
if (-not $defs) { $defs = Get-ChildItem $libOut -Filter *.def -ErrorAction SilentlyContinue }
if (-not $defs) { throw "no .def files produced; cannot build MSVC import libraries" }

foreach ($d in $defs) {
    # avcodec-62.def -> avcodec.lib, which is the name CMake's find_library wants.
    $stem = ($d.BaseName -replace '-\d+$', '')
    $out  = Join-Path $libOut "$stem.lib"
    $cmd  = '"' + $vcvars + '" >nul && lib /nologo /def:"' + $d.FullName + '" /machine:x64 /out:"' + $out + '"'
    # cmd.exe BY FULL PATH: msys2's usr/bin is on PATH by now and contains a `cmd`
    # shim, which PowerShell refuses to run ("Cannot run a document in the middle
    # of a pipeline"). Nothing about the import libraries is msys2's business.
    $prevEA = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    & "$env:SystemRoot\System32\cmd.exe" /c $cmd | Out-Null
    $ErrorActionPreference = $prevEA
    # The file existing is the test, not the exit code: vcvars64.bat writes
    # harmless chatter to stderr on some installs ("vswhere.exe is not
    # recognized"), which PowerShell 5.1 would otherwise promote to a failure.
    if (-not (Test-Path $out)) { throw "lib.exe did not produce $out" }
    Write-Host ("  import lib {0,-16} <- {1}" -f "$stem.lib", $d.Name)
}

Write-Host "`n--- result ---"
Get-ChildItem $binOut -Filter *.dll | ForEach-Object { "{0,-20} {1,10:N0} bytes" -f $_.Name, $_.Length }
$total = (Get-ChildItem $binOut -Filter *.dll | Measure-Object Length -Sum).Sum
Write-Host ("TOTAL {0:N1} MB" -f ($total / 1MB))
