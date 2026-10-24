# Cancels Windows Firewall "allow access" prompts in the desktop session. The first network use
# of a new exe raises one (pgr4_recompiled.exe opens a socket at race start). Cancel keeps the
# inbound Block rules Windows added when it raised the prompt and adds no Allow rule.
#
# The prompt is a high-integrity PickerHost.exe window, class Shell_SystemDialog, centred on the
# primary monitor. It sits in a system z-order band that EnumWindows does not list, managed
# UIAutomation sees none of its XAML content, and it ignores Escape. So this finds it with
# WindowFromPoint at the primary monitor's centre and clicks Cancel (bottom-right button, 113 px
# from the right edge and 40 px from the bottom at 100% scaling). Mouse input to a high-integrity
# window needs an elevated sender: bench.ps1 runs this through a /rl HIGHEST /it scheduled task.
# Loops until -GamePid exits; without -GamePid it makes one pass.
param([int]$GamePid = 0, [string]$Log = 'C:\recomp\logs\pgr4-cancel-firewall.log')
$ErrorActionPreference = 'Stop'
Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class Fw {
  public struct POINT { public int X, Y; }
  public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);
  [DllImport("user32.dll")] public static extern IntPtr WindowFromPoint(POINT p);
  [DllImport("user32.dll")] public static extern IntPtr GetAncestor(IntPtr h, uint flags);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint flags, int dx, int dy, uint data, UIntPtr extra);
  public static string Text(IntPtr h) { var s = new StringBuilder(128); GetWindowText(h, s, 128); return s.ToString(); }
  public static string Class(IntPtr h) { var s = new StringBuilder(128); GetClassName(h, s, 128); return s.ToString(); }
}
'@
$null = [Fw]::SetProcessDPIAware()
function Say([string]$m) { Add-Content $Log "$(Get-Date -Format o) $m" }
function Get-Prompt {
    $c = New-Object Fw+POINT
    $c.X = [int]([Fw]::GetSystemMetrics(0) / 2); $c.Y = [int]([Fw]::GetSystemMetrics(1) / 2)
    $h = [Fw]::GetAncestor([Fw]::WindowFromPoint($c), 2)
    if ([Fw]::Class($h) -ne 'Shell_SystemDialog' -or [Fw]::Text($h) -ne 'Windows Security') { return $null }
    $owner = 0; $null = [Fw]::GetWindowThreadProcessId($h, [ref]$owner)
    if ((Get-Process -Id $owner -ErrorAction SilentlyContinue).ProcessName -ne 'PickerHost') { return $null }
    return $h
}
function Cancel-Prompts {
    for ($n = 0; $n -lt 5 -and ($h = Get-Prompt); $n++) {
        $r = New-Object Fw+RECT; $null = [Fw]::GetWindowRect($h, [ref]$r)
        $x = $r.R - 113; $y = $r.B - 40
        $at = New-Object Fw+POINT; $at.X = $x; $at.Y = $y
        if ([Fw]::GetAncestor([Fw]::WindowFromPoint($at), 2) -ne $h) { Say "prompt $h rect $($r.L),$($r.T),$($r.R),$($r.B): Cancel point not on it"; return }
        $old = New-Object Fw+POINT; $null = [Fw]::GetCursorPos([ref]$old)
        $null = [Fw]::SetCursorPos($x, $y)
        [Fw]::mouse_event(0x2, 0, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 50; [Fw]::mouse_event(0x4, 0, 0, 0, [UIntPtr]::Zero)
        Start-Sleep -Milliseconds 300
        $null = [Fw]::SetCursorPos($old.X, $old.Y)
        Say "clicked Cancel on firewall prompt $h at $x,$y"
        Start-Sleep -Milliseconds 700
    }
}
do {
    Cancel-Prompts
    if (-not $GamePid) { break }
    Start-Sleep -Seconds 1
} while (Get-Process -Id $GamePid -ErrorAction SilentlyContinue)
