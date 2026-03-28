param(
  [string]$ComPort = "COM7",
  [int]$Baud = 115200,
  [string]$LogPath = ""
)

$ErrorActionPreference = "Stop"

function New-Timestamp {
  (Get-Date).ToString("HH:mm:ss.fff")
}

function Write-LogLine {
  param([string]$Text)
  $line = "[{0}] {1}" -f (New-Timestamp), $Text
  Write-Host $line
  Add-Content -Path $script:LogFile -Value $line
}

function Open-SerialPort {
  param([string]$Name, [int]$BaudRate)
  $port = [System.IO.Ports.SerialPort]::new(
    $Name,
    $BaudRate,
    [System.IO.Ports.Parity]::None,
    8,
    [System.IO.Ports.StopBits]::One
  )
  $port.Handshake = [System.IO.Ports.Handshake]::None
  $port.NewLine = "`n"
  $port.ReadTimeout = 200
  $port.WriteTimeout = 1000
  # Keep modem-control lines low so opening the port does not reset ESP_AIR
  # into the ROM bootloader.
  $port.DtrEnable = $false
  $port.RtsEnable = $false
  $port.Open()
  return $port
}

function Drain-Serial {
  param($Port, [int]$DurationMs = 3000)
  $deadline = [Environment]::TickCount + $DurationMs
  $buf = New-Object System.Text.StringBuilder
  while ([Environment]::TickCount -lt $deadline) {
    Start-Sleep -Milliseconds 50
    $chunk = $Port.ReadExisting()
    if ($chunk) { [void]$buf.Append($chunk) }
  }
  return $buf.ToString()
}

function Invoke-SerialCommand {
  param(
    $Port,
    [string]$Command,
    [string]$MatchRegex,
    [int]$TimeoutMs = 4000
  )

  Write-LogLine "CMD $Command"
  $Port.DiscardInBuffer()
  $Port.WriteLine($Command)

  $deadline = [Environment]::TickCount + $TimeoutMs
  $buf = New-Object System.Text.StringBuilder
  while ([Environment]::TickCount -lt $deadline) {
    Start-Sleep -Milliseconds 50
    $chunk = $Port.ReadExisting()
    if ($chunk) {
      [void]$buf.Append($chunk)
      if ($buf.ToString() -match $MatchRegex) {
        break
      }
    }
  }

  $text = $buf.ToString().Trim()
  if (-not $text) {
    Write-LogLine "(no response captured)"
  } else {
    foreach ($line in ($text -split "`r?`n")) {
      if ($line.Trim().Length -gt 0) {
        Write-LogLine $line.TrimEnd()
      }
    }
  }
  return $text
}

function Get-ResultState {
  param([string]$Response, [string]$ResultPrefix)
  if ($ResultPrefix -eq "BENCH standalone=1") {
    if ($Response -match "BENCH standalone=1") { return "pass" }
    return "failure"
  }
  if ($ResultPrefix -eq "QUIET on") {
    if ($Response -match "QUIET on") { return "pass" }
    return "failure"
  }
  if ($ResultPrefix -eq "TAPI STATUS") {
    if ($Response -match "TAPI STATUS") { return "pass" }
    return "failure"
  }
  if ($Response -match [regex]::Escape($ResultPrefix) + ".*ok=(\d+)") {
    if ($Matches[1] -eq "1") { return "pass" }
    return "failure"
  }
  if ($Response -match [regex]::Escape($ResultPrefix) + ".*RESULT ok=(\d+)") {
    if ($Matches[1] -eq "1") { return "pass" }
    return "failure"
  }
  return "failure"
}

if ([string]::IsNullOrWhiteSpace($LogPath)) {
  $stamp = (Get-Date).ToString("yyyyMMdd_HHmmss")
  $LogPath = "C:\Users\dell\Platformio\esp32_crsf_telemetry\Teensy_ESP_AIR_GND\scripts\teensy_api_exerciser_$stamp.log"
}
$script:LogFile = $LogPath
"Teensy API exerciser log" | Set-Content -Path $script:LogFile
Write-LogLine "Logging to $script:LogFile"
Write-LogLine "Opening $ComPort at $Baud"

$port = $null
try {
  $port = Open-SerialPort -Name $ComPort -BaudRate $Baud
  $drain = Drain-Serial -Port $port -DurationMs 3000
  if ($drain.Trim()) {
    foreach ($line in ($drain -split "`r?`n")) {
      if ($line.Trim()) { Write-LogLine $line.TrimEnd() }
    }
  } else {
    Write-LogLine "(startup quiet)"
  }

  $steps = @(
    @{ Label = "Bench"; Command = "bench on"; Regex = "BENCH standalone=1"; Prefix = "BENCH standalone=1"; TimeoutMs = 3000 },
    @{ Label = "Quiet"; Command = "quiet on"; Regex = "QUIET on"; Prefix = "QUIET on"; TimeoutMs = 2000 },
    @{ Label = "Status"; Command = "tapi status"; Regex = "TAPI STATUS"; Prefix = "TAPI STATUS"; TimeoutMs = 3000 },
    @{ Label = "GetFusion"; Command = "tapi getfusion"; Regex = "TAPI GETFUSION RESULT"; Prefix = "TAPI GETFUSION RESULT"; TimeoutMs = 4000 },
    @{ Label = "SetCap"; Command = "tapi setcap 1600"; Regex = "TAPI SETCAP RESULT"; Prefix = "TAPI SETCAP RESULT"; TimeoutMs = 4000 },
    @{ Label = "SetStream"; Command = "tapi setstream 1600 1600"; Regex = "TAPI SETSTREAM RESULT"; Prefix = "TAPI SETSTREAM RESULT"; TimeoutMs = 4000 },
    @{ Label = "Carry"; Command = "tapi carry 8"; Regex = "TAPI CARRY RESULT"; Prefix = "TAPI CARRY RESULT"; TimeoutMs = 15000 },
    @{ Label = "CarryCsv"; Command = "tapi carrycsv 2000 16"; Regex = "TAPI CARRYCSV RESULT"; Prefix = "TAPI CARRYCSV RESULT"; TimeoutMs = 10000 },
    @{ Label = "SelfTest"; Command = "tapi selftest 1600 8"; Regex = "TAPI SELFTEST RESULT"; Prefix = "TAPI SELFTEST RESULT"; TimeoutMs = 20000 }
  )

  $passed = 0
  $failed = 0
  foreach ($step in $steps) {
    $resp = Invoke-SerialCommand -Port $port -Command $step.Command -MatchRegex $step.Regex -TimeoutMs $step.TimeoutMs
    $state = Get-ResultState -Response $resp -ResultPrefix $step.Prefix
    if ($state -eq "pass") { $passed++ } else { $failed++ }
    Write-LogLine ("STEP STATUS label={0} state={1}" -f $step.Label, $state)
  }

  Write-LogLine ("FINAL SUMMARY passed={0} failed={1} port={2}" -f $passed, $failed, $ComPort)
}
finally {
  if ($port -and $port.IsOpen) {
    $port.Close()
  }
}
