# Draws the UzAlpha icon (a rounded blue tile with a white "Ş") and writes it as
# a multi-size .ico. Run with Windows PowerShell:
#   powershell -ExecutionPolicy Bypass -File tools\make-icon.ps1 assets\uzalpha.ico
param([string]$OutPath = "assets\uzalpha.ico")

Add-Type -AssemblyName System.Drawing

function New-TileBitmap([int]$size) {
    $bmp = New-Object System.Drawing.Bitmap $size, $size, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAlias
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.Clear([System.Drawing.Color]::Transparent)

    $r = [single]($size * 0.22)
    $d = $r * 2
    $w = [single]$size
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $path.AddArc(0, 0, $d, $d, 180, 90)
    $path.AddArc($w - $d, 0, $d, $d, 270, 90)
    $path.AddArc($w - $d, $w - $d, $d, $d, 0, 90)
    $path.AddArc(0, $w - $d, $d, $d, 90, 90)
    $path.CloseFigure()

    $rect = New-Object System.Drawing.RectangleF 0, 0, $w, $w
    $top = [System.Drawing.Color]::FromArgb(255, 59, 130, 246)
    $bottom = [System.Drawing.Color]::FromArgb(255, 29, 78, 216)
    $brush = New-Object System.Drawing.Drawing2D.LinearGradientBrush $rect, $top, $bottom, 90.0
    $g.FillPath($brush, $path)

    $font = New-Object System.Drawing.Font "Segoe UI", ([single]($size * 0.56)), ([System.Drawing.FontStyle]::Bold), ([System.Drawing.GraphicsUnit]::Pixel)
    $fmt = New-Object System.Drawing.StringFormat
    $fmt.Alignment = [System.Drawing.StringAlignment]::Center
    $fmt.LineAlignment = [System.Drawing.StringAlignment]::Center
    # Nudge up a little so the cedilla stays inside the tile.
    $textRect = New-Object System.Drawing.RectangleF 0, ([single](-$size * 0.07)), $w, $w
    $g.DrawString([string][char]0x015E, $font, [System.Drawing.Brushes]::White, $textRect, $fmt)

    $g.Dispose(); $brush.Dispose(); $font.Dispose(); $path.Dispose()
    return $bmp
}

# One .ico image: a 32-bit DIB with an (empty) AND mask, or PNG for 256 px.
function Get-IconImageBytes([System.Drawing.Bitmap]$bmp) {
    $size = $bmp.Width
    $ms = New-Object System.IO.MemoryStream
    if ($size -ge 256) {
        $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
        return ,$ms.ToArray()
    }
    $bw = New-Object System.IO.BinaryWriter $ms
    $maskStride = [int]([math]::Ceiling($size / 32.0) * 4)
    $bw.Write([int]40); $bw.Write([int]$size); $bw.Write([int]($size * 2))
    $bw.Write([int16]1); $bw.Write([int16]32); $bw.Write([int]0)
    $bw.Write([int]($size * $size * 4 + $maskStride * $size))
    $bw.Write([int]0); $bw.Write([int]0); $bw.Write([int]0); $bw.Write([int]0)

    $rect = New-Object System.Drawing.Rectangle 0, 0, $size, $size
    $data = $bmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $row = New-Object byte[] ($size * 4)
    for ($y = $size - 1; $y -ge 0; $y--) {   # bottom-up
        $ptr = [IntPtr]::Add($data.Scan0, $y * $data.Stride)
        [System.Runtime.InteropServices.Marshal]::Copy($ptr, $row, 0, $row.Length)
        $bw.Write($row)
    }
    $bmp.UnlockBits($data)
    $bw.Write((New-Object byte[] ($maskStride * $size)))
    $bw.Flush()
    return ,$ms.ToArray()
}

# A language-code "icon": nothing but three letters on a transparent background,
# so that in the Windows keyboard switcher it reads like the text codes Windows
# draws itself (ENG, UZB...). Windows offers no way to supply real text there.
# The colour is a mid blue that stays readable on both the dark and the light theme.
function New-CodeBitmap([int]$size, [string]$text) {
    $bmp = New-Object System.Drawing.Bitmap $size, $size, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAliasGridFit
    $g.Clear([System.Drawing.Color]::Transparent)
    $fmt = New-Object System.Drawing.StringFormat([System.Drawing.StringFormat]::GenericTypographic)
    # The largest size at which the three letters still fit the width.
    $px = [single]$size
    do {
        $font = New-Object System.Drawing.Font "Bahnschrift Condensed", $px, ([System.Drawing.FontStyle]::Bold), ([System.Drawing.GraphicsUnit]::Pixel)
        $m = $g.MeasureString($text, $font, 1000, $fmt)
        if ($m.Width -le $size) { break }
        $font.Dispose(); $px -= 0.5
    } while ($px -gt 4)
    $brush = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 52, 120, 240))
    $g.DrawString($text, $font, $brush, [single](($size - $m.Width) / 2), [single](($size - $m.Height) / 2), $fmt)
    $g.Dispose(); $brush.Dispose(); $font.Dispose()
    return $bmp
}

function Write-Icon([string]$path, [scriptblock]$draw) {
    $sizes = 16, 20, 24, 32, 40, 48, 64, 256
    $images = @()
    foreach ($s in $sizes) {
        $bmp = & $draw $s
        $images += ,(Get-IconImageBytes $bmp)
        $bmp.Dispose()
    }
    $dir = Split-Path -Parent $path
    if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Force $dir | Out-Null }
    $fs = [System.IO.File]::Create((Join-Path (Get-Location) $path))
    $w = New-Object System.IO.BinaryWriter $fs
    $w.Write([int16]0); $w.Write([int16]1); $w.Write([int16]$sizes.Count)
    $offset = 6 + 16 * $sizes.Count
    for ($i = 0; $i -lt $sizes.Count; $i++) {
        $s = $sizes[$i]
        $dim = if ($s -ge 256) { 0 } else { $s }
        $w.Write([byte]$dim); $w.Write([byte]$dim); $w.Write([byte]0); $w.Write([byte]0)
        $w.Write([int16]1); $w.Write([int16]32)
        $w.Write([int]$images[$i].Length); $w.Write([int]$offset)
        $offset += $images[$i].Length
    }
    foreach ($img in $images) { $w.Write($img) }
    $w.Flush(); $fs.Close()
    Write-Output "wrote $path"
}

$assets = Split-Path -Parent $OutPath
Write-Icon $OutPath { param($s) New-TileBitmap $s }
Write-Icon (Join-Path $assets 'uza.ico') { param($s) New-CodeBitmap $s 'UZA' }
Write-Icon (Join-Path $assets 'uzm.ico') { param($s) New-CodeBitmap $s 'UZM' }
