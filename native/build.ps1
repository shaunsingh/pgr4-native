# Builds the PGR4 native-renderer checkout on the Windows build machine: MTR-PGR4 (source) + the D3D API hooks and
# frame trace in src\ (design A, milestone 1) + the native D3D12 renderer in src\native\
# (milestone 2). The MTR build under $Root\MTR-PGR4 and the other
# build trees are only read, never modified.
#
#   pwsh -NoProfile -ExecutionPolicy Bypass -File build.ps1 [-Stage prep,sdk,game,install] [-Jobs 6]
#
# Expects this directory and ..\instrument (for rexglue-drawcount.patch, the SDK's
# --gpu_stream_stats counters used as the oracle) side by side, as in the pgr4-native repo.
#
# Stages, in order (default: all):
#   prep     robocopy $Root\MTR-PGR4 (source and patched SDK source; no build dirs, no
#            generated code) to $Dest\MTR-PGR4, apply rexglue-drawcount.patch and then
#            rexglue-native-primary.patch (--gpu_skip_draws) and rexglue-runtime-perf.patch
#            (blocking timer queue, cheaper write-watch faults) to the SDK copy,
#            copy src\pgr4_d3d_trace.cpp and src\d3d_surface.inc into the game's src\ and add
#            the .cpp to its CMakeLists.txt; mirror src\native\ to the game's src\native\ and
#            append a CMake block that compiles src/native/*.cpp and links d3d12, dxgi, dxguid.
#   sdk      configure + build + install the SDK copy (clang-cl, Release) into $Dest\sdk-install.
#   game     codegen default.xex with the copy's manifest (MTR's game patches included), build
#            out\pgr4\pgr4_recompiled.exe (no Geometry Wars, no launcher).
#   install  $Dest\run\runtime: exe, rexruntime.dll, rexgpu-xenos.dll; seeds userdata\ from the
#            instrumented build's profile if this one has none (read-only copy).
#
# Run: run.ps1 beside this script.
param(
    [string[]]$Stage = 'all',
    [string]$Root = 'C:\recomp\builds\pgr4',
    [string]$Dest = '',                              # default: $Root\native
    [string]$Game = 'C:\recomp\dumps\pgr4\game',
    [int]$Jobs = 6,
    [switch]$Force
)
$ErrorActionPreference = 'Stop'
if (-not $Dest) { $Dest = Join-Path $Root 'native' }
$Src = Join-Path $Root 'MTR-PGR4'
$Repo = Join-Path $Dest 'MTR-PGR4'
$SdkSrc = Join-Path $Repo 'thirdparty\rexglue-sdk'
$SdkBuild = Join-Path $SdkSrc 'out\build\native-release'
$Sdk = Join-Path $Dest 'sdk-install'
$Patch = Join-Path $PSScriptRoot '..\instrument\rexglue-drawcount.patch'
# Applied after rexglue-drawcount.patch: --gpu_skip_draws for --pgr4_native_primary.
$PrimaryPatch = Join-Path $PSScriptRoot 'rexglue-native-primary.patch'
# Runtime fixes: a blocking timer queue and write-watch faults without VirtualQuery.
$RuntimePatch = Join-Path $PSScriptRoot 'rexglue-runtime-perf.patch'
$Sources = @('pgr4_d3d_trace.cpp', 'd3d_surface.inc') | ForEach-Object { Join-Path $PSScriptRoot "src\$_" }
$NativeSrc = Join-Path $PSScriptRoot 'src\native'
# Appended to the game's CMakeLists.txt by prep, replaced whole when it changes. The two SDK
# texture sources (BSD) are compiled into the game: their functions are not exported from
# rexgpu-xenos.dll (textures module).
$NativeCmake = @'
# begin pgr4-native native renderer (native/build.ps1)
file(GLOB PGR4_NATIVE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/native/*.cpp")
target_sources(pgr4_recompiled PRIVATE ${PGR4_NATIVE_SOURCES}
    "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/rexglue-sdk/src/graphics/pipeline/texture/util.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/rexglue-sdk/src/graphics/pipeline/texture/info_formats.cpp")
target_link_libraries(pgr4_recompiled PRIVATE d3d12 dxgi dxguid)
# Shaders module: ReXGlue's DXBC translator sources as a static library.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/native/shader_xlate.cmake")
    include("${CMAKE_CURRENT_SOURCE_DIR}/src/native/shader_xlate.cmake")
    target_link_libraries(pgr4_recompiled PRIVATE pgr4_shader_xlate)
endif()
# end pgr4-native native renderer
'@
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
        foreach ($f in @($Patch, $PrimaryPatch, $NativeSrc) + $Sources) { if (-not (Test-Path $f)) { throw "missing $f" } }
        New-Item -ItemType Directory -Force -Path $Dest | Out-Null
        $exclude = @('out', 'sdk-install', 'generated', 'geometry-wars\generated', 'thirdparty\rexglue-sdk\out') |
            ForEach-Object { Join-Path $Src $_ }
        # Keep an already patched CMakeLists.txt, so a re-run of prep does not touch it (and does
        # not force a full rebuild of the game).
        $destCml = Join-Path $Repo 'CMakeLists.txt'
        $keep = if ((Test-Path $destCml) -and ([IO.File]::ReadAllText($destCml) -match 'pgr4_d3d_trace\.cpp')) {
            @('/XF', (Join-Path $Src 'CMakeLists.txt'))
        } else { @() }
        robocopy $Src $Repo /E /XD @exclude @keep /NFL /NDL /NJH /NP /R:1 /W:1 | Out-Host
        if ($LASTEXITCODE -ge 8) { throw "robocopy failed ($LASTEXITCODE)" }
        $global:LASTEXITCODE = 0

        foreach ($p in @($Patch, $PrimaryPatch, $RuntimePatch)) {
            $name = Split-Path $p -Leaf
            & git -C $SdkSrc apply --check --reverse $p 2>$null
            if ($LASTEXITCODE -eq 0) {
                Write-Host "$name already applied"
            } else {
                # Files a patch creates are not in the source, so robocopy leaves an earlier prep's.
                git -C $SdkSrc apply --summary $p | ForEach-Object {
                    if ($_ -match '^ create mode [0-7]+ (.+)$') {
                        Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path $SdkSrc $Matches[1])
                    }
                }
                git -C $SdkSrc apply --check $p
                if ($LASTEXITCODE -ne 0) { throw "$name does not apply to the MTR-patched SDK copy" }
                git -C $SdkSrc apply $p
                if ($LASTEXITCODE -ne 0) { throw "git apply $name failed" }
                Write-Host "applied $name"
            }
        }

        foreach ($f in $Sources) { Copy-Item $f (Join-Path $Repo 'src') -Force }
        robocopy $NativeSrc (Join-Path $Repo 'src\native') /MIR /NFL /NDL /NJH /NP /R:1 /W:1 | Out-Host
        if ($LASTEXITCODE -ge 8) { throw "robocopy src\native failed ($LASTEXITCODE)" }
        $global:LASTEXITCODE = 0
        $cml = Join-Path $Repo 'CMakeLists.txt'
        $text = [IO.File]::ReadAllText($cml)
        $changed = $false
        if ($text -notmatch 'pgr4_d3d_trace\.cpp') {
            if ($text -notmatch '(?m)^(\s*)src/pgr4_patches\.cpp\r?$') { throw 'CMakeLists.txt: src/pgr4_patches.cpp line not found' }
            $text = $text -replace '(?m)^(\s*)src/pgr4_patches\.cpp(\r?)$', "`$1src/pgr4_patches.cpp`$2`n`$1src/pgr4_d3d_trace.cpp`$2"
            $changed = $true
            Write-Host 'added src/pgr4_d3d_trace.cpp to CMakeLists.txt'
        }
        $block = '(?s)# begin pgr4-native native renderer.*?# end pgr4-native native renderer\r?\n?'
        $want = ($NativeCmake -replace "`r", '') + "`n"
        $m = [regex]::Match($text, $block)
        if (-not $m.Success) {
            $text = $text.TrimEnd() + "`n`n" + $want
            $changed = $true
            Write-Host 'added the src/native block to CMakeLists.txt'
        } elseif (($m.Value -replace "`r", '') -ne $want) {
            $text = $text.Remove($m.Index, $m.Length).Insert($m.Index, $want)
            $changed = $true
            Write-Host 'updated the src/native block in CMakeLists.txt'
        }
        if ($changed) { [IO.File]::WriteAllText($cml, $text, (New-Object Text.UTF8Encoding($false))) }
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
        # Register-as-local codegen: guest CR, CTR, XER and the non-volatile registers become C++
        # locals instead of PPCContext fields, and the register save/restore helper calls disappear
        # (XenonRecomp's README; UnleashedRecomp's SWA.toml sets all eight options). Arguments and r1
        # stay in ctx. Measured on the race: -17.9 % frame time.
        # reserved_as_local: the lwarx/ldarx reservation becomes a local. Every function in this game
        # with a load-reserve also has its store-conditional (167 functions, none split), so the
        # reservation never has to cross a call.
        # The other three stay off, each because this game's code depends on what it removes:
        #   skip_lr: two blrl sites branch to ctx.lr, and MTR's garage collision patch reads ctx.lr
        #     to identify its caller.
        #   skip_msr: about 195 mtmsrd r13 ... mtmsrd interrupt-off regions guard shared data; they
        #     are the REX_ENTER/LEAVE_GLOBAL_LOCK pairs.
        #   non_argument_as_local: about 845 exception-handler funclets take their frame pointer in
        #     r12 (addi r31,r12,-N at entry), and some tail-branch chunks take r11 from the function
        #     that branched to them; as locals those values would be lost.
        if ($content -match '(?m)^\s*((cr|ctr|xer|non_volatile|non_argument|reserved)_as_local|skip_lr|skip_msr)\s*=') {
            throw 'the MTR manifest already sets codegen register options; merge by hand'
        }
        $locals = "cr_as_local = true`nctr_as_local = true`nxer_as_local = true`nnon_volatile_as_local = true`nreserved_as_local = true`n"
        $entry = [regex]::Match($content, '(?m)^\[entrypoint\]\r?\n')
        if (-not $entry.Success) { throw 'pgr4_recompiled_manifest.toml: [entrypoint] not found' }
        $content = $content.Insert($entry.Index + $entry.Length, $locals)
        [IO.File]::WriteAllText($manifest, $content, (New-Object Text.UTF8Encoding($false)))
        $stamp = Join-Path $Repo 'generated\default\codegen.build.stamp'
        if ($Force -and (Test-Path $stamp)) { Remove-Item $stamp }
        Push-Location $Repo
        try {
            & $rexglue codegen $manifest
            if ($LASTEXITCODE -ne 0) { throw 'codegen failed' }
        } finally { Pop-Location }
        # MTR's tools\apply_game_patches.ps1 (run by the game build) matches generated C++ text that
        # names these registers ctx.r31, ctx.cr6, ctx.ctr. With the options above they are locals
        # (r31, cr6, ctr), so rewrite the copy's patterns. prep restores MTR's original each time.
        # A pattern that still finds nothing fails the build in the patch script itself.
        $patcher = Join-Path $Repo 'tools\apply_game_patches.ps1'
        $ptext = [IO.File]::ReadAllText($patcher)
        $plocal = [regex]::Replace($ptext, '\bctx\.(r1[4-9]|r2[0-9]|r3[01]|f1[4-9]|f2[0-9]|f3[01]|cr[0-7]|ctr|xer)\b', '$1')
        if ($plocal -ne $ptext) { [IO.File]::WriteAllText($patcher, $plocal, (New-Object Text.UTF8Encoding($false))) }
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
        $user = Join-Path $runtime 'userdata'
        $seed = Join-Path $Root 'instrumented\run\runtime\userdata'
        if (-not (Test-Path $user) -and (Test-Path $seed)) {
            robocopy $seed $user /E /NFL /NDL /NJH /NP /R:1 /W:1 | Out-Host
            if ($LASTEXITCODE -ge 8) { throw "robocopy userdata failed ($LASTEXITCODE)" }
            $global:LASTEXITCODE = 0
            Write-Host "seeded userdata from $seed"
        }
        Get-ChildItem $runtime -File | Format-Table Name, Length, LastWriteTime | Out-Host
    }
}
