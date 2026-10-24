# Cleans up after a PGR4 run whose wrapper died (its `finally` never ran): stops the game started
# from one build tree, then, if no other tree's game is running, ends and deletes the input
# tasks and removes the game lock and the draw-debug file. Argument: base64 JSON {dest, force};
# force also cleans while another tree's game runs (that would break its run). `lab pgr4 clean`.
param([string]$Spec)
$ErrorActionPreference = 'Stop'
$a = [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($Spec)) | ConvertFrom-Json
$exe = Join-Path $a.dest 'run\runtime\pgr4_recompiled.exe'
$mine = Get-Process pgr4_recompiled -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $exe }
foreach ($p in $mine) { "stopping pid $($p.Id) ($exe)"; Stop-Process -Id $p.Id -Force }
if (-not $mine) { "no game from $($a.dest) running" }
$others = Get-Process pgr4_recompiled -ErrorAction SilentlyContinue | Where-Object { $_.Path -ne $exe }
if ($others -and -not $a.force) {
    "left alone: another tree's game is running ($(($others | ForEach-Object Path) -join ', ')); its run owns the lock and input task"
    return
}
foreach ($t in 'recomp-pgr4-co-input', 'recomp-pgr4-ro-input') {
    schtasks /end /tn $t 2>$null | Out-Null
    schtasks /delete /f /tn $t 2>$null | Out-Null
}
'input tasks ended and deleted'
if (Test-Path C:\recomp\locks\game.lock) {
    "removing lock: $((Get-Content C:\recomp\locks\game.lock -Raw).Trim())"
    Remove-Item C:\recomp\locks\game.lock -Force
}
if (Test-Path C:\recomp\logs\pgr4-native-captures\draw-debug.txt) {
    Remove-Item C:\recomp\logs\pgr4-native-captures\draw-debug.txt -Force
    'removed draw-debug.txt'
}
'clean'
