# Integrator check of a merged master in one tree: build, a cold profiled race run (pipeline cache
# wiped), a warm one, the race oracle, then unprofiled primary and Xenos-only runs.
#   pwsh -NoProfile -File C:\recomp\builds\pgr4\lab-scripts\perf\int_chain.ps1 -Dest <tree> -Prefix <p> [-NoBuild] [-Extra "<flags>"]
param([Parameter(Mandatory)][string]$Dest, [Parameter(Mandatory)][string]$Prefix, [switch]$NoBuild,
      [string]$Extra = '')
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$lab = "$Dest\lab"
if (-not $NoBuild) {
  pwsh -NoProfile -File "$here\perf_build.ps1" -Dest $Dest
  if (-not (Select-String -Path "$lab\perf-build.log" -Pattern '=== install done' -Quiet)) { throw 'build failed' }
  if (Select-String -Path "$lab\perf-build.log" -Pattern '\): error|error LNK' -Quiet) { throw 'build failed' }
}
$cache = "$Dest\run\runtime\userdata\cache\pgr4-native"
if (Test-Path $cache) { Remove-Item -Recurse -Force $cache }
foreach ($t in "$Prefix-cold", "$Prefix-warm") {
  Write-Host "=== $t"
  pwsh -NoProfile -File "$here\perf_buildrun.ps1" -Dest $Dest -Tag $t -Owner Integrator -NoBuild -Extra $Extra
}
Write-Host "=== $Prefix-oracle"
pwsh -NoProfile -File "$here\oracle_q.ps1" -Dest $Dest -Tag "$Prefix-oracle" -Owner Integrator
Write-Host "=== compare"
pwsh -NoProfile -File "$here\perf_compare.ps1" -Dest $Dest -Owner Integrator -Suffix "-$Prefix" -Extra $Extra
