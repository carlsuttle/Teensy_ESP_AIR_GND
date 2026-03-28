param(
  [string]$AirComPort = "COM7",
  [string]$TeensyComPort = "COM10",
  [int]$Baud = 115200,
  [int]$SweepDurationMs = 5000,
  [int]$SoakDurationMs = 60000,
  [string]$LogPath = "",
  [string]$CsvPath = ""
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

function Ensure-AirBenchMode {
  param($Port)
  [void](Invoke-SerialCommand -Port $Port -Command "bench on" -MatchRegex "BENCH standalone=1" -TimeoutMs 3000)
  [void](Invoke-SerialCommand -Port $Port -Command "quiet on" -MatchRegex "QUIET on" -TimeoutMs 2000)
}

function Parse-AirBenchResult {
  param([string]$Response)
  $pattern = 'TAPI REPLAYBENCH RESULT ok=(\d+) batch_hz=(\d+) batch_records=(\d+) duration_ms=(\d+) elapsed_ms=(\d+) sent=(\d+) recv=(\d+) pass=(\d+) fail=(\d+) timeout=(\d+) validated_rps=([0-9.]+)'
  if ($Response -match $pattern) {
    return @{
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
  return $null
}

function Parse-TeensyPerf {
  param([string]$Response)
  $replayPattern = 'REPLAY PERF .*inputs_per_poll\[avg/max\]=(\d+)\/(\d+) ctrls_per_poll\[avg/max\]=(\d+)\/(\d+) outq_max=(\d+)'
  $spiPattern = 'SPI PERF .*tx_overflows=(\d+) rx_records=(\d+) rx_overflows=(\d+) crc_err=(\d+) type_err=(\d+) state_tx_occ_max=(\d+) state_tx_free_min=(\d+) raw_tx_occ_max=(\d+) raw_tx_free_min=(\d+) replay_rx_occ_max=(\d+) replay_rx_free_min=(\d+)'
  $replay = @{}
  $spi = @{}
  if ($Response -match $replayPattern) {
    $replay = @{
      inputs_avg = [int]$Matches[1]
      inputs_max = [int]$Matches[2]
      ctrls_avg = [int]$Matches[3]
      ctrls_max = [int]$Matches[4]
      outq_max = [int]$Matches[5]
    }
  }
  if ($Response -match $spiPattern) {
    $spi = @{
      tx_overflows = [int]$Matches[1]
      rx_records = [int]$Matches[2]
      rx_overflows = [int]$Matches[3]
      crc_err = [int]$Matches[4]
      type_err = [int]$Matches[5]
      state_tx_occ_max = [int]$Matches[6]
      state_tx_free_min = [int]$Matches[7]
      raw_tx_occ_max = [int]$Matches[8]
      raw_tx_free_min = [int]$Matches[9]
      replay_rx_occ_max = [int]$Matches[10]
      replay_rx_free_min = [int]$Matches[11]
    }
  }
  if ($replay.Count -eq 0 -or $spi.Count -eq 0) {
    return $null
  }
  return @{
    inputs_avg = $replay.inputs_avg
    inputs_max = $replay.inputs_max
    ctrls_avg = $replay.ctrls_avg
    ctrls_max = $replay.ctrls_max
    outq_max = $replay.outq_max
    tx_overflows = $spi.tx_overflows
    rx_overflows = $spi.rx_overflows
    crc_err = $spi.crc_err
    type_err = $spi.type_err
    state_tx_occ_max = $spi.state_tx_occ_max
    state_tx_free_min = $spi.state_tx_free_min
    raw_tx_occ_max = $spi.raw_tx_occ_max
    raw_tx_free_min = $spi.raw_tx_free_min
    replay_rx_occ_max = $spi.replay_rx_occ_max
    replay_rx_free_min = $spi.replay_rx_free_min
  }
}

function Get-CaseState {
  param($Air, $Teensy)
  if ($null -eq $Air -or $null -eq $Teensy) { return "failure" }
  if ($Air.ok -ne 1 -or $Air.fail -ne 0 -or $Air.timeout -ne 0) { return "failure" }
  if ($Teensy.rx_overflows -ne 0 -or $Teensy.crc_err -ne 0 -or $Teensy.type_err -ne 0) { return "failure" }
  if ($Teensy.outq_max -ge 64) { return "failure" }
  if ($Teensy.replay_rx_free_min -le 0) { return "failure" }
  if ($Teensy.state_tx_free_min -le 0) { return "failure" }
  return "pass"
}

function Run-BenchmarkCase {
  param(
    $AirPort,
    $TeensyPort,
    [int]$Index,
    [int]$Total,
    [int]$BatchHz,
    [int]$BatchRecords,
    [int]$DurationMs
  )

  $label = "{0}Hz x {1}" -f $BatchHz, $BatchRecords
  Write-LogLine ("===== CASE {0}/{1} START label={2} =====" -f $Index, $Total, $label)
  [void](Invoke-SerialCommand -Port $TeensyPort -Command "resetloopperf" -MatchRegex "loop perf reset" -TimeoutMs 3000)
  $airResponse = Invoke-SerialCommand -Port $AirPort -Command ("tapi replaybench {0} {1} {2}" -f $DurationMs, $BatchHz, $BatchRecords) -MatchRegex "TAPI REPLAYBENCH RESULT" -TimeoutMs ($DurationMs + 12000)
  $teensyResponse = Invoke-SerialCommand -Port $TeensyPort -Command "showsource" -MatchRegex "SPI PERF" -TimeoutMs 4000
  $air = Parse-AirBenchResult -Response $airResponse
  $teensy = Parse-TeensyPerf -Response $teensyResponse
  $state = Get-CaseState -Air $air -Teensy $teensy

  $row = [ordered]@{
    index = $Index
    batch_hz = $BatchHz
    batch_records = $BatchRecords
    state = $state
    validated_rps = if ($air) { $air.validated_rps } else { 0.0 }
    sent = if ($air) { $air.sent } else { 0 }
    recv = if ($air) { $air.recv } else { 0 }
    pass = if ($air) { $air.pass } else { 0 }
    fail = if ($air) { $air.fail } else { 0 }
    timeout = if ($air) { $air.timeout } else { 0 }
    outq_max = if ($teensy) { $teensy.outq_max } else { 0 }
    replay_rx_occ_max = if ($teensy) { $teensy.replay_rx_occ_max } else { 0 }
    replay_rx_free_min = if ($teensy) { $teensy.replay_rx_free_min } else { 0 }
    state_tx_occ_max = if ($teensy) { $teensy.state_tx_occ_max } else { 0 }
    state_tx_free_min = if ($teensy) { $teensy.state_tx_free_min } else { 0 }
    tx_overflows = if ($teensy) { $teensy.tx_overflows } else { 0 }
    rx_overflows = if ($teensy) { $teensy.rx_overflows } else { 0 }
    inputs_max = if ($teensy) { $teensy.inputs_max } else { 0 }
  }
  $script:Rows.Add([PSCustomObject]$row) | Out-Null
  Write-LogLine ("CASE STATUS index={0} label={1} state={2} validated_rps={3} outq_max={4}/64 replay_rx_occ_max={5}/511 state_tx_occ_max={6}/511 detail=sent:{7} recv:{8} fail:{9} timeout:{10}" -f
    $Index, $label, $state, $row.validated_rps, $row.outq_max, $row.replay_rx_occ_max, $row.state_tx_occ_max, $row.sent, $row.recv, $row.fail, $row.timeout)
  Write-LogLine ("===== CASE {0}/{1} END =====" -f $Index, $Total)
}

function Run-Soak {
  param(
    $AirPort,
    $TeensyPort,
    [string]$Label,
    [int]$BatchHz,
    [int]$BatchRecords,
    [int]$DurationMs
  )
  Write-LogLine ("===== SOAK START label={0} duration_ms={1} =====" -f $Label, $DurationMs)
  [void](Invoke-SerialCommand -Port $TeensyPort -Command "resetloopperf" -MatchRegex "loop perf reset" -TimeoutMs 3000)
  $airResponse = Invoke-SerialCommand -Port $AirPort -Command ("tapi replaybench {0} {1} {2}" -f $DurationMs, $BatchHz, $BatchRecords) -MatchRegex "TAPI REPLAYBENCH RESULT" -TimeoutMs ($DurationMs + 15000)
  $teensyResponse = Invoke-SerialCommand -Port $TeensyPort -Command "showsource" -MatchRegex "SPI PERF" -TimeoutMs 4000
  $air = Parse-AirBenchResult -Response $airResponse
  $teensy = Parse-TeensyPerf -Response $teensyResponse
  $state = Get-CaseState -Air $air -Teensy $teensy
  Write-LogLine ("SOAK STATUS label={0} state={1} validated_rps={2} outq_max={3}/64 replay_rx_occ_max={4}/511 state_tx_occ_max={5}/511 fail={6} timeout={7}" -f
    $Label,
    $state,
    $(if ($air) { $air.validated_rps } else { 0.0 }),
    $(if ($teensy) { $teensy.outq_max } else { 0 }),
    $(if ($teensy) { $teensy.replay_rx_occ_max } else { 0 }),
    $(if ($teensy) { $teensy.state_tx_occ_max } else { 0 }),
    $(if ($air) { $air.fail } else { 0 }),
    $(if ($air) { $air.timeout } else { 0 }))
  Write-LogLine ("===== SOAK END label={0} =====" -f $Label)
  return @{
    state = $state
    air = $air
    teensy = $teensy
  }
}

if ([string]::IsNullOrWhiteSpace($LogPath)) {
  $stamp = (Get-Date).ToString("yyyyMMdd_HHmmss")
  $LogPath = "C:\Users\dell\Platformio\esp32_crsf_telemetry\Teensy_ESP_AIR_GND\scripts\teensy_api_replay_benchmark_$stamp.log"
  if ([string]::IsNullOrWhiteSpace($CsvPath)) {
    $CsvPath = "C:\Users\dell\Platformio\esp32_crsf_telemetry\Teensy_ESP_AIR_GND\scripts\teensy_api_replay_benchmark_$stamp.csv"
  }
}
if ([string]::IsNullOrWhiteSpace($CsvPath)) {
  $CsvPath = [System.IO.Path]::ChangeExtension($LogPath, ".csv")
}

$script:LogFile = $LogPath
$script:Rows = New-Object System.Collections.Generic.List[object]
"Teensy API replay benchmark log" | Set-Content -Path $script:LogFile
Write-LogLine "Logging to $script:LogFile"
Write-LogLine "CSV results to $CsvPath"

$cases = @(
  @{ batch_hz = 100; batch_records = 8 },
  @{ batch_hz = 100; batch_records = 16 },
  @{ batch_hz = 100; batch_records = 24 },
  @{ batch_hz = 100; batch_records = 32 },
  @{ batch_hz = 100; batch_records = 40 },
  @{ batch_hz = 100; batch_records = 48 },
  @{ batch_hz = 50; batch_records = 16 },
  @{ batch_hz = 50; batch_records = 24 },
  @{ batch_hz = 50; batch_records = 32 },
  @{ batch_hz = 50; batch_records = 40 },
  @{ batch_hz = 50; batch_records = 48 }
)

$airPort = $null
$teensyPort = $null
try {
  $airPort = Open-SerialPort -Name $AirComPort -BaudRate $Baud
  $teensyPort = Open-SerialPort -Name $TeensyComPort -BaudRate $Baud
  Write-LogLine ">>> Startup drain AIR"
  $airDrain = Drain-Serial -Port $airPort -DurationMs 3000
  if ($airDrain.Trim()) {
    foreach ($line in ($airDrain -split "`r?`n")) {
      if ($line.Trim()) { Write-LogLine ("AIR " + $line.TrimEnd()) }
    }
  } else {
    Write-LogLine "(AIR startup quiet)"
  }
  Ensure-AirBenchMode -Port $airPort
  Write-LogLine ">>> Startup drain Teensy"
  $teensyDrain = Drain-Serial -Port $teensyPort -DurationMs 3000
  if ($teensyDrain.Trim()) {
    foreach ($line in ($teensyDrain -split "`r?`n")) {
      if ($line.Trim()) { Write-LogLine ("TEENSY " + $line.TrimEnd()) }
    }
  } else {
    Write-LogLine "(Teensy startup quiet)"
  }

  [void](Invoke-SerialCommand -Port $airPort -Command "quiet on" -MatchRegex "QUIET on" -TimeoutMs 2000)
  [void](Invoke-SerialCommand -Port $teensyPort -Command "quiet on" -MatchRegex "QUIET enabled=1|QUIET on" -TimeoutMs 2000)

  $index = 0
  foreach ($case in $cases) {
    $index++
    Run-BenchmarkCase -AirPort $airPort -TeensyPort $teensyPort -Index $index -Total $cases.Count -BatchHz $case.batch_hz -BatchRecords $case.batch_records -DurationMs $SweepDurationMs
    Start-Sleep -Milliseconds 250
  }

  $script:Rows | Export-Csv -NoTypeInformation -Path $CsvPath

  foreach ($targetHz in @(100, 50)) {
    $candidates = $script:Rows | Where-Object { $_.state -eq "pass" -and $_.batch_hz -eq $targetHz } | Sort-Object -Property validated_rps -Descending
    if (-not $candidates) { continue }
    Write-LogLine ("BEST {0}HZ SHORT batch_records={1} validated_rps={2} outq_max={3}/64 replay_rx_occ_max={4}/511 state_tx_occ_max={5}/511" -f
      $targetHz, $candidates[0].batch_records, $candidates[0].validated_rps, $candidates[0].outq_max, $candidates[0].replay_rx_occ_max, $candidates[0].state_tx_occ_max)
    $soakWinner = $null
    foreach ($candidate in $candidates) {
      $soak = Run-Soak -AirPort $airPort -TeensyPort $teensyPort -Label ("{0}Hz x {1}" -f $targetHz, $candidate.batch_records) -BatchHz $targetHz -BatchRecords $candidate.batch_records -DurationMs $SoakDurationMs
      if ($soak.state -eq "pass") {
        $soakWinner = @{ candidate = $candidate; soak = $soak }
        break
      }
    }
    if ($soakWinner) {
      Write-LogLine ("BEST {0}HZ SOAK-PROVEN batch_records={1} validated_rps={2} outq_max={3}/64 replay_rx_occ_max={4}/511 state_tx_occ_max={5}/511" -f
        $targetHz,
        $soakWinner.candidate.batch_records,
        $soakWinner.soak.air.validated_rps,
        $soakWinner.soak.teensy.outq_max,
        $soakWinner.soak.teensy.replay_rx_occ_max,
        $soakWinner.soak.teensy.state_tx_occ_max)
    }
  }
}
finally {
  if ($airPort -and $airPort.IsOpen) { $airPort.Close() }
  if ($teensyPort -and $teensyPort.IsOpen) { $teensyPort.Close() }
}
