# Replays a timed key script into pgr4_recompiled.exe (started with --mnk_mode) from the
# interactive desktop session. bench.ps1 starts it through a scheduled task; it cannot inject
# input from session 0.
#
#   input-player.ps1 -Script C:\...\route-stpetersburg.keys [-Log C:\recomp\logs\x-input.log]
#   input-player.ps1 -Inline "0 return 250; +3000 space 250" -NoWait
#
# Script format, one event per line ('#' starts a comment, ';' also separates events):
#   <at_ms> <key> [hold_ms]     press <key> at <at_ms> after time zero, hold it hold_ms (default 250)
#   +<ms>   <key> [hold_ms]     same, <ms> after the previous event's start time
#   <at_ms> mark <label>        write "mark <label>" to the log (bench.ps1 waits for these)
# Time zero is when the game's window first exists (or the player's start with -NoWait).
# Keys: return, space, escape, backspace, tab, up/down/left/right, a letter or digit, and
# chords like shift+e. --mnk_mode binds Return = Start, Space = A, Backspace = B, E = right
# trigger, Q = left trigger, WASD = left stick (ReXGlue mnk_input_driver.cpp keybind_* cvars).
# A tap shorter than a guest XInput poll can be missed, so every press is a hold (>= 150 ms).
param(
    [string]$Script = '',
    [string]$Inline = '',
    [string]$Log = 'C:\recomp\logs\pgr4-input.log',
    [string]$Process = 'pgr4_recompiled',
    [int]$WindowTimeoutSec = 120,
    [switch]$NoWait
)
$ErrorActionPreference = 'Stop'
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class K {
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
  [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint type);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
}
'@
$vk = @{ return = 0x0D; enter = 0x0D; space = 0x20; escape = 0x1B; backspace = 0x08; tab = 0x09
         shift = 0x10; up = 0x26; down = 0x28; left = 0x25; right = 0x27 }
function Code([string]$name) {
    $name = $name.ToLower()
    if ($vk.ContainsKey($name)) { return [byte]$vk[$name] }
    if ($name.Length -eq 1) { return [byte][char]$name.ToUpper() }
    throw "unknown key $name"
}
function Send([byte]$c, [bool]$up) {
    $flags = if ($c -ge 0x25 -and $c -le 0x28) { 1 } else { 0 }
    if ($up) { $flags = $flags -bor 2 }
    [K]::keybd_event($c, [byte][K]::MapVirtualKey($c, 0), $flags, [UIntPtr]::Zero)
}
function Say([string]$msg) { Add-Content -Path $Log -Value "$(Get-Date -Format o) $msg" }
# Bring the game to the foreground. Windows refuses SetForegroundWindow from a background process
# unless it just sent input, hence the Alt tap before each retry. A Windows Firewall prompt can
# take focus on a new exe's first network use; cancel-firewall.ps1 (elevated) clears those.
function Focus([IntPtr]$h, [int]$timeoutMs = 5000) {
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    while ([K]::GetForegroundWindow() -ne $h) {
        if ($sw.ElapsedMilliseconds -gt $timeoutMs) { return $false }
        Send 0x12 $false; Send 0x12 $true
        $null = [K]::SetForegroundWindow($h)
        Start-Sleep -Milliseconds 100
    }
    return $true
}

# Parse into absolute events.
$text = if ($Script) { Get-Content -Raw $Script } elseif ($Inline) { $Inline } else { throw '-Script or -Inline is required' }
$events = [System.Collections.Generic.List[object]]::new()
$prev = 0
foreach ($raw in $text -split "[`r`n;]+") {
    $line = ($raw -replace '#.*$', '').Trim()
    if (-not $line) { continue }
    $f = $line -split '\s+'
    if ($f.Count -lt 2) { throw "bad line: $raw" }
    $at = if ($f[0].StartsWith('+')) { $prev + [int]$f[0].Substring(1) } else { [int]$f[0] }
    if ($at -lt $prev) { throw "events out of order at: $raw" }
    $prev = $at
    if ($f[1] -eq 'mark') {
        $events.Add([pscustomobject]@{ At = $at; Mark = ($f[2..($f.Count - 1)] -join ' '); Keys = $null; Hold = 0 })
    } else {
        $hold = if ($f.Count -ge 3) { [int]$f[2] } else { 250 }
        $codes = @($f[1] -split '\+' | ForEach-Object { Code $_ })
        $events.Add([pscustomobject]@{ At = $at; Mark = $null; Keys = $f[1]; Codes = $codes; Hold = $hold })
    }
}

