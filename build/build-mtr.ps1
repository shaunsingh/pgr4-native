# Builds MTR-PGR4 (MissingThePixel/MTR-PGR4) from source on Windows. Re-runnable: each stage
# skips work that is already done (pass -Force to redo the SDK build or codegen).
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File build-mtr.ps1 [-Stage sdk,launcher,...]
#
# Stages, in order (default: all):
#   prep      clone MTR-PGR4 into $Root\MTR-PGR4 (an existing checkout is left as is).
#   sdk       scripts\setup-sdk.ps1: ReXGlue v0.10.0 (f5337cd) + patches\rexglue-v0.10.0.patch +
#             libmspack patch + sdk-overrides, built Release with clang-cl into sdk-install.
#             No game data.
#   launcher  launcher\build-launcher.ps1 (csc.exe, .NET Framework 4.8): out\launcher\MTR-PGR4.exe.
#             No game data.
#   game      needs the disc: scripts\build.ps1 -GameDataRoot $Game. It writes pgr4_local.toml and
#             geometry-wars\gw_local.toml (game path filled in), runs `rexglue codegen` on both
#             (default.xex and gw.xex, the base disc executables, not the title update), applies
#             tools\apply_game_patches.ps1 and builds out\pgr4\pgr4_recompiled.exe and
#             out\geometry-wars\gw_recompiled.exe, then the launcher again.
#   install   lays the build out like the release ZIP under $RunDir: MTR-PGR4.exe at the top,
#             runtime\pgr4_recompiled.exe + rexruntime.dll + rexgpu-xenos.dll, and
#             runtime\geometry-wars\ likewise. Existing userdata, logs and launcher settings stay.
#
# Generated C++ stays in the checkout (generated\, geometry-wars\generated\); it is game-derived
# and must never be committed anywhere.
param(
    [string[]]$Stage = 'all',                       # comma list; -File passes "a,b" as one string
    [string]$Root = 'C:\recomp\builds\pgr4',
    [string]$Game = 'C:\recomp\dumps\pgr4\game',   # extracted disc: default.xex, gw.xex, Game\, UI\ ...
    [string]$RunDir = '',                           # default: $Root\run\MTR-PGR4
    [string]$Ref = '',                              # optional commit to check out (fresh clone only)
    [int]$Jobs = 16,
    [switch]$Force
)
$ErrorActionPreference = 'Stop'
$Repo = Join-Path $Root 'MTR-PGR4'
$Sdk = Join-Path $Repo 'sdk-install'
if (-not $RunDir) { $RunDir = Join-Path $Root 'run\MTR-PGR4' }
$Vcvars = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
$Llvm = 'C:\Program Files\LLVM\bin'
$Stage = @($Stage | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
$bad = $Stage | Where-Object { $_ -notin 'all', 'prep', 'sdk', 'launcher', 'game', 'install' }
if ($bad) { throw "unknown stage(s): $($bad -join ', ')" }
$all = $Stage -contains 'all'
function Want($s) { $all -or ($Stage -contains $s) }

function Step([string]$Name, [scriptblock]$Body) {
    $t = [Diagnostics.Stopwatch]::StartNew()
    Write-Host "=== $Name"
    & $Body
    Write-Host ("=== $Name done in {0:N0} s" -f $t.Elapsed.TotalSeconds)
}

# Imports the x64 MSVC environment (INCLUDE, LIB, Windows SDK) into this PowerShell session and
# puts clang-cl and ninja first on PATH, which is what a "Developer PowerShell" provides.
#
# MTR's scripts configure with clang-cl and no target flags. ReXGlue's CMake adds GNU-style
# options (-ffp-model=strict, -fno-char8_t) that clang-cl 21 drops as unknown, and only its
# CMakePresets add -march=x86-64-v2; rexglue_apply_target_settings adds -msse4.1 only when not
# MSVC. Without them src\core\memory.cpp fails: "_mm_shuffle_epi8 requires target feature
# 'ssse3'". CFLAGS/CXXFLAGS seed CMAKE_<LANG>_FLAGS on a fresh configure, so set the
# equivalents here for the SDK and the game builds alike. The FP model is precise, not ReXGlue's
# strict, which blocks most FP optimization of the generated code (see native/build.sh).
$ClangClFlags = '/clang:-march=x86-64-v2 /clang:-ffp-model=precise /clang:-fno-math-errno /Zc:char8_t-'
function Use-VcEnv {
    if ($env:VSCMD_ARG_TGT_ARCH -ne 'x64') {
        if (-not (Test-Path $Vcvars)) { throw "vcvars64.bat not found: $Vcvars" }
        cmd /c "`"$Vcvars`" >nul && set" | ForEach-Object {
            if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($matches[1])" -Value $matches[2] }
        }
        $env:PATH = "$Llvm;$env:PATH"
    }
    if (-not (Get-Command clang-cl -ErrorAction SilentlyContinue)) { throw 'clang-cl not on PATH' }
    if (-not (Get-Command ninja -ErrorAction SilentlyContinue)) { throw 'ninja not on PATH' }
    $env:CFLAGS = $ClangClFlags
    $env:CXXFLAGS = $ClangClFlags
}

# A build directory configured before the flags above keeps its old cache; drop it.
function Reset-StaleCache([string]$BuildDir) {
    $cache = Join-Path $BuildDir 'CMakeCache.txt'
    if ((Test-Path $cache) -and -not (Select-String -Path $cache -SimpleMatch 'x86-64-v2' -Quiet)) {
        Write-Host "removing stale $cache (configured without $ClangClFlags)"
        Remove-Item $cache
    }
}

# Runs one of the repo's own PowerShell scripts in this session (so it sees the VC environment).
function Invoke-RepoScript([string]$Script, [hashtable]$Params) {
    Write-Host "> $Script $(($Params.GetEnumerator() | ForEach-Object { "-$($_.Key) $($_.Value)" }) -join ' ')"
    & $Script @Params
}

New-Item -ItemType Directory -Force -Path $Root | Out-Null

if (Want 'prep') {
    Step 'prep' {
        if (-not (Test-Path (Join-Path $Repo '.git'))) {
            git -c core.longpaths=true clone https://github.com/MissingThePixel/MTR-PGR4.git $Repo
            if ($LASTEXITCODE -ne 0) { throw 'clone failed' }
            if ($Ref) { git -C $Repo checkout $Ref; if ($LASTEXITCODE -ne 0) { throw "checkout $Ref failed" } }
        }
        Write-Host "MTR-PGR4 at $(git -C $Repo log -1 --format='%h %ad %s' --date=short)"
    }
}

if (Want 'sdk') {
    Step 'sdk' {
        $rexglue = Join-Path $Sdk 'bin\rexglue.exe'
        if ((Test-Path $rexglue) -and -not $Force) {
            Write-Host "patched SDK already installed: $rexglue"
        } else {
            Use-VcEnv
            Reset-StaleCache (Join-Path $Repo 'thirdparty\rexglue-sdk\out\build\mtr-release')
            Invoke-RepoScript (Join-Path $Repo 'scripts\setup-sdk.ps1') @{ Jobs = $Jobs }
        }
        & $rexglue --version
    }
}

if (Want 'launcher') {
    Step 'launcher' {
        Invoke-RepoScript (Join-Path $Repo 'launcher\build-launcher.ps1') @{}
        Get-Item (Join-Path $Repo 'out\launcher\MTR-PGR4.exe') | Format-Table Name, Length, LastWriteTime | Out-Host
    }
}

if (Want 'game') {
    Step 'game' {
        foreach ($f in 'default.xex', 'gw.xex') {
            if (-not (Test-Path (Join-Path $Game $f))) {
                throw "missing $Game\$f; extract the disc's files there first"
            }
        }
        $stamp = Join-Path $Repo 'generated\default\codegen.build.stamp'
        if ($Force -and (Test-Path $stamp)) { Remove-Item $stamp }
        Use-VcEnv
        Reset-StaleCache (Join-Path $Repo 'out\pgr4')
        Reset-StaleCache (Join-Path $Repo 'out\geometry-wars')
        Invoke-RepoScript (Join-Path $Repo 'scripts\build.ps1') @{ GameDataRoot = $Game; SdkPrefix = $Sdk; Jobs = $Jobs }
        Get-Item (Join-Path $Repo 'out\pgr4\pgr4_recompiled.exe'), (Join-Path $Repo 'out\geometry-wars\gw_recompiled.exe') |
            Format-Table Name, Length, LastWriteTime | Out-Host
    }
}

if (Want 'install') {
    Step 'install' {
        $pgr = Join-Path $Repo 'out\pgr4\pgr4_recompiled.exe'
        $gwExe = Join-Path $Repo 'out\geometry-wars\gw_recompiled.exe'
        $launcher = Join-Path $Repo 'out\launcher\MTR-PGR4.exe'
        foreach ($f in $pgr, $gwExe, $launcher, (Join-Path $Sdk 'bin\rexruntime.dll'), (Join-Path $Sdk 'bin\rexgpu-xenos.dll')) {
            if (-not (Test-Path $f)) { throw "missing $f; run the sdk, launcher and game stages first" }
        }
        $runtime = Join-Path $RunDir 'runtime'
        $gwDir = Join-Path $runtime 'geometry-wars'
        New-Item -ItemType Directory -Force -Path $runtime, $gwDir | Out-Null
        Copy-Item $launcher (Join-Path $RunDir 'MTR-PGR4.exe') -Force
        Copy-Item $pgr $runtime -Force
        Copy-Item $gwExe $gwDir -Force
        foreach ($dll in 'rexruntime.dll', 'rexgpu-xenos.dll') {
            Copy-Item (Join-Path $Sdk "bin\$dll") $runtime -Force
            Copy-Item (Join-Path $Sdk "bin\$dll") $gwDir -Force
        }
        Get-ChildItem $RunDir -Recurse -File -Include *.exe, *.dll | Format-Table FullName, Length | Out-Host
        Write-Host "Run: $RunDir\MTR-PGR4.exe (launcher) or runtime\pgr4_recompiled.exe directly"
    }
}
