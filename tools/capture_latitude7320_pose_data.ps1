[CmdletBinding()]
param(
    [ValidateRange(1, 15)]
    [int]$CaptureSeconds = 5,

    [ValidateRange(25, 2000)]
    [int]$PollIntervalMs = 100,

    [ValidateRange(0, 10)]
    [int]$SettleSeconds = 2
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

if ($PSVersionTable.PSEdition -ne 'Desktop') {
    throw 'Run this script with Windows PowerShell 5.1 (powershell.exe), not PowerShell 7.'
}
if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT) {
    throw 'This capture requires Windows.'
}

Add-Type -AssemblyName System.Runtime.WindowsRuntime
$accelerometer = [Windows.Devices.Sensors.Accelerometer,Windows.Devices.Sensors,ContentType=WindowsRuntime]::GetDefault()
if ($null -eq $accelerometer) {
    throw 'Windows did not provide a default accelerometer. No output files were written.'
}

$runId = [DateTimeOffset]::UtcNow.ToString('yyyyMMdd-HHmmssZ', [Globalization.CultureInfo]::InvariantCulture)
$desktop = [Environment]::GetFolderPath('Desktop')
if ([string]::IsNullOrWhiteSpace($desktop) -or -not (Test-Path -LiteralPath $desktop -PathType Container)) {
    throw 'Could not locate the current user Desktop folder.'
}

$csvPath = Join-Path $desktop "latitude7320-sensor-poses-$runId.csv"
$summaryPath = Join-Path $desktop "latitude7320-sensor-poses-$runId-summary.txt"
$deviceId = [string]$accelerometer.DeviceId
$readingTransform = [string]$accelerometer.ReadingTransform
$minimumReportInterval = [uint32]$accelerometer.MinimumReportInterval
$reportInterval = [uint32]$accelerometer.ReportInterval
$samples = New-Object 'System.Collections.Generic.List[object]'

$poses = @(
    [pscustomobject]@{
        Id = 'landscape_top_up'
        Description = 'Upright landscape; screen toward you; camera/top bezel at the top.'
    },
    [pscustomobject]@{
        Id = 'portrait_top_right'
        Description = 'Rotate 90 degrees clockwise in the screen plane; camera/top bezel points right.'
    },
    [pscustomobject]@{
        Id = 'landscape_top_down'
        Description = 'Rotate 180 degrees from the first pose; camera/top bezel at the bottom.'
    },
    [pscustomobject]@{
        Id = 'portrait_top_left'
        Description = 'Rotate 270 degrees from the first pose; camera/top bezel points left.'
    }
)

Write-Host 'This records four short, pose-labeled holds from the Windows accelerometer.'
Write-Host 'Keep the screen facing you. Use the physical camera/top bezel for labels even if Windows rotates its image.'
Write-Host ("DeviceId: {0}" -f $deviceId)
Write-Host ("ReadingTransform: {0}; ReportInterval: {1} ms; MinimumReportInterval: {2} ms" -f `
    $readingTransform, $reportInterval, $minimumReportInterval)
Write-Host ("Each pose settles for {0}s, then captures for {1}s at about {2}ms intervals." -f `
    $SettleSeconds, $CaptureSeconds, $PollIntervalMs)
Write-Host 'The four poses cover in-plane screen rotation. An optional screen-up/down pair checks the screen-normal axis.'
$optionalDuration = 2 * ($CaptureSeconds + $SettleSeconds)
$includeFlatPoses = Read-Host ("Add the optional screen-up/down pair (about {0} more seconds)? [y/N]" -f $optionalDuration)
if ($includeFlatPoses -match '^(y|yes)$') {
    $poses += @(
        [pscustomobject]@{
            Id = 'screen_up_top_up'
            Description = 'Tablet flat; screen facing ceiling; camera/top bezel points away from you.'
        },
        [pscustomobject]@{
            Id = 'screen_down_top_up'
            Description = 'Tablet flat; screen facing desk; camera/top bezel points away from you.'
        }
    )
}
Write-Host 'Press Ctrl+C to stop; no sensor settings are changed.'

