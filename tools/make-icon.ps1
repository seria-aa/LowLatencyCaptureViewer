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

    # Graphite + Coral: a compact capture frame with a single live-signal line.
    # The icon deliberately avoids small decorative elements so it stays readable at 16 px.
    $outer = New-RoundedPath (12 * $scale) (12 * $scale) (232 * $scale) (232 * $scale) (48 * $scale)
    $graphics.FillPath([System.Drawing.SolidBrush]::new([System.Drawing.Color]::FromArgb(255, 32, 32, 33)), $outer)

    $framePen = [System.Drawing.Pen]::new([System.Drawing.Color]::FromArgb(255, 242, 239, 232), (15 * $scale))
    $framePen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
    $framePen.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
    $graphics.DrawLine($framePen, (76 * $scale), (88 * $scale), (76 * $scale), (72 * $scale))
    $graphics.DrawLine($framePen, (76 * $scale), (72 * $scale), (112 * $scale), (72 * $scale))
    $graphics.DrawLine($framePen, (180 * $scale), (88 * $scale), (180 * $scale), (72 * $scale))
    $graphics.DrawLine($framePen, (180 * $scale), (72 * $scale), (144 * $scale), (72 * $scale))
    $graphics.DrawLine($framePen, (76 * $scale), (168 * $scale), (76 * $scale), (184 * $scale))
    $graphics.DrawLine($framePen, (76 * $scale), (184 * $scale), (112 * $scale), (184 * $scale))
    $graphics.DrawLine($framePen, (180 * $scale), (168 * $scale), (180 * $scale), (184 * $scale))
    $graphics.DrawLine($framePen, (180 * $scale), (184 * $scale), (144 * $scale), (184 * $scale))

    $signalPen = [System.Drawing.Pen]::new([System.Drawing.Color]::FromArgb(255, 255, 111, 82), (14 * $scale))
    $signalPen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
    $signalPen.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
    $signalPen.LineJoin = [System.Drawing.Drawing2D.LineJoin]::Round
    [System.Drawing.PointF[]]$signal = @(
        [System.Drawing.PointF]::new((84 * $scale), (128 * $scale)),
        [System.Drawing.PointF]::new((109 * $scale), (128 * $scale)),
        [System.Drawing.PointF]::new((124 * $scale), (97 * $scale)),
        [System.Drawing.PointF]::new((144 * $scale), (159 * $scale)),
        [System.Drawing.PointF]::new((157 * $scale), (128 * $scale)),
        [System.Drawing.PointF]::new((176 * $scale), (128 * $scale))
    )
    $graphics.DrawLines($signalPen, $signal)

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
