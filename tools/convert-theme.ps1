# DSi Recorder theme converter
# Turns any picture (PNG, JPG, GIF, BMP, WebP, ...) into theme.bmp for DSi Recorder:
#  - crops the centre to the DSi screen shape (4:3)
#  - resizes to 256x192 (one DSi screen) with high-quality smoothing
#  - gives it a gentle colour / contrast boost so it reads well on the DSi screen
#  - saves a 24-bit BMP and copies it to <SD card>\DSiRecorder\theme.bmp if a card is found
#
# Usage: drag a picture onto "Make DSi theme.bat", or
#        powershell -ExecutionPolicy Bypass -File convert-theme.ps1 <picture> [-Saturation 1.2] [-Contrast 1.1] [-Brightness 0]

param(
    [Parameter(Mandatory = $true, Position = 0)][string]$Path,
    [double]$Saturation = 1.20,   # 1.0 = unchanged
    [double]$Contrast   = 1.10,   # 1.0 = unchanged
    [double]$Brightness = 0.00,   # -0.2 .. 0.2
    [int]$Width = 256,
    [int]$Height = 192
)

Add-Type -AssemblyName System.Drawing
$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $Path)) { throw "File not found: $Path" }
$fullPath = (Resolve-Path -LiteralPath $Path).Path

# Load the picture. GDI+ (System.Drawing) can't read WebP/AVIF/HEIC and reports that
# as "Out of memory", so fall back to Windows Imaging Component (WPF), which uses the
# codecs installed in Windows, and hand the result to System.Drawing as a PNG.
function Load-Picture([string]$file) {
    try {
        $bytes = [System.IO.File]::ReadAllBytes($file)
        $ms = New-Object System.IO.MemoryStream(, $bytes)
        return [System.Drawing.Image]::FromStream($ms)
    }
    catch {
        Add-Type -AssemblyName PresentationCore, WindowsBase
        try {
            $fs = [System.IO.File]::OpenRead($file)
            $decoder = [System.Windows.Media.Imaging.BitmapDecoder]::Create(
                $fs,
                [System.Windows.Media.Imaging.BitmapCreateOptions]::PreservePixelFormat,
                [System.Windows.Media.Imaging.BitmapCacheOption]::OnLoad)
            $frame = $decoder.Frames[0]
            $fs.Dispose()
            $enc = New-Object System.Windows.Media.Imaging.PngBitmapEncoder
            $enc.Frames.Add([System.Windows.Media.Imaging.BitmapFrame]::Create($frame))
            $png = New-Object System.IO.MemoryStream
            $enc.Save($png)
            $png.Position = 0
            return [System.Drawing.Image]::FromStream($png)
        }
        catch {
            Write-Host ""
            Write-Host "Windows can't read this picture's format." -ForegroundColor Red
            Write-Host "Open it in Paint (or a browser), save it as PNG or JPG, and drag that file on instead." -ForegroundColor Yellow
            Write-Host "(WebP/HEIC/AVIF need the matching 'Image Extensions' from the Microsoft Store.)" -ForegroundColor Yellow
            exit 1
        }
    }
}

$src = Load-Picture $fullPath

try {
    # Centre crop to the screen's 4:3 shape
    $cropW = $src.Width; $cropH = $src.Height
    if ($src.Width * $Height -gt $src.Height * $Width) { $cropW = [int]($src.Height * $Width / $Height) }
    else { $cropH = [int]($src.Width * $Height / $Width) }
    $cropX = [int](($src.Width - $cropW) / 2)
    $cropY = [int](($src.Height - $cropH) / 2)

    # Colour matrix: saturation, then contrast/brightness
    $lr = 0.3086; $lg = 0.6094; $lb = 0.0820
    $s = $Saturation
    $sr = (1 - $s) * $lr; $sg = (1 - $s) * $lg; $sb = (1 - $s) * $lb
    $c = $Contrast
    $t = (1 - $c) / 2 + $Brightness

    $m = New-Object System.Drawing.Imaging.ColorMatrix
    $m.Matrix00 = ($sr + $s) * $c; $m.Matrix01 = $sr * $c;        $m.Matrix02 = $sr * $c
    $m.Matrix10 = $sg * $c;        $m.Matrix11 = ($sg + $s) * $c; $m.Matrix12 = $sg * $c
    $m.Matrix20 = $sb * $c;        $m.Matrix21 = $sb * $c;        $m.Matrix22 = ($sb + $s) * $c
    $m.Matrix33 = 1
    $m.Matrix40 = $t; $m.Matrix41 = $t; $m.Matrix42 = $t; $m.Matrix44 = 1

    $attrs = New-Object System.Drawing.Imaging.ImageAttributes
    $attrs.SetColorMatrix($m)
    $attrs.SetWrapMode([System.Drawing.Drawing2D.WrapMode]::TileFlipXY)   # no dark edges

    $out = New-Object System.Drawing.Bitmap($Width, $Height, [System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
    $g = [System.Drawing.Graphics]::FromImage($out)
    $g.Clear([System.Drawing.Color]::Black)          # transparent areas become black
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
    $g.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
    $dest = New-Object System.Drawing.Rectangle(0, 0, $Width, $Height)
    $g.DrawImage($src, $dest, $cropX, $cropY, $cropW, $cropH, [System.Drawing.GraphicsUnit]::Pixel, $attrs)
    $g.Dispose()

    # Save next to the original picture
    $folder = Split-Path -Parent $fullPath
    $local = Join-Path $folder 'theme.bmp'
    $out.Save($local, [System.Drawing.Imaging.ImageFormat]::Bmp)
    $out.Dispose()
    Write-Host ""
    Write-Host "Saved: $local" -ForegroundColor Green

    # Copy to the SD card if one with a DSiRecorder folder (or a DSi-looking card) is plugged in
    $copied = $false
    foreach ($d in [System.IO.DriveInfo]::GetDrives()) {
        if (-not $d.IsReady -or $d.DriveType -ne 'Removable') { continue }
        $root = $d.RootDirectory.FullName
        $recDir = Join-Path $root 'DSiRecorder'
        $looksLikeDsi = (Test-Path (Join-Path $root '_nds')) -or (Test-Path (Join-Path $root 'private\ds'))
        if ((Test-Path $recDir) -or $looksLikeDsi) {
            if (-not (Test-Path $recDir)) { New-Item -ItemType Directory -Path $recDir | Out-Null }
            Copy-Item -LiteralPath $local -Destination (Join-Path $recDir 'theme.bmp') -Force
            Write-Host "Copied to SD card: $(Join-Path $recDir 'theme.bmp')" -ForegroundColor Green
            $copied = $true
            break
        }
    }
    if (-not $copied) {
        Write-Host "No DSi SD card found - copy theme.bmp into the DSiRecorder folder on your SD card yourself." -ForegroundColor Yellow
    }
}
finally {
    $src.Dispose()
}
