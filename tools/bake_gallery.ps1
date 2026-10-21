<#
.SYNOPSIS
  Pack photos for the gallery into gallery/gallery.bin.

.DESCRIPTION
  Every JPEG, PNG, BMP or GIF in -In, sorted by file name, is turned upright (EXIF),
  cropped square, sized to 480x480 and re-encoded as a baseline JPEG — the only kind
  TJpgDec on the device reads, which is why a photo is never packed as it came.

  The caption is the file name: "01-cesc_fabregas.jpg" shows as "Cesc Fabregas".
  A leading number only sets the order. Accents are dropped, because the device's one
  font is ASCII.

  Windows PowerShell 5.1 and System.Drawing; nothing to install.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\bake_gallery.ps1
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\bake_gallery.ps1 -Demo
  Named placeholder cards instead of photos, to try the swipe before choosing any.
#>
param(
    [string]$In = "gallery",
    [string]$Out = "gallery\gallery.bin",
    [ValidateRange(30, 100)][int]$Quality = 85,
    [switch]$Demo
)
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing

$Side = 480          # the panel
$NameLen = 24        # gallery.c NAME_LEN, terminator included
$MaxPhotos = 64      # gallery.c MAX_PHOTOS
$Budget = 1000000    # roughly what the 3 MB app slot has left

$jpegCodec = [System.Drawing.Imaging.ImageCodecInfo]::GetImageEncoders() |
    Where-Object { $_.MimeType -eq "image/jpeg" }

function Get-Caption([string]$file) {
    $s = [IO.Path]::GetFileNameWithoutExtension($file)
    $s = $s -replace '^\d+[\s._-]*', '' -replace '[_-]+', ' '
    $s = $s.Replace([string][char]0x0111, 'd').Replace([string][char]0x0110, 'D')   # đ Đ
    $s = -join ($s.Normalize([Text.NormalizationForm]::FormD).ToCharArray() |
        Where-Object { [Globalization.CharUnicodeInfo]::GetUnicodeCategory($_) -ne 'NonSpacingMark' })
    $s = -join ($s.ToCharArray() | Where-Object { [int]$_ -ge 32 -and [int]$_ -lt 127 })
    $s = (Get-Culture).TextInfo.ToTitleCase($s.Trim().ToLower())
    if ($s.Length -gt $NameLen - 1) { $s = $s.Substring(0, $NameLen - 1) }
    return $s
}

# Phones store photos sideways and say so in EXIF; System.Drawing does not apply it.
function Set-Upright([System.Drawing.Image]$img) {
    if ($img.PropertyIdList -notcontains 0x0112) { return }
    $o = [BitConverter]::ToUInt16($img.GetPropertyItem(0x0112).Value, 0)
    $rf = [System.Drawing.RotateFlipType]
    $map = @{ 2 = $rf::RotateNoneFlipX; 3 = $rf::Rotate180FlipNone; 4 = $rf::Rotate180FlipX
              5 = $rf::Rotate90FlipX;   6 = $rf::Rotate90FlipNone;  7 = $rf::Rotate270FlipX
              8 = $rf::Rotate270FlipNone }
    if ($map.ContainsKey([int]$o)) { $img.RotateFlip($map[[int]$o]) }
}

