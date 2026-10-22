# Builds an instrumented copy of MTR-PGR4 for the draw-path measurement (DrawCounter) on the
# Windows build machine.
# The existing MTR build under $Root\MTR-PGR4 is only read, never modified.
#
#   pwsh -NoProfile -ExecutionPolicy Bypass -File build.ps1 [-Stage prep,sdk,game,install] [-Jobs 6]
#
# Stages, in order (default: all):
#   prep     robocopy $Root\MTR-PGR4 (source, patched SDK source; no build dirs, no generated
#            code) to $Dest\MTR-PGR4, apply rexglue-drawcount.patch to the SDK copy, add
#            pgr4_drawcount_hooks.cpp to the game target.
#   sdk      configure + build + install the SDK copy (clang-cl, Release) into $Dest\sdk-install.
#   game     codegen default.xex with the copy's manifest, MTR's game patches, build
#            out\pgr4\pgr4_recompiled.exe (Geometry Wars and the launcher are not built).
#   install  $Dest\run\runtime: pgr4_recompiled.exe, rexruntime.dll, rexgpu-xenos.dll.
#
# Run: see run.ps1 beside this script. The SDK side logs with --gpu_stream_stats
# [--gpu_stream_stats_file=PATH]; the app side is picked up automatically.
param(
    [string[]]$Stage = 'all',
    [string]$Root = 'C:\recomp\builds\pgr4',
    [string]$Dest = '',                              # default: $Root\instrumented
    [string]$Game = 'C:\recomp\dumps\pgr4\game',
    [int]$Jobs = 6,
    [switch]$Force
)
$ErrorActionPreference = 'Stop'
if (-not $Dest) { $Dest = Join-Path $Root 'instrumented' }
$Src = Join-Path $Root 'MTR-PGR4'
$Repo = Join-Path $Dest 'MTR-PGR4'
$SdkSrc = Join-Path $Repo 'thirdparty\rexglue-sdk'
$SdkBuild = Join-Path $SdkSrc 'out\build\drawcount-release'
$Sdk = Join-Path $Dest 'sdk-install'
$Patch = Join-Path $PSScriptRoot 'rexglue-drawcount.patch'
$Hooks = Join-Path $PSScriptRoot 'pgr4_drawcount_hooks.cpp'
$Vcvars = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
$Llvm = 'C:\Program Files\LLVM\bin'
$Stage = @($Stage | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
$bad = $Stage | Where-Object { $_ -notin 'all', 'prep', 'sdk', 'game', 'install' }
if ($bad) { throw "unknown stage(s): $($bad -join ', ')" }
function Want($s) { ($Stage -contains 'all') -or ($Stage -contains $s) }

function Step([string]$Name, [scriptblock]$Body) {
    $t = [Diagnostics.Stopwatch]::StartNew()
    Write-Host "=== $Name"
    & $Body
    Write-Host ("=== $Name done in {0:N0} s" -f $t.Elapsed.TotalSeconds)
}

# Same environment as build-mtr.ps1 (see the comment there for the clang-cl flags).
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

if (Want 'prep') {
    Step 'prep' {
        if (-not (Test-Path (Join-Path $Src 'thirdparty\rexglue-sdk\.git'))) {
            throw "missing $Src with its patched SDK; run build-mtr.ps1 -Stage prep,sdk first"
        }
        New-Item -ItemType Directory -Force -Path $Dest | Out-Null
        $exclude = @('out', 'sdk-install', 'generated', 'geometry-wars\generated', 'thirdparty\rexglue-sdk\out') |
            ForEach-Object { Join-Path $Src $_ }
        robocopy $Src $Repo /E /XD @exclude /NFL /NDL /NJH /NP /R:1 /W:1 | Out-Host
        if ($LASTEXITCODE -ge 8) { throw "robocopy failed ($LASTEXITCODE)" }
        $global:LASTEXITCODE = 0

        & git -C $SdkSrc apply --check --reverse $Patch 2>$null
        if ($LASTEXITCODE -eq 0) {
            Write-Host 'rexglue-drawcount.patch already applied'
        } else {
            git -C $SdkSrc apply --check $Patch
            if ($LASTEXITCODE -ne 0) { throw 'rexglue-drawcount.patch does not apply to the MTR-patched SDK copy' }
            git -C $SdkSrc apply $Patch
            if ($LASTEXITCODE -ne 0) { throw 'git apply failed' }
            Write-Host 'applied rexglue-drawcount.patch'
        }

        Copy-Item $Hooks (Join-Path $Repo 'src\pgr4_drawcount_hooks.cpp') -Force
        $cml = Join-Path $Repo 'CMakeLists.txt'
        $text = [IO.File]::ReadAllText($cml)
        if ($text -notmatch 'pgr4_drawcount_hooks\.cpp') {
            if ($text -notmatch '(?m)^(\s*)src/pgr4_patches\.cpp\r?$') { throw 'CMakeLists.txt: src/pgr4_patches.cpp line not found' }
            $text = $text -replace '(?m)^(\s*)src/pgr4_patches\.cpp(\r?)$', "`$1src/pgr4_patches.cpp`$2`n`$1src/pgr4_drawcount_hooks.cpp`$2"
            [IO.File]::WriteAllText($cml, $text, (New-Object Text.UTF8Encoding($false)))
            Write-Host 'added src/pgr4_drawcount_hooks.cpp to CMakeLists.txt'
        }
    }
}

if (Want 'sdk') {
    Step 'sdk' {
        $rexglue = Join-Path $Sdk 'bin\rexglue.exe'
        Use-VcEnv
        & cmake -S $SdkSrc -B $SdkBuild -G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl `
            -DCMAKE_BUILD_TYPE=Release "-DCMAKE_INSTALL_PREFIX=$Sdk"
        if ($LASTEXITCODE -ne 0) { throw 'SDK configure failed' }
        & cmake --build $SdkBuild --target install -j $Jobs
        if ($LASTEXITCODE -ne 0) { throw 'SDK build failed' }
        & $rexglue --version
    }
}

if (Want 'game') {
    Step 'game' {
        if (-not (Test-Path (Join-Path $Game 'default.xex'))) { throw "missing $Game\default.xex" }
        $rexglue = Join-Path $Sdk 'bin\rexglue.exe'
        if (-not (Test-Path $rexglue)) { throw "missing $rexglue; run the sdk stage first" }
        Use-VcEnv
        # Same rewrite as MTR's scripts\build.ps1 (Local-Manifest), PGR4 only.
        $manifest = Join-Path $Repo 'pgr4_local.toml'
        $content = [IO.File]::ReadAllText((Join-Path $Repo 'pgr4_recompiled_manifest.toml'))
        $gamePath = $Game.Replace('\', '/')
        $content = $content -replace 'game_root = "[^"]*"', ('game_root = "' + $gamePath + '"')
        $content = $content -replace 'file_path = "[^"]*"', ('file_path = "' + $gamePath + '/default.xex"')
        [IO.File]::WriteAllText($manifest, $content, (New-Object Text.UTF8Encoding($false)))
        $stamp = Join-Path $Repo 'generated\default\codegen.build.stamp'
        if ($Force -and (Test-Path $stamp)) { Remove-Item $stamp }
        Push-Location $Repo
        try {
            & $rexglue codegen $manifest
            if ($LASTEXITCODE -ne 0) { throw 'codegen failed' }
        } finally { Pop-Location }
        $build = Join-Path $Repo 'out\pgr4'
        & cmake -S $Repo -B $build -G Ninja -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_BUILD_TYPE=Release `
            "-DCMAKE_PREFIX_PATH=$Sdk" "-DMTR_GAME_MANIFEST=$manifest"
        if ($LASTEXITCODE -ne 0) { throw 'game configure failed' }
        & cmake --build $build -j $Jobs
        if ($LASTEXITCODE -ne 0) { throw 'game build failed' }
        Get-Item (Join-Path $build 'pgr4_recompiled.exe') | Format-Table Name, Length, LastWriteTime | Out-Host
    }
}

if (Want 'install') {
    Step 'install' {
        $runtime = Join-Path $Dest 'run\runtime'
        New-Item -ItemType Directory -Force -Path $runtime | Out-Null
        $files = @((Join-Path $Repo 'out\pgr4\pgr4_recompiled.exe'),
                   (Join-Path $Sdk 'bin\rexruntime.dll'), (Join-Path $Sdk 'bin\rexgpu-xenos.dll'))
        foreach ($f in $files) {
            if (-not (Test-Path $f)) { throw "missing $f" }
            Copy-Item $f $runtime -Force
        }
        Get-ChildItem $runtime -File | Format-Table Name, Length, LastWriteTime | Out-Host
    }
}
