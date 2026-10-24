# What is happening on the host for PGR4 runs: queue tickets, the game lock, running games, compiler
# processes and fleet jobs. `lab pgr4 status`. No argument.
$ErrorActionPreference = 'SilentlyContinue'
$now = Get-Date
"== queue (C:\recomp\locks\queue, oldest first)"
$tickets = Get-ChildItem C:\recomp\locks\queue -File | Sort-Object Name
if ($tickets) {
    foreach ($t in $tickets) { '  {0}  touched {1:N0} s ago' -f $t.Name, ($now - $t.LastWriteTime).TotalSeconds }
} else { '  empty' }
"== game lock"
if (Test-Path C:\recomp\locks\game.lock) {
    $l = Get-Item C:\recomp\locks\game.lock
    '  {0}  (age {1:N1} min)' -f (Get-Content $l.FullName -Raw).Trim(), ($now - $l.LastWriteTime).TotalMinutes
} else { '  free' }
"== games running"
$games = Get-Process pgr4_recompiled
if ($games) {
    foreach ($g in $games) { '  pid {0}  {1}  started {2:HH:mm:ss}' -f $g.Id, $g.Path, $g.StartTime }
} else { '  none' }
"== compiler processes"
$c = @(Get-Process clang-cl, cl, ninja, lld-link, link, cmake)
'  {0}' -f $c.Count
foreach ($n in ($c | Group-Object Name)) { '    {0} x{1}' -f $n.Name, $n.Count }
"== draw-debug file"
if (Test-Path C:\recomp\logs\pgr4-native-captures\draw-debug.txt) {
    '  PRESENT: {0}  (delete it when done)' -f (Get-Content C:\recomp\logs\pgr4-native-captures\draw-debug.txt -Raw).Trim()
} else { '  none' }
"== scheduled tasks"
$tasks = schtasks /query /fo csv 2>$null | ConvertFrom-Csv | Where-Object TaskName -match 'recomp'
if ($tasks) { foreach ($t in $tasks) { '  {0}  {1}' -f $t.TaskName, $t.Status } } else { '  none' }
