# Lists files in a directory on the host as "FILE <path>" lines, for the lab side to copy back.
# Argument: base64 JSON {dir, include: [wildcards]}.
param([string]$Spec)
$ErrorActionPreference = 'Stop'
$a = [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($Spec)) | ConvertFrom-Json
if (-not (Test-Path $a.dir)) { throw "no directory $($a.dir)" }
foreach ($pat in $a.include) {
    Get-ChildItem $a.dir -Filter $pat -File | ForEach-Object { "FILE $($_.FullName)" }
}
