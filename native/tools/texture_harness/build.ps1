# Builds texture_harness.exe (standalone texture untiling check, M2 textures module) on Windows.
#
#   pwsh -NoProfile -File build.ps1 [-Sdk C:\recomp\builds\pgr4\native-TextureMod\MTR-PGR4\thirdparty\rexglue-sdk]
#           [-Out C:\recomp\builds\pgr4\native-TextureMod\texture_harness]
#
# Compiles the harness, ..\..\src\native\clock.cpp, texture_layout.cpp, textures.cpp (with
# PGR4_TEXTURES_OFFLINE: no ReXGlue runtime, no write tracking) and the SDK's BSD texture helpers
# (src/graphics/pipeline/texture/util.cpp, info_formats.cpp) with clang-cl; headers from the
# SDK source tree (include\) and its installed copy (fmt, header-only here). Links d3d12.lib.
param(
    [string]$Sdk = 'C:\recomp\builds\pgr4\native-TextureMod\MTR-PGR4\thirdparty\rexglue-sdk',
    [string]$SdkInstall = 'C:\recomp\builds\pgr4\native\sdk-install',
    [string]$Out = 'C:\recomp\builds\pgr4\native-TextureMod\texture_harness'
)
$ErrorActionPreference = 'Stop'
$Vcvars = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
$Llvm = 'C:\Program Files\LLVM\bin'
if ($env:VSCMD_ARG_TGT_ARCH -ne 'x64') {
    cmd /c "`"$Vcvars`" >nul && set" | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($matches[1])" -Value $matches[2] }
    }
    $env:PATH = "$Llvm;$env:PATH"
}
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$native = Join-Path $PSScriptRoot '..\..\src\native'
$srcs = @(
    (Join-Path $PSScriptRoot 'texture_harness.cpp'),
    (Join-Path $native 'clock.cpp'),
    (Join-Path $native 'texture_layout.cpp'),
    (Join-Path $native 'textures.cpp'),
    (Join-Path $Sdk 'src\graphics\pipeline\texture\util.cpp'),
    (Join-Path $Sdk 'src\graphics\pipeline\texture\info_formats.cpp'),
    (Join-Path $Sdk 'src\core\math_msvc.cpp')  # rex::lzcnt (exported by rexruntime.dll in the game)
)
$cl = @('/nologo', '/std:c++latest', '/O2', '/EHsc', '/MD', '/DNDEBUG', '/DNOMINMAX', '/DWIN32_LEAN_AND_MEAN',
          '/DPGR4_TEXTURES_OFFLINE', '/DFMT_HEADER_ONLY',
          '/clang:-march=x86-64-v2', '/Zc:char8_t-', '-Wno-everything',
          "/I$(Join-Path $Sdk 'include')", "/I$(Join-Path $SdkInstall 'include')",
          "/Fo$Out\", "/Fe$(Join-Path $Out 'texture_harness.exe')") + $srcs + @('/link', 'd3d12.lib')
& clang-cl @cl
if ($LASTEXITCODE -ne 0) { throw "clang-cl failed ($LASTEXITCODE)" }
Get-Item (Join-Path $Out 'texture_harness.exe') | Format-Table Name, Length, LastWriteTime | Out-Host
