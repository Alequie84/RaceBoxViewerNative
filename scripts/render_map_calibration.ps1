param(
    [Parameter(Mandatory = $true)]
    [string[]] $RaceBoxCsv,

    [string] $Background = (Join-Path $PSScriptRoot '..\assets\rrr-map.png'),
    [string] $Output = (Join-Path $PSScriptRoot '..\out\map-calibration-reference.png'),
    [switch] $MedianGuideOnly,
    [switch] $CorrectedMap,
    [ValidateRange(3.0, 3.7)]
    [double] $LaneWidthMetres = 3.4
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$culture = [System.Globalization.CultureInfo]::InvariantCulture
$paths = [System.Collections.Generic.List[object]]::new()

foreach ($csvPath in $RaceBoxCsv) {
    $lines = [System.IO.File]::ReadAllLines($csvPath)
    $headerIndex = -1
    for ($index = 0; $index -lt $lines.Length; $index++) {
        if ($lines[$index].StartsWith('Record,Time,Latitude,Longitude,')) {
            $headerIndex = $index
            break
        }
    }
    if ($headerIndex -lt 0) {
        throw "RaceBox header not found in $csvPath"
    }

    $rows = $lines[$headerIndex..($lines.Length - 1)] | ConvertFrom-Csv
    foreach ($group in ($rows | Group-Object Lap)) {
        $lap = 0
        if (-not [int]::TryParse($group.Name, [ref] $lap) -or $lap -le 0) { continue }

        $maximumSpeed = [double](($group.Group | Measure-Object Speed -Maximum).Maximum)
        if ($group.Count -lt 350 -or $group.Count -gt 500 -or $maximumSpeed -lt 80.0) { continue }

        $points = [System.Collections.Generic.List[object]]::new($group.Count)
        foreach ($row in $group.Group) {
            $latitude = [double]::Parse($row.Latitude, $culture)
            $longitude = [double]::Parse($row.Longitude, $culture)
            $points.Add([pscustomobject]@{ Latitude = $latitude; Longitude = $longitude })
        }
        $paths.Add([pscustomobject]@{
            Source = [System.IO.Path]::GetFileNameWithoutExtension($csvPath)
            Lap = $lap
            Points = $points
        })
    }
}

if ($paths.Count -eq 0) { throw 'No complete laps passed the calibration filters.' }

$sourceNames = @($paths | Select-Object -ExpandProperty Source -Unique)
$referenceSource = $sourceNames[-1]
$referenceAnchors = [System.Collections.Generic.List[object]]::new()

foreach ($path in $paths) {
    $sortedLongitude = @($path.Points | Sort-Object Longitude | Select-Object -ExpandProperty Longitude)
    $threshold = $sortedLongitude[[math]::Floor(($sortedLongitude.Count - 1) * 0.90)]
    $anchorPoints = @($path.Points | Where-Object { $_.Longitude -ge $threshold })
    $path | Add-Member -NotePropertyName AnchorLatitude -NotePropertyValue (($anchorPoints | Measure-Object Latitude -Average).Average)
    $path | Add-Member -NotePropertyName AnchorLongitude -NotePropertyValue (($anchorPoints | Measure-Object Longitude -Average).Average)
    if ($path.Source -eq $referenceSource) {
        $referenceAnchors.Add([pscustomobject]@{ Latitude = $path.AnchorLatitude; Longitude = $path.AnchorLongitude })
    }
}

function Get-Median([double[]] $values) {
    $ordered = @($values | Sort-Object)
    $middle = [math]::Floor($ordered.Count / 2)
    if (($ordered.Count % 2) -eq 0) { return ($ordered[$middle - 1] + $ordered[$middle]) * 0.5 }
    return $ordered[$middle]
}

$referenceLatitude = Get-Median @($referenceAnchors | Select-Object -ExpandProperty Latitude)
$referenceLongitude = Get-Median @($referenceAnchors | Select-Object -ExpandProperty Longitude)
foreach ($path in $paths) {
    $path | Add-Member -NotePropertyName LatitudeOffset -NotePropertyValue ($referenceLatitude - $path.AnchorLatitude)
    $path | Add-Member -NotePropertyName LongitudeOffset -NotePropertyValue ($referenceLongitude - $path.AnchorLongitude)
}

$medianGuide = [System.Collections.Generic.List[object]]::new()
if ($MedianGuideOnly -or $CorrectedMap) {
    $profiles = [System.Collections.Generic.List[object]]::new()
    foreach ($path in $paths) {
        $adjusted = [System.Collections.Generic.List[object]]::new($path.Points.Count)
        $distance = [System.Collections.Generic.List[double]]::new($path.Points.Count)
        $distance.Add(0.0)
        foreach ($point in $path.Points) {
            $adjusted.Add([pscustomobject]@{
                Latitude = $point.Latitude + $path.LatitudeOffset
                Longitude = $point.Longitude + $path.LongitudeOffset
            })
        }
        for ($index = 1; $index -lt $adjusted.Count; $index++) {
            $averageLatitude = ($adjusted[$index - 1].Latitude + $adjusted[$index].Latitude) * 0.5
            $localLongitudeMetres = 111320.0 * [math]::Cos($averageLatitude * [math]::PI / 180.0)
            $dx = ($adjusted[$index].Longitude - $adjusted[$index - 1].Longitude) * $localLongitudeMetres
            $dy = ($adjusted[$index].Latitude - $adjusted[$index - 1].Latitude) * 110540.0
            $step = [math]::Sqrt($dx * $dx + $dy * $dy)
            $distance.Add($distance[$index - 1] + $(if ($step -lt 20.0) { $step } else { 0.0 }))
        }

        $resampled = [System.Collections.Generic.List[object]]::new(401)
        $cursor = 1
        for ($sample = 0; $sample -lt 401; $sample++) {
            $target = $distance[$distance.Count - 1] * $sample / 400.0
            while ($cursor -lt $distance.Count - 1 -and $distance[$cursor] -lt $target) { $cursor++ }
            $lower = [math]::Max(0, $cursor - 1)
            $span = [math]::Max(0.000001, $distance[$cursor] - $distance[$lower])
            $ratio = [math]::Max(0.0, [math]::Min(1.0, ($target - $distance[$lower]) / $span))
            $resampled.Add([pscustomobject]@{
                Latitude = $adjusted[$lower].Latitude + ($adjusted[$cursor].Latitude - $adjusted[$lower].Latitude) * $ratio
                Longitude = $adjusted[$lower].Longitude + ($adjusted[$cursor].Longitude - $adjusted[$lower].Longitude) * $ratio
            })
        }
        $profiles.Add($resampled)
    }

    function Get-TrimmedMean([double[]] $values) {
        $ordered = @($values | Sort-Object)
        $trim = if ($ordered.Count -ge 5) { [math]::Floor($ordered.Count / 5) } else { 0 }
        $sum = 0.0
        for ($index = $trim; $index -lt $ordered.Count - $trim; $index++) { $sum += $ordered[$index] }
        return $sum / [math]::Max(1, $ordered.Count - 2 * $trim)
    }

    for ($sample = 0; $sample -lt 401; $sample++) {
        $medianGuide.Add([pscustomobject]@{
            Latitude = Get-TrimmedMean @($profiles | ForEach-Object { $_[$sample].Latitude })
            Longitude = Get-TrimmedMean @($profiles | ForEach-Object { $_[$sample].Longitude })
        })
    }
}

$allPoints = @($paths | ForEach-Object {
    $path = $_
    $path.Points | ForEach-Object {
        [pscustomobject]@{
            Latitude = $_.Latitude + $path.LatitudeOffset
            Longitude = $_.Longitude + $path.LongitudeOffset
        }
    }
})
$minimumLatitude = ($allPoints | Measure-Object Latitude -Minimum).Minimum
$maximumLatitude = ($allPoints | Measure-Object Latitude -Maximum).Maximum
$minimumLongitude = ($allPoints | Measure-Object Longitude -Minimum).Minimum
$maximumLongitude = ($allPoints | Measure-Object Longitude -Maximum).Maximum
$centerLatitude = ($minimumLatitude + $maximumLatitude) * 0.5
$centerLongitude = ($minimumLongitude + $maximumLongitude) * 0.5
$longitudeMetres = 111320.0 * [math]::Cos($centerLatitude * [math]::PI / 180.0)
$minimumX = ($minimumLongitude - $centerLongitude) * $longitudeMetres
$maximumX = ($maximumLongitude - $centerLongitude) * $longitudeMetres
$minimumY = ($minimumLatitude - $centerLatitude) * 110540.0
$maximumY = ($maximumLatitude - $centerLatitude) * 110540.0

$source = [System.Drawing.Bitmap]::FromFile((Resolve-Path $Background))
$reference = [System.Drawing.Bitmap]::new($source.Width, $source.Height, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$graphics = [System.Drawing.Graphics]::FromImage($reference)
$graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
$graphics.DrawImage($source, 0, 0, $source.Width, $source.Height)

$panelWidth = [double]$source.Width
$panelHeight = [double]$source.Height
$projectionScale = [math]::Min(($panelWidth - 24.0) / [math]::Max(0.1, $maximumX - $minimumX),
                               ($panelHeight - 24.0) / [math]::Max(0.1, $maximumY - $minimumY))

# These values are the native app's saved default background placement.
$backgroundOffsetX = 0.0182
$backgroundOffsetY = 0.0308
$backgroundScaleX = 1.4543
$backgroundScaleY = 1.0421

function Convert-ToImagePoint([double] $latitude, [double] $longitude) {
    $xMetres = ($longitude - $centerLongitude) * $longitudeMetres
    $yMetres = ($latitude - $centerLatitude) * 110540.0
    $panelX = $panelWidth * 0.5 + $xMetres * $projectionScale
    $panelY = $panelHeight * 0.5 - $yMetres * $projectionScale
    $u = (($panelX / $panelWidth) - (0.5 + $backgroundOffsetX) + $backgroundScaleX * 0.5) / $backgroundScaleX
    $v = (($panelY / $panelHeight) - (0.5 + $backgroundOffsetY) + $backgroundScaleY * 0.5) / $backgroundScaleY
    return [System.Drawing.PointF]::new([float]($u * $source.Width), [float]($v * $source.Height))
}

$sessionColours = @(
    [System.Drawing.Color]::FromArgb(70, 0, 230, 255),
    [System.Drawing.Color]::FromArgb(70, 255, 0, 220),
    [System.Drawing.Color]::FromArgb(70, 255, 210, 0)
)
$sampleCount = 0
$darkGrooveCount = 0
$whiteBoundaryCount = 0
foreach ($path in $paths) {
    $sourceIndex = [array]::IndexOf($sourceNames, $path.Source)
    $pen = [System.Drawing.Pen]::new($sessionColours[$sourceIndex % $sessionColours.Count], 1.5)
    $last = Convert-ToImagePoint ($path.Points[0].Latitude + $path.LatitudeOffset) ($path.Points[0].Longitude + $path.LongitudeOffset)
    for ($index = 2; $index -lt $path.Points.Count; $index += 2) {
        $point = Convert-ToImagePoint ($path.Points[$index].Latitude + $path.LatitudeOffset) ($path.Points[$index].Longitude + $path.LongitudeOffset)
        $pixelX = [math]::Round($point.X)
        $pixelY = [math]::Round($point.Y)
        if ($pixelX -ge 0 -and $pixelX -lt $source.Width -and $pixelY -ge 0 -and $pixelY -lt $source.Height) {
            $pixel = $source.GetPixel($pixelX, $pixelY)
            $luminance = 0.2126 * $pixel.R + 0.7152 * $pixel.G + 0.0722 * $pixel.B
            $chroma = [math]::Max($pixel.R, [math]::Max($pixel.G, $pixel.B)) - [math]::Min($pixel.R, [math]::Min($pixel.G, $pixel.B))
            $sampleCount++
            if ($luminance -lt 145.0) { $darkGrooveCount++ }
            if ($luminance -gt 195.0 -and $chroma -lt 45.0) { $whiteBoundaryCount++ }
        }
        if (-not $MedianGuideOnly -and -not $CorrectedMap) { $graphics.DrawLine($pen, $last, $point) }
        $last = $point
    }
    $pen.Dispose()
}

if ($CorrectedMap -and $medianGuide.Count -gt 2) {
    $halfLaneMetres = $LaneWidthMetres * 0.5
    $segments = [System.Collections.Generic.List[object]]::new()
    $segment = $null

    for ($index = 0; $index -lt $medianGuide.Count; $index++) {
        $before = $medianGuide[[math]::Max(0, $index - 1)]
        $after = $medianGuide[[math]::Min($medianGuide.Count - 1, $index + 1)]
        $dx = ($after.Longitude - $before.Longitude) * $longitudeMetres
        $dy = ($after.Latitude - $before.Latitude) * 110540.0
        $length = [math]::Max(0.000001, [math]::Sqrt($dx * $dx + $dy * $dy))
        $normalX = -$dy / $length
        $normalY = $dx / $length
        $point = $medianGuide[$index]
        $centerPoint = Convert-ToImagePoint $point.Latitude $point.Longitude

        # Use one continuous physical loop. The unmodified aerial remains fixed
        # beneath it, so aligned sections stay at their original 1:1 position.
        $isChangedSection = $true
        if (-not $isChangedSection) {
            if ($null -ne $segment -and $segment.Center.Count -gt 2) { $segments.Add($segment) }
            $segment = $null
            continue
        }

        if ($null -eq $segment) {
            $segment = [pscustomobject]@{
                Center = [System.Collections.Generic.List[System.Drawing.PointF]]::new()
                Left = [System.Collections.Generic.List[System.Drawing.PointF]]::new()
                Right = [System.Collections.Generic.List[System.Drawing.PointF]]::new()
            }
        }
        $segment.Center.Add($centerPoint)
        $leftLatitude = $point.Latitude + $normalY * $halfLaneMetres / 110540.0
        $leftLongitude = $point.Longitude + $normalX * $halfLaneMetres / $longitudeMetres
        $rightLatitude = $point.Latitude - $normalY * $halfLaneMetres / 110540.0
        $rightLongitude = $point.Longitude - $normalX * $halfLaneMetres / $longitudeMetres
        $segment.Left.Add((Convert-ToImagePoint $leftLatitude $leftLongitude))
        $segment.Right.Add((Convert-ToImagePoint $rightLatitude $rightLongitude))
    }
    if ($null -ne $segment -and $segment.Center.Count -gt 2) { $segments.Add($segment) }

    $grooveBrush = [System.Drawing.SolidBrush]::new([System.Drawing.Color]::FromArgb(188, 28, 31, 33))
    $groovePen = [System.Drawing.Pen]::new([System.Drawing.Color]::FromArgb(120, 8, 9, 10), 7.0)
    $boardShadowPen = [System.Drawing.Pen]::new([System.Drawing.Color]::FromArgb(190, 35, 36, 37), 5.0)
    $boardPen = [System.Drawing.Pen]::new([System.Drawing.Color]::FromArgb(245, 226, 227, 223), 2.5)

    foreach ($current in $segments) {
        $polygon = [System.Collections.Generic.List[System.Drawing.PointF]]::new()
        foreach ($point in $current.Left) { $polygon.Add($point) }
        for ($index = $current.Right.Count - 1; $index -ge 0; $index--) { $polygon.Add($current.Right[$index]) }
        $graphics.FillPolygon($grooveBrush, $polygon.ToArray())
        $graphics.DrawLines($groovePen, $current.Center.ToArray())
        $graphics.DrawLines($boardShadowPen, $current.Left.ToArray())
        $graphics.DrawLines($boardShadowPen, $current.Right.ToArray())
        $graphics.DrawLines($boardPen, $current.Left.ToArray())
        $graphics.DrawLines($boardPen, $current.Right.ToArray())
    }

    $boardPen.Dispose()
    $boardShadowPen.Dispose()
    $groovePen.Dispose()
    $grooveBrush.Dispose()
}

if ($MedianGuideOnly -and $medianGuide.Count -gt 2) {
    $guidePen = [System.Drawing.Pen]::new([System.Drawing.Color]::FromArgb(245, 40, 255, 90), 3.0)
    $boundaryPen = [System.Drawing.Pen]::new([System.Drawing.Color]::FromArgb(245, 255, 205, 20), 2.0)
    $halfLaneMetres = $LaneWidthMetres * 0.5
    $previousCenter = $null
    $previousLeft = $null
    $previousRight = $null
    for ($index = 0; $index -lt $medianGuide.Count; $index++) {
        $before = $medianGuide[[math]::Max(0, $index - 1)]
        $after = $medianGuide[[math]::Min($medianGuide.Count - 1, $index + 1)]
        $dx = ($after.Longitude - $before.Longitude) * $longitudeMetres
        $dy = ($after.Latitude - $before.Latitude) * 110540.0
        $length = [math]::Max(0.000001, [math]::Sqrt($dx * $dx + $dy * $dy))
        $normalX = -$dy / $length
        $normalY = $dx / $length
        $point = $medianGuide[$index]
        $centerPoint = Convert-ToImagePoint $point.Latitude $point.Longitude
        $leftPoint = Convert-ToImagePoint ($point.Latitude + $normalY * $halfLaneMetres / 110540.0) ($point.Longitude + $normalX * $halfLaneMetres / $longitudeMetres)
        $rightPoint = Convert-ToImagePoint ($point.Latitude - $normalY * $halfLaneMetres / 110540.0) ($point.Longitude - $normalX * $halfLaneMetres / $longitudeMetres)
        if ($null -ne $previousCenter) {
            $graphics.DrawLine($guidePen, $previousCenter, $centerPoint)
            $graphics.DrawLine($boundaryPen, $previousLeft, $leftPoint)
            $graphics.DrawLine($boundaryPen, $previousRight, $rightPoint)
        }
        $previousCenter = $centerPoint
        $previousLeft = $leftPoint
        $previousRight = $rightPoint
    }
    $boundaryPen.Dispose()
    $guidePen.Dispose()
}

if (-not $CorrectedMap) {
    $legendBrush = [System.Drawing.SolidBrush]::new([System.Drawing.Color]::FromArgb(220, 5, 12, 18))
    $graphics.FillRectangle($legendBrush, 10, 10, 410, 29)
    $font = [System.Drawing.Font]::new('Segoe UI', 11, [System.Drawing.FontStyle]::Bold)
    $textBrush = [System.Drawing.SolidBrush]::new([System.Drawing.Color]::White)
    $graphics.DrawString("$($paths.Count) complete laps from $($sourceNames.Count) RaceBox sessions", $font, $textBrush, 18, 15)
}

$outputDirectory = Split-Path -Parent $Output
[System.IO.Directory]::CreateDirectory($outputDirectory) | Out-Null
$reference.Save($Output, [System.Drawing.Imaging.ImageFormat]::Png)

if (-not $CorrectedMap) {
    $textBrush.Dispose()
    $font.Dispose()
    $legendBrush.Dispose()
}
$graphics.Dispose()
$reference.Dispose()
$source.Dispose()

Write-Output "Rendered $($paths.Count) complete laps from $($sourceNames.Count) sessions to $Output"
if ($sampleCount -gt 0) {
    Write-Output ("Source samples: {0:N1}% dark groove, {1:N2}% white/bright boundary" -f
        (100.0 * $darkGrooveCount / $sampleCount), (100.0 * $whiteBoundaryCount / $sampleCount))
}
