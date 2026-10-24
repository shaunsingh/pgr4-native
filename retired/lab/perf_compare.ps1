# Unprofiled, uncapped race runs of one tree in several modes (default primary, then none = Xenos
# only), each timed from the game log over the 40 s after the race mark (perf_fps.py).
#   pwsh -NoProfile -File C:\recomp\builds\pgr4\lab-scripts\perf\perf_compare.ps1 -Dest <tree> [-Suffix s] [-Extra "<flags>"] [-Modes primary,none]
param([Parameter(Mandatory)][string]$Dest, [string]$Owner = 'agent', [string]$Suffix = '',
      [string]$Extra = '', [string[]]$Modes = @('primary', 'none'))
# Through `pwsh -File`, "-Modes primary,none" arrives as one string: split it.
$Modes = @($Modes | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$lab = "$Dest\lab"
$name = Split-Path $Dest -Leaf
foreach ($mode in $Modes) {
  $tag = "cmp-$name-$mode$Suffix"
  pwsh -NoProfile -File "$here\co_run.ps1" -Dest $Dest -Tag $tag -Owner $Owner -Mode $mode -Race -Uncapped -Extra $Extra *> "$lab\$tag.out"
  Write-Host "=== $tag"
  $out = Get-Content "$lab\$tag.out"
  $out | Select-String -Pattern 'race:|XThread \(|GPU Commands|compiler processes|alive'
  $mark = ($out | Select-String -Pattern 'mark race: True (\d\d:\d\d:\d\d)' | Select-Object -First 1).Matches.Groups[1].Value
  $log = Get-ChildItem C:\recomp\logs -Filter "pgr4-native-$tag-20*.log" | Where-Object Name -notmatch 'stream|stdout' | Sort-Object LastWriteTime | Select-Object -Last 1
  if ($mark) { python "$here\perf_fps.py" $log.FullName $mark 40 | Select-Object -First 1 }
  $n = Get-ChildItem C:\recomp\logs -Filter "pgr4-native-$tag-20*-native.csv" | Sort-Object LastWriteTime | Select-Object -Last 1
  if ($n) { python "$here\perf_stats.py" $n.FullName 600 | Select-Object -First 10 }
}
