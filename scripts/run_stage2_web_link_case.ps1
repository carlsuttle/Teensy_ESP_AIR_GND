param(
  [Parameter(Mandatory = $true)]
  [string]$CaseId,
  [string]$AirComPort = "COM7",
  [string]$TeensyComPort = "COM10",
  [string]$GndComPort = "COM9",
  [int]$Baud = 115200,
  [int]$DurationMs = 60000,
  [int]$BatchHz = 50,
  [int]$BatchRecords = 48,
  [int]$ExpectWsClientsMin = 0,
  [int]$ExpectWsClientsMax = 999,
  [string]$OperatorNote = "",
  [string]$LogPath = "",
  [string]$JsonPath = ""
)

$ErrorActionPreference = "Stop"

function New-Timestamp {
  (Get-Date).ToString("yyyyMMdd_HHmmss")
}

function New-LogTimestamp {
  (Get-Date).ToString("HH:mm:ss.fff")
}

if (-not $LogPath) {
  $stamp = New-Timestamp
  $LogPath = Join-Path $PSScriptRoot ("stage2_web_link_{0}_{1}.log" -f $stamp, $CaseId)
  if (-not $JsonPath) {
    $JsonPath = Join-Path $PSScriptRoot ("stage2_web_link_{0}_{1}.json" -f $stamp, $CaseId)
  }
} elseif (-not $JsonPath) {
  $JsonPath = [System.IO.Path]::ChangeExtension($LogPath, ".json")
}

"Stage 2 web-link case log" | Set-Content -Path $LogPath

function Write-LogLine {
  param(
    [string]$Source,
    [string]$Text
  )
  $line = "[{0}] [{1}] {2}" -f (New-LogTimestamp), $Source, $Text
  Write-Host $line
  Add-Content -Path $LogPath -Value $line
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
  $port.ReadTimeout = 100
  $port.WriteTimeout = 1000
  $port.DtrEnable = $false
  $port.RtsEnable = $false
  $port.Open()
  return $port
}

$ctx = @{
  ports = @{
    AIR = @{
      port = $null
      partial = ""
      lines = New-Object System.Collections.ArrayList
    }
    GND = @{
      port = $null
      partial = ""
      lines = New-Object System.Collections.ArrayList
    }
    TEENSY = @{
      port = $null
      partial = ""
      lines = New-Object System.Collections.ArrayList
    }
  }
  warnings = New-Object System.Collections.ArrayList
  validity = @{
    valid = $true
    reasons = New-Object System.Collections.ArrayList
  }
  phase = "startup"
}

function Add-Warning {
  param([string]$Text)
  [void]$ctx.warnings.Add($Text)
  Write-LogLine "WARN" $Text
}

function Invalidate-Run {
  param([string]$Reason)
  $ctx.validity.valid = $false
  [void]$ctx.validity.reasons.Add($Reason)
  Write-LogLine "INVALID" $Reason
}

function Add-Line {
  param(
    [string]$Name,
    [string]$Line
  )
  if ([string]::IsNullOrWhiteSpace($Line)) { return }
  [void]$ctx.ports[$Name].lines.Add($Line)
  Write-LogLine $Name $Line
  if ($Line -match "WARN air_packets_stale" -and $ctx.phase -eq "benchmark") {
    Invalidate-Run "GND reported stale AIR packets during case"
  }
  if ($Line -match "WARN radio_watchdog restart" -and $ctx.phase -eq "benchmark") {
    Invalidate-Run "AIR radio watchdog restarted during case"
  }
}

function Poll-Ports {
  foreach ($name in @("AIR", "GND", "TEENSY")) {
    $entry = $ctx.ports[$name]
    $chunk = $entry.port.ReadExisting()
    if (-not $chunk) { continue }
    $entry.partial += $chunk
    while ($true) {
      $m = [regex]::Match($entry.partial, "^(.*?)(`r`n|`n|`r)")
      if (-not $m.Success) { break }
      $line = $m.Groups[1].Value.Trim()
      $entry.partial = $entry.partial.Substring($m.Length)
      Add-Line -Name $name -Line $line
    }
  }
}

