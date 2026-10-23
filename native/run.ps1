# Starts or stops the native-renderer checkout (build.ps1) in the interactive desktop session
# through a non-elevated scheduled task (docs/running-on-windows.md), and drives it.
#
#   pwsh -NoProfile -File run.ps1 -Action start [-Tag menu] [-Native]  # logs: C:\recomp\logs\pgr4-native-<tag>-*
#   pwsh -NoProfile -File run.ps1 -Action ctl -Command "trace menu 1"  # also: "skip draw", "skip none", "tap off"
#   pwsh -NoProfile -File run.ps1 -Action keys -Keys "return:250 wait:2000 space:250"   # ..\instrument\keys.ps1
#   pwsh -NoProfile -File run.ps1 -Action stop                     # ends the pgr4_recompiled.exe this build started
#
# Arguments: the same MTR "720p, 30 FPS" set as ..\instrument\run.ps1 (windowed 1280x720, guest in
# its native 1280x720 mode, --mnk_mode), the SDK stream counters and the D3D hooks. -Tap turns on
# the push-buffer decoder (--pgr4_d3d_tap: traces, draw counts; it costs the native path ~0.8 ms
# per frame, so performance runs leave it off). Traces go to C:\recomp\logs\pgr4-native-traces.
# -Native adds the native D3D12 renderer (--pgr4_native, debug window), its frame CSV
# (<base>-native.csv) and the capture folder <Logs>\pgr4-native-captures (capture.request,
# draw-debug.txt; unless -Extra names another).
param(
    [ValidateSet('start', 'stop', 'keys', 'ctl')][string]$Action = 'start',
    [string]$Keys = '',
    [string]$Command = '',
    [string]$Tag = 'run',
    [string]$Extra = '',
    [switch]$Native,
    [switch]$Tap,
    [string]$Dest = 'C:\recomp\builds\pgr4\native',
    [string]$Game = 'C:\recomp\dumps\pgr4\game',
    [string]$User = $env:USERNAME,  # the interactive user the game runs as
    [string]$Logs = 'C:\recomp\logs'
)
$ErrorActionPreference = 'Stop'
$runtime = Join-Path $Dest 'run\runtime'
$exe = Join-Path $runtime 'pgr4_recompiled.exe'
$task = 'recomp-pgr4-native'
$control = Join-Path $Logs 'pgr4-native.ctl'

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

if ($Action -eq 'ctl') {
    if (-not $Command) { throw '-Command is required' }
    # The game reads and deletes this file at its next Swap.
    Add-Content -Path $control -Value $Command -Encoding ascii
    Write-Host "queued: $Command"
    return
}

if ($Action -eq 'keys') {
    if (-not $Keys) { throw '-Keys is required' }
    $script = Join-Path $PSScriptRoot '..\instrument\keys.ps1'
    $cmd = Join-Path $Logs 'pgr4-native-keys.cmd'
    Set-Content -Path $cmd -Encoding ascii -Value "@echo off`r`npowershell -NoProfile -ExecutionPolicy Bypass -File `"$script`" -Keys `"$Keys`" -Log `"$Logs\pgr4-native-keys.log`""
    schtasks /create /f /tn "$task-keys" /sc once /st 00:00 /ru $User /it /rl LIMITED /tr "cmd /c `"$cmd`"" | Out-Null
    schtasks /run /tn "$task-keys" | Out-Host
    return
}

if (-not (Test-Path $exe)) { throw "missing $exe; run build.ps1 first" }
if (Get-Process pgr4_recompiled -ErrorAction SilentlyContinue) { throw 'a pgr4_recompiled.exe is already running' }
New-Item -ItemType Directory -Force -Path $Logs, (Join-Path $runtime 'userdata'), (Join-Path $Logs 'pgr4-native-traces') | Out-Null
Remove-Item $control -ErrorAction SilentlyContinue
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$base = Join-Path $Logs "pgr4-native-$Tag-$stamp"
$gameArgs = @(
    "--game_data_root $Game", "--user_data_root $runtime\userdata",
    '--execute_unclipped_draw_vs_on_cpu', '--gpu_plugin=xenos', '--d3d12_present_vsync', '--xmp_music_gain 0.35',
    '--d3d12_texture_shared_heaps', '--gpu_vblank_deadline_timer', '--gpu_wait_reg_mem_high_res',
    '--no-clear_memory_page_state', '--gpu_ring_wake_fast', '--texture_resource_reuse',
    '--texture_reuse_pool_limit_mib=256', '--texture_cache_memory_limit_soft_lifetime=30',
    '--fullscreen=false', '--window_width=1280', '--window_height=720', '--mnk_mode',
    '--guest_frame_stats', '--gpu_stream_stats', "--gpu_stream_stats_file=$base-stream.log",
    $(if ($Tap) { '--pgr4_d3d_tap' } else { '' }), "--pgr4_d3d_control=$control", "--pgr4_d3d_trace_dir=$Logs\pgr4-native-traces",
    "--pgr4_d3d_frame_csv=$base-frames.csv",
    $(if ($Native) { "--pgr4_native --pgr4_native_csv=$base-native.csv" } else { '' }),
    $(if ($Native -and $Extra -notmatch '--pgr4_native_capture_dir') { "--pgr4_native_capture_dir=$Logs\pgr4-native-captures" } else { '' }),
    $Extra,
    "--log_file $base.log"
) -join ' '
$cmd = "$base.cmd"
Set-Content -Path $cmd -Encoding ascii -Value "@echo off`r`ncd /d $runtime`r`n`"$exe`" $gameArgs > `"$base-stdout.log`" 2>&1"
schtasks /create /f /tn $task /sc once /st 00:00 /ru $User /it /rl LIMITED /tr "cmd /c `"$cmd`"" | Out-Host
schtasks /run /tn $task | Out-Host
Write-Host "log:        $base.log"
Write-Host "stream log: $base-stream.log"
Write-Host "frames:     $base-frames.csv"
if ($Native) { Write-Host "native:     $base-native.csv" }
