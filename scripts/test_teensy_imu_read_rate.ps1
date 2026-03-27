param(
  [string]$Port = "COM10",
  [int]$Baud = 115200,
  [int[]]$RatesHz = @(25, 50, 100, 200, 400, 800, 1600),
  [int]$SampleWindowMs = 2000,
  [int]$ReadTimeoutMs = 250,
  [int]$CommandTimeoutMs = 4000,
  [int]$SettleMs = 300,
  [int]$RestoreHz = 400,
  [string]$OutputJson = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function New-TimeoutDeadline {
  param([int]$TimeoutMs)
  return [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
}

function Read-MatchingLine {
  param(
    [System.IO.Ports.SerialPort]$Serial,
    [string]$Pattern,
    [int]$TimeoutMs
  )

  $deadline = New-TimeoutDeadline $TimeoutMs
  while ([DateTime]::UtcNow -lt $deadline) {
    try {
      $line = $Serial.ReadLine().Trim()
    } catch [System.TimeoutException] {
      continue
    }
    if ([string]::IsNullOrWhiteSpace($line)) { continue }
    if ($line -match $Pattern) {
      return $line
    }
  }

  throw "Timed out waiting for pattern: $Pattern"
}

function Drain-Serial {
  param(
    [System.IO.Ports.SerialPort]$Serial,
    [int]$QuietMs = 150
  )

  $quietDeadline = [DateTime]::UtcNow.AddMilliseconds($QuietMs)
  while ([DateTime]::UtcNow -lt $quietDeadline) {
    try {
      [void]$Serial.ReadLine()
      $quietDeadline = [DateTime]::UtcNow.AddMilliseconds($QuietMs)
    } catch [System.TimeoutException] {
      Start-Sleep -Milliseconds 20
    }
  }
}

function Invoke-CommandAndMatch {
  param(
    [System.IO.Ports.SerialPort]$Serial,
    [string]$Command,
    [string]$Pattern,
    [int]$TimeoutMs
  )

  Drain-Serial -Serial $Serial
  $Serial.WriteLine($Command)
  return Read-MatchingLine -Serial $Serial -Pattern $Pattern -TimeoutMs $TimeoutMs
}

function Get-SourceSnapshot {
  param(
    [System.IO.Ports.SerialPort]$Serial,
    [int]$TimeoutMs
  )

  $pattern = '^SOURCE CFG rate=(\d+)Hz period_us=(\d+) reads=(\d+) updates=(\d+) imu_acc_hz=([-\d.]+) imu_gyr_hz=([-\d.]+) supported=(.+)$'
  $line = Invoke-CommandAndMatch -Serial $Serial -Command "showsource" -Pattern $pattern -TimeoutMs $TimeoutMs
  $match = [regex]::Match($line, $pattern)
  if (-not $match.Success) {
    throw "Failed to parse source snapshot line: $line"
  }
  return [pscustomobject]@{
    Line = $line
    RateHz = [int]$match.Groups[1].Value
    PeriodUs = [int]$match.Groups[2].Value
    Reads = [uint32]$match.Groups[3].Value
    Updates = [uint32]$match.Groups[4].Value
    ImuAccHz = [double]$match.Groups[5].Value
    ImuGyrHz = [double]$match.Groups[6].Value
    Supported = $match.Groups[7].Value
  }
}

function Set-SourceRate {
  param(
    [System.IO.Ports.SerialPort]$Serial,
    [int]$RateHz,
    [int]$TimeoutMs
  )

  $pattern = '^setsourcehz applied requested=(\d+) applied=(\d+)$'
  $line = Invoke-CommandAndMatch -Serial $Serial -Command ("setsourcehz {0}" -f $RateHz) -Pattern $pattern -TimeoutMs $TimeoutMs
  $match = [regex]::Match($line, $pattern)
  if (-not $match.Success) {
    throw "Failed to parse setsourcehz response: $line"
  }
  return [pscustomobject]@{
    Line = $line
    RequestedHz = [int]$match.Groups[1].Value
    AppliedHz = [int]$match.Groups[2].Value
  }
}

function Test-Approx {
  param(
    [double]$Value,
    [double]$Expected,
    [double]$PercentTolerance
  )

  if ($Expected -eq 0.0) { return $false }
  $deltaPct = [Math]::Abs(($Value - $Expected) / $Expected) * 100.0
  return $deltaPct -le $PercentTolerance
}

$serial = [System.IO.Ports.SerialPort]::new($Port, $Baud, [System.IO.Ports.Parity]::None, 8, [System.IO.Ports.StopBits]::One)
$serial.NewLine = "`r`n"
$serial.ReadTimeout = $ReadTimeoutMs
$serial.WriteTimeout = 1000
$serial.DtrEnable = $false
$serial.RtsEnable = $false

$results = New-Object System.Collections.Generic.List[object]

try {
  $serial.Open()
  Start-Sleep -Milliseconds 1500
  Drain-Serial -Serial $serial -QuietMs 250

  try { $serial.WriteLine("x") } catch {}
  Start-Sleep -Milliseconds 100
  Drain-Serial -Serial $serial -QuietMs 150

  foreach ($rate in $RatesHz) {
    Write-Host ("Testing IMU read cadence at {0} Hz..." -f $rate) -ForegroundColor Cyan
    $setResult = Set-SourceRate -Serial $serial -RateHz $rate -TimeoutMs $CommandTimeoutMs
    Start-Sleep -Milliseconds $SettleMs

    $snap1 = Get-SourceSnapshot -Serial $serial -TimeoutMs $CommandTimeoutMs
    $start = [DateTime]::UtcNow
    Start-Sleep -Milliseconds $SampleWindowMs
    $snap2 = Get-SourceSnapshot -Serial $serial -TimeoutMs $CommandTimeoutMs
    $stop = [DateTime]::UtcNow

    $elapsedSec = ($stop - $start).TotalSeconds
    if ($elapsedSec -le 0.0) {
      throw "Invalid elapsed time while measuring IMU read rate"
    }

    $deltaReads = [double]([uint64]$snap2.Reads - [uint64]$snap1.Reads)
    $deltaUpdates = [double]([uint64]$snap2.Updates - [uint64]$snap1.Updates)
    $measuredReadHz = $deltaReads / $elapsedSec
    $measuredUpdateHz = $deltaUpdates / $elapsedSec

    $sourcePass = ($setResult.AppliedHz -eq $rate) -and ($snap2.RateHz -eq $rate)
    $imuAccPass = Test-Approx -Value $snap2.ImuAccHz -Expected $rate -PercentTolerance 5.0
    $imuGyrPass = Test-Approx -Value $snap2.ImuGyrHz -Expected $rate -PercentTolerance 5.0
    $readPass = Test-Approx -Value $measuredReadHz -Expected $rate -PercentTolerance 12.0

    $results.Add([pscustomobject]@{
      target_hz = $rate
      requested_hz = $setResult.RequestedHz
      applied_hz = $setResult.AppliedHz
      reported_rate_hz = $snap2.RateHz
      period_us = $snap2.PeriodUs
      imu_acc_hz = [Math]::Round($snap2.ImuAccHz, 2)
      imu_gyr_hz = [Math]::Round($snap2.ImuGyrHz, 2)
      reads_start = [uint64]$snap1.Reads
      reads_end = [uint64]$snap2.Reads
      reads_delta = [uint64]$deltaReads
      updates_start = [uint64]$snap1.Updates
      updates_end = [uint64]$snap2.Updates
      updates_delta = [uint64]$deltaUpdates
      elapsed_s = [Math]::Round($elapsedSec, 3)
      measured_read_hz = [Math]::Round($measuredReadHz, 2)
      measured_update_hz = [Math]::Round($measuredUpdateHz, 2)
      pass_source = $sourcePass
      pass_imu_acc = $imuAccPass
      pass_imu_gyr = $imuGyrPass
      pass_read = $readPass
      pass_all = ($sourcePass -and $imuAccPass -and $imuGyrPass -and $readPass)
    })
  }

  if ($RestoreHz -gt 0) {
    Write-Host ("Restoring source rate to {0} Hz..." -f $RestoreHz) -ForegroundColor Yellow
    [void](Set-SourceRate -Serial $serial -RateHz $RestoreHz -TimeoutMs $CommandTimeoutMs)
  }

  Write-Host ""
  Write-Host "IMU Read Cadence Summary" -ForegroundColor Green
  $results |
    Select-Object target_hz, applied_hz, reported_rate_hz, imu_acc_hz, imu_gyr_hz, measured_read_hz, measured_update_hz, pass_all |
    Format-Table -AutoSize

  if ($OutputJson) {
    $serializable = foreach ($result in $results) {
      [ordered]@{
        target_hz = $result.target_hz
        requested_hz = $result.requested_hz
        applied_hz = $result.applied_hz
        reported_rate_hz = $result.reported_rate_hz
        period_us = $result.period_us
        imu_acc_hz = $result.imu_acc_hz
        imu_gyr_hz = $result.imu_gyr_hz
        reads_start = $result.reads_start
        reads_end = $result.reads_end
        reads_delta = $result.reads_delta
        updates_start = $result.updates_start
        updates_end = $result.updates_end
        updates_delta = $result.updates_delta
        elapsed_s = $result.elapsed_s
        measured_read_hz = $result.measured_read_hz
        measured_update_hz = $result.measured_update_hz
        pass_source = $result.pass_source
        pass_imu_acc = $result.pass_imu_acc
        pass_imu_gyr = $result.pass_imu_gyr
        pass_read = $result.pass_read
        pass_all = $result.pass_all
      }
    }
    $serializable | ConvertTo-Json -Depth 4 | Set-Content -Path $OutputJson -Encoding UTF8
    Write-Host ("Wrote JSON results to {0}" -f $OutputJson) -ForegroundColor Yellow
  }
}
finally {
  try {
    if ($serial.IsOpen) {
      $serial.Close()
    }
  } catch {}
}
