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

function Clear-ControlLineHistory {
  param([hashtable]$Context)

  if ($Context.Lines.Count -gt 0) {
    $Context.Lines.Clear()
  }
  $Context.Partial = ""
}

function Invoke-PreCaseDrain {
  param(
    [hashtable]$Context,
    [string]$Label,
    [int]$CaseId,
    [int]$DrainMs = 300
  )

  $drainedChars = 0
  $drainedLines = 0
  try {
    $Context.Port.DiscardInBuffer()
  } catch {}

  $deadline = (Get-Date).AddMilliseconds($DrainMs)
  while ((Get-Date) -lt $deadline) {
    $chunk = $Context.Port.ReadExisting()
    if ($chunk) {
      $drainedChars += $chunk.Length
      foreach ($line in ($chunk -split "`r?`n")) {
        if ($line.Trim()) { $drainedLines++ }
      }
    }
    Start-Sleep -Milliseconds 25
  }

  Clear-ControlLineHistory -Context $Context
  Write-ControlLog -Context $Context -Source "HARNESS" -Text (
    "pre_case_drain label={0} case_id={1} drained_lines={2} drained_chars={3}" -f
    $Label,
    $CaseId,
    $drainedLines,
    $drainedChars
  )
}

function Write-ReplayBenchBuffer {
  param(
    [hashtable]$Context,
    [int]$CaseId,
    [System.Text.StringBuilder]$Buffer
  )

  Write-ControlLog -Context $Context -Source "HARNESS" -Text ("REPLAYBENCH BUFFER BEGIN case_id={0}" -f $CaseId)
  $text = $Buffer.ToString().Trim()
  if ($text) {
    foreach ($line in ($text -split "`r?`n")) {
      if ($line.Trim()) {
        Write-ControlLog -Context $Context -Source "HARNESS" -Text $line.TrimEnd()
      }
    }
  } else {
    Write-ControlLog -Context $Context -Source "HARNESS" -Text "(empty)"
  }
  Write-ControlLog -Context $Context -Source "HARNESS" -Text ("REPLAYBENCH BUFFER END case_id={0}" -f $CaseId)
}

function Invoke-PerfTimeoutForensics {
  param(
    [hashtable]$Context,
    [System.IO.Ports.SerialPort]$TeensyPort,
    [string]$Label,
    [int]$CaseId
  )

  Write-ControlLog -Context $Context -Source "HARNESS" -Text (
    "TIMEOUT FORENSICS case_id={0} label={1}" -f $CaseId, $Label
  )

  foreach ($request in @(
    @{ category = "state"; action = "get" },
    @{ category = "recording"; action = "status" },
    @{ category = "replay"; action = "status" },
    @{ category = "file"; action = "storage_status" },
    @{ category = "fusion"; action = "get" }
  )) {
    try {
      $resp = Invoke-AirControlRequest -Context $Context -Request $request -TimeoutMs 4000
      if ($null -ne $resp.Final) {
        Write-ControlLog -Context $Context -Source "FORENSIC" -Text (
          "{0}/{1} final={2}/{3}" -f
          $request.category,
          $request.action,
          $resp.Final.status,
          $resp.Final.code
        )
      } elseif ($null -ne $resp.Immediate) {
        Write-ControlLog -Context $Context -Source "FORENSIC" -Text (
          "{0}/{1} immediate={2}/{3}" -f
          $request.category,
          $request.action,
          $resp.Immediate.status,
          $resp.Immediate.code
        )
      } else {
        Write-ControlLog -Context $Context -Source "FORENSIC" -Text (
          "{0}/{1} no_response" -f $request.category, $request.action
        )
      }
    } catch {
      Write-ControlLog -Context $Context -Source "FORENSIC" -Text (
        "{0}/{1} error={2}" -f
        $request.category,
        $request.action,
        $_.Exception.Message
      )
    }
  }

  try {
    [void](Invoke-TextCommand -Port $TeensyPort -Command "showsource" -MatchRegex "SPI PERF" -TimeoutMs 4000 -LogContext $Context)
  } catch {
    Write-ControlLog -Context $Context -Source "FORENSIC" -Text ("showsource error={0}" -f $_.Exception.Message)
  }
}

