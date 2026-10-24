# Prints a run's output file from a build tree's lab folder as clean UTF-8 text (the files are
# written by PowerShell redirection, with CRLF and console escape codes), minus noise lines.
# Argument: base64 JSON {dest, tag, all}. `lab pgr4 out <tree> <tag> [--all]`.
param([string]$Spec)
$ErrorActionPreference = 'Stop'
$a = [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($Spec)) | ConvertFrom-Json
$path = Join-Path $a.dest "lab\$($a.tag).out"
if (-not (Test-Path $path)) {
    $cands = Get-ChildItem (Join-Path $a.dest 'lab') -Filter '*.out' | Sort-Object LastWriteTime -Descending | Select-Object -First 8
    throw "no $path; newest outputs: $(($cands | ForEach-Object BaseName) -join ', ')"
}
$esc = [char]27
foreach ($line in Get-Content $path) {
    $line = $line -replace "$esc\[[0-9;?]*[A-Za-z]", ''
    if (-not $a.all -and $line -match 'slow upload|Building CXX|^\s*$|SUCCESS: |WARNING: Task may not run') { continue }
    $line
}
