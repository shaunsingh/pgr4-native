# Sends held key presses to pgr4_recompiled.exe (run with --mnk_mode) from the desktop session.
# A tap from SendInput/PostMessage can fall between two guest XInput polls, so each key is held.
# Must run in the interactive session: run.ps1 -Action keys -Keys "..." creates the task.
#
#   keys.ps1 -Keys "return:200 wait:3000 space:200 e:15000"   # key:hold_ms, wait:ms
#   key names: return, space, escape, backspace, up/down/left/right, shift+<key>, or a letter/digit
param([Parameter(Mandatory = $true)][string]$Keys, [string]$Log = 'C:\recomp\logs\pgr4-drawcount-keys.log')
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
$vk = @{ return = 0x0D; space = 0x20; escape = 0x1B; backspace = 0x08; shift = 0x10
         up = 0x26; down = 0x28; left = 0x25; right = 0x27 }
function Code([string]$name) {
    if ($vk.ContainsKey($name)) { return [byte]$vk[$name] }
    if ($name.Length -eq 1) { return [byte][char]$name.ToUpper() }
    throw "unknown key $name"
}
function Down([byte]$c) { $ext = if ($c -ge 0x25 -and $c -le 0x28) { 1 } else { 0 }; [K]::keybd_event($c, [byte][K]::MapVirtualKey($c, 0), $ext, [UIntPtr]::Zero) }
function Up([byte]$c) { $ext = if ($c -ge 0x25 -and $c -le 0x28) { 1 } else { 0 }; [K]::keybd_event($c, [byte][K]::MapVirtualKey($c, 0), 2 -bor $ext, [UIntPtr]::Zero) }

$p = Get-Process pgr4_recompiled -ErrorAction Stop | Select-Object -First 1
$ok = [K]::SetForegroundWindow($p.MainWindowHandle)
Add-Content $Log "$(Get-Date -Format o) start fg=$ok hwnd=$($p.MainWindowHandle) foreground=$([K]::GetForegroundWindow()) keys=$Keys"
foreach ($step in $Keys -split '\s+' | Where-Object { $_ }) {
    $name, $ms = $step -split ':'
    $ms = if ($ms) { [int]$ms } else { 200 }
    if ($name -eq 'wait') { Start-Sleep -Milliseconds $ms; continue }
    $parts = $name -split '\+'
    $codes = @($parts | ForEach-Object { Code $_ })
    $codes | ForEach-Object { Down $_ }
    Start-Sleep -Milliseconds $ms
    [array]::Reverse($codes); $codes | ForEach-Object { Up $_ }
    Start-Sleep -Milliseconds 150
}
Add-Content $Log "$(Get-Date -Format o) done"
