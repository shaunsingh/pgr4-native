# Lists PGR4 build trees on the host with their firewall rule and exe time. Only trees that have a
# rule can be used for runs: a new exe path pops a firewall prompt that steals the input player's
# focus. `lab pgr4 trees`. No argument.
$ErrorActionPreference = 'SilentlyContinue'
$rules = @{}
foreach ($f in Get-NetFirewallApplicationFilter | Where-Object Program -match 'recomp\\builds\\pgr4') {
    $r = $f | Get-NetFirewallRule
    $rules[$f.Program.ToLower()] = $r.Action | Select-Object -First 1
}
Get-ChildItem C:\recomp\builds\pgr4 -Directory -Filter 'native*' | Sort-Object Name | ForEach-Object {
    $exe = Join-Path $_.FullName 'run\runtime\pgr4_recompiled.exe'
    $rule = $rules[$exe.ToLower()]
    $t = if (Test-Path $exe) { '{0:yyyy-MM-dd HH:mm}' -f (Get-Item $exe).LastWriteTime } else { 'no exe' }
    '{0,-22} {1,-6} {2}' -f $_.Name, ($(if ($rule) { $rule } else { 'NONE' })), $t
}
