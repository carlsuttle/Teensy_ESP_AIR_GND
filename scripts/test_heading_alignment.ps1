param(
  [string]$Port = "COM10",
  [int]$Baud = 115200,
  [int]$SamplesPerHeading = 12,
  [int]$WarmupSamples = 3,
  [string[]]$Headings = @("N", "E", "S", "W"),
  [int]$ReadTimeoutMs = 300,
  [int]$SettleSeconds = 2
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Normalize-Degrees {
  param([double]$Degrees)
  $value = $Degrees % 360.0
  if ($value -lt 0.0) { $value += 360.0 }
  return $value
}

function Circular-Mean {
  param([double[]]$Values)
  if (-not $Values -or $Values.Count -eq 0) { return $null }
  $sumSin = 0.0
  $sumCos = 0.0
  foreach ($value in $Values) {
    $radians = (Normalize-Degrees $value) * [Math]::PI / 180.0
    $sumSin += [Math]::Sin($radians)
    $sumCos += [Math]::Cos($radians)
  }
  if (($sumSin -eq 0.0) -and ($sumCos -eq 0.0)) { return $null }
  $mean = [Math]::Atan2($sumSin, $sumCos) * 180.0 / [Math]::PI
  return Normalize-Degrees $mean
}

function Circular-Error {
  param(
    [double]$Measured,
    [double]$Target
  )
  $delta = (Normalize-Degrees $Measured) - (Normalize-Degrees $Target)
  while ($delta -gt 180.0) { $delta -= 360.0 }
  while ($delta -lt -180.0) { $delta += 360.0 }
  return $delta
}

function Heading-Target {
  param([string]$Name)
  switch ($Name.ToUpperInvariant()) {
    "N" { return 0.0 }
    "E" { return 90.0 }
    "S" { return 180.0 }
    "W" { return 270.0 }
    default { throw "Unsupported heading '$Name'. Use N, E, S, or W." }
  }
}

function Parse-HeadingLine {
  param([string]$Line)
  if ($Line -notmatch 'GETHDG raw=([-\d.]+) deg fusion=([-\d.]+) deg mag_heading=([-\d.]+) deg mag_in=\(([-\d.]+), ([-\d.]+), ([-\d.]+)\)') {
    return $null
  }
  return [ordered]@{
    Raw = [double]$matches[1]
    Fusion = [double]$matches[2]
    Mag = [double]$matches[3]
    MagX = [double]$matches[4]
    MagY = [double]$matches[5]
    MagZ = [double]$matches[6]
    Line = $Line
  }
}

function Write-HeadingSummary {
  param(
    [string]$Name,
    [double]$Target,
    [object[]]$Samples
  )

  $rawMean = Circular-Mean ($Samples | ForEach-Object { $_.Raw })
  $fusionMean = Circular-Mean ($Samples | ForEach-Object { $_.Fusion })
  $magMean = Circular-Mean ($Samples | ForEach-Object { $_.Mag })
  $magXMean = ($Samples | Measure-Object -Property MagX -Average).Average
  $magYMean = ($Samples | Measure-Object -Property MagY -Average).Average
  $magZMean = ($Samples | Measure-Object -Property MagZ -Average).Average

  Write-Host ""
  Write-Host ("[{0}] target={1:000}" -f $Name, $Target) -ForegroundColor Cyan
  if ($rawMean -ne $null) {
    Write-Host ("  raw     mean={0,6:0.0} err={1,+6:0.0;-0.0;0.0}" -f $rawMean, (Circular-Error $rawMean $Target))
  }
  if ($magMean -ne $null) {
    Write-Host ("  mag     mean={0,6:0.0} err={1,+6:0.0;-0.0;0.0}" -f $magMean, (Circular-Error $magMean $Target))
  }
  if ($fusionMean -ne $null) {
    Write-Host ("  fusion  mean={0,6:0.0} err={1,+6:0.0;-0.0;0.0}" -f $fusionMean, (Circular-Error $fusionMean $Target))
  }
  Write-Host ("  mag_in  mean=({0,6:0.1}, {1,6:0.1}, {2,6:0.1})" -f $magXMean, $magYMean, $magZMean)
}

$serial = [System.IO.Ports.SerialPort]::new($Port, $Baud)
$serial.NewLine = "`r`n"
$serial.ReadTimeout = $ReadTimeoutMs
$serial.WriteTimeout = 1000
$serial.DtrEnable = $false
$serial.RtsEnable = $false

try {
  $serial.Open()
  Start-Sleep -Milliseconds 300
  $serial.DiscardInBuffer()
  $serial.DiscardOutBuffer()

  $serial.WriteLine("x")
  Start-Sleep -Milliseconds 100
  $serial.WriteLine("gethdg")

  $results = @()

  foreach ($heading in $Headings) {
    $target = Heading-Target $heading
    Write-Host ""
    Write-Host ("Position the aircraft at {0} ({1:000} deg), keep it level, then press Enter." -f $heading.ToUpperInvariant(), $target) -ForegroundColor Yellow
    [void](Read-Host)
    if ($SettleSeconds -gt 0) {
      Write-Host ("Settling for {0}s..." -f $SettleSeconds)
      Start-Sleep -Seconds $SettleSeconds
    }

    $headingSamples = New-Object System.Collections.Generic.List[object]
    $wanted = $WarmupSamples + $SamplesPerHeading

    while ($headingSamples.Count -lt $wanted) {
      try {
        $line = $serial.ReadLine()
      } catch [System.TimeoutException] {
        continue
      }
      $parsed = Parse-HeadingLine $line
      if ($null -eq $parsed) { continue }
      $headingSamples.Add([pscustomobject]$parsed)
      Write-Host ("  sample {0}/{1}: raw={2,6:0.0} fusion={3,6:0.0} mag={4,6:0.0}" -f $headingSamples.Count, $wanted, $parsed.Raw, $parsed.Fusion, $parsed.Mag)
    }

    $usable = @($headingSamples | Select-Object -Skip $WarmupSamples)
    $results += [pscustomobject]@{
      Heading = $heading.ToUpperInvariant()
      Target = $target
      Samples = $usable
    }
    Write-HeadingSummary -Name $heading.ToUpperInvariant() -Target $target -Samples $usable
  }

  Write-Host ""
  Write-Host "Summary" -ForegroundColor Green
  foreach ($result in $results) {
    $rawMean = Circular-Mean ($result.Samples | ForEach-Object { $_.Raw })
    $fusionMean = Circular-Mean ($result.Samples | ForEach-Object { $_.Fusion })
    $magMean = Circular-Mean ($result.Samples | ForEach-Object { $_.Mag })
    Write-Host ("{0}: raw={1,6:0.0} mag={2,6:0.0} fusion={3,6:0.0}" -f $result.Heading, $rawMean, $magMean, $fusionMean)
  }
}
finally {
  try { if ($serial.IsOpen) { $serial.WriteLine("x") } } catch {}
  try { if ($serial.IsOpen) { $serial.Close() } } catch {}
}
