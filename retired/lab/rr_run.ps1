# Race oracle run, called by oracle_q.ps1 (`lab pgr4 oracle`): Xenos and native side by side, fresh
# save, St Petersburg route (route-stpetersburg.keys), then an optional resolve dump and N front-buffer
# captures during the race (captures.csv, last column = PSNR in dB). Installed into
# C:\recomp\builds\pgr4\lab-scripts\perf\ by `lab pgr4 scripts`.
param([string]$Tag = 'ro', [int]$RaceWaitSec = 8, [string]$Extra = '', [int]$Captures = 3, [switch]$Dump,
      [string]$Dest = 'C:\recomp\builds\pgr4\native-RaceOracle')
$ErrorActionPreference = 'Stop'
$lab = "$Dest\lab"
$lock = 'C:\recomp\locks\game.lock'
$cap = "C:\recomp\logs\pgr4-$Tag-captures"
$dumpDir = "C:\recomp\logs\pgr4-$Tag-resolve-dump"
$ilog = "C:\recomp\logs\pgr4-$Tag-input.log"
New-Item -ItemType Directory -Force (Split-Path $lock) | Out-Null
if (Test-Path $lock) {
    $age = (Get-Date) - (Get-Item $lock).LastWriteTime
    $game = Get-Process pgr4_recompiled -ErrorAction SilentlyContinue
    if ($game -or $age.TotalMinutes -lt 45) { throw "game.lock held: $(Get-Content $lock)" }
}
if (Get-Process pgr4_recompiled -ErrorAction SilentlyContinue) { throw 'a pgr4_recompiled is running' }
Set-Content $lock "RaceOracle $(Get-Date -Format o)"
try {
    New-Item -ItemType Directory -Force $cap, $dumpDir | Out-Null
    Remove-Item $ilog -ErrorAction SilentlyContinue
    $ud = "$Dest\run\runtime\userdata"
    if (Test-Path $ud) { Get-ChildItem $ud | Where-Object Name -ne 'cache' | Remove-Item -Recurse -Force }
    $x = "--readback_resolve=fast --pgr4_native_capture_dir=$cap $Extra"
    if ($Dump) { $x += " --pgr4_native_resolve_dump=$dumpDir" }
    & pwsh -NoProfile -File "$lab\native\run.ps1" -Action start -Dest $Dest -Tag $Tag -Native "-Extra:$x"
    $exe = "$Dest\run\runtime\pgr4_recompiled.exe"
    $deadline = (Get-Date).AddSeconds(60)
    $proc = $null
    while (-not $proc -and (Get-Date) -lt $deadline) {
        Start-Sleep -Seconds 1
        $proc = Get-Process pgr4_recompiled -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $exe } | Select-Object -First 1
    }
    if (-not $proc) { throw 'game did not start' }
    $keys = 'C:\recomp\builds\pgr4\lab-scripts\run\route-stpetersburg.keys'
    $cmd = "C:\recomp\logs\pgr4-$Tag-input.cmd"
    Set-Content -Path $cmd -Encoding ascii -Value "@echo off`r`npowershell -NoProfile -ExecutionPolicy Bypass -File `"C:\recomp\builds\pgr4\lab-scripts\run\input-player.ps1`" -Script `"$keys`" -Log $ilog"
    schtasks /create /f /tn recomp-pgr4-ro-input /sc once /st 23:59 /ru $env:USERNAME /it /rl LIMITED /tr "cmd /c `"$cmd`"" | Out-Null
    schtasks /run /tn recomp-pgr4-ro-input | Out-Null
    $deadline = (Get-Date).AddSeconds(220)
    $ok = $false
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Seconds 3
        if (Test-Path $ilog) {
            if (Select-String -Path $ilog -Pattern '^\S+\s+mark throttle' -Quiet) { $ok = $true; break }
            if (Select-String -Path $ilog -Pattern '^.*error' -Quiet) { break }
        }
    }
    Write-Host "route reached throttle: $ok"
    Start-Sleep -Seconds $RaceWaitSec
    if ($Dump) { New-Item -ItemType File -Force "$dumpDir\dump.request" | Out-Null; Start-Sleep -Seconds 5 }
    foreach ($i in 1..$Captures) {
        New-Item -ItemType File -Force "$cap\capture.request" | Out-Null
        Start-Sleep -Seconds 5
    }
    Write-Host "captures:"; Get-Content "$cap\captures.csv" -ErrorAction SilentlyContinue | Select-Object -Last ($Captures + 1)
} finally {
    & pwsh -NoProfile -File "$lab\native\run.ps1" -Action stop -Dest $Dest 2>&1 | Out-Host
    # No schtasks /end: the player releases the held throttle itself (a killed player would not).
    schtasks /delete /f /tn recomp-pgr4-ro-input 2>$null | Out-Null
    Remove-Item $lock -ErrorAction SilentlyContinue
    Write-Host 'stopped, lock removed'
}
