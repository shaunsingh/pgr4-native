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
#            rexglue-native-primary.patch (--gpu_skip_draws), rexglue-runtime-perf.patch
#            (blocking timer queue, cheaper write-watch faults) and rexglue-content.patch (title
#            update on update:\, DLC install, image check) to the SDK copy,
#            copy src\pgr4_d3d_trace.cpp and src\d3d_surface.inc into the game's src\ and add
#            the .cpp to its CMakeLists.txt; mirror src\native\ to the game's src\native\ and
#            append a CMake block that compiles src/native/*.cpp and links d3d12, dxgi, dxguid;
#            rewrite the base-executable addresses in the copy's game patches
#            (tools\apply_game_patches.ps1, src\pgr4_patches.cpp) with Title Update #2's
#            (..\title-update\addresses.txt), and apply ..\title-update\apply_game_patches.patch
#            for the one patched function TU#2 changed.
#   sdk      configure + build + install the SDK copy (clang-cl, Release) into $Dest\sdk-install.
#   game     codegen Title Update #2: $Game\default.xex with ..\title-update\default.xexp beside
#            it (copies in $Dest\title-update-image), with the copy's manifest and TU#2's entry
#            options (..\title-update\entrypoint.toml); MTR's game patches included. Build
#            out\pgr4\pgr4_recompiled.exe (no Geometry Wars, no launcher).
#   install  $Dest\run\runtime: exe, rexruntime.dll, rexgpu-xenos.dll and title-update\ (the
#            runtime's update:\ when no --update_data_root is given); seeds userdata\ from the
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
# The title update on update:\, DLC install from --content_root, the XEX image size check.
$ContentPatch = Join-Path $PSScriptRoot 'rexglue-content.patch'
$TitleUpdate = Join-Path $PSScriptRoot '..\title-update'
$AddressMap = Join-Path $TitleUpdate 'addresses.txt'
$Retarget = Join-Path $PSScriptRoot 'tools\tu_address_map.py'
$GamePatch = Join-Path $TitleUpdate 'apply_game_patches.patch'
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
        foreach ($f in @($Patch, $PrimaryPatch, $RuntimePatch, $ContentPatch, $NativeSrc, $AddressMap, $Retarget, $GamePatch) + $Sources) {
            if (-not (Test-Path $f)) { throw "missing $f" }
        }
        New-Item -ItemType Directory -Force -Path $Dest | Out-Null
        $exclude = @('out', 'sdk-install', 'generated', 'geometry-wars\generated', 'thirdparty\rexglue-sdk\out') |
            ForEach-Object { Join-Path $Src $_ }
        # Keep an already patched CMakeLists.txt, so a re-run of prep does not touch it (and does
        # not force a full rebuild of the game).
        $destCml = Join-Path $Repo 'CMakeLists.txt'
        $xf = @(Join-Path $Src 'src\pgr4_patches.cpp')
        if ((Test-Path $destCml) -and ([IO.File]::ReadAllText($destCml) -match 'pgr4_d3d_trace\.cpp')) {
            $xf += Join-Path $Src 'CMakeLists.txt'
        }
        robocopy $Src $Repo /E /XD @exclude /XF @xf /NFL /NDL /NJH /NP /R:1 /W:1 | Out-Host
        if ($LASTEXITCODE -ge 8) { throw "robocopy failed ($LASTEXITCODE)" }
        $global:LASTEXITCODE = 0

        # MTR's game patches name the base executable's addresses; the code is Title Update #2's,
        # which also changed one patched function (apply_game_patches.patch). The patch script
        # takes the time of the newest of its inputs; pgr4_patches.cpp is replaced only when its
        # text changes, so it recompiles only then.
        $mtrPatcher = Join-Path $Src 'tools\apply_game_patches.ps1'
        $patcher = Join-Path $Repo 'tools\apply_game_patches.ps1'
        Copy-Item $mtrPatcher $patcher -Force
        & python $Retarget retarget $AddressMap $patcher
        if ($LASTEXITCODE -ne 0) { throw 'retargeting tools\apply_game_patches.ps1 failed' }
        & git -C $Repo apply --ignore-whitespace $GamePatch
        if ($LASTEXITCODE -ne 0) { throw "git apply $GamePatch failed" }
        $newest = @($mtrPatcher, $AddressMap, $GamePatch) | Get-Item | Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
        (Get-Item $patcher).LastWriteTimeUtc = $newest.LastWriteTimeUtc
        $tmp = New-TemporaryFile
        try {
            Copy-Item (Join-Path $Src 'src\pgr4_patches.cpp') $tmp.FullName -Force
            & python $Retarget retarget $AddressMap $tmp.FullName
            if ($LASTEXITCODE -ne 0) { throw 'retargeting src\pgr4_patches.cpp failed' }
            $dst = Join-Path $Repo 'src\pgr4_patches.cpp'
            if (-not (Test-Path $dst) -or [IO.File]::ReadAllText($tmp.FullName) -cne [IO.File]::ReadAllText($dst)) {
                Copy-Item $tmp.FullName $dst -Force
            }
        } finally { Remove-Item $tmp.FullName -ErrorAction SilentlyContinue }

        foreach ($p in @($Patch, $PrimaryPatch, $RuntimePatch, $ContentPatch)) {
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
        $tuEntry = Join-Path $TitleUpdate 'entrypoint.toml'
        foreach ($f in @((Join-Path $Game 'default.xex'), (Join-Path $TitleUpdate 'default.xexp'), $tuEntry, $AddressMap)) {
            if (-not (Test-Path $f)) { throw "missing $f" }
        }
        $rexglue = Join-Path $Sdk 'bin\rexglue.exe'
        if (-not (Test-Path $rexglue)) { throw "missing $rexglue; run the sdk stage first" }
        Use-VcEnv
        # Codegen applies an XEX patch found beside the XEX it loads: a copy of the disc's
        # default.xex with TU#2's default.xexp next to it (copied only when changed).
        $image = Join-Path $Dest 'title-update-image'
        New-Item -ItemType Directory -Force -Path $image | Out-Null
        foreach ($f in @((Join-Path $Game 'default.xex'), (Join-Path $TitleUpdate 'default.xexp'))) {
            $to = Join-Path $image (Split-Path $f -Leaf)
            if (-not (Test-Path $to) -or (Get-FileHash $f).Hash -ne (Get-FileHash $to).Hash) { Copy-Item $f $to -Force }
        }
        # Same rewrite as MTR's scripts\build.ps1 (Local-Manifest), PGR4 only, on the image above.
        $manifest = Join-Path $Repo 'pgr4_local.toml'
        $content = [IO.File]::ReadAllText((Join-Path $Repo 'pgr4_recompiled_manifest.toml')) -replace "`r", ''
        $imagePath = $image.Replace('\', '/')
        $content = $content -replace 'game_root = "[^"]*"', ('game_root = "' + $imagePath + '"')
        $content = $content -replace 'file_path = "[^"]*"', ('file_path = "' + $imagePath + '/default.xex"')
        # TU#2's setjmp, longjmp and function entries in place of MTR's, each of which
        # addresses.txt must account for.
        $known = @{}
        foreach ($line in [IO.File]::ReadAllLines($AddressMap)) {
            if ($line -match '^(82[0-9A-Fa-f]{6})\s') { $known[[Convert]::ToUInt32($Matches[1], 16)] = $true }
        }
        $funcs = [regex]::Match($content, '(?ms)^\[entrypoint\.functions\]\n(.*?)(?=^\[|\z)')
        if (-not $funcs.Success) { throw 'pgr4_recompiled_manifest.toml: [entrypoint.functions] not found' }
        $mtr = @([regex]::Matches($funcs.Groups[1].Value, '(?m)^\s*0x([0-9A-Fa-f]{8})\s*=') | ForEach-Object { $_.Groups[1].Value }) +
            @([regex]::Matches($content, '(?m)^\s*(?:setjmp|longjmp)_address\s*=\s*0x([0-9A-Fa-f]{8})') | ForEach-Object { $_.Groups[1].Value })
        $unknown = @($mtr | Where-Object { -not $known.ContainsKey([Convert]::ToUInt32($_, 16)) } | Sort-Object -Unique)
        if ($unknown) {
            throw "MTR's manifest has entries ..\title-update\addresses.txt lacks: 0x$($unknown -join ', 0x') (native\tools\tu_address_map.py maps them)"
        }
        $tu = [IO.File]::ReadAllText($tuEntry) -replace "`r", ''
        $jumps = -join @([regex]::Matches($tu, '(?m)^(?:setjmp|longjmp)_address\s*=.*\n') | ForEach-Object { $_.Value })
        $content = $content.Remove($funcs.Index, $funcs.Length).Insert($funcs.Index, $tu.Substring($tu.IndexOf('[entrypoint.functions]')))
        $content = $content -replace '(?m)^(?:setjmp|longjmp)_address\s*=.*\n', ''
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
        $entry = [regex]::Match($content, '(?m)^\[entrypoint\]\n')
        if (-not $entry.Success) { throw 'pgr4_recompiled_manifest.toml: [entrypoint] not found' }
        $content = $content.Insert($entry.Index + $entry.Length, $locals + $jumps)
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
        # Not \b in front: PowerShell's `t and `n run into the name (`tctx.r29).
        $plocal = [regex]::Replace($ptext, '(?:(?<!\w)|(?<=`[tn]))ctx\.(r1[4-9]|r2[0-9]|r3[01]|f1[4-9]|f2[0-9]|f3[01]|cr[0-7]|ctr|xer)\b', '$1')
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
        $tuDir = Join-Path $runtime 'title-update'
        New-Item -ItemType Directory -Force -Path $tuDir | Out-Null
        foreach ($f in @('default.xexp', 'Pgr4TitleUpdate.bin')) { Copy-Item (Join-Path $TitleUpdate $f) $tuDir -Force }
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