$poseOrder = 0
foreach ($pose in $poses) {
    $poseOrder++
    Write-Host ''
    Write-Host ("Pose {0}/{1} - {2}" -f $poseOrder, $poses.Count, $pose.Id) -ForegroundColor Cyan
    Write-Host $pose.Description
    $null = Read-Host 'Position the tablet and hold it still, then press Enter'

    if ($SettleSeconds -gt 0) {
        Write-Host ("Settling for {0}s..." -f $SettleSeconds)
        Start-Sleep -Seconds $SettleSeconds
    }

    $poseSampleNumber = 0
    $lastSensorTimestamp = $null
    $timer = [Diagnostics.Stopwatch]::StartNew()
    while ($timer.Elapsed.TotalSeconds -lt $CaptureSeconds) {
        $reading = $accelerometer.GetCurrentReading()
        if ($null -ne $reading) {
            $sensorTimestamp = [string]$reading.Timestamp
            if ([string]::IsNullOrWhiteSpace($sensorTimestamp) -or $sensorTimestamp -ne $lastSensorTimestamp) {
                $poseSampleNumber++
                $lastSensorTimestamp = $sensorTimestamp
                $samples.Add([pscustomobject]@{
                    RunId = $runId
                    DeviceId = $deviceId
                    ReadingTransform = $readingTransform
                    ReportIntervalMs = $reportInterval
                    MinimumReportIntervalMs = $minimumReportInterval
                    PoseOrder = $poseOrder
                    PoseId = $pose.Id
                    PoseDescription = $pose.Description
                    SampleNumber = $poseSampleNumber
                    CapturedAtUtc = [DateTimeOffset]::UtcNow.ToString('o', [Globalization.CultureInfo]::InvariantCulture)
                    SensorTimestamp = $sensorTimestamp
                    X_g = [double]$reading.AccelerationX
                    Y_g = [double]$reading.AccelerationY
                    Z_g = [double]$reading.AccelerationZ
                })
            }
        }
        Start-Sleep -Milliseconds $PollIntervalMs
    }

    if ($poseSampleNumber -lt 3) {
        Write-Warning ("Only {0} reading(s) were captured for {1}; that pose may not have a robust median." -f `
            $poseSampleNumber, $pose.Id)
    } else {
        Write-Host ("Captured {0} readings." -f $poseSampleNumber)
    }
}

if ($samples.Count -eq 0) {
    throw 'No accelerometer readings were captured. No output files were written.'
}

function Get-Median {
    param([double[]]$Values)

    if ($null -eq $Values -or $Values.Count -eq 0) {
        return $null
    }

    $sorted = @($Values | Sort-Object)
    $middle = [int][Math]::Floor($sorted.Count / 2)
    if (($sorted.Count % 2) -eq 1) {
        return [double]$sorted[$middle]
    }
    return ([double]$sorted[$middle - 1] + [double]$sorted[$middle]) / 2.0
}

function Get-RobustAxisStats {
    param([double[]]$Values)

    $median = Get-Median -Values $Values
    if ($null -eq $median) {
        return [pscustomobject]@{ Median = $null; Mad = $null }
    }

    $deviations = New-Object 'System.Collections.Generic.List[double]'
    foreach ($value in $Values) {
        $deviations.Add([Math]::Abs([double]$value - [double]$median))
    }

    return [pscustomobject]@{
        Median = $median
        Mad = Get-Median -Values $deviations.ToArray()
    }
}

function Format-Number {
    param($Value)

    if ($null -eq $Value) {
        return 'n/a'
    }
    return ([double]$Value).ToString('F6', [Globalization.CultureInfo]::InvariantCulture)
}

$summary = New-Object 'System.Collections.Generic.List[string]'
$summary.Add('Latitude 7320 Detachable - pose-labeled accelerometer capture')
$summary.Add(("RunId UTC: {0}" -f $runId))
$summary.Add(("DeviceId: {0}" -f $deviceId))
$summary.Add(("ReadingTransform: {0}" -f $readingTransform))
$summary.Add(("ReportIntervalMs: {0}; MinimumReportIntervalMs: {1}" -f `
    $reportInterval, $minimumReportInterval))
$summary.Add(("CaptureSecondsPerPose: {0}; PollIntervalMs: {1}; SettleSeconds: {2}" -f `
    $CaptureSeconds, $PollIntervalMs, $SettleSeconds))
$summary.Add(("PoseCount: {0}" -f $poses.Count))
$summary.Add('')
$summary.Add('Per-pose robust statistics (WinRT acceleration units are g):')
$summary.Add('PoseId,n,median_X_g,median_Y_g,median_Z_g,MAD_X_g,MAD_Y_g,MAD_Z_g')

foreach ($pose in $poses) {
    $poseRows = @($samples | Where-Object { $_.PoseId -eq $pose.Id })
    $xStats = Get-RobustAxisStats -Values ([double[]]@($poseRows | ForEach-Object { $_.X_g }))
    $yStats = Get-RobustAxisStats -Values ([double[]]@($poseRows | ForEach-Object { $_.Y_g }))
    $zStats = Get-RobustAxisStats -Values ([double[]]@($poseRows | ForEach-Object { $_.Z_g }))

    $summary.Add(('{0},{1},{2},{3},{4},{5},{6},{7}' -f `
        $pose.Id,
        $poseRows.Count,
        (Format-Number $xStats.Median),
        (Format-Number $yStats.Median),
        (Format-Number $zStats.Median),
        (Format-Number $xStats.Mad),
        (Format-Number $yStats.Mad),
        (Format-Number $zStats.Mad)))
}

$summary.Add('')
$summary.Add('MAD is median absolute deviation; lower values indicate a steadier hold.')
$summary.Add('This test records orientation evidence only. It does not determine sensor maxRange or full-scale range.')
$summary.Add('The Windows display transform is recorded but not changed; verify axis mapping against the device natural landscape orientation.')

$samples | Export-Csv -LiteralPath $csvPath -NoTypeInformation -Encoding UTF8
Set-Content -LiteralPath $summaryPath -Value $summary.ToArray() -Encoding UTF8

Write-Host ''
Write-Host ("Saved raw readings: {0}" -f $csvPath) -ForegroundColor Green
Write-Host ("Saved per-pose summary: {0}" -f $summaryPath) -ForegroundColor Green
Write-Host ("Total readings: {0}" -f $samples.Count)