function New-Canvas {
    $bmp = New-Object System.Drawing.Bitmap $Side, $Side, ([System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAliasGridFit
    return $bmp, $g
}

function ConvertTo-Jpeg([System.Drawing.Bitmap]$bmp) {
    $ep = New-Object System.Drawing.Imaging.EncoderParameters 1
    $ep.Param[0] = New-Object System.Drawing.Imaging.EncoderParameter ([System.Drawing.Imaging.Encoder]::Quality), ([long]$Quality)
    $ms = New-Object IO.MemoryStream
    $bmp.Save($ms, $jpegCodec, $ep)
    return , $ms.ToArray()
}

function Convert-Photo([string]$path) {
    $img = [System.Drawing.Image]::FromFile($path)
    try {
        Set-Upright $img
        $w = $img.Width; $h = $img.Height
        $s = [Math]::Min($w, $h)
        # A portrait keeps more of its top than its bottom: that is where the face is. Not
        # all of it, though: the glass is round and narrows to nothing at the top edge, so a
        # head cropped flush with it loses its hair to the circle.
        $x = [int](($w - $s) / 2)
        $y = [int](($h - $s) / 6)
        $bmp, $g = New-Canvas
        $attr = New-Object System.Drawing.Imaging.ImageAttributes
        $attr.SetWrapMode([System.Drawing.Drawing2D.WrapMode]::TileFlipXY)   # no dark rim
        $g.DrawImage($img, (New-Object System.Drawing.Rectangle 0, 0, $Side, $Side),
                     $x, $y, $s, $s, [System.Drawing.GraphicsUnit]::Pixel, $attr)
        $g.Dispose()
        $jpg = ConvertTo-Jpeg $bmp
        $bmp.Dispose()
        return , $jpg
    } finally { $img.Dispose() }
}

# Something to swipe through before any photo has been chosen. Plainly a placeholder.
function New-Card([string]$name, [int]$number, [System.Drawing.Color]$c1, [System.Drawing.Color]$c2) {
    $bmp, $g = New-Canvas
    $rect = New-Object System.Drawing.Rectangle 0, 0, $Side, $Side
    $brush = New-Object System.Drawing.Drawing2D.LinearGradientBrush $rect, $c1, $c2, 45.0
    $g.FillRectangle($brush, $rect)
    $fmt = New-Object System.Drawing.StringFormat
    $fmt.Alignment = [System.Drawing.StringAlignment]::Center
    $fmt.LineAlignment = [System.Drawing.StringAlignment]::Center
    $white = [System.Drawing.Brushes]::White
    $big = New-Object System.Drawing.Font "Segoe UI", 150, ([System.Drawing.FontStyle]::Bold), ([System.Drawing.GraphicsUnit]::Pixel)
    $mid = New-Object System.Drawing.Font "Segoe UI", 52, ([System.Drawing.FontStyle]::Bold), ([System.Drawing.GraphicsUnit]::Pixel)
    $small = New-Object System.Drawing.Font "Segoe UI", 20, ([System.Drawing.FontStyle]::Regular), ([System.Drawing.GraphicsUnit]::Pixel)
    $g.DrawString("$number", $big, $white, (New-Object System.Drawing.RectangleF 0, 70, $Side, 200), $fmt)
    $g.DrawString($name.ToUpper(), $mid, $white, (New-Object System.Drawing.RectangleF 0, 250, $Side, 80), $fmt)
    $g.DrawString("placeholder - put a photo in gallery/", $small, $white, (New-Object System.Drawing.RectangleF 0, 320, $Side, 40), $fmt)
    $g.Dispose()
    $jpg = ConvertTo-Jpeg $bmp
    $bmp.Dispose()
    return , $jpg
}

$items = New-Object System.Collections.ArrayList
if ($Demo) {
    $C = [System.Drawing.Color]
    $cards = @(
        @("Ronaldo", 7,  $C::FromArgb(200, 16, 46),  $C::FromArgb(90, 0, 20)),
        @("Messi",   10, $C::FromArgb(108, 172, 228), $C::FromArgb(20, 60, 140)),
        @("Neymar",  10, $C::FromArgb(254, 221, 0),  $C::FromArgb(0, 120, 60)),
        @("Mbappe",  10, $C::FromArgb(0, 35, 149),   $C::FromArgb(237, 41, 57)),
        @("Haaland", 9,  $C::FromArgb(108, 171, 221), $C::FromArgb(28, 44, 91))
    )
    foreach ($c in $cards) {
        [void]$items.Add(@{ Name = $c[0]; Jpg = (New-Card $c[0] $c[1] $c[2] $c[3]) })
    }
} else {
    if (-not (Test-Path $In)) { New-Item -ItemType Directory $In | Out-Null }
    $files = Get-ChildItem -File $In | Where-Object { $_.Extension -match '^\.(jpe?g|png|bmp|gif)$' } |
        Sort-Object Name
    if (-not $files) { throw "No photos in '$In'. Add some (JPEG/PNG/BMP), or try -Demo." }
    foreach ($f in $files) {
        if ($items.Count -ge $MaxPhotos) { Write-Warning "only the first $MaxPhotos are packed"; break }
        try {
            [void]$items.Add(@{ Name = (Get-Caption $f.Name); Jpg = (Convert-Photo $f.FullName) })
        } catch {
            Write-Warning "$($f.Name): skipped ($($_.Exception.Message))"
        }
    }
}

$outDir = Split-Path -Parent $Out
if ($outDir -and -not (Test-Path $outDir)) { New-Item -ItemType Directory $outDir | Out-Null }
$fs = [IO.File]::Create((Join-Path (Get-Location) $Out))
$bw = New-Object IO.BinaryWriter $fs
try {
    $bw.Write([Text.Encoding]::ASCII.GetBytes("GLRY"))
    $bw.Write([uint32]$items.Count)
    $off = 8 + $items.Count * (8 + $NameLen)
    foreach ($it in $items) {
        $bw.Write([uint32]$off)
        $bw.Write([uint32]$it.Jpg.Length)
        $name = New-Object byte[] $NameLen
        $nb = [Text.Encoding]::ASCII.GetBytes($it.Name)
        [Array]::Copy($nb, $name, [Math]::Min($nb.Length, $NameLen - 1))
        $bw.Write($name)
        $off += $it.Jpg.Length
    }
    foreach ($it in $items) { $bw.Write($it.Jpg) }
} finally { $bw.Close() }

foreach ($it in $items) { "{0,-24} {1,7:N0} B" -f $it.Name, $it.Jpg.Length }
$total = (Get-Item $Out).Length
"{0} photos, {1:N0} B -> {2}" -f $items.Count, $total, $Out
if ($total -gt $Budget) {
    Write-Warning "over ~$($Budget / 1000) KB: the app may not fit its 3 MB slot. Fewer photos, or -Quality 70."
}
