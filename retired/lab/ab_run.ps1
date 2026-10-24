# A/B bench: alternates race runs of A and B (A,B then B,A each round, so drift during the session
# hits both sides), unprofiled and uncapped, then compares frame time bin by bin after the race
# mark (ab_compare.py). A and B are a tree plus flags: two trees for two builds, or one tree with
# two flag sets for a runtime switch.
#   pwsh -NoProfile -File ab_run.ps1 -DestA <tree> [-DestB <tree>] [-ExtraA "..."] [-ExtraB "..."]
#        [-ModeA primary] [-ModeB primary] [-Rounds 2] [-Tag ab] [-Owner name] [-BuildA] [-BuildB]
# -BuildA/-BuildB build that tree from its lab\perf-native.tar first (`lab pgr4 ab --a-ref/--b-ref`
# uploads them). Output: <DestA>\lab\<Tag>-runs.csv + the table.
param([Parameter(Mandatory)][string]$DestA, [string]$DestB = '', [string]$ExtraA = '', [string]$ExtraB = '',
      [ValidateSet('primary', 'native', 'none')][string]$ModeA = 'primary',
      [ValidateSet('primary', 'native', 'none')][string]$ModeB = 'primary',
      [int]$Rounds = 2, [string]$Tag = 'ab', [string]$Owner = 'agent', [switch]$BuildA, [switch]$BuildB)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
if (-not $DestB) { $DestB = $DestA }
foreach ($b in @(@($BuildA, $DestA), @($BuildB, $DestB))) {
    if (-not $b[0]) { continue }
    Write-Host "=== build $($b[1])"
    pwsh -NoProfile -File "$here\perf_build.ps1" -Dest $b[1] | Select-Object -Last 3
    $blog = "$($b[1])\lab\perf-build.log"
    if (-not (Select-String -Path $blog -Pattern '=== install done' -Quiet) -or (Select-String -Path $blog -Pattern '\): error|error LNK' -Quiet)) {
        throw "build of $($b[1]) failed (see $blog)"
    }
}
$sides = @{
    A = @{ Dest = $DestA; Extra = $ExtraA; Mode = $ModeA }
    B = @{ Dest = $DestB; Extra = $ExtraB; Mode = $ModeB }
}
$rows = @()
for ($r = 1; $r -le $Rounds; $r++) {
    $order = if ($r % 2) { 'A', 'B' } else { 'B', 'A' }
    foreach ($s in $order) {
        $cfg = $sides[$s]
        $t = "$Tag-$s$r"
        $out = "$($cfg.Dest)\lab\$t.out"
        New-Item -ItemType Directory -Force "$($cfg.Dest)\lab" | Out-Null
        Write-Host "=== $t ($($cfg.Mode) $($cfg.Extra)) in $($cfg.Dest)"
        pwsh -NoProfile -File "$here\co_run.ps1" -Dest $cfg.Dest -Tag $t -Owner $Owner -Mode $cfg.Mode -Race -Uncapped -Extra $cfg.Extra *> $out
        $text = Get-Content $out
        $comp = ($text | Select-String -Pattern 'compiler processes at hold start / end: (\d+ / \d+)' | Select-Object -First 1).Matches.Groups[1].Value
        $ilog = "C:\recomp\logs\pgr4-co-input-$t.log"
        $mark = if (Test-Path $ilog) { ((Select-String -Path $ilog -Pattern '^(\S+)\s+mark race' | Select-Object -First 1).Matches.Groups[1].Value) } else { '' }
        # "-20*": the stamp after the tag, so tag ab-A1 never picks up ab-A10's log.
        $log = Get-ChildItem C:\recomp\logs -Filter "pgr4-native-$t-20*.log" | Where-Object Name -notmatch 'stream|stdout' |
            Sort-Object LastWriteTime | Select-Object -Last 1
        if (-not $mark -or -not $log) { Write-Host "  $t did not reach the race (see $out); skipped"; continue }
        Write-Host "  mark $mark, compilers $comp"
        $rows += [pscustomobject]@{ tag = "$s$r"; side = $s; log = $log.FullName; mark = $mark; compilers = $comp }
    }
}
$csv = "$DestA\lab\$Tag-runs.csv"
$rows | Export-Csv -NoTypeInformation -Path $csv
Write-Host "=== comparison ($csv)"
python "$here\ab_compare.py" $csv