function Wait-Until {
  param(
    [scriptblock]$Predicate,
    [int]$TimeoutMs
  )
  $deadline = [Environment]::TickCount + $TimeoutMs
  while ([Environment]::TickCount -lt $deadline) {
    Poll-Ports
    if (& $Predicate) { return $true }
    Start-Sleep -Milliseconds 50
  }
  Poll-Ports
  return (& $Predicate)
}

function Send-Command {
  param(
    [string]$Name,
    [string]$Command
  )
  Write-LogLine "CMD/$Name" $Command
  $ctx.ports[$Name].port.WriteLine($Command)
}

function Latest-Match {
  param(
    [string]$Name,
    [string]$Pattern
  )
  $lines = $ctx.ports[$Name].lines
  for ($i = $lines.Count - 1; $i -ge 0; $i--) {
    $line = [string]$lines[$i]
    if ($line -match $Pattern) { return $line }
  }
  return $null
}

function Count-Matches {
  param(
    [string]$Name,
    [string]$Pattern
  )
  $count = 0
  foreach ($line in $ctx.ports[$Name].lines) {
    if ([string]$line -match $Pattern) { $count++ }
  }
  return $count
}

function Parse-AirBenchResult {
  param([string]$Line)
  if (-not $Line) { return $null }
  $pattern = 'TAPI REPLAYBENCH RESULT ok=(\d+) batch_hz=(\d+) batch_records=(\d+) duration_ms=(\d+) elapsed_ms=(\d+) sent=(\d+) recv=(\d+) pass=(\d+) fail=(\d+) timeout=(\d+) validated_rps=([0-9.]+)'
  if ($Line -match $pattern) {
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
  param([string[]]$Lines)
  $replayPattern = 'REPLAY PERF .*inputs_per_poll\[avg/max\]=(\d+)\/(\d+) ctrls_per_poll\[avg/max\]=(\d+)\/(\d+) outq_max=(\d+)'
  $spiPattern = 'SPI PERF .*tx_overflows=(\d+) rx_records=(\d+) rx_overflows=(\d+) crc_err=(\d+) type_err=(\d+) state_tx_occ_max=(\d+) state_tx_free_min=(\d+) raw_tx_occ_max=(\d+) raw_tx_free_min=(\d+) replay_rx_occ_max=(\d+) replay_rx_free_min=(\d+)'
  $replay = $null
  $spi = $null
  foreach ($line in $Lines) {
    if (-not $replay -and $line -match $replayPattern) {
      $replay = @{
        inputs_avg = [int]$Matches[1]
        inputs_max = [int]$Matches[2]
        ctrls_avg = [int]$Matches[3]
        ctrls_max = [int]$Matches[4]
        outq_max = [int]$Matches[5]
      }
    }
    if (-not $spi -and $line -match $spiPattern) {
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
  }
  if (-not $replay -or -not $spi) { return $null }
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

function Parse-AirLog {
  param([string[]]$Lines)
  $out = @{}
  foreach ($line in $Lines) {
    if ($line -match 'AIRLOG enabled=(\d+) active=(\d+) backend_ready=(\d+) media_present=(\d+) session=(\d+) init_hz=(\d+) bytes=(\d+) free=(\d+)') {
      $out.enabled = [int]$Matches[1]
      $out.active = [int]$Matches[2]
      $out.backend_ready = [int]$Matches[3]
      $out.media_present = [int]$Matches[4]
      $out.session = [int64]$Matches[5]
      $out.init_hz = [int64]$Matches[6]
      $out.bytes = [int64]$Matches[7]
      $out.free = [int64]$Matches[8]
    } elseif ($line -match 'AIRLOG queue_cur=(\d+) queue_max=(\d+) enqueued=(\d+) dropped=(\d+) written=(\d+) blocks_written=(\d+) blocks_dropped=(\d+) no_free=(\d+) min_free=(\d+)') {
      $out.queue_cur = [int]$Matches[1]
      $out.queue_max = [int]$Matches[2]
      $out.enqueued = [int64]$Matches[3]
      $out.dropped = [int64]$Matches[4]
      $out.written = [int64]$Matches[5]
      $out.blocks_written = [int64]$Matches[6]
      $out.blocks_dropped = [int64]$Matches[7]
      $out.no_free = [int64]$Matches[8]
      $out.min_free = [int64]$Matches[9]
    }
  }
  return $out
}

function Parse-AirState {
  param([string]$Line)
  if (-not $Line) { return $null }
  if ($Line -match 'AIRSTATE has=(\d+) seq=(\d+) t_us=(\d+) fix=(\d+) sv=(\d+) lat=(-?\d+) lon=(-?\d+) iTOW=(\d+).*last_gps_ms=(\d+).*gps_time=(\d+)-(\d+)-(\d+) (\d+):(\d+):(\d+).*raw_mask=0x([0-9A-Fa-f]+)') {
    return @{
      has = [int]$Matches[1]
      seq = [int64]$Matches[2]
      fix = [int]$Matches[4]
      sv = [int]$Matches[5]
      lat = [int64]$Matches[6]
      lon = [int64]$Matches[7]
      iTOW = [int64]$Matches[8]
      last_gps_ms = [int64]$Matches[9]
      gps_year = [int]$Matches[10]
      gps_month = [int]$Matches[11]
      gps_day = [int]$Matches[12]
      gps_hour = [int]$Matches[13]
      gps_min = [int]$Matches[14]
      gps_sec = [int]$Matches[15]
      raw_mask_hex = $Matches[16]
    }
  }
  return $null
}

function Parse-GndStats {
  param([string[]]$Lines)
  $stats = @()
  foreach ($line in $Lines) {
    if ($line -match 'STAT unit=GND seq=(\d+) t_us=(\d+) has=(\d+) ack=(\d+) cmd=(\d+) ack_ok=(\d+) code=(\d+) rx_bytes=(\d+) ok=(\d+) state_rx=(\d+) state_full=(\d+) state_uni=(\d+) state_gap=(\d+) state_rewind=(\d+) sink_seq=(\d+) sink_t_us=(\d+) sink_rx_ms=(\d+) crc=(\d+) cobs=(\d+) len=(\d+) unk=(\d+) drop=(\d+) link_tx=(\d+) link_rx=(\d+) link_drop=(\d+) rtt=(\d+) ws_clients=(\d+) ws_seq=(\d+) ws_state_seq=(\d+) ws_src_t_us=(\d+) ws_rx_ms=(\d+) ui_tx_ms=(\d+) ui_lat=(\d+)') {
      $stats += [PSCustomObject]@{
        seq = [int64]$Matches[1]
        has = [int]$Matches[3]
        state_gap = [int64]$Matches[13]
        state_rewind = [int64]$Matches[14]
        len = [int64]$Matches[20]
        unk = [int64]$Matches[21]
        drop = [int64]$Matches[22]
        link_rx = [int64]$Matches[24]
        link_drop = [int64]$Matches[25]
        rtt = [int64]$Matches[26]
        ws_clients = [int64]$Matches[27]
        ws_seq = [int64]$Matches[28]
        ws_state_seq = [int64]$Matches[29]
        ws_src_t_us = [int64]$Matches[30]
        ws_rx_ms = [int64]$Matches[31]
        ui_tx_ms = [int64]$Matches[32]
        ui_lat = [int64]$Matches[33]
      }
    }
  }
  return $stats
}

function Parse-AirStats {
  param([string[]]$Lines)
  $stats = @()
  foreach ($line in $Lines) {
    if ($line -match 'STAT unit=AIR seq=(\d+) t_us=(\d+) has=(\d+) ack=(\d+) cmd=(\d+) ack_ok=(\d+) code=(\d+) rx_bytes=(\d+) ok=(\d+) crc=(\d+) cobs=(\d+) len=(\d+) unk=(\d+) drop=(\d+) poll_ms=(\d+) poll_gap_max=(\d+) poll_runs=(\d+) state_drain=(\d+) raw_drain=(\d+) link_tx=(\d+) link_rx=(\d+) link_drop=(\d+) link_state_tx=(\d+) link_unified_tx=(\d+) src_seen=(\d+) src_seen_seq=(\d+) src_seen_t_us=(\d+) src_seen_rx_ms=(\d+) pub_try=(\d+) pub_ok=(\d+) pub_skip_ns=(\d+) pub_skip_np=(\d+) pub_skip_rate=(\d+) pub_skip_old=(\d+) link_last_seq=(\d+) link_last_t_us=(\d+) link_last_tx_ms=(\d+) pub_try_ms=(\d+) pub_age_ms=(\d+) spi_txn=(\d+) spi_fail=(\d+) spi_state=(\d+) spi_last_ms=(\d+) spi_replay=(\d+) spi_crc=(\d+) spi_type=(\d+) spi_rxof=(\d+) spi_txof=(\d+) spi_hdr=([0-9A-Fa-f]+)/(\d+)/(\d+)/(\d+) sdcap=(\d+) sdcap_drop=(\d+) sdcap_qmax=(\d+)') {
      $stats += [PSCustomObject]@{
        seq = [int64]$Matches[1]
        len = [int64]$Matches[12]
        unk = [int64]$Matches[13]
        drop = [int64]$Matches[14]
        link_tx = [int64]$Matches[20]
        link_rx = [int64]$Matches[21]
        link_drop = [int64]$Matches[22]
        link_unified_tx = [int64]$Matches[24]
        pub_ok = [int64]$Matches[30]
        pub_skip_np = [int64]$Matches[32]
        pub_skip_rate = [int64]$Matches[33]
        pub_age_ms = [int64]$Matches[39]
        spi_rxof = [int64]$Matches[47]
        spi_txof = [int64]$Matches[48]
        sdcap = [int64]$Matches[53]
        sdcap_drop = [int64]$Matches[54]
        sdcap_qmax = [int64]$Matches[55]
      }
    }
  }
  return $stats
}

function Get-Max {
  param($Items, [string]$Property)
  if (-not $Items -or $Items.Count -eq 0) { return $null }
  return ($Items | Measure-Object -Property $Property -Maximum).Maximum
}

function Get-Min {
  param($Items, [string]$Property)
  if (-not $Items -or $Items.Count -eq 0) { return $null }
  return ($Items | Measure-Object -Property $Property -Minimum).Minimum
}

function Close-AllPorts {
  foreach ($name in @("AIR", "GND", "TEENSY")) {
    if ($ctx.ports[$name].port) {
      try { $ctx.ports[$name].port.Close() } catch {}
    }
  }
}

try {
  $ctx.ports.AIR.port = Open-SerialPort -Name $AirComPort -BaudRate $Baud
  $ctx.ports.GND.port = Open-SerialPort -Name $GndComPort -BaudRate $Baud
  $ctx.ports.TEENSY.port = Open-SerialPort -Name $TeensyComPort -BaudRate $Baud

  Write-LogLine "INFO" ("CASE {0} start duration_ms={1} batch={2}Hzx{3} expect_ws_clients={4}..{5}" -f
      $CaseId, $DurationMs, $BatchHz, $BatchRecords, $ExpectWsClientsMin, $ExpectWsClientsMax)
  if ($OperatorNote) {
    Write-LogLine "INFO" ("OPERATOR_NOTE {0}" -f $OperatorNote)
  }

  Wait-Until -Predicate { $false } -TimeoutMs 1000 | Out-Null
  foreach ($name in @("AIR", "GND")) {
    Send-Command -Name $name -Command "x"
  }
  Send-Command -Name "AIR" -Command "quiet off"
  Wait-Until -Predicate { (Latest-Match -Name "AIR" -Pattern "QUIET off") -ne $null } -TimeoutMs 2000 | Out-Null

  Send-Command -Name "AIR" -Command "relink"
  Send-Command -Name "GND" -Command "relink"
  $ready = Wait-Until -Predicate { (Latest-Match -Name "GND" -Pattern "GND READY air_link") -ne $null } -TimeoutMs 12000
  if (-not $ready) {
    Add-Warning "GND did not emit a READY line after relink; continuing with stats-based health checks"
  }

  Send-Command -Name "AIR" -Command "airstate"
  Wait-Until -Predicate { (Latest-Match -Name "AIR" -Pattern "^AIRSTATE ") -ne $null } -TimeoutMs 4000 | Out-Null
  $preAirState = Parse-AirState -Line (Latest-Match -Name "AIR" -Pattern "^AIRSTATE ")
  if (-not $preAirState -or $preAirState.has -ne 1) {
    Invalidate-Run "AIR did not report a valid upstream state before soak"
  }

  Send-Command -Name "GND" -Command "stats"
  $haveGndStat = Wait-Until -Predicate { (Latest-Match -Name "GND" -Pattern "^STAT unit=GND ") -ne $null } -TimeoutMs 4000
  if (-not $haveGndStat) {
    Invalidate-Run "GND did not emit a STAT line before soak"
  }

  $preGndStats = @(Parse-GndStats -Lines @((Latest-Match -Name "GND" -Pattern "^STAT unit=GND ")))
  if ($preGndStats.Count -gt 0) {
    $preWsClients = [int]$preGndStats[0].ws_clients
    if ($preWsClients -lt $ExpectWsClientsMin -or $preWsClients -gt $ExpectWsClientsMax) {
      Invalidate-Run ("Preflight ws_clients={0} outside expected range {1}..{2}" -f $preWsClients, $ExpectWsClientsMin, $ExpectWsClientsMax)
    }
  }

  Send-Command -Name "AIR" -Command "stats"
  Wait-Until -Predicate { (Latest-Match -Name "AIR" -Pattern "^STAT unit=AIR ") -ne $null } -TimeoutMs 4000 | Out-Null

  Send-Command -Name "TEENSY" -Command "resetloopperf"
  Wait-Until -Predicate { (Latest-Match -Name "TEENSY" -Pattern "loop perf reset") -ne $null } -TimeoutMs 3000 | Out-Null

  $ctx.phase = "benchmark"
  Send-Command -Name "AIR" -Command ("tapi replaybench {0} {1} {2}" -f $DurationMs, $BatchHz, $BatchRecords)
  $haveReplayResult = Wait-Until -Predicate { (Latest-Match -Name "AIR" -Pattern "^TAPI REPLAYBENCH RESULT ") -ne $null } -TimeoutMs ($DurationMs + 20000)
  if (-not $haveReplayResult) {
    Invalidate-Run "AIR did not emit a replaybench result before timeout"
  }

  $ctx.phase = "post"
  Wait-Until -Predicate { $false } -TimeoutMs 1500 | Out-Null
  Send-Command -Name "AIR" -Command "x"
  Send-Command -Name "GND" -Command "x"

  Send-Command -Name "TEENSY" -Command "showsource"
  Wait-Until -Predicate { (Latest-Match -Name "TEENSY" -Pattern "^SPI PERF ") -ne $null } -TimeoutMs 4000 | Out-Null

  Send-Command -Name "AIR" -Command "airstate"
  Wait-Until -Predicate { (Latest-Match -Name "AIR" -Pattern "^AIRSTATE ") -ne $null } -TimeoutMs 4000 | Out-Null
  Send-Command -Name "AIR" -Command "logstat"
  Wait-Until -Predicate { (Latest-Match -Name "AIR" -Pattern "^AIRLOG queue_cur=") -ne $null } -TimeoutMs 4000 | Out-Null

  $airResult = Parse-AirBenchResult -Line (Latest-Match -Name "AIR" -Pattern "^TAPI REPLAYBENCH RESULT ")
  $teensyPerf = Parse-TeensyPerf -Lines @($ctx.ports.TEENSY.lines)
  $airLog = Parse-AirLog -Lines @($ctx.ports.AIR.lines)
  $postAirState = Parse-AirState -Line (Latest-Match -Name "AIR" -Pattern "^AIRSTATE ")
  $gndStats = @(Parse-GndStats -Lines @($ctx.ports.GND.lines))
  $airStats = @(Parse-AirStats -Lines @($ctx.ports.AIR.lines))

  $caseState = "failure"
  if ($airResult -and $teensyPerf) {
    if ($airResult.ok -eq 1 -and $airResult.fail -eq 0 -and $airResult.timeout -eq 0 -and
        $teensyPerf.rx_overflows -eq 0 -and $teensyPerf.crc_err -eq 0 -and $teensyPerf.type_err -eq 0 -and
        $teensyPerf.outq_max -lt 64 -and $teensyPerf.replay_rx_free_min -gt 0 -and $teensyPerf.state_tx_free_min -gt 0) {
      $caseState = "pass"
    }
  }

  $wsClientsMinSeen = Get-Min -Items $gndStats -Property "ws_clients"
  $wsClientsMaxSeen = Get-Max -Items $gndStats -Property "ws_clients"
  $wsSeqFirst = if ($gndStats.Count -gt 0) { $gndStats[0].ws_seq } else { $null }
  $wsSeqLast = if ($gndStats.Count -gt 0) { $gndStats[$gndStats.Count - 1].ws_seq } else { $null }
  $summary = [ordered]@{
    case_id = $CaseId
    log_path = $LogPath
    json_path = $JsonPath
    operator_note = $OperatorNote
    valid_live_path = [bool]$ctx.validity.valid
    invalid_reasons = @($ctx.validity.reasons)
    replay_case_state = $caseState
    air = $airResult
    teensy = $teensyPerf
    air_state_pre = $preAirState
    air_state_post = $postAirState
    air_log = $airLog
    air_observations = @{
      stale_warning_count = Count-Matches -Name "AIR" -Pattern "WARN radio_watchdog restart"
      link_drop_max = Get-Max -Items $airStats -Property "link_drop"
      len_max = Get-Max -Items $airStats -Property "len"
      unk_max = Get-Max -Items $airStats -Property "unk"
      drop_max = Get-Max -Items $airStats -Property "drop"
      spi_rxof_max = Get-Max -Items $airStats -Property "spi_rxof"
      spi_txof_max = Get-Max -Items $airStats -Property "spi_txof"
      sd_capture_drop_max = Get-Max -Items $airStats -Property "sdcap_drop"
    }
    gnd_observations = @{
      ready_count = Count-Matches -Name "GND" -Pattern "GND READY air_link"
      stale_warning_count = Count-Matches -Name "GND" -Pattern "WARN air_packets_stale"
      drop_max = Get-Max -Items $gndStats -Property "drop"
      len_max = Get-Max -Items $gndStats -Property "len"
      unk_max = Get-Max -Items $gndStats -Property "unk"
      link_drop_max = Get-Max -Items $gndStats -Property "link_drop"
      state_gap_max = Get-Max -Items $gndStats -Property "state_gap"
      state_rewind_max = Get-Max -Items $gndStats -Property "state_rewind"
      ws_clients_min = $wsClientsMinSeen
      ws_clients_max = $wsClientsMaxSeen
      ws_seq_first = $wsSeqFirst
      ws_seq_last = $wsSeqLast
      ws_seq_delta = if ($null -ne $wsSeqFirst -and $null -ne $wsSeqLast) { ($wsSeqLast - $wsSeqFirst) } else { $null }
      ws_state_seq_last = if ($gndStats.Count -gt 0) { $gndStats[$gndStats.Count - 1].ws_state_seq } else { $null }
      telemetry_fresh_warnings = Count-Matches -Name "GND" -Pattern "WARN air_packets_stale"
      relink_count = Count-Matches -Name "GND" -Pattern "^RELINK "
    }
    raw_counts = @{
      air_stat_lines = Count-Matches -Name "AIR" -Pattern "^STAT unit=AIR "
      gnd_stat_lines = Count-Matches -Name "GND" -Pattern "^STAT unit=GND "
      teensy_spi_perf_lines = Count-Matches -Name "TEENSY" -Pattern "^SPI PERF "
    }
    warning_lines = @($ctx.warnings)
  }

  $summary | ConvertTo-Json -Depth 6 | Set-Content -Path $JsonPath
  Write-LogLine "INFO" ("SUMMARY saved json={0}" -f $JsonPath)
  Write-Host ""
  Write-Host ($summary | ConvertTo-Json -Depth 6)
}
finally {
  Close-AllPorts
}
