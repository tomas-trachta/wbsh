# installer/make-icon.ps1 -- regenerate installer\wbsh.ico from scratch.
#
# Renders the icon at the standard Windows sizes (16/24/32/48/64/128/256)
# using System.Drawing, then packs the PNG-encoded frames into a multi-image
# .ico. Run this if you tweak the design; the produced wbsh.ico is committed
# so the build doesn't depend on having .NET drawing primitives at compile
# time.
[CmdletBinding()]
param(
    [string]$OutPath = (Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) 'wbsh.ico')
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$sizes = @(16, 24, 32, 48, 64, 128, 256)

# Pick a monospace face that's actually installed, in preference order.
function Find-MonoFamily {
    $preferred = @('Cascadia Mono', 'Cascadia Code', 'Consolas', 'Lucida Console', 'Courier New')
    $installed = (New-Object System.Drawing.Text.InstalledFontCollection).Families | ForEach-Object Name
    foreach ($p in $preferred) {
        if ($installed -contains $p) { return $p }
    }
    return 'Courier New'
}
$family = Find-MonoFamily

function Render-Frame([int]$size) {
    $bmp = New-Object System.Drawing.Bitmap($size, $size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g   = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode     = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.PixelOffsetMode   = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAlias

    # Rounded dark square background (Tokyo-Night-ish).
    $radius = [Math]::Max(1, [int]($size * 0.18))
    $d = $radius * 2
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $path.AddArc(0,             0,             $d, $d, 180, 90)
    $path.AddArc($size - $d,    0,             $d, $d, 270, 90)
    $path.AddArc($size - $d,    $size - $d,    $d, $d,   0, 90)
    $path.AddArc(0,             $size - $d,    $d, $d,  90, 90)
    $path.CloseFigure()

    $bg = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 12, 14, 20))
    $g.FillPath($bg, $path)
    $bg.Dispose()

    # Subtle 1px inner stroke for contrast on light wallpapers.
    if ($size -ge 32) {
        $stroke = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(60, 255, 255, 255)), 1
        $g.DrawPath($stroke, $path)
        $stroke.Dispose()
    }

    # Green "$" glyph, centered.
    $fontSize = [single]($size * 0.66)
    $font = New-Object System.Drawing.Font($family, $fontSize, [System.Drawing.FontStyle]::Bold, [System.Drawing.GraphicsUnit]::Pixel)
    $glyph = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 80, 230, 130))
    $sf = New-Object System.Drawing.StringFormat
    $sf.Alignment     = [System.Drawing.StringAlignment]::Center
    $sf.LineAlignment = [System.Drawing.StringAlignment]::Center
    # Nudge baseline up slightly -- glyph metrics put "$" visually low.
    $rect = New-Object System.Drawing.RectangleF(0, [single](-$size * 0.04), [single]$size, [single]$size)
    $g.DrawString('$', $font, $glyph, $rect, $sf)
    $glyph.Dispose()
    $font.Dispose()
    $sf.Dispose()
    $g.Dispose()
    $path.Dispose()

    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    return ,$ms.ToArray()
}

# Render each size to a PNG blob.
$frames = @{}
foreach ($s in $sizes) { $frames[$s] = Render-Frame $s }

# Build the .ico: ICONDIR (6) + N * ICONDIRENTRY (16) + concatenated PNGs.
$out = New-Object System.IO.MemoryStream
$bw  = New-Object System.IO.BinaryWriter($out)
$bw.Write([uint16]0)              # reserved
$bw.Write([uint16]1)              # type = 1 (icon)
$bw.Write([uint16]$sizes.Count)   # image count

$offset = 6 + 16 * $sizes.Count
foreach ($s in $sizes) {
    $png = $frames[$s]
    # Width/height: byte field; 0 means 256.
    $dim = [byte]($s -band 0xff)
    $bw.Write([byte]$dim)
    $bw.Write([byte]$dim)
    $bw.Write([byte]0)             # palette count (0 for non-palettized)
    $bw.Write([byte]0)             # reserved
    $bw.Write([uint16]1)           # color planes
    $bw.Write([uint16]32)          # bit count
    $bw.Write([uint32]$png.Length) # data size
    $bw.Write([uint32]$offset)     # data offset
    $offset += $png.Length
}
foreach ($s in $sizes) { $bw.Write($frames[$s]) }
$bw.Flush()

[System.IO.File]::WriteAllBytes($OutPath, $out.ToArray())
$bw.Dispose()
$out.Dispose()

Write-Host "wrote $OutPath ($([int]((Get-Item $OutPath).Length / 1KB)) KB, $($sizes.Count) frames using '$family')" -ForegroundColor Green
