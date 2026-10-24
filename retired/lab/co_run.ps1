# Native race/menu run. Installed into C:\recomp\builds\pgr4\lab-scripts\perf\ by `lab pgr4 scripts`
# and started by perf_buildrun.ps1, perf_compare.ps1, ab_run.ps1 and demo_run.ps1.
#   -Dest: the build tree (its lab\native\run.ps1 starts the game).
#   default: --pgr4_native_primary (native presents into the game window, Xenos skips draws).
#   -Mode native: --pgr4_native (debug window, Xenos draws); -Mode none: no native renderer.
#   -Race: St Petersburg route, else the menu route. After the mark, holds HoldSec seconds and
#   samples per-thread CPU over the last MeasureSec seconds (thread names via GetThreadDescription).
# Takes C:\recomp\locks\game.lock atomically, in FIFO order through C:\recomp\locks\queue\ (up to
# 90 min). At the head of the queue it waits up to 6 min for no compiler on the host, and at the end it
# reports compilers seen during the measurement: a parallel build makes every stage 15-20 % slower.
#   -RecordSec N -RecordOut <file.mp4>: records the game window for N seconds, starting RecordDelay
#   seconds after the route's race mark (record_window.ps1, interactive task); the hold grows to fit.
#   -Tap turns on the push-buffer decoder (--pgr4_d3d_tap). It is off otherwise, including in trees
#   whose run.ps1 predates the switch (they get --pgr4_d3d_tap=false), because it costs the native
#   path ~0.8 ms per frame and made every number before 2026-10-21 21:40 pessimistic.
param([Parameter(Mandatory)][string]$Dest, [string]$Tag = 'co', [string]$Owner = 'agent',
      [ValidateSet('primary', 'native', 'none')][string]$Mode = 'primary',
      [string]$Extra = '', [switch]$Race, [int]$HoldSec = 40, [int]$MeasureSec = 20, [switch]$Uncapped,
      [string]$NotAfter = '23:59', [int]$RecordSec = 0, [double]$RecordDelay = 8, [string]$RecordOut = '',
      [switch]$Tap)
