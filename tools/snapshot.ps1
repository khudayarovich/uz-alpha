# Developer helper: renders the UzAlpha window to a PNG without showing it on
# screen. Starts the app hidden, places its window far off-screen, captures it
# and closes the app again.
#   powershell -ExecutionPolicy Bypass -File tools\snapshot.ps1 build\app\UzAlpha.exe out.png
param(
    [string]$Exe = "build\app\UzAlpha.exe",
    [string]$Out = "build\snapshot.png"
)

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class UzaSnap {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindowW(string cls, IntPtr title);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool RedrawWindow(IntPtr h, IntPtr rect, IntPtr rgn, uint flags);
    [DllImport("user32.dll")] public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr ctx);
}
"@
# Real pixels, not DPI-virtualised ones (per-monitor v2 = -4).
[UzaSnap]::SetThreadDpiAwarenessContext([IntPtr](-4)) | Out-Null

# Never touch a copy that is already running (the user's own tray app).
if ([UzaSnap]::FindWindowW("UzAlphaMainWindow", [IntPtr]::Zero) -ne [IntPtr]::Zero) {
    Write-Output "UzAlpha is already running; close it first (tray icon > Çiqiş). Nothing was done."
    exit 1
}
$proc = Start-Process -FilePath $Exe -ArgumentList "/tray" -PassThru
$hwnd = [IntPtr]::Zero
for ($i = 0; $i -lt 50 -and $hwnd -eq [IntPtr]::Zero; $i++) {
    Start-Sleep -Milliseconds 100
    $hwnd = [UzaSnap]::FindWindowW("UzAlphaMainWindow", [IntPtr]::Zero)
}
if ($hwnd -eq [IntPtr]::Zero) {
    Write-Output "window not found"
    if (-not $proc.HasExited) { $proc.Kill() }
    exit 1
}

# SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW
[UzaSnap]::SetWindowPos($hwnd, [IntPtr]::Zero, -4000, -4000, 0, 0, 0x0001 -bor 0x0004 -bor 0x0010 -bor 0x0040) | Out-Null
[UzaSnap]::RedrawWindow($hwnd, [IntPtr]::Zero, [IntPtr]::Zero, 0x0001 -bor 0x0080 -bor 0x0100) | Out-Null
Start-Sleep -Milliseconds 700

$r = New-Object UzaSnap+RECT
[UzaSnap]::GetWindowRect($hwnd, [ref]$r) | Out-Null
$w = $r.R - $r.L; $h = $r.B - $r.T
$bmp = New-Object System.Drawing.Bitmap $w, $h
$g = [System.Drawing.Graphics]::FromImage($bmp)
$hdc = $g.GetHdc()
[UzaSnap]::PrintWindow($hwnd, $hdc, 2) | Out-Null   # PW_RENDERFULLCONTENT
$g.ReleaseHdc($hdc); $g.Dispose()
$full = [System.IO.Path]::GetFullPath((Join-Path (Get-Location) $Out))
$bmp.Save($full, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()

[UzaSnap]::PostMessageW($hwnd, 0x8003, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null   # WM_APP_QUIT
if (-not $proc.WaitForExit(3000)) { $proc.Kill() }
Write-Output "saved $full (${w}x${h})"
