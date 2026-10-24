# Parse-checks every .ps1 in a folder (the shared PGR4 lab scripts after `lab pgr4 scripts`).
# Argument: base64 JSON {dir}.
param([string]$Spec)
$ErrorActionPreference = 'Stop'
$a = [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($Spec)) | ConvertFrom-Json
$bad = 0
foreach ($f in Get-ChildItem $a.dir -Filter *.ps1) {
    $errs = $null
    [System.Management.Automation.Language.Parser]::ParseFile($f.FullName, [ref]$null, [ref]$errs) | Out-Null
    if ($errs.Count) { $bad++; '{0}: {1}' -f $f.Name, (($errs | ForEach-Object Message) -join '; ') }
}
if ($bad) { throw "$bad script(s) failed to parse" }
'all scripts parse'
