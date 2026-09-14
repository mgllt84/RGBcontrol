$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$source = (Resolve-Path "$PSScriptRoot\..\..\outputs\RGBCcontrol\assets\setup-rgb-logo.png").Path
$destination = Join-Path $PSScriptRoot '..\resources\RGBCcontrol.ico'
$sizes = @(256, 64, 48, 32, 16)
$sourceImage = [Drawing.Image]::FromFile($source)
$images = New-Object System.Collections.Generic.List[byte[]]
try {
    foreach ($size in $sizes) {
        $bitmap = New-Object Drawing.Bitmap($size, $size, [Drawing.Imaging.PixelFormat]::Format32bppArgb)
        try {
            $graphics = [Drawing.Graphics]::FromImage($bitmap)
            try {
                $graphics.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
                $graphics.SmoothingMode = [Drawing.Drawing2D.SmoothingMode]::HighQuality
                $graphics.PixelOffsetMode = [Drawing.Drawing2D.PixelOffsetMode]::HighQuality
                $graphics.DrawImage($sourceImage, 0, 0, $size, $size)
            } finally { $graphics.Dispose() }
            $stream = New-Object IO.MemoryStream
            try {
                $bitmap.Save($stream, [Drawing.Imaging.ImageFormat]::Png)
                $images.Add($stream.ToArray())
            } finally { $stream.Dispose() }
        } finally { $bitmap.Dispose() }
    }
} finally { $sourceImage.Dispose() }

$output = New-Object IO.MemoryStream
$writer = New-Object IO.BinaryWriter($output)
try {
    $writer.Write([uint16]0)
    $writer.Write([uint16]1)
    $writer.Write([uint16]$images.Count)
    $offset = 6 + 16 * $images.Count
    for ($index = 0; $index -lt $images.Count; $index++) {
        $size = $sizes[$index]
        $writer.Write([byte]$(if ($size -eq 256) { 0 } else { $size }))
        $writer.Write([byte]$(if ($size -eq 256) { 0 } else { $size }))
        $writer.Write([byte]0)
        $writer.Write([byte]0)
        $writer.Write([uint16]1)
        $writer.Write([uint16]32)
        $writer.Write([uint32]$images[$index].Length)
        $writer.Write([uint32]$offset)
        $offset += $images[$index].Length
    }
    foreach ($image in $images) { $writer.Write($image) }
    [IO.File]::WriteAllBytes($destination, $output.ToArray())
} finally {
    $writer.Dispose()
    $output.Dispose()
}
Write-Output $destination

