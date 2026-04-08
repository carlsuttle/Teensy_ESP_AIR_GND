param(
  [string]$AirComPort = "COM7",
  [string]$TeensyComPort = "COM10",
  [int]$Baud = 115200,
  [int]$DurationMs = 5000,
  [int]$BatchHz = 50,
  [int]$BatchRecords = 48,
  [int]$ControlPollMs = 300,
  [string]$LogPath = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

. "$PSScriptRoot\air_serial_control_common.ps1"

if (-not $LogPath) {
  $LogPath = New-ControlLogPath -Prefix "air_serial_control_perf"
}

function Open-TextPort {
  param([string]$Name, [int]$BaudRate)
  return Open-ControlSerialPort -Name $Name -BaudRate $BaudRate
}

function Invoke-TextCommand {
  param(
    [System.IO.Ports.SerialPort]$Port,
    [string]$Command,
    [string]$MatchRegex,
    [int]$TimeoutMs = 4000,
    [hashtable]$LogContext
  )

  Write-ControlLog -Context $LogContext -Source "CMD" -Text $Command
  $Port.DiscardInBuffer()
  $Port.WriteLine($Command)

  $deadline = (Get-Date).AddMilliseconds($TimeoutMs)
  $buffer = New-Object System.Text.StringBuilder
  while ((Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 50
    $chunk = $Port.ReadExisting()
    if (-not $chunk) { continue }
    [void]$buffer.Append($chunk)
    if ($buffer.ToString() -match $MatchRegex) {
      break
    }
  }

  $text = $buffer.ToString().Trim()
  foreach ($line in ($text -split "`r?`n")) {
    if ($line.Trim()) {
      Write-ControlLog -Context $LogContext -Source "TXT" -Text $line.TrimEnd()
    }
  }
  return $text
}

function Parse-AirReplayBench {
  param([string]$Response)

  $pattern = 'TAPI REPLAYBENCH RESULT ok=(\d+) batch_hz=(\d+) batch_records=(\d+) duration_ms=(\d+) elapsed_ms=(\d+) sent=(\d+) recv=(\d+) pass=(\d+) fail=(\d+) timeout=(\d+) validated_rps=([0-9.]+)'
  if ($Response -notmatch $pattern) { return $null }
  return [pscustomobject]@{
    ok = [int]$Matches[1]
    batch_hz = [int]$Matches[2]
    batch_records = [int]$Matches[3]
    duration_ms = [int]$Matches[4]
    elapsed_ms = [int]$Matches[5]
    sent = [int]$Matches[6]
    recv = [int]$Matches[7]
    pass = [int]$Matches[8]
    fail = [int]$Matches[9]
    timeout = [int]$Matches[10]
    validated_rps = [double]$Matches[11]
  }
}

function Parse-TeensyPerf {
  param([string]$Response)

  $replayPattern = 'REPLAY PERF .*inputs_per_poll\[avg/max\]=(\d+)\/(\d+) ctrls_per_poll\[avg/max\]=(\d+)\/(\d+) outq_max=(\d+)'
  $spiPattern = 'SPI PERF .*tx_overflows=(\d+) rx_records=(\d+) rx_overflows=(\d+) crc_err=(\d+) type_err=(\d+) state_tx_occ_max=(\d+) state_tx_free_min=(\d+) raw_tx_occ_max=(\d+) raw_tx_free_min=(\d+) replay_rx_occ_max=(\d+) replay_rx_free_min=(\d+)'
  if ($Response -notmatch $replayPattern) { return $null }
  $inputsAvg = [int]$Matches[1]
  $inputsMax = [int]$Matches[2]
  $ctrlsAvg = [int]$Matches[3]
  $ctrlsMax = [int]$Matches[4]
  $outqMax = [int]$Matches[5]
  if ($Response -notmatch $spiPattern) { return $null }
  return [pscustomobject]@{
    inputs_avg = $inputsAvg
    inputs_max = $inputsMax
    ctrls_avg = $ctrlsAvg
    ctrls_max = $ctrlsMax
    outq_max = $outqMax
    tx_overflows = [int]$Matches[1]
    rx_overflows = [int]$Matches[3]
    crc_err = [int]$Matches[4]
    type_err = [int]$Matches[5]
    state_tx_occ_max = [int]$Matches[6]
    state_tx_free_min = [int]$Matches[7]
    replay_rx_occ_max = [int]$Matches[10]
    replay_rx_free_min = [int]$Matches[11]
  }
}

function Invoke-ReplayBenchCase {
  param(
    [hashtable]$AirContext,
    [System.IO.Ports.SerialPort]$TeensyPort,
    [string]$Label,
    [bool]$InjectControl
  )

  Write-ControlLog -Context $AirContext -Source "CASE" -Text ("START {0}" -f $Label)
  [void](Invoke-TextCommand -Port $TeensyPort -Command "resetloopperf" -MatchRegex "loop perf reset" -TimeoutMs 3000 -LogContext $AirContext)

  $benchCommand = "tapi replaybench $DurationMs $BatchHz $BatchRecords"
  Write-ControlLog -Context $AirContext -Source "CMD" -Text $benchCommand
  $AirContext.Port.DiscardInBuffer()
  $scanIndex = $AirContext.Lines.Count
  $AirContext.Port.WriteLine($benchCommand)

  $deadline = (Get-Date).AddMilliseconds($DurationMs + 12000)
  $benchBuffer = New-Object System.Text.StringBuilder
  $jsonCount = 0
  $nextControlAt = (Get-Date)

  while ((Get-Date) -lt $deadline) {
    if ($InjectControl -and (Get-Date) -ge $nextControlAt) {
      foreach ($request in @(
        @{ category = "state"; action = "get" },
        @{ category = "recording"; action = "status" },
        @{ category = "replay"; action = "status" },
        @{ category = "file"; action = "storage_status" },
        @{ category = "fusion"; action = "get" }
      )) {
        $resp = Invoke-AirControlRequest -Context $AirContext -Request $request -TimeoutMs 3000
        Assert-ControlCompletedOk -Response $resp -Label ("perf {0}/{1}" -f $request.category, $request.action)
        $jsonCount++
      }
      $nextControlAt = (Get-Date).AddMilliseconds($ControlPollMs)
    }

    Read-ControlPort -Context $AirContext
    for ($i = $scanIndex; $i -lt $AirContext.Lines.Count; $i++) {
      $line = $AirContext.Lines[$i]
      if ($line -match 'TAPI REPLAYBENCH RESULT') {
        [void]$benchBuffer.AppendLine($line)
        $scanIndex = $AirContext.Lines.Count
        break
      }
      if ($line -match '^TAPI REPLAYBENCH ' -or $line -match '^TAPI CARRY' -or $line -match '^TAPI STATUS') {
        [void]$benchBuffer.AppendLine($line)
      }
    }
    $scanIndex = $AirContext.Lines.Count
    if ($benchBuffer.ToString() -match 'TAPI REPLAYBENCH RESULT') {
      break
    }
    Start-Sleep -Milliseconds 25
  }

  if ($benchBuffer.ToString() -notmatch 'TAPI REPLAYBENCH RESULT') {
    throw ("{0}: timed out waiting for TAPI REPLAYBENCH RESULT" -f $Label)
  }

  $teensyResponse = Invoke-TextCommand -Port $TeensyPort -Command "showsource" -MatchRegex "SPI PERF" -TimeoutMs 4000 -LogContext $AirContext
  $airPerf = Parse-AirReplayBench -Response $benchBuffer.ToString()
  $teensyPerf = Parse-TeensyPerf -Response $teensyResponse
  if ($null -eq $airPerf) { throw ("{0}: unable to parse AIR replay benchmark output" -f $Label) }
  if ($null -eq $teensyPerf) { throw ("{0}: unable to parse Teensy perf output" -f $Label) }

  return [pscustomobject]@{
    label = $Label
    json_requests = $jsonCount
    air = $airPerf
    teensy = $teensyPerf
  }
}

function Write-PerfSummary {
  param(
    [hashtable]$Context,
    $Case
  )

  Write-ControlLog -Context $Context -Source "SUMMARY" -Text (
    "{0} validated_rps={1} state_tx_occ_max={2} state_tx_free_min={3} tx_overflows={4} rx_overflows={5} replay_rx_occ_max={6} replay_rx_free_min={7} outq_max={8} crc_err={9} type_err={10} json_requests={11}" -f
    $Case.label,
    $Case.air.validated_rps,
    $Case.teensy.state_tx_occ_max,
    $Case.teensy.state_tx_free_min,
    $Case.teensy.tx_overflows,
    $Case.teensy.rx_overflows,
    $Case.teensy.replay_rx_occ_max,
    $Case.teensy.replay_rx_free_min,
    $Case.teensy.outq_max,
    $Case.teensy.crc_err,
    $Case.teensy.type_err,
    $Case.json_requests
  )
}

$airContext = $null
$teensyPort = $null
try {
  $airPort = Open-ControlSerialPort -Name $AirComPort -BaudRate $Baud
  $airContext = New-ControlContext -Port $airPort -LogPath $LogPath
  Drain-ControlPort -Context $airContext -DurationMs 1000
  $teensyPort = Open-TextPort -Name $TeensyComPort -BaudRate $Baud

  [void](Invoke-TextCommand -Port $airContext.Port -Command "bench on" -MatchRegex "BENCH standalone=1" -TimeoutMs 3000 -LogContext $airContext)
  [void](Invoke-TextCommand -Port $airContext.Port -Command "quiet on" -MatchRegex "QUIET on" -TimeoutMs 2000 -LogContext $airContext)
  [void](Invoke-TextCommand -Port $teensyPort -Command "quiet on" -MatchRegex "QUIET enabled=1|QUIET on" -TimeoutMs 2000 -LogContext $airContext)

  $baseline = Invoke-ReplayBenchCase -AirContext $airContext -TeensyPort $teensyPort -Label "post_integration_baseline" -InjectControl:$false
  $withControl = Invoke-ReplayBenchCase -AirContext $airContext -TeensyPort $teensyPort -Label "with_serial_control_activity" -InjectControl:$true

  Write-PerfSummary -Context $airContext -Case $baseline
  Write-PerfSummary -Context $airContext -Case $withControl

  $rpsDelta = [Math]::Round([double]$withControl.air.validated_rps - [double]$baseline.air.validated_rps, 1)
  Write-ControlLog -Context $airContext -Source "COMPARE" -Text (
    "baseline_vs_control validated_rps_delta={0} baseline={1} control={2}" -f
    $rpsDelta,
    $baseline.air.validated_rps,
    $withControl.air.validated_rps
  )
}
finally {
  if ($null -ne $teensyPort) {
    try {
      if ($teensyPort.IsOpen) { $teensyPort.Close() }
    } catch {}
    $teensyPort.Dispose()
  }
  if ($null -ne $airContext) {
    Close-ControlContext -Context $airContext
  }
}
