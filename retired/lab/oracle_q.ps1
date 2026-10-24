# Race oracle (rr_run.ps1 next to this script: Xenos and native side by side, front-buffer PSNR
# at 3 race frames) through co_run's FIFO queue, so it neither jumps nor starves the queue.
#   pwsh -NoProfile -File C:\recomp\builds\pgr4\lab-scripts\perf\oracle_q.ps1 -Dest <tree> -Tag <t> [-Owner <name>]
# Output: <Dest>\lab\<Tag>.out; prints the captures lines (last column = dB).
param([Parameter(Mandatory)][string]$Dest, [Parameter(Mandatory)][string]$Tag, [string]$Owner = 'agent')
$out = "$Dest\lab\$Tag.out"
$qdir = 'C:\recomp\locks\queue'
New-Item -ItemType Directory -Force $qdir | Out-Null
$ticket = Join-Path $qdir ('{0:yyyyMMdd-HHmmss-fff}-{1}-{2}' -f (Get-Date), $Owner, $Tag)
Set-Content $ticket "$Owner $Tag"
try {
  for ($i = 0; $i -lt 30; $i++) {
    while ($true) {
      (Get-Item $ticket).LastWriteTime = Get-Date
      Get-ChildItem $qdir -File | Where-Object { $_.FullName -ne $ticket -and ((Get-Date) - $_.LastWriteTime).TotalMinutes -ge 2 } |
        ForEach-Object { Remove-Item $_.FullName -Force -ErrorAction SilentlyContinue }
      $head = Get-ChildItem $qdir -File | Sort-Object Name | Select-Object -First 1
      if ($head.FullName -eq $ticket -and -not (Test-Path C:\recomp\locks\game.lock)) { break }
      Start-Sleep -Seconds 10
    }
    pwsh -NoProfile -File "$PSScriptRoot\rr_run.ps1" -Tag $Tag -Dest $Dest -Captures 3 *> $out
    if (Select-String -Path $out -Pattern 'captures:' -Quiet) { break }
    if (-not (Select-String -Path $out -Pattern 'game.lock' -Quiet)) { break }  # failed for another reason
    Start-Sleep -Seconds 5
  }
} finally { Remove-Item $ticket -Force -ErrorAction SilentlyContinue }
Get-Content $out | Select-String -Pattern 'captures:' -Context 0,4 | ForEach-Object { $_.ToString() }
