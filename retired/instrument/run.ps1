# Starts or stops the instrumented MTR-PGR4 build (see build.ps1) in the interactive desktop
# session through a non-elevated scheduled task (docs/running-on-windows.md).
#
#   pwsh -NoProfile -File run.ps1 -Action start [-Tag menu]   # logs to C:\recomp\logs\pgr4-drawcount-<tag>-*.log
#   pwsh -NoProfile -File run.ps1 -Action stop                # ends the pgr4_recompiled.exe this build started
#   pwsh -NoProfile -File run.ps1 -Action keys -Keys "return:200 wait:2000 space:200"   # keys.ps1, in the desktop session
#
# Arguments: MTR v0.1.1 launcher's "720p, 30 FPS" set (STATE.md command without --pgr4_60fps and
# --pgr4_garage_walk_fix), windowed 1280x720 so the guest keeps its native 1280x720 video mode
# (a 1280x800 window would also change the guest mode, and with it the tiling), plus
# --mnk_mode (keyboard as pad: Return = Start, Space = A, E = right trigger) and the counters.
param(
    [ValidateSet('start', 'stop', 'keys')][string]$Action = 'start',
    [string]$Keys = '',
    [string]$Tag = 'run',
    [string]$Dest = 'C:\recomp\builds\pgr4\instrumented',
    [string]$Game = 'C:\recomp\dumps\pgr4\game',
    [string]$User = $env:USERNAME,  # the interactive user the game runs as
    [string]$Logs = 'C:\recomp\logs'
)
$ErrorActionPreference = 'Stop'
$runtime = Join-Path $Dest 'run\runtime'
$exe = Join-Path $runtime 'pgr4_recompiled.exe'
$task = 'recomp-pgr4-drawcount'

if ($Action -eq 'stop') {
    $procs = Get-Process pgr4_recompiled -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $exe }
    foreach ($p in $procs) {
        Write-Host "closing pid $($p.Id)"
        $null = $p.CloseMainWindow()
    }
    Start-Sleep -Seconds 5
    $procs | Where-Object { -not $_.HasExited } | ForEach-Object { Write-Host "killing pid $($_.Id)"; Stop-Process -Id $_.Id -Force }
    schtasks /delete /f /tn $task 2>$null | Out-Null
    schtasks /delete /f /tn "$task-keys" 2>$null | Out-Null
    return
}

if ($Action -eq 'keys') {
    if (-not $Keys) { throw '-Keys is required' }
    $script = Join-Path $PSScriptRoot 'keys.ps1'
    $cmd = Join-Path $Logs 'pgr4-drawcount-keys.cmd'
    Set-Content -Path $cmd -Encoding ascii -Value "@echo off`r`npowershell -NoProfile -ExecutionPolicy Bypass -File `"$script`" -Keys `"$Keys`""
    schtasks /create /f /tn "$task-keys" /sc once /st 00:00 /ru $User /it /rl LIMITED /tr "cmd /c `"$cmd`"" | Out-Null
    schtasks /run /tn "$task-keys" | Out-Host
    return
}

if (-not (Test-Path $exe)) { throw "missing $exe; run build.ps1 first" }
if (Get-Process pgr4_recompiled -ErrorAction SilentlyContinue) { throw 'a pgr4_recompiled.exe is already running' }
New-Item -ItemType Directory -Force -Path $Logs, (Join-Path $runtime 'userdata') | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$base = Join-Path $Logs "pgr4-drawcount-$Tag-$stamp"
$gameArgs = @(
    "--game_data_root $Game", "--user_data_root $runtime\userdata",
    '--execute_unclipped_draw_vs_on_cpu', '--gpu_plugin=xenos', '--d3d12_present_vsync', '--xmp_music_gain 0.35',
    '--d3d12_texture_shared_heaps', '--gpu_vblank_deadline_timer', '--gpu_wait_reg_mem_high_res',
    '--no-clear_memory_page_state', '--gpu_ring_wake_fast', '--texture_resource_reuse',
    '--texture_reuse_pool_limit_mib=256', '--texture_cache_memory_limit_soft_lifetime=30',
    '--fullscreen=false', '--window_width=1280', '--window_height=720', '--mnk_mode',
    '--guest_frame_stats', '--gpu_stream_stats', "--gpu_stream_stats_file=$base-stream.log",
    "--log_file $base.log"
) -join ' '
$cmd = Join-Path $Logs "pgr4-drawcount-$Tag-$stamp.cmd"
Set-Content -Path $cmd -Encoding ascii -Value "@echo off`r`ncd /d $runtime`r`n`"$exe`" $gameArgs > `"$base-stdout.log`" 2>&1"
schtasks /create /f /tn $task /sc once /st 00:00 /ru $User /it /rl LIMITED /tr "cmd /c `"$cmd`"" | Out-Host
schtasks /run /tn $task | Out-Host
Write-Host "stream log: $base-stream.log"
Write-Host "log:        $base.log"
