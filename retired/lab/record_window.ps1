# Records the game window during a run, for demos. Runs in the interactive desktop session (co_run
# starts it as a scheduled task with -RecordSec): Desktop Duplication (ffmpeg ddagrab) only sees
# the logged-in user's desktop, and the window rectangle is only meaningful there.
#   -InputLog: the input player's log; recording starts at its "mark race" time + Delay, so two
#              runs (native, Xenos) start at the same point of the route.
#   -Out: .mp4 (H.264 NVENC); a .json beside it records the rectangle, output index and start.
param([Parameter(Mandatory)][string]$InputLog, [Parameter(Mandatory)][string]$Out,
      [double]$Delay = 8, [int]$Seconds = 20, [int]$Fps = 60, [string]$Ffmpeg = 'ffmpeg')
$ErrorActionPreference = 'Stop'
$logf = [IO.Path]::ChangeExtension($Out, '.record.log')
function Say($m) { "$(Get-Date -Format o) $m" | Add-Content $logf }
try {
    Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class W {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
}
'@
    [W]::SetProcessDPIAware() | Out-Null
    Add-Type -AssemblyName System.Windows.Forms
    $deadline = (Get-Date).AddMinutes(6)
    $mark = $null
    while (-not $mark -and (Get-Date) -lt $deadline) {
        $m = if (Test-Path $InputLog) { Select-String -Path $InputLog -Pattern '^(\S+)\s+mark race' | Select-Object -First 1 }
        if ($m) { $mark = [datetime]::Parse($m.Matches[0].Groups[1].Value) } else { Start-Sleep -Milliseconds 250 }
    }
    if (-not $mark) { throw "no 'mark race' in $InputLog" }
    $p = Get-Process pgr4_recompiled | Where-Object MainWindowHandle -ne 0 | Select-Object -First 1
    if (-not $p) { throw 'no game window' }
    $h = $p.MainWindowHandle
    $r = New-Object W+RECT; [W]::GetClientRect($h, [ref]$r) | Out-Null
    $pt = New-Object W+POINT; [W]::ClientToScreen($h, [ref]$pt) | Out-Null
    $w = ($r.R - $r.L) -band -2; $hgt = ($r.B - $r.T) -band -2   # even sizes for H.264
    $scr = [System.Windows.Forms.Screen]::FromHandle($h).Bounds
    # ddagrab numbers DXGI outputs its own way: probe each and take the one the size of this screen.
    # ffmpeg logs to stderr; with 'Stop' a redirected stderr line would end the script.
    $ErrorActionPreference = 'Continue'
    $idx = -1
    for ($i = 0; $i -lt 4 -and $idx -lt 0; $i++) {
        $probe = & $Ffmpeg -hide_banner -f lavfi -i "ddagrab=output_idx=$($i):framerate=5" -frames:v 1 -f null - 2>&1 | Out-String
        if ($probe -match "(\d{3,5})x(\d{3,5})" -and [int]$Matches[1] -eq $scr.Width -and [int]$Matches[2] -eq $scr.Height) { $idx = $i }
    }
    if ($idx -lt 0) { throw "no ddagrab output matches screen $($scr.Width)x$($scr.Height)" }
    $ox = $pt.X - $scr.X; $oy = $pt.Y - $scr.Y
    Say "window client ${w}x${hgt} at $($pt.X),$($pt.Y); screen $scr; output $idx offset $ox,$oy; mark $mark"
    $start = $mark.AddSeconds($Delay)
    $wait = ($start - (Get-Date)).TotalMilliseconds
    if ($wait -gt 0) { Start-Sleep -Milliseconds ([int]$wait) } else { Say "late by $(-$wait) ms" }
    $t0 = Get-Date
    # ddagrab hands NVENC D3D11 frames directly; forcing -pix_fmt would need a software
    # conversion that ffmpeg cannot insert after a d3d11 source ("Impossible to convert").
    & $Ffmpeg -hide_banner -loglevel error -y -f lavfi `
        -i "ddagrab=output_idx=$($idx):framerate=$($Fps):offset_x=$($ox):offset_y=$($oy):video_size=$($w)x$($hgt):draw_mouse=0" `
        -t $Seconds -c:v h264_nvenc -preset p5 -cq 20 $Out 2>&1 | ForEach-Object { Say "ffmpeg: $_" }
    if (-not (Test-Path $Out) -or (Get-Item $Out).Length -lt 100KB) { throw "ffmpeg wrote no clip ($Out)" }
    @{ rect = @($pt.X, $pt.Y, $w, $hgt); output = $idx; mark = $mark.ToString('o'); start = $t0.ToString('o'); seconds = $Seconds; fps = $Fps } |
        ConvertTo-Json | Set-Content ([IO.Path]::ChangeExtension($Out, '.json'))
    Say "done: $Out $([math]::Round((Get-Item $Out).Length / 1MB, 1)) MB"
} catch {
    Say "FAILED: $_"
}
