param(
    [string]$Output = "..\assets\LowLatencyCaptureViewer.ico"
)

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Drawing.Common -ErrorAction SilentlyContinue

$outputPath = Join-Path $PSScriptRoot $Output
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $outputPath) | Out-Null

function New-RoundedPath([float]$x, [float]$y, [float]$width, [float]$height, [float]$radius) {
    $path = [System.Drawing.Drawing2D.GraphicsPath]::new()
    $diameter = $radius * 2
    $path.AddArc($x, $y, $diameter, $diameter, 180, 90)
    $path.AddArc($x + $width - $diameter, $y, $diameter, $diameter, 270, 90)
    $path.AddArc($x + $width - $diameter, $y + $height - $diameter, $diameter, $diameter, 0, 90)
    $path.AddArc($x, $y + $height - $diameter, $diameter, $diameter, 90, 90)
    $path.CloseFigure()
    return $path
}

function New-IconPng([int]$size) {
    $bitmap = [System.Drawing.Bitmap]::new($size, $size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $graphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $graphics.Clear([System.Drawing.Color]::Transparent)
    $scale = $size / 256.0

    $outer = New-RoundedPath (12 * $scale) (12 * $scale) (232 * $scale) (232 * $scale) (48 * $scale)
    $graphics.FillPath([System.Drawing.SolidBrush]::new([System.Drawing.Color]::FromArgb(255, 9, 20, 42)), $outer)
    $graphics.DrawPath([System.Drawing.Pen]::new([System.Drawing.Color]::FromArgb(255, 53, 224, 220), (8 * $scale)), $outer)

    $screen = New-RoundedPath (43 * $scale) (48 * $scale) (170 * $scale) (130 * $scale) (13 * $scale)
    $graphics.FillPath([System.Drawing.SolidBrush]::new([System.Drawing.Color]::FromArgb(255, 18, 54, 86)), $screen)
    $graphics.DrawPath([System.Drawing.Pen]::new([System.Drawing.Color]::FromArgb(255, 104, 247, 235), (6 * $scale)), $screen)

    $standPen = [System.Drawing.Pen]::new([System.Drawing.Color]::FromArgb(255, 104, 247, 235), (7 * $scale))
    $graphics.DrawLine($standPen, (128 * $scale), (181 * $scale), (128 * $scale), (205 * $scale))
    $graphics.DrawLine($standPen, (89 * $scale), (207 * $scale), (167 * $scale), (207 * $scale))

    $wavePen = [System.Drawing.Pen]::new([System.Drawing.Color]::FromArgb(255, 42, 189, 255), (7 * $scale))
    $wavePen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
    $wavePen.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
    [System.Drawing.PointF[]]$points = @(
        [System.Drawing.PointF]::new((60 * $scale), (128 * $scale)),
        [System.Drawing.PointF]::new((79 * $scale), (110 * $scale)),
        [System.Drawing.PointF]::new((98 * $scale), (128 * $scale))
    )
    $graphics.DrawLines($wavePen, $points)

    [System.Drawing.PointF[]]$bolt = @(
        [System.Drawing.PointF]::new((151 * $scale), (64 * $scale)),
        [System.Drawing.PointF]::new((116 * $scale), (131 * $scale)),
        [System.Drawing.PointF]::new((140 * $scale), (127 * $scale)),
        [System.Drawing.PointF]::new((122 * $scale), (191 * $scale)),
        [System.Drawing.PointF]::new((174 * $scale), (112 * $scale)),
        [System.Drawing.PointF]::new((147 * $scale), (116 * $scale))
    )
    $graphics.FillPolygon([System.Drawing.SolidBrush]::new([System.Drawing.Color]::FromArgb(255, 255, 210, 74)), $bolt)
    $graphics.DrawPolygon([System.Drawing.Pen]::new([System.Drawing.Color]::FromArgb(255, 255, 244, 170), (3 * $scale)), $bolt)

    $stream = [System.IO.MemoryStream]::new()
    $bitmap.Save($stream, [System.Drawing.Imaging.ImageFormat]::Png)
    $bytes = $stream.ToArray()
    $stream.Dispose()
    $graphics.Dispose()
    $bitmap.Dispose()
    return ,$bytes
}

$sizes = @(16, 24, 32, 48, 64, 128, 256)
$images = @()
foreach ($size in $sizes) { $images += ,(New-IconPng $size) }

$stream = [System.IO.FileStream]::new($outputPath, [System.IO.FileMode]::Create, [System.IO.FileAccess]::Write)
$writer = [System.IO.BinaryWriter]::new($stream)
$writer.Write([UInt16]0)
$writer.Write([UInt16]1)
$writer.Write([UInt16]$sizes.Count)
$offset = 6 + (16 * $sizes.Count)
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $dimension = if ($sizes[$i] -eq 256) { [byte]0 } else { [byte]$sizes[$i] }
    $writer.Write($dimension)
    $writer.Write($dimension)
    $writer.Write([byte]0)
    $writer.Write([byte]0)
    $writer.Write([UInt16]1)
    $writer.Write([UInt16]32)
    $writer.Write([UInt32]$images[$i].Length)
    $writer.Write([UInt32]$offset)
    $offset += $images[$i].Length
}
foreach ($image in $images) { $writer.Write($image) }
$writer.Flush()
$writer.Dispose()
$stream.Dispose()
Write-Host "Created $outputPath"