function Invoke-ReplayBenchCase {
  param(
    [hashtable]$AirContext,
    [System.IO.Ports.SerialPort]$TeensyPort,
    [string]$Label,
    [bool]$ControlAroundRun,
    [int]$CaseId
  )

  $caseStart = Get-Date
  $deadlineMs = $DurationMs + 12000
  Write-ControlLog -Context $AirContext -Source "CASE" -Text (
    "START label={0} case_id={1} duration_ms={2} deadline_ms={3}" -f
    $Label,
    $CaseId,
    $DurationMs,
    $deadlineMs
  )
  if ($ControlAroundRun) {
    foreach ($request in @(
      @{ category = "state"; action = "get" },
      @{ category = "recording"; action = "status" },
      @{ category = "replay"; action = "status" },
      @{ category = "file"; action = "storage_status" },
      @{ category = "fusion"; action = "get" }
    )) {
      $resp = Invoke-AirControlRequest -Context $AirContext -Request $request -TimeoutMs 3000
      Assert-ControlCompletedOk -Response $resp -Label ("perf pre {0}/{1}" -f $request.category, $request.action)
    }
  }

  [void](Invoke-TextCommand -Port $TeensyPort -Command "resetloopperf" -MatchRegex "loop perf reset" -TimeoutMs 3000 -LogContext $AirContext)
  Invoke-PreCaseDrain -Context $AirContext -Label $Label -CaseId $CaseId

  $benchCommand = "tapi replaybench $DurationMs $BatchHz $BatchRecords"
  Write-ControlLog -Context $AirContext -Source "CMD" -Text ("case_id={0} {1}" -f $CaseId, $benchCommand)
  $scanIndex = $AirContext.Lines.Count
  $AirContext.Port.WriteLine($benchCommand)

  $deadline = $caseStart.AddMilliseconds($deadlineMs)
  $benchBuffer = New-Object System.Text.StringBuilder
  $jsonCount = 0
  if ($ControlAroundRun) {
    $jsonCount += 5
  }

  while ((Get-Date) -lt $deadline) {
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
    $elapsedMs = [int]((Get-Date) - $caseStart).TotalMilliseconds
    Write-ReplayBenchBuffer -Context $AirContext -CaseId $CaseId -Buffer $benchBuffer
    Invoke-PerfTimeoutForensics -Context $AirContext -TeensyPort $TeensyPort -Label $Label -CaseId $CaseId
    Write-ControlLog -Context $AirContext -Source "CASE" -Text (
      "END label={0} case_id={1} elapsed_ms={2} status=timeout" -f
      $Label,
      $CaseId,
      $elapsedMs
    )
    throw ("{0}: timed out waiting for TAPI REPLAYBENCH RESULT" -f $Label)
  }

  # Sample Teensy perf immediately after replaybench completes so the counters
  # represent the benchmark window, not post-benchmark live standalone traffic.
  $teensyResponse = Invoke-TextCommand -Port $TeensyPort -Command "showsource" -MatchRegex "SPI PERF" -TimeoutMs 4000 -LogContext $AirContext

  if ($ControlAroundRun) {
    foreach ($request in @(
      @{ category = "state"; action = "get" },
      @{ category = "recording"; action = "status" },
      @{ category = "replay"; action = "status" },
      @{ category = "file"; action = "storage_status" },
      @{ category = "fusion"; action = "get" }
    )) {
      $resp = Invoke-AirControlRequest -Context $AirContext -Request $request -TimeoutMs 3000
      Assert-ControlCompletedOk -Response $resp -Label ("perf post {0}/{1}" -f $request.category, $request.action)
    }
    $jsonCount += 5
  }

  $airPerf = Parse-AirReplayBench -Response $benchBuffer.ToString()
  $teensyPerf = Parse-TeensyPerf -Response $teensyResponse
  if ($null -eq $airPerf) { throw ("{0}: unable to parse AIR replay benchmark output" -f $Label) }
  if ($null -eq $teensyPerf) { throw ("{0}: unable to parse Teensy perf output" -f $Label) }

  $elapsedMs = [int]((Get-Date) - $caseStart).TotalMilliseconds
  Write-ControlLog -Context $AirContext -Source "CASE" -Text (
    "END label={0} case_id={1} elapsed_ms={2} status=ok" -f
    $Label,
    $CaseId,
    $elapsedMs
  )

  return [pscustomobject]@{
    label = $Label
    case_id = $CaseId
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

  $baseline = Invoke-ReplayBenchCase -AirContext $airContext -TeensyPort $teensyPort -Label "post_integration_baseline" -ControlAroundRun:$false -CaseId 1
  $withControl = Invoke-ReplayBenchCase -AirContext $airContext -TeensyPort $teensyPort -Label "with_serial_control_activity" -ControlAroundRun:$true -CaseId 2

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
