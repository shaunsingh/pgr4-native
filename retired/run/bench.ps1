# Xenos-emulation baseline for PGR4 on the Windows build host: launches one build in the desktop
# session through an interactive, non-elevated scheduled task (docs/running-on-windows.md), drives it
# to a race with input-player.ps1, then records PresentMon frame times, per-thread CPU and GPU load.
# cancel-firewall.ps1 runs beside the game (elevated task) and cancels Windows Firewall prompts.
# Run it elevated over SSH: `fleet exec <host> 'pwsh -File ...\bench.ps1 -Config ...'` (~4 min).
# `fleet spawn` jobs are not elevated, and PresentMon then fails with "access denied".
# PresentMon: -PresentMon <exe>, else $env:PRESENTMON, else PresentMon on PATH.
#
#   bench.ps1 -Config mtr-src-30                       # full run: boot, route, 60 s measurement
#   bench.ps1 -Config mtr-src-60 -Action start         # launch only (fresh save), no input
#   bench.ps1 -Action keys -Inline "+0 space 250"      # one-off input into the running game
#   bench.ps1 -Action stop                             # end the game this script started
#
# Configs (all windowed 1280x720, guest in its native 1280x720 mode, --mnk_mode):
#   mtr-src-30       MTR-PGR4 source build, MTR launcher's "720p 30 FPS" arguments
#   mtr-src-60       same + --pgr4_60fps --pgr4_garage_walk_fix (launcher's "60 FPS")
#   mtr-rel-60       MTR-PGR4 v0.1.1 release, 60 FPS arguments
#   mtr-src-uncapped mtr-src-60 + --vsync=false --d3d12_present_vsync=false
#   recomp-src-30    PGR4-Recomp 6314fc4 source build (stock SDK v0.10.0), run.bat arguments
#   recomp-rel-30    PGR4-Recomp v1.2.2 release, run.bat arguments
# Each config keeps its own user data in C:\recomp\bench\pgr4\<config>\userdata. Before every
# run the save data is deleted (the route creates a new profile) and cache\ (shaders) is kept.
param(
    [ValidateSet('run', 'start', 'stop', 'keys')][string]$Action = 'run',
    [ValidateSet('mtr-src-30', 'mtr-src-60', 'mtr-rel-60', 'mtr-src-uncapped', 'recomp-src-30', 'recomp-rel-30')]
    [string]$Config = 'mtr-src-30',
    [string]$Script = (Join-Path $PSScriptRoot 'route-stpetersburg.keys'),
    [string]$Inline = '',
    [string]$Mark = 'race',
    [int]$Measure = 60,
    [int]$RouteTimeoutSec = 900,
    [string]$Game = 'C:\recomp\dumps\pgr4\game',
    [string]$Bench = 'C:\recomp\bench\pgr4',
    [string]$Logs = 'C:\recomp\logs',
    [string]$PresentMon = $(if ($env:PRESENTMON) { $env:PRESENTMON } else { (Get-Command PresentMon -ErrorAction SilentlyContinue).Source }),
    [string]$User = $env:USERNAME
)
$ErrorActionPreference = 'Stop'
$task = 'recomp-pgr4-bench'
$player = Join-Path $PSScriptRoot 'input-player.ps1'

$mtrArgs = @('--execute_unclipped_draw_vs_on_cpu', '--gpu_plugin=xenos', '--d3d12_present_vsync',
    '--xmp_music_gain 0.35', '--d3d12_texture_shared_heaps', '--gpu_vblank_deadline_timer',
    '--gpu_wait_reg_mem_high_res', '--no-clear_memory_page_state', '--gpu_ring_wake_fast',
    '--texture_resource_reuse', '--texture_reuse_pool_limit_mib=256',
    '--texture_cache_memory_limit_soft_lifetime=30', '--guest_frame_stats')
