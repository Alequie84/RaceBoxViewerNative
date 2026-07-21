param(
    [string] $ReferenceImage = (Join-Path $PSScriptRoot '..\out\google-map-triangulation-reference.png'),
    [string] $RaceBoxCsv = (Join-Path $PSScriptRoot '..\golden\session.csv'),
    [string] $OutputPath = (Join-Path $PSScriptRoot '..\out\google-map-trace-overlay.png'),
    [int] $RawLap = 17
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

# Three Google Maps right-click anchors, captured without moving the map.
# The source aerial appears in the browser at +84,+199 pixels, so the equivalent
# source-image pixels are A=(495,73), B=(105,273), C=(815,339).
$anchors = @(
    [pscustomobject]@{ Name = 'A'; X = 579.0; Y = 272.0; Latitude = 49.184236; Longitude = -123.145111 },
    [pscustomobject]@{ Name = 'B'; X = 189.0; Y = 472.0; Latitude = 49.184060; Longitude = -123.145634 },
    [pscustomobject]@{ Name = 'C'; X = 899.0; Y = 538.0; Latitude = 49.184003; Longitude = -123.144682 }
)

# Best-fit rigid, uniform-scale solution from all three anchors.  The 0.06 m RMS
# residual is below Google Maps' six-decimal coordinate rounding.  X and Y use
# the same 0.0974800703 metres/pixel scale: no GPS or image-axis stretching.
$referencePixelX = 578.5
$referencePixelY = 455.5
$referenceLatitude = 49.18407486000555
$referenceLongitude = -123.14511168306714
$metresPerPixel = 0.09748007033810534
$rotationRadians = 0.09155874851718505 * [Math]::PI / 180.0
$longitudeMetres = 111320.0 * [Math]::Cos($referenceLatitude * [Math]::PI / 180.0)
$latitudeMetres = 110540.0

function Convert-GpsToPixel([double] $Latitude, [double] $Longitude) {
    $east = ($Longitude - $referenceLongitude) * $longitudeMetres
    $north = ($Latitude - $referenceLatitude) * $latitudeMetres
    $cos = [Math]::Cos($rotationRadians)
    $sin = [Math]::Sin($rotationRadians)

    # Inverse of world = scale * rotation * (pixel-x, -pixel-y).
    $pixelX = ($east * $cos + $north * $sin) / $metresPerPixel
    $pixelY = ($east * $sin - $north * $cos) / $metresPerPixel
    return [Drawing.PointF]::new(
        [single]($referencePixelX + $pixelX),
        [single]($referencePixelY + $pixelY))
}

if (-not (Test-Path -LiteralPath $ReferenceImage)) { throw "Reference image not found: $ReferenceImage" }
if (-not (Test-Path -LiteralPath $RaceBoxCsv)) { throw "RaceBox CSV not found: $RaceBoxCsv" }

$rows = @(Import-Csv -LiteralPath $RaceBoxCsv | Where-Object { [int]$_.Lap -eq $RawLap })
if ($rows.Count -lt 2) { throw "Raw lap $RawLap has fewer than two RaceBox samples." }

$points = [Drawing.PointF[]]@($rows | ForEach-Object {
    Convert-GpsToPixel -Latitude ([double]$_.Latitude) -Longitude ([double]$_.Longitude)
})

$source = [Drawing.Image]::FromFile([IO.Path]::GetFullPath($ReferenceImage))
$bitmap = [Drawing.Bitmap]::new($source.Width, $source.Height, [Drawing.Imaging.PixelFormat]::Format32bppArgb)
$graphics = [Drawing.Graphics]::FromImage($bitmap)
$graphics.SmoothingMode = [Drawing.Drawing2D.SmoothingMode]::AntiAlias
$graphics.DrawImageUnscaled($source, 0, 0)

$outline = [Drawing.Pen]::new([Drawing.Color]::FromArgb(230, 4, 10, 16), 7.0)
$trace = [Drawing.Pen]::new([Drawing.Color]::FromArgb(255, 20, 220, 255), 3.5)
$anchorPen = [Drawing.Pen]::new([Drawing.Color]::FromArgb(255, 255, 190, 35), 2.0)
$anchorFill = [Drawing.SolidBrush]::new([Drawing.Color]::FromArgb(210, 18, 20, 24))
$labelBrush = [Drawing.SolidBrush]::new([Drawing.Color]::White)
$panelBrush = [Drawing.SolidBrush]::new([Drawing.Color]::FromArgb(205, 5, 11, 17))
$font = [Drawing.Font]::new('Segoe UI', 11.0, [Drawing.FontStyle]::Bold)
$smallFont = [Drawing.Font]::new('Segoe UI', 8.5, [Drawing.FontStyle]::Regular)

$graphics.DrawLines($outline, $points)
$graphics.DrawLines($trace, $points)
$graphics.FillEllipse([Drawing.Brushes]::Magenta, $points[0].X - 4, $points[0].Y - 4, 8, 8)

foreach ($anchor in $anchors) {
    $graphics.FillEllipse($anchorFill, [single]($anchor.X - 5), [single]($anchor.Y - 5), 10, 10)
    $graphics.DrawEllipse($anchorPen, [single]($anchor.X - 6), [single]($anchor.Y - 6), 12, 12)
    $graphics.DrawString($anchor.Name, $font, $labelBrush, [single]($anchor.X + 8), [single]($anchor.Y - 9))
}

$graphics.FillRectangle($panelBrush, 82, 70, 445, 48)
$graphics.DrawString("Actual RaceBox raw lap $RawLap - 1:1 GPS overlay", $font, $labelBrush, 94, 76)
$graphics.DrawString('Three Google GPS anchors; uniform 0.097480 m/px; RMS 0.06 m', $smallFont, $labelBrush, 94, 98)

$destination = [IO.Path]::GetFullPath($OutputPath)
[IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($destination)) | Out-Null
$bitmap.Save($destination, [Drawing.Imaging.ImageFormat]::Png)

$graphics.Dispose()
$bitmap.Dispose()
$source.Dispose()
$outline.Dispose()
$trace.Dispose()
$anchorPen.Dispose()
$anchorFill.Dispose()
$labelBrush.Dispose()
$panelBrush.Dispose()
$font.Dispose()
$smallFont.Dispose()

Write-Host "Trace overlay: $destination"
Write-Host "Raw lap $RawLap samples: $($points.Count)"
Write-Host 'Calibration: 0.0974800703 m/px on both axes; rotation 0.091559 degrees; 0.06 m RMS'
