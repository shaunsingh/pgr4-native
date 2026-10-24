# Build a native tree, then one profiled, uncapped, primary race run, then the stage table and
# the hitch table of its native CSV. Output: <Dest>\lab\<Tag>.out (run) and this script's stdout.
#   pwsh -NoProfile -File C:\recomp\builds\pgr4\lab-scripts\perf\perf_buildrun.ps1 -Dest <tree> -Tag <t> [-Owner <name>] [-Extra "<flags>"] [-NoBuild]
param([Parameter(Mandatory)][string]$Dest, [Parameter(Mandatory)][string]$Tag, [string]$Owner = 'agent',
      [string]$Extra = '', [string]$Stage = 'prep,game,install', [switch]$NoBuild)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$lab = "$Dest\lab"
if (-not $NoBuild) {
  pwsh -NoProfile -File "$here\perf_build.ps1" -Dest $Dest -Stage $Stage
  if (-not (Select-String -Path "$lab\perf-build.log" -Pattern '=== install done' -Quiet)) { throw 'build failed (see perf-build.log)' }
  # Compiler/linker errors only ("(line,col): error" and LNK); SDK warning text can contain "error".
  if (Select-String -Path "$lab\perf-build.log" -Pattern '\): error|error LNK' -Quiet) { throw 'build failed (see perf-build.log)' }
}
pwsh -NoProfile -File "$here\co_run.ps1" -Dest $Dest -Tag $Tag -Owner $Owner -Race -Uncapped -Extra "--pgr4_native_profile $Extra" *> "$lab\$Tag.out"
Get-Content "$lab\$Tag.out" | Select-String -Pattern 'race:|compiler processes|alive at end|XThread \(|GPU Commands|PGR4 native'
$csv = Get-ChildItem C:\recomp\logs -Filter "pgr4-native-$Tag-20*-native.csv" | Sort-Object LastWriteTime | Select-Object -Last 1
python "$here\perf_stats.py" $csv.FullName 600
Write-Host '=== hitches'
python "$here\perf_hitch.py" $csv.FullName 50