$mtr60 = @('--pgr4_60fps', '--pgr4_garage_walk_fix')
$recompArgs = @('--execute_unclipped_draw_vs_on_cpu', '--gpu_plugin=xenos')
$builds = @{
    'mtr-src-30'       = @{ Dir = 'C:\recomp\builds\pgr4\run\MTR-PGR4\runtime'; Args = $mtrArgs }
    'mtr-src-60'       = @{ Dir = 'C:\recomp\builds\pgr4\run\MTR-PGR4\runtime'; Args = $mtrArgs + $mtr60 }
    'mtr-rel-60'       = @{ Dir = 'C:\recomp\releases\pgr4\MTR-PGR4\v0.1.1\files\MTR-PGR4-windows-x64\runtime'; Args = $mtrArgs + $mtr60 }
    'mtr-src-uncapped' = @{ Dir = 'C:\recomp\builds\pgr4\run\MTR-PGR4\runtime'
                            Args = ($mtrArgs -ne '--d3d12_present_vsync') + $mtr60 + @('--vsync=false', '--d3d12_present_vsync=false') }
    'recomp-src-30'    = @{ Dir = 'C:\recomp\builds\pgr4\run\PGR4-Recomp'; Args = $recompArgs }
    'recomp-rel-30'    = @{ Dir = 'C:\recomp\releases\pgr4\PGR4-Recomp\v1.2.2\files\win-amd64-release-xenos'; Args = $recompArgs }
}

function Get-OwnGame {
    Get-Process pgr4_recompiled -ErrorAction SilentlyContinue |
        Where-Object { $p = $_.Path; $builds.Values | Where-Object { $p -eq (Join-Path $_.Dir 'pgr4_recompiled.exe') } }
}
function Start-DesktopTask([string]$name, [string]$cmdFile, [string]$level = 'LIMITED') {
    schtasks /create /f /tn $name /sc once /st 00:00 /ru $User /it /rl $level /tr "cmd /c `"$cmdFile`"" | Out-Null
    schtasks /run /tn $name | Out-Null
}
function Stop-Game {
    foreach ($p in @(Get-OwnGame)) { Write-Host "stopping pid $($p.Id)"; Stop-Process -Id $p.Id -Force }
    foreach ($t in $task, "$task-input", "$task-fw") { schtasks /delete /f /tn $t 2>$null | Out-Null }
}
function Start-Player([string]$tag, [string]$scriptArg) {
    $cmd = Join-Path $Logs "$tag-input.cmd"
    Set-Content -Path $cmd -Encoding ascii -Value "@echo off`r`npowershell -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$player`" $scriptArg -Log `"$Logs\$tag-input.log`" > `"$Logs\$tag-input-stdout.log`" 2>&1"
    Start-DesktopTask "$task-input" $cmd
}

