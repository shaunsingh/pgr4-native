# Runs oracle_diff.py on a capture directory on the host. Argument: base64 JSON {dir, tiles, gain}.
# `lab pgr4 diff <tag|dir>`; the lab side then copies diff\*.png and summary.txt back.
param([string]$Spec)
$ErrorActionPreference = 'Stop'
$a = [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($Spec)) | ConvertFrom-Json
if (-not (Test-Path $a.dir)) { throw "no capture directory $($a.dir)" }
$tool = 'C:\recomp\builds\pgr4\lab-scripts\perf\oracle_diff.py'
if (-not (Test-Path $tool)) { throw "missing $tool; run lab pgr4 scripts" }
$args2 = @($tool, $a.dir, '--tiles', $a.tiles, '--gain', $a.gain)
& python @args2
if ($LASTEXITCODE -ne 0) { throw "oracle_diff.py exited $LASTEXITCODE" }
Get-ChildItem (Join-Path $a.dir 'diff') -File | ForEach-Object { "FILE $($_.FullName)" }