if ($RecordSec -gt 0) {
    if (-not $Race -or -not $RecordOut) { throw '-RecordSec needs -Race and -RecordOut' }
    $HoldSec = [Math]::Max($HoldSec, [int]($RecordDelay + $RecordSec) + 5)
}
$ErrorActionPreference = 'Stop'
$lab = "$Dest\lab"
$lock = 'C:\recomp\locks\game.lock'
$ilog = "C:\recomp\logs\pgr4-co-input-$Tag.log"
$compilers = 'clang-cl', 'cl', 'ninja', 'lld-link', 'link', 'cmake'
New-Item -ItemType Directory -Force (Split-Path $lock) | Out-Null
# FIFO queue: a ticket per waiting run, oldest name goes first. Waiting runs touch their ticket
# every 10 s; a ticket untouched for 2 min belongs to a dead run and is removed. Runs that predate
# the queue (or rr_run.ps1) still take a free lock directly.
$qdir = 'C:\recomp\locks\queue'
New-Item -ItemType Directory -Force $qdir | Out-Null
$ticket = Join-Path $qdir ('{0:yyyyMMdd-HHmmss-fff}-{1}-{2}' -f (Get-Date), $Owner, $Tag)
Set-Content $ticket "$Owner $Tag"
$waitUntil = (Get-Date).AddMinutes(90)
$headSince = $null
try {
    while ($true) {
        (Get-Item $ticket).LastWriteTime = Get-Date
        Get-ChildItem $qdir -File | Where-Object { $_.FullName -ne $ticket -and ((Get-Date) - $_.LastWriteTime).TotalMinutes -ge 2 } |
            ForEach-Object { Write-Host "removing stale ticket $($_.Name)"; Remove-Item $_.FullName -Force -ErrorAction SilentlyContinue }
        $head = Get-ChildItem $qdir -File | Sort-Object Name | Select-Object -First 1
        if ((Get-Date) -gt $waitUntil) { throw "gave up waiting for game.lock (queue head $($head.Name))" }
        if ($head.FullName -ne $ticket) {
            Write-Host "queued behind $($head.Name)"; Start-Sleep -Seconds 10; continue
        }
        if (-not $headSince) { $headSince = Get-Date }
        if ((Get-Process $compilers -ErrorAction SilentlyContinue) -and ((Get-Date) - $headSince).TotalMinutes -lt 6) {
            Write-Host "compiler running, waiting"; Start-Sleep -Seconds 10; continue
        }
        try {
            $fs = [System.IO.File]::Open($lock, 'CreateNew', 'Write', 'None')
            $bytes = [System.Text.Encoding]::ASCII.GetBytes("$Owner $Tag $(Get-Date -Format o)")
            $fs.Write($bytes, 0, $bytes.Length); $fs.Close()
            break
        } catch {
            if (Test-Path $lock) {
                $age = (Get-Date) - (Get-Item $lock).LastWriteTime
                $game = Get-Process pgr4_recompiled -ErrorAction SilentlyContinue
                if (-not $game -and $age.TotalMinutes -ge 45) { Write-Host "stale lock: $(Get-Content $lock)"; Remove-Item $lock -Force; continue }
                Write-Host "lock held ($(Get-Content $lock)), waiting"
            }
            Start-Sleep -Seconds 10
        }
    }
} finally {
    Remove-Item $ticket -Force -ErrorAction SilentlyContinue
}
if ((Get-Date) -gt [datetime]::ParseExact($NotAfter, 'HH:mm', $null)) { Remove-Item $lock -Force; throw "past $NotAfter, not starting" }
try {
    Remove-Item $ilog -ErrorAction SilentlyContinue
    $ud = "$Dest\run\runtime\userdata"
    if (Test-Path $ud) { Get-ChildItem $ud | Where-Object Name -ne 'cache' | Remove-Item -Recurse -Force }
    $x = "--pgr4_d3d_stats_frames=150 $(if ($Tap) { '--pgr4_d3d_tap' } else { '--pgr4_d3d_tap=false' }) $Extra"
    if ($Mode -eq 'primary') { $x = "--pgr4_native_primary $x" }
    if ($Uncapped) { $x = "--vsync=false --d3d12_present_vsync=false $x" }
    $runArgs = @('-NoProfile', '-File', "$lab\native\run.ps1", '-Action', 'start', '-Dest', $Dest, '-Tag', $Tag, "-Extra:$x")
    if ($Mode -ne 'none') { $runArgs += '-Native' }
    & pwsh @runArgs
    $exe = "$Dest\run\runtime\pgr4_recompiled.exe"
    $deadline = (Get-Date).AddSeconds(60)
    $proc = $null
    while (-not $proc -and (Get-Date) -lt $deadline) {
        Start-Sleep -Seconds 1
        $proc = Get-Process pgr4_recompiled -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $exe } | Select-Object -First 1
    }
    if (-not $proc) { throw 'game did not start' }
    Write-Host "game pid $($proc.Id) started $(Get-Date -Format HH:mm:ss)"
    $player = 'C:\recomp\builds\pgr4\lab-scripts\run\input-player.ps1'
    $cmd = "C:\recomp\logs\pgr4-co-input.cmd"
    if ($Race) {
        $keys = 'C:\recomp\builds\pgr4\lab-scripts\run\route-stpetersburg.keys'
        Set-Content -Path $cmd -Encoding ascii -Value "@echo off`r`npowershell -NoProfile -ExecutionPolicy Bypass -File `"$player`" -Script `"$keys`" -Log $ilog"
        $want = 'mark race'; $limit = 200
    } else {
        $route = '30000 return; +12000 return; +8000 space; +8000 d 300; +1800 space; +8000 space; +8000 space; +1000 mark menu'
        Set-Content -Path $cmd -Encoding ascii -Value "@echo off`r`npowershell -NoProfile -ExecutionPolicy Bypass -File `"$player`" -Inline `"$route`" -Log $ilog"
        $want = 'mark menu'; $limit = 150
    }
    schtasks /create /f /tn recomp-pgr4-co-input /sc once /st 23:59 /ru $env:USERNAME /it /rl LIMITED /tr "cmd /c `"$cmd`"" | Out-Null
    schtasks /run /tn recomp-pgr4-co-input | Out-Null
    if ($RecordSec -gt 0) {
        Remove-Item ([IO.Path]::ChangeExtension($RecordOut, '.record.log')), $RecordOut -ErrorAction SilentlyContinue
        $ff = (Get-Command ffmpeg -ErrorAction Stop).Source
        $rcmd = 'C:\recomp\logs\pgr4-co-record.cmd'
        Set-Content -Path $rcmd -Encoding ascii -Value ("@echo off`r`npwsh -NoProfile -ExecutionPolicy Bypass -File `"$PSScriptRoot\record_window.ps1`" " +
            "-InputLog `"$ilog`" -Out `"$RecordOut`" -Delay $RecordDelay -Seconds $RecordSec -Ffmpeg `"$ff`"")
        schtasks /create /f /tn recomp-pgr4-co-record /sc once /st 23:59 /ru $env:USERNAME /it /rl LIMITED /tr "cmd /c `"$rcmd`"" | Out-Null
        schtasks /run /tn recomp-pgr4-co-record | Out-Null
    }
    $deadline = (Get-Date).AddSeconds($limit)
    $ok = $false
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Seconds 5
        if ($proc.HasExited) { Write-Host "GAME EXITED code $($proc.ExitCode) $(Get-Date -Format HH:mm:ss)"; break }
        if (Test-Path $ilog) {
            if (Select-String -Path $ilog -Pattern "^\S+\s+$want" -Quiet) { $ok = $true; break }
            if (Select-String -Path $ilog -Pattern '^.*error' -Quiet) { break }
        }
    }
    Write-Host "route reached ${want}: $ok $(Get-Date -Format HH:mm:ss)"
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
    $c0 = @(Get-Process $compilers -ErrorAction SilentlyContinue).Count
    Start-Sleep -Seconds ([Math]::Max(0, $HoldSec - $MeasureSec))
    if (-not $proc.HasExited) {
        $proc.Refresh(); $a = @{}; foreach ($t in $proc.Threads) { try { $a[$t.Id] = $t.TotalProcessorTime.TotalMilliseconds } catch {} }
        $sw = [Diagnostics.Stopwatch]::StartNew()
        Start-Sleep -Seconds $MeasureSec
        $proc.Refresh(); $ms = $sw.Elapsed.TotalMilliseconds
        $rows = foreach ($t in $proc.Threads) {
            try { $b = $t.TotalProcessorTime.TotalMilliseconds } catch { continue }
            if (-not $a.ContainsKey($t.Id)) { continue }
            [pscustomobject]@{ Id = $t.Id; Name = [T]::Name([uint32]$t.Id); Pct = [Math]::Round(100 * ($b - $a[$t.Id]) / $ms, 1) }
        }
        Write-Host "thread CPU % over $MeasureSec s (100 = one core):"
        $rows | Sort-Object Pct -Descending | Select-Object -First 12 | Format-Table -AutoSize | Out-Host
    }
    $c1 = @(Get-Process $compilers -ErrorAction SilentlyContinue).Count
    Write-Host "compiler processes at hold start / end: $c0 / $c1 (nonzero: numbers are inflated, rerun)"
    Write-Host "alive at end: $(-not $proc.HasExited) $(Get-Date -Format HH:mm:ss)"
    if ($RecordSec -gt 0) {
        # The recorder ends on its own after RecordSec; give ffmpeg time to finish the file.
        $rlog = [IO.Path]::ChangeExtension($RecordOut, '.record.log')
        $rdl = (Get-Date).AddSeconds(45)
        while ((Get-Date) -lt $rdl -and -not ((Test-Path $rlog) -and (Select-String -Path $rlog -Pattern 'done:|FAILED' -Quiet))) { Start-Sleep -Seconds 1 }
        if (Test-Path $rlog) { Get-Content $rlog | ForEach-Object { Write-Host "record: $_" } } else { Write-Host 'record: no log (recorder task did not run?)' }
    }
} finally {
    & pwsh -NoProfile -File "$lab\native\run.ps1" -Action stop -Dest $Dest 2>&1 | Out-Host
    schtasks /delete /f /tn recomp-pgr4-co-input 2>$null | Out-Null
    if ($RecordSec -gt 0) { schtasks /delete /f /tn recomp-pgr4-co-record 2>$null | Out-Null }
    Remove-Item $lock -ErrorAction SilentlyContinue
    Write-Host 'stopped, lock removed'
    Get-ChildItem C:\recomp\logs -Filter "pgr4-native-$Tag-20*" | Sort-Object LastWriteTime | Select-Object -Last 4 | ForEach-Object { Write-Host "log: $($_.FullName)" }
}