if ($Action -eq 'stop') { Stop-Game; return }
if ($Action -eq 'keys') {
    if (-not $Inline) { throw '-Inline is required' }
    Start-Player 'pgr4-bench-keys' "-NoWait -Inline `"$Inline`""
    return
}

# --- launch ---
$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
if ($Action -eq 'run' -and -not $admin) { throw 'not elevated: PresentMon needs admin (use fleet exec, not fleet spawn)' }
if ($Action -eq 'run' -and -not ($PresentMon -and (Test-Path $PresentMon))) { throw 'PresentMon not found: pass -PresentMon <exe> or set PRESENTMON' }
if (Get-Process pgr4_recompiled -ErrorAction SilentlyContinue) { throw 'a pgr4_recompiled.exe is already running' }
$b = $builds[$Config]
$exe = Join-Path $b.Dir 'pgr4_recompiled.exe'
if (-not (Test-Path $exe)) { throw "missing $exe" }
$userdata = Join-Path $Bench "$Config\userdata"
New-Item -ItemType Directory -Force -Path $userdata, $Logs | Out-Null
Get-ChildItem $userdata | Where-Object Name -ne 'cache' | Remove-Item -Recurse -Force
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$tag = "pgr4-bench-$Config-$stamp"
$base = Join-Path $Logs $tag
$gameArgs = (@("--game_data_root $Game", "--user_data_root $userdata") + $b.Args +
    @('--fullscreen=false', '--window_width=1280', '--window_height=720', '--mnk_mode', "--log_file $base.log")) -join ' '
Set-Content -Path "$base.cmd" -Encoding ascii -Value "@echo off`r`ncd /d `"$($b.Dir)`"`r`n`"$exe`" $gameArgs > `"$base-stdout.log`" 2>&1"
Write-Host "config $Config"
Write-Host "args   $gameArgs"
Start-DesktopTask $task "$base.cmd"
$launchDeadline = (Get-Date).AddSeconds(60)
while (-not (Get-OwnGame)) {
    if ((Get-Date) -gt $launchDeadline) { Stop-Game; throw "no pgr4_recompiled.exe 60 s after starting the task; see $base-stdout.log" }
    Start-Sleep -Milliseconds 500
}
$gamePid = @(Get-OwnGame)[0].Id
Write-Host "started $(Get-Date -Format o) pid $gamePid log $base.log"
# The game itself runs non-elevated; only the firewall-prompt canceller needs HIGHEST (see its header).
Set-Content -Path "$base-fw.cmd" -Encoding ascii -Value "@echo off`r`npowershell -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$PSScriptRoot\cancel-firewall.ps1`" -GamePid $gamePid -Log `"$base-fw.log`""
Start-DesktopTask "$task-fw" "$base-fw.cmd" 'HIGHEST'
if ($Action -eq 'start') { return }

# --- route ---
Start-Player $tag "-Script `"$Script`""
$inputLog = "$base-input.log"
$deadline = (Get-Date).AddSeconds($RouteTimeoutSec)
while (-not (Test-Path $inputLog) -or -not (Select-String -Path $inputLog -Pattern "mark $Mark " -Quiet)) {
    if ((Get-Date) -gt $deadline) { Stop-Game; throw "no 'mark $Mark' after $RouteTimeoutSec s" }
    if (-not (Get-OwnGame)) { Stop-Game; throw 'game exited during the route' }
    if ((Test-Path $inputLog) -and (Select-String -Path $inputLog -Pattern '^\S+ error' -Quiet)) { Stop-Game; throw (Get-Content $inputLog -Tail 1) }
    if ((Test-Path $inputLog) -and (Get-ScheduledTask -TaskName "$task-input").State -ne 'Running') {
        Stop-Game; throw "input player ended without 'mark $Mark'; see $base-input-stdout.log"
    }
    Start-Sleep -Seconds 1
}
$p = @(Get-OwnGame)[0]

# --- measure ---
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class T {
  [DllImport("kernel32.dll")] public static extern IntPtr OpenThread(uint access, bool inherit, uint id);
  [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr h);
  [DllImport("kernel32.dll")] public static extern int GetThreadDescription(IntPtr h, out IntPtr desc);
  [DllImport("kernel32.dll")] public static extern IntPtr LocalFree(IntPtr p);
  public static string Name(uint id) {
    IntPtr h = OpenThread(0x0800, false, id);
    if (h == IntPtr.Zero) return "";
    IntPtr d; string s = "";
    if (GetThreadDescription(h, out d) >= 0 && d != IntPtr.Zero) { s = Marshal.PtrToStringUni(d); LocalFree(d); }
    CloseHandle(h);
    return s;
  }
}
'@
function Snap($proc) {
    $proc.Refresh()
    $m = @{}
    foreach ($t in $proc.Threads) { try { $m[$t.Id] = $t.TotalProcessorTime.TotalMilliseconds } catch {} }
    [pscustomobject]@{ When = [Diagnostics.Stopwatch]::GetTimestamp(); Threads = $m; Total = $proc.TotalProcessorTime.TotalMilliseconds }
}
$csv = "$base-presentmon.csv"
$pm = Start-Process -FilePath $PresentMon -PassThru -WindowStyle Hidden -ArgumentList @(
    '--process_id', $p.Id, '--output_file', "`"$csv`"", '--timed', $Measure, '--terminate_after_timed',
    '--no_console_stats', '--stop_existing_session', '--session_name', 'recomp-pgr4-bench')
$smi = Start-Process -FilePath nvidia-smi -PassThru -WindowStyle Hidden -RedirectStandardOutput "$base-gpu.csv" -ArgumentList @(
    '--query-gpu=utilization.gpu,clocks.gr,power.draw,memory.used', '--format=csv,noheader,nounits', '-lms', '1000')
Start-Sleep -Milliseconds 500
$s0 = Snap $p
Write-Host "measuring $Measure s from $(Get-Date -Format o)"
Start-Sleep -Seconds $Measure
$s1 = Snap $p
$pm.WaitForExit(30000) | Out-Null
Stop-Process -Id $smi.Id -ErrorAction SilentlyContinue
$wallMs = ($s1.When - $s0.When) * 1000.0 / [Diagnostics.Stopwatch]::Frequency
$threads = foreach ($id in $s1.Threads.Keys) {
    $d = $s1.Threads[$id] - ($(if ($s0.Threads.ContainsKey($id)) { $s0.Threads[$id] } else { 0 }))
    [pscustomobject]@{ Id = $id; Name = [T]::Name([uint32]$id); Pct = [math]::Round(100.0 * $d / $wallMs, 1) }
}
$threads = @($threads | Sort-Object Pct -Descending)
Get-Content (Join-Path $Logs "$tag-input.log") -Tail 2 | Write-Host
Stop-Game

# --- summarise ---
$rows = @(Import-Csv $csv)
$col = @('MsBetweenPresents', 'FrameTime', 'MsBetweenAppStart') | Where-Object { $rows.Count -and $rows[0].PSObject.Properties.Name -contains $_ } | Select-Object -First 1
$ft = @($rows | ForEach-Object { [double]$_.$col } | Where-Object { $_ -gt 0 })
$sorted = @($ft | Sort-Object)
function Pct([double]$q) { $sorted[[math]::Min($sorted.Count - 1, [int][math]::Floor($q * $sorted.Count))] }
function Low([double]$q) { $n = [math]::Max(1, [int][math]::Ceiling($q * $sorted.Count)); 1000.0 / (($sorted | Select-Object -Last $n | Measure-Object -Average).Average) }
$sum = ($ft | Measure-Object -Sum).Sum
$gpu = @(Get-Content "$base-gpu.csv" | ForEach-Object { [double](($_ -split ',')[0]) })
$summary = [ordered]@{
    config = $Config; stamp = $stamp; frames = $ft.Count; column = $col
    avg_fps = [math]::Round(1000.0 * $ft.Count / $sum, 2)
    avg_ms = [math]::Round($sum / $ft.Count, 2); p50_ms = [math]::Round((Pct 0.5), 2)
    p99_ms = [math]::Round((Pct 0.99), 2); p999_ms = [math]::Round((Pct 0.999), 2); max_ms = [math]::Round($sorted[-1], 2)
    low1_fps = [math]::Round((Low 0.01), 1); low01_fps = [math]::Round((Low 0.001), 1)
    over_25ms = @($ft | Where-Object { $_ -gt 25 }).Count; over_40ms = @($ft | Where-Object { $_ -gt 40 }).Count
    process_cpu_cores = [math]::Round(($s1.Total - $s0.Total) / $wallMs, 2)
    threads_over_50pct = @($threads | Where-Object Pct -ge 50).Count
    threads_over_10pct = @($threads | Where-Object Pct -ge 10).Count
    gpu_util_avg = [math]::Round(($gpu | Measure-Object -Average).Average, 1)
    top_threads = @($threads | Select-Object -First 12 | ForEach-Object { "$($_.Pct)% $($_.Name) [$($_.Id)]" })
}
$summary | ConvertTo-Json -Depth 3 | Set-Content "$base-summary.json"
$summary | ConvertTo-Json -Depth 3 | Write-Host
Write-Host "summary $base-summary.json"
