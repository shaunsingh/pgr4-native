# Demo clips of the race: records the game window for -Seconds, starting -Delay seconds after the
# route's race mark, with the native renderer (primary mode). -Xenos also records a Xenos-only run
# at the same route time and puts the two side by side, labelled. Writes stills at 25/50/75 %.
#   pwsh -NoProfile -File demo_run.ps1 -Dest <tree> [-Tag demo] [-Seconds 20] [-Delay 8]
#        [-Xenos] [-Uncapped] [-Extra "..."] [-Owner name]
# Output in C:\recomp\logs\pgr4-demo-<Tag>\: native.mp4 [xenos.mp4 sbs.mp4] still-*.png;
# `lab pgr4 demo-get <Tag>` copies the main clip (sbs.mp4, else native.mp4) and the stills.
param([Parameter(Mandatory)][string]$Dest, [string]$Tag = 'demo', [string]$Owner = 'agent', [string]$Extra = '',
      [int]$Seconds = 20, [double]$Delay = 8, [switch]$Xenos, [switch]$Uncapped)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$outDir = "C:\recomp\logs\pgr4-demo-$Tag"
New-Item -ItemType Directory -Force $outDir, "$Dest\lab" | Out-Null
$runs = @(@{ Mode = 'primary'; Name = 'native' })
if ($Xenos) { $runs += @{ Mode = 'none'; Name = 'xenos' } }
$clips = @{}
foreach ($r in $runs) {
    $t = "demo-$Tag-$($r.Name)"
    $mp4 = "$outDir\$($r.Name).mp4"
    $a = @('-NoProfile', '-File', "$here\co_run.ps1", '-Dest', $Dest, '-Tag', $t, '-Owner', $Owner, '-Mode', $r.Mode,
           '-Race', '-RecordSec', $Seconds, '-RecordDelay', $Delay, '-RecordOut', $mp4)
    if ($Extra) { $a += "-Extra:$Extra" }
    if ($Uncapped) { $a += '-Uncapped' }
    Write-Host "=== $t"
    pwsh @a *> "$Dest\lab\$t.out"
    Get-Content "$Dest\lab\$t.out" | Select-String -Pattern 'route reached|alive at end|record:' | ForEach-Object { Write-Host $_.Line }
    # Game frame rate over the recorded stretch, from the game log.
    $ilog = "C:\recomp\logs\pgr4-co-input-$t.log"
    $m = if (Test-Path $ilog) { Select-String -Path $ilog -Pattern '^(\S+)\s+mark race' | Select-Object -First 1 }
    $log = Get-ChildItem C:\recomp\logs -Filter "pgr4-native-$t-20*.log" | Where-Object Name -notmatch 'stream|stdout' |
        Sort-Object LastWriteTime | Select-Object -Last 1
    if ($m -and $log) {
        $from = ([datetime]::Parse($m.Matches[0].Groups[1].Value)).AddSeconds($Delay).ToString('HH:mm:ss')
        Write-Host "  $($r.Name) game frame rate while recording: $(python "$here\perf_fps.py" $log.FullName $from $Seconds | Select-Object -First 1)"
    }
    if (Test-Path $mp4) { $clips[$r.Name] = $mp4 } else { Write-Host "  no clip for $($r.Name)" }
}
if (-not $clips.Count) { throw 'no clips recorded' }
$ErrorActionPreference = 'Continue'   # ffmpeg logs to stderr
$main = $clips['native']
if ($clips.ContainsKey('native') -and $clips.ContainsKey('xenos')) {
    # Top centre: the race HUD fills the corners and the bottom-left (lap counter).
    $font = "fontfile='C\:/Windows/Fonts/arialbd.ttf':fontsize=26:fontcolor=white:box=1:boxcolor=black@0.55:boxborderw=8:x=(w-tw)/2:y=14"
    $fc = "[0:v]drawtext=$($font):text='Xenos GPU emulation'[a];[1:v]drawtext=$($font):text='Native D3D12 renderer'[b];[a][b]hstack=inputs=2"
    $main = "$outDir\sbs.mp4"
    & ffmpeg -hide_banner -loglevel error -y -i $clips['xenos'] -i $clips['native'] -filter_complex $fc -c:v h264_nvenc -preset p5 -cq 21 -pix_fmt yuv420p $main
    if (-not (Test-Path $main)) { throw 'side-by-side encode failed' }
}
if (-not $main) { $main = $clips['xenos'] }
foreach ($pct in 25, 50, 75) {
    $png = "$outDir\still-$pct.png"
    & ffmpeg -hide_banner -loglevel error -y -ss ($Seconds * $pct / 100) -i $main -frames:v 1 $png
}
Get-ChildItem $outDir -File | ForEach-Object { Write-Host ('{0,-14} {1,8:N1} MB' -f $_.Name, ($_.Length / 1MB)) }
