# Builds a native tree from <Dest>\lab\perf-native.tar: a tar of native AND
# instrument (build.ps1 needs ..\instrument\rexglue-drawcount.patch). `lab pgr4 sync`
# makes and uploads it; by hand: COPYFILE_DISABLE=1 tar -cf x.tar native instrument + fleet cp.
#   -Stage: prep,game,install for source changes (prep copies the new native\ sources in);
#           prep,sdk,game,install the first time in a tree or after an SDK patch change.
#   -NoGate: compile even while a game run holds the lock (see below).
# Log: <Dest>\lab\perf-build.log. Ends with "=== install done" on success.
#
# Build gate: a compile during someone's measured run makes every profiled stage 15-20 % slower
# (2026-10-21: 9-18 compiler processes during runs of four parallel agents). So the build waits
# while C:\recomp\locks\game.lock exists (up to 15 min), and runs at the head of co_run's queue
# wait up to 6 min for compilers to finish. Builds still overlap other builds.
param([Parameter(Mandatory)][string]$Dest, [string]$Stage = 'prep,game,install', [int]$Jobs = 6,
      [switch]$NoGate)
$ErrorActionPreference = 'Stop'
$lab = "$Dest\lab"
New-Item -ItemType Directory -Force $lab | Out-Null
Set-Location $lab
# -m: extracted files get the extraction time, so ninja always sees edited sources as newer
# than their objects (tar otherwise restores the Mac mtimes, which can be older: an agent ran a
# stale exe and got link errors that way).
tar -xmf perf-native.tar
if (-not (Test-Path "$lab\instrument\rexglue-drawcount.patch")) {
    throw "perf-native.tar has no instrument\: tar 'native instrument' together"
}
if (-not $NoGate) {
    $gateUntil = (Get-Date).AddMinutes(15)
    while ((Test-Path C:\recomp\locks\game.lock) -and (Get-Date) -lt $gateUntil) {
        Write-Host "build gate: a game run holds the lock ($(Get-Content C:\recomp\locks\game.lock -ErrorAction SilentlyContinue)), waiting"
        Start-Sleep -Seconds 15
    }
}
pwsh -NoProfile -ExecutionPolicy Bypass -File "$lab\native\build.ps1" -Dest $Dest -Stage $Stage -Jobs $Jobs *>&1 | Tee-Object "$lab\perf-build.log"