$deadline = (Get-Date).AddSeconds($WindowTimeoutSec)
$p = $null
while (-not $NoWait) {
    $p = Get-Process $Process -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
    if ($p) { break }
    if ((Get-Date) -gt $deadline) { Say "error: no $Process window after $WindowTimeoutSec s"; exit 1 }
    Start-Sleep -Milliseconds 100
}
if (-not $p) { $p = Get-Process $Process -ErrorAction Stop | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1 }
$hwnd = $p.MainWindowHandle
$clock = [System.Diagnostics.Stopwatch]::StartNew()
Say "start pid=$($p.Id) hwnd=$hwnd events=$($events.Count) script=$Script$Inline"

# Presses and releases run on one timeline, so a long hold does not delay later events.
$actions = [System.Collections.Generic.List[object]]::new()
$n = 0
foreach ($e in $events) {
    if ($e.Mark) { $actions.Add([pscustomobject]@{ At = $e.At; Order = 1; N = $n++; Kind = 'mark'; E = $e }); continue }
    $actions.Add([pscustomobject]@{ At = $e.At; Order = 1; N = $n++; Kind = 'down'; E = $e })
    $actions.Add([pscustomobject]@{ At = $e.At + $e.Hold; Order = 0; N = $n++; Kind = 'up'; E = $e })
}
$held = [System.Collections.Generic.List[object]]::new()
try {
    foreach ($a in ($actions | Sort-Object At, Order, N)) {
        # Wait in short steps; if the game lost focus, win it back and press the held keys again
        # (the guest's key state is cleared when its window loses focus).
        while (($wait = $a.At - $clock.ElapsedMilliseconds) -gt 0) {
            Start-Sleep -Milliseconds ([math]::Min($wait, 500))
            if (-not $p.HasExited -and [K]::GetForegroundWindow() -ne $hwnd) {
                $ok = Focus $hwnd 2000
                if ($ok) { foreach ($h in $held) { foreach ($c in $h.Codes) { Send $c $false } } }
                Say "refocus t=$($clock.ElapsedMilliseconds) ok=$ok held=$(($held | ForEach-Object Keys) -join ',')"
            }
        }
        $e = $a.E
        if ($a.Kind -eq 'up') {
            for ($i = $e.Codes.Count - 1; $i -ge 0; $i--) { Send $e.Codes[$i] $true }
            $null = $held.Remove($e)
            continue
        }
        if ($p.HasExited) { Say "error: game exited at $($clock.ElapsedMilliseconds) ms"; exit 1 }
        if ($a.Kind -eq 'mark') { Say "mark $($e.Mark) t=$($clock.ElapsedMilliseconds)"; continue }
        # A key sent while another window is in front could answer a dialog, so stop instead.
        if (-not (Focus $hwnd)) { Say "error: could not focus the game for $($e.Keys) at $($clock.ElapsedMilliseconds) ms (foreground $([K]::GetForegroundWindow()))"; exit 1 }
        $late = $clock.ElapsedMilliseconds - $e.At
        foreach ($c in $e.Codes) { Send $c $false }
        $held.Add($e)
        Say "key $($e.Keys) hold=$($e.Hold) at=$($e.At) late=$late"
    }
    Say 'done'
} finally {
    # Never leave a key down in the desktop session, even if the game exits mid-hold.
    foreach ($e in $held) { for ($i = $e.Codes.Count - 1; $i -ge 0; $i--) { Send $e.Codes[$i] $true } }
}
