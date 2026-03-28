param(
  [string]$ComPort = "COM7",
  [int]$Baud = 115200,
  [int]$CapHz = 1600,
  [int]$StreamHz = 1600,
  [int]$LogHz = 1600,
  [int]$CarryCount = 8,
  [int]$CarryCsvMs = 2000,
  [int]$CarryCsvWindow = 16,
  [single]$FusionGain = 0.123,
  [single]$AccelRej = 11.0,
  [single]$MagRej = 22.0,
  [int]$Recovery = 333,
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

function Invoke-Step {
  param(
    $Port,
    [string]$Phase,
    [string]$Label,
    [string]$Command,
    [string]$Regex,
    [string]$Prefix,
    [int]$TimeoutMs
  )

  $resp = Invoke-SerialCommand -Port $Port -Command $Command -MatchRegex $Regex -TimeoutMs $TimeoutMs
  $state = Get-ResultState -Response $resp -ResultPrefix $Prefix
  $phasePassCount = 0
  $phaseFailCount = 0
  if ($script:PhasePassed.ContainsKey($Phase)) { $phasePassCount = [int]$script:PhasePassed[$Phase] }
  if ($script:PhaseFailed.ContainsKey($Phase)) { $phaseFailCount = [int]$script:PhaseFailed[$Phase] }
  if ($state -eq "pass") {
    $script:Passed++
    $script:PhasePassed[$Phase] = $phasePassCount + 1
  } else {
    $script:Failed++
    $script:PhaseFailed[$Phase] = $phaseFailCount + 1
  }
  Write-LogLine ("STEP STATUS phase={0} label={1} state={2}" -f $Phase, $Label, $state)
}

if ([string]::IsNullOrWhiteSpace($LogPath)) {
  $stamp = (Get-Date).ToString("yyyyMMdd_HHmmss")
  $LogPath = "C:\Users\dell\Platformio\esp32_crsf_telemetry\Teensy_ESP_AIR_GND\scripts\teensy_api_mode_sweep_$stamp.log"
}
$script:LogFile = $LogPath
$script:Passed = 0
$script:Failed = 0
$script:PhasePassed = @{}
$script:PhaseFailed = @{}

"Teensy API mode sweep log" | Set-Content -Path $script:LogFile
Write-LogLine "Logging to $script:LogFile"
Write-LogLine "Opening $ComPort at $Baud"

$port = $null
try {
  $port = Open-SerialPort -Name $ComPort -BaudRate $Baud
  Write-LogLine ">>> Startup drain"
  $drain = Drain-Serial -Port $port -DurationMs 3000
  if ($drain.Trim()) {
    foreach ($line in ($drain -split "`r?`n")) {
      if ($line.Trim()) { Write-LogLine $line.TrimEnd() }
    }
  } else {
    Write-LogLine "(startup quiet)"
  }

  Write-LogLine ">>> Phase Quiet"
  Invoke-Step -Port $port -Phase "Quiet" -Label "BenchOn" -Command "bench on" -Regex "BENCH standalone=1" -Prefix "BENCH standalone=1" -TimeoutMs 3000
  Invoke-Step -Port $port -Phase "Quiet" -Label "QuietOn" -Command "quiet on" -Regex "QUIET on" -Prefix "QUIET on" -TimeoutMs 2000

  Write-LogLine ">>> Phase Live"
  Invoke-Step -Port $port -Phase "Live" -Label "Status" -Command "tapi status" -Regex "TAPI STATUS" -Prefix "TAPI STATUS" -TimeoutMs 3000

  Write-LogLine ">>> Phase Control"
  Invoke-Step -Port $port -Phase "Control" -Label "GetFusion" -Command "tapi getfusion" -Regex "TAPI GETFUSION RESULT" -Prefix "TAPI GETFUSION RESULT" -TimeoutMs 4000
  Invoke-Step -Port $port -Phase "Control" -Label "SetCap" -Command ("tapi setcap {0}" -f $CapHz) -Regex "TAPI SETCAP RESULT" -Prefix "TAPI SETCAP RESULT" -TimeoutMs 4000
  Invoke-Step -Port $port -Phase "Control" -Label "SetStream" -Command ("tapi setstream {0} {1}" -f $StreamHz, $LogHz) -Regex "TAPI SETSTREAM RESULT" -Prefix "TAPI SETSTREAM RESULT" -TimeoutMs 4000
  Invoke-Step -Port $port -Phase "Control" -Label "SetFusion" -Command ("tapi setfusion {0} {1} {2} {3}" -f $FusionGain.ToString("0.###"), $AccelRej.ToString("0.###"), $MagRej.ToString("0.###"), $Recovery) -Regex "TAPI SETFUSION RESULT" -Prefix "TAPI SETFUSION RESULT" -TimeoutMs 4000

  Write-LogLine ">>> Phase ReplayCarry"
  Invoke-Step -Port $port -Phase "ReplayCarry" -Label "Carry" -Command ("tapi carry {0}" -f $CarryCount) -Regex "TAPI CARRY RESULT" -Prefix "TAPI CARRY RESULT" -TimeoutMs 15000

  Write-LogLine ">>> Phase ReplaySequence"
  Invoke-Step -Port $port -Phase "ReplaySequence" -Label "CarryCsv" -Command ("tapi carrycsv {0} {1}" -f $CarryCsvMs, $CarryCsvWindow) -Regex "TAPI CARRYCSV RESULT" -Prefix "TAPI CARRYCSV RESULT" -TimeoutMs ([Math]::Max(10000, $CarryCsvMs + 4000))

  Write-LogLine ">>> Phase SelfTest"
  Invoke-Step -Port $port -Phase "SelfTest" -Label "SelfTest" -Command ("tapi selftest {0} {1}" -f $CapHz, $CarryCount) -Regex "TAPI SELFTEST RESULT" -Prefix "TAPI SELFTEST RESULT" -TimeoutMs 20000

  foreach ($phase in @("Quiet", "Live", "Control", "ReplayCarry", "ReplaySequence", "SelfTest")) {
    $phasePass = 0
    $phaseFail = 0
    if ($script:PhasePassed.ContainsKey($phase)) { $phasePass = [int]$script:PhasePassed[$phase] }
    if ($script:PhaseFailed.ContainsKey($phase)) { $phaseFail = [int]$script:PhaseFailed[$phase] }
    Write-LogLine ("PHASE SUMMARY phase={0} passed={1} failed={2}" -f $phase, $phasePass, $phaseFail)
  }
  Write-LogLine ("FINAL SUMMARY passed={0} failed={1} port={2}" -f $script:Passed, $script:Failed, $ComPort)
}
finally {
  if ($port -and $port.IsOpen) {
    $port.Close()
  }
}
