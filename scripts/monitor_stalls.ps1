param(
  [string]$AirPort = "",
  [string]$GndPort = "",
  [string]$TeensyPort = "",
  [switch]$TeensyStats,
  [string]$OutDir = ".\\soak_logs",
  [int]$Baud = 115200,
  [int]$StatsDelayMs = 2500,
  [int]$StatsRetryMs = 5000,
  [int]$PortRetryMs = 1000,
  [int]$ApReadyDeadlineSec = 15,
  [int]$WifiReadyDeadlineSec = 30,
  [int]$LinkReadyDeadlineSec = 30,
  [int]$StatStallMs = 4000,
  [int]$LoopSleepMs = 100,
  [int]$RunSeconds = 0
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if (-not $AirPort -and -not $GndPort -and -not $TeensyPort) {
  throw "Specify at least one of -AirPort, -GndPort, or -TeensyPort."
}

$sessionStart = Get-Date
$sessionDir = Join-Path $OutDir ($sessionStart.ToString("yyyyMMdd-HHmmss"))
New-Item -ItemType Directory -Force -Path $sessionDir | Out-Null

$script:EventPath = Join-Path $sessionDir "events.csv"
$script:EventWriter = [System.IO.StreamWriter]::new($script:EventPath, $false, [System.Text.Encoding]::ASCII)
$script:EventWriter.AutoFlush = $true
$script:EventWriter.WriteLine("ts,level,unit,type,message")

function Write-EventLog {
  param(
    [string]$Level,
    [string]$Unit,
    [string]$Type,
    [string]$Message
  )

  $ts = Get-Date
  $safeMessage = $Message.Replace('"', '""')
  $script:EventWriter.WriteLine(('{0},{1},{2},{3},"{4}"' -f $ts.ToString("o"), $Level, $Unit, $Type, $safeMessage))
  Write-Host ('[{0}] {1,-5} {2,-6} {3} {4}' -f $ts.ToString("HH:mm:ss.fff"), $Level, $Unit, $Type, $Message)
}

function New-UnitState {
  param(
    [string]$Name,
    [string]$PortName,
    [bool]$SendStats = $true
  )

  $logPath = Join-Path $sessionDir ("{0}.log" -f $Name)
  $writer = [System.IO.StreamWriter]::new($logPath, $false, [System.Text.Encoding]::ASCII)
  $writer.AutoFlush = $true

  return [ordered]@{
    Name = $Name
    PortName = $PortName
    Port = $null
    LogPath = $logPath
    Writer = $writer
    Buffer = ""
    Active = -not [string]::IsNullOrWhiteSpace($PortName)
    MonitorSince = $null
    BootCount = 0
    BootAt = $null
    LastLineAt = $null
    LastStatAt = $null
    LastProgressAt = $null
    LastSeq = $null
    LastTus = $null
    ReadyApAt = $null
    ReadyWifiAt = $null
    ReadyAirLinkAt = $null
    ReadyTeensyLinkAt = $null
    ReadyMirrorAt = $null
    ReadyStatsAt = $null
    WaitingOn = ""
    SendStats = $SendStats
    StatsDueAt = if (-not [string]::IsNullOrWhiteSpace($PortName) -and $SendStats) {
      (Get-Date).AddMilliseconds($StatsDelayMs)
    } else {
      $null
    }
    StatsStreaming = $false
    TimeoutFlags = @{}
    StallOpen = $false
    DisconnectCount = 0
    ReconnectCount = 0
    StartupTimeoutCount = 0
    StallCount = 0
    LastWifiStatus = ""
    NextOpenAttemptAt = if (-not [string]::IsNullOrWhiteSpace($PortName)) { Get-Date } else { $null }
    WaitingForPort = $false
  }
}

function Reset-Readiness {
  param([System.Collections.Specialized.OrderedDictionary]$Unit)

  switch ($Unit.Name) {
    "AIR" {
      $Unit.ReadyWifiAt = $null
      $Unit.ReadyTeensyLinkAt = $null
      $Unit.WaitingOn = "gnd_ap"
    }
    "GND" {
      $Unit.ReadyApAt = $null
      $Unit.ReadyAirLinkAt = $null
      $Unit.WaitingOn = "air_packets"
    }
    "TEENSY" {
      $Unit.ReadyMirrorAt = $null
      $Unit.WaitingOn = if ($Unit.SendStats) { "stats" } else { "mirror_tx" }
    }
  }

  $Unit.LastStatAt = $null
  $Unit.LastProgressAt = $null
  $Unit.LastSeq = $null
  $Unit.LastTus = $null
  $Unit.ReadyStatsAt = $null
  $Unit.StatsStreaming = $false
  $Unit.TimeoutFlags = @{}
  $Unit.StallOpen = $false
}

function Mark-Boot {
  param([System.Collections.Specialized.OrderedDictionary]$Unit)

  $Unit.BootCount++
  $Unit.BootAt = Get-Date
  Reset-Readiness $Unit
  if ($Unit.Port -and $Unit.SendStats) {
    $Unit.StatsDueAt = (Get-Date).AddMilliseconds($StatsDelayMs)
  }
  Write-EventLog "INFO" $Unit.Name "boot" ("boot_count={0}" -f $Unit.BootCount)
}

function Open-UnitPort {
  param([System.Collections.Specialized.OrderedDictionary]$Unit)

  if (-not $Unit.Active) {
    return $false
  }

  if ($Unit.Port) {
    return $true
  }

  if ($Unit.NextOpenAttemptAt -and (Get-Date) -lt $Unit.NextOpenAttemptAt) {
    return $false
  }

  $port = $null
  try {
    $port = [System.IO.Ports.SerialPort]::new($Unit.PortName, $Baud, [System.IO.Ports.Parity]::None, 8, [System.IO.Ports.StopBits]::One)
    $port.NewLine = "`n"
    $port.ReadTimeout = 50
    $port.WriteTimeout = 500
    $port.DtrEnable = $false
    $port.RtsEnable = $false
    $port.Open()
    $Unit.Port = $port
    $Unit.Buffer = ""
    $Unit.MonitorSince = Get-Date
    $Unit.StatsDueAt = if ($Unit.SendStats) { (Get-Date).AddMilliseconds($StatsDelayMs) } else { $null }
    $Unit.NextOpenAttemptAt = $null
    $Unit.WaitingForPort = $false
    Write-EventLog "INFO" $Unit.Name "port_open" ("opened {0}" -f $Unit.PortName)
    return $true
  } catch {
    if ($port) {
      try {
        $port.Dispose()
      } catch {
      }
    }
    if (-not $Unit.WaitingForPort) {
      Write-EventLog "WARN" $Unit.Name "port_wait" ("waiting for {0}: {1}" -f $Unit.PortName, $_.Exception.Message)
      $Unit.WaitingForPort = $true
    }
    $Unit.NextOpenAttemptAt = (Get-Date).AddMilliseconds($PortRetryMs)
    return $false
  }
}

function Detach-UnitPort {
  param([System.Collections.Specialized.OrderedDictionary]$Unit)

  if ($Unit.Port) {
    try {
      $Unit.Port.Close()
    } catch {
    }
    try {
      $Unit.Port.Dispose()
    } catch {
    }
    $Unit.Port = $null
  }
  $Unit.Buffer = ""
  $Unit.StatsStreaming = $false
  $Unit.StatsDueAt = if ($Unit.SendStats) { (Get-Date).AddMilliseconds($StatsDelayMs) } else { $null }
  $Unit.NextOpenAttemptAt = (Get-Date).AddMilliseconds($PortRetryMs)
  $Unit.WaitingForPort = $false
}

function Close-UnitPort {
  param([System.Collections.Specialized.OrderedDictionary]$Unit)

  Detach-UnitPort $Unit
  if ($Unit.Writer) {
    try {
      $Unit.Writer.Dispose()
    } catch {
    }
  }
}

function Handle-PortLoss {
  param(
    [System.Collections.Specialized.OrderedDictionary]$Unit,
    [string]$Context,
    [System.Exception]$Exception
  )

  $message = if ($Exception) { $Exception.Message } else { "port unavailable" }
  Write-EventLog "WARN" $Unit.Name "port_lost" ("{0}: {1}" -f $Context, $message)
  Detach-UnitPort $Unit
}

function Ensure-UnitPort {
  param([System.Collections.Specialized.OrderedDictionary]$Unit)

  if (-not $Unit.Active) {
    return
  }

  if ($Unit.Port) {
    return
  }

  [void](Open-UnitPort $Unit)
}

function Append-RawLine {
  param(
    [System.Collections.Specialized.OrderedDictionary]$Unit,
    [string]$Line
  )

  $Unit.Writer.WriteLine(('{0} {1}' -f (Get-Date).ToString("o"), $Line))
}

function Append-RawChunk {
  param(
    [System.Collections.Specialized.OrderedDictionary]$Unit,
    [string]$Chunk
  )

  if ([string]::IsNullOrEmpty($Chunk)) {
    return
  }

  $Unit.Writer.Write($Chunk)
}

function Later-Of {
  param(
    [Nullable[datetime]]$A,
    [Nullable[datetime]]$B
  )

  if (-not $A) { return $B }
  if (-not $B) { return $A }
  if ($A -gt $B) { return $A }
  return $B
}

function Flag-TimeoutOnce {
  param(
    [System.Collections.Specialized.OrderedDictionary]$Unit,
    [string]$Key,
    [string]$Message
  )

  if ($Unit.TimeoutFlags.ContainsKey($Key)) {
    return
  }
  $Unit.TimeoutFlags[$Key] = $true
  $Unit.StartupTimeoutCount++
  Write-EventLog "ERROR" $Unit.Name "startup_timeout" $Message
}

function Send-StatsIfDue {
  param([System.Collections.Specialized.OrderedDictionary]$Unit)

  if (-not $Unit.SendStats -or -not $Unit.Port -or -not $Unit.StatsDueAt) {
    return
  }

  if ((Get-Date) -lt $Unit.StatsDueAt) {
    return
  }

  try {
    $Unit.Port.Write("stats`r`n")
    Write-EventLog "INFO" $Unit.Name "stats_cmd" "sent stats"
  } catch {
    Handle-PortLoss $Unit "stats_cmd" $_.Exception
    return
  }
  if ($Unit.StatsStreaming) {
    $Unit.StatsDueAt = $null
  } else {
    $Unit.StatsDueAt = (Get-Date).AddMilliseconds($StatsRetryMs)
  }
}

function Update-SeqProgress {
  param(
    [System.Collections.Specialized.OrderedDictionary]$Unit,
    [uint64]$Seq,
    [uint64]$Tus
  )

  $now = Get-Date
  $Unit.LastStatAt = $now

  if (-not $Unit.ReadyStatsAt -and $Seq -gt 0) {
    $Unit.ReadyStatsAt = $now
    $Unit.StatsStreaming = $true
    $Unit.StatsDueAt = $null
    Write-EventLog "INFO" $Unit.Name "first_stat" ("seq={0} t_us={1}" -f $Seq, $Tus)
  }

  if ($Seq -gt 0) {
    if ($null -eq $Unit.LastSeq -or $Seq -gt $Unit.LastSeq -or $Seq -lt $Unit.LastSeq) {
      if ($Unit.StallOpen) {
        $gapMs = [int](($now - $Unit.LastProgressAt).TotalMilliseconds)
        Write-EventLog "INFO" $Unit.Name "stall_recovered" ("progress resumed after {0} ms at seq={1}" -f $gapMs, $Seq)
        $Unit.StallOpen = $false
      }
      $Unit.LastProgressAt = $now
    }
  }

  if ($null -ne $Unit.LastSeq -and $Seq -lt $Unit.LastSeq) {
    Write-EventLog "WARN" $Unit.Name "seq_reset" ("seq reset from {0} to {1}" -f $Unit.LastSeq, $Seq)
  }

  $Unit.LastSeq = $Seq
  $Unit.LastTus = $Tus

  if ($Unit.Name -eq "TEENSY" -and -not $Unit.BootAt) {
    $Unit.BootAt = $now
  }
}

function Handle-WifiStatus {
  param(
    [System.Collections.Specialized.OrderedDictionary]$Unit,
    [string]$Status
  )

  if ($Unit.LastWifiStatus -eq $Status) {
    return
  }

  if ($Unit.LastWifiStatus -eq "CONNECTED" -and $Status -ne "CONNECTED") {
    $Unit.DisconnectCount++
    Write-EventLog "WARN" $Unit.Name "wifi_disconnect" ("status={0}" -f $Status)
  } elseif ($Unit.LastWifiStatus -and $Unit.LastWifiStatus -ne "CONNECTED" -and $Status -eq "CONNECTED") {
    $Unit.ReconnectCount++
    Write-EventLog "INFO" $Unit.Name "wifi_reconnect" "status=CONNECTED"
  } else {
    Write-EventLog "INFO" $Unit.Name "wifi_status" ("status={0}" -f $Status)
  }

  $Unit.LastWifiStatus = $Status
}

function Process-Line {
  param(
    [System.Collections.Specialized.OrderedDictionary]$Unit,
    [string]$Line
  )

  $trimmed = $Line.Trim()
  if ([string]::IsNullOrWhiteSpace($trimmed)) {
    return
  }

  $Unit.LastLineAt = Get-Date
  Append-RawLine $Unit $trimmed

  switch ($Unit.Name) {
    "AIR" {
      if ($trimmed -eq "ESP_AIR boot") {
        Mark-Boot $Unit
        return
      }
      if ($trimmed -match '^AIR READY wifi ') {
        if (-not $Unit.ReadyWifiAt) {
          $Unit.ReadyWifiAt = Get-Date
          Write-EventLog "INFO" $Unit.Name "ready_wifi" $trimmed
        }
        $Unit.WaitingOn = ""
      } elseif ($trimmed -match '^AIR READY teensy_link ') {
        if (-not $Unit.ReadyTeensyLinkAt) {
          $Unit.ReadyTeensyLinkAt = Get-Date
          Write-EventLog "INFO" $Unit.Name "ready_teensy_link" $trimmed
        }
        $Unit.WaitingOn = ""
      } elseif ($trimmed -match '^AIR WAIT gnd_ap ') {
        if ($Unit.WaitingOn -ne "gnd_ap") {
          Write-EventLog "WARN" $Unit.Name "wait_gnd_ap" $trimmed
        }
        $Unit.WaitingOn = "gnd_ap"
      } elseif ($trimmed -match '^AIR WAIT teensy telemetry') {
        if ($Unit.WaitingOn -ne "teensy") {
          Write-EventLog "WARN" $Unit.Name "wait_teensy" $trimmed
        }
        $Unit.WaitingOn = "teensy"
      } elseif ($trimmed -match '^Wi-Fi status=([A-Z_]+)\(\d+\)') {
        Handle-WifiStatus $Unit $matches[1]
      }
    }
    "GND" {
      if ($trimmed -eq "ESP_GND boot") {
        Mark-Boot $Unit
        return
      }
      if ($trimmed -match '^GND READY ap ') {
        if (-not $Unit.ReadyApAt) {
          $Unit.ReadyApAt = Get-Date
          Write-EventLog "INFO" $Unit.Name "ready_ap" $trimmed
        }
        $Unit.WaitingOn = ""
      } elseif ($trimmed -match '^GND READY air_link ') {
        if (-not $Unit.ReadyAirLinkAt) {
          $Unit.ReadyAirLinkAt = Get-Date
          Write-EventLog "INFO" $Unit.Name "ready_air_link" $trimmed
        }
        $Unit.WaitingOn = ""
      } elseif ($trimmed -match '^GND WAIT air_packets') {
        if ($Unit.WaitingOn -ne "air_packets") {
          Write-EventLog "WARN" $Unit.Name "wait_air_packets" $trimmed
        }
        $Unit.WaitingOn = "air_packets"
      }
    }
    "TEENSY" {
      if ($trimmed -eq "FAST Teensy Avionics") {
        Mark-Boot $Unit
        return
      }
      if ($trimmed -match '^TEENSY READY mirror_tx ') {
        if (-not $Unit.ReadyMirrorAt) {
          $Unit.ReadyMirrorAt = Get-Date
          Write-EventLog "INFO" $Unit.Name "ready_mirror_tx" $trimmed
        }
        $Unit.WaitingOn = ""
      } elseif ($trimmed -match 'STATS STREAM START') {
        $Unit.StatsStreaming = $true
        $Unit.StatsDueAt = $null
        Write-EventLog "INFO" $Unit.Name "stats_stream" $trimmed
      }
    }
  }

  if ($trimmed -match '^STAT unit=([A-Z]+) seq=(\d+) t_us=(\d+)') {
    Update-SeqProgress $Unit ([uint64]$matches[2]) ([uint64]$matches[3])
  }
}

function Check-Stall {
  param([System.Collections.Specialized.OrderedDictionary]$Unit)

  if (-not $Unit.Active -or -not $Unit.LastProgressAt) {
    return
  }

  $gapMs = [int]((Get-Date) - $Unit.LastProgressAt).TotalMilliseconds
  if ($gapMs -gt $StatStallMs) {
    if (-not $Unit.StallOpen) {
      $Unit.StallOpen = $true
      $Unit.StallCount++
      Write-EventLog "ERROR" $Unit.Name "stall" ("no seq progress for {0} ms" -f $gapMs)
    }
  }
}

function Check-Deadlines {
  param(
    [System.Collections.Specialized.OrderedDictionary]$Air,
    [System.Collections.Specialized.OrderedDictionary]$Gnd,
    [System.Collections.Specialized.OrderedDictionary]$Teensy
  )

  $now = Get-Date

  if ($Gnd.Active -and $Gnd.BootAt -and -not $Gnd.ReadyApAt) {
    if (($now - $Gnd.BootAt).TotalSeconds -gt $ApReadyDeadlineSec) {
      Flag-TimeoutOnce $Gnd "ready_ap" ("GND AP not ready within {0}s of boot" -f $ApReadyDeadlineSec)
    }
  }

  if ($Air.Active -and $Air.BootAt -and $Gnd.ReadyApAt -and -not $Air.ReadyWifiAt) {
    $anchor = Later-Of $Air.BootAt $Gnd.ReadyApAt
    if ($anchor -and ($now - $anchor).TotalSeconds -gt $WifiReadyDeadlineSec) {
      Flag-TimeoutOnce $Air "ready_wifi" ("AIR wifi not ready within {0}s of AIR boot + GND AP ready" -f $WifiReadyDeadlineSec)
    }
  }

  if ($Air.Active -and $Air.ReadyWifiAt -and $Teensy.Active -and $Teensy.ReadyStatsAt -and -not $Air.ReadyTeensyLinkAt) {
    $anchor = Later-Of $Air.ReadyWifiAt $Teensy.ReadyStatsAt
    if ($anchor -and ($now - $anchor).TotalSeconds -gt $LinkReadyDeadlineSec) {
      Flag-TimeoutOnce $Air "ready_teensy_link" ("AIR teensy link not ready within {0}s of AIR wifi + Teensy stats" -f $LinkReadyDeadlineSec)
    }
  }

  if ($Gnd.Active -and $Gnd.ReadyApAt -and $Air.Active -and $Air.ReadyWifiAt -and -not $Gnd.ReadyAirLinkAt) {
    $anchor = Later-Of $Gnd.ReadyApAt $Air.ReadyWifiAt
    if ($anchor -and ($now - $anchor).TotalSeconds -gt $LinkReadyDeadlineSec) {
      Flag-TimeoutOnce $Gnd "ready_air_link" ("GND air link not ready within {0}s of GND AP + AIR wifi" -f $LinkReadyDeadlineSec)
    }
  }

  if ($Teensy.Active -and $Teensy.MonitorSince -and -not $Teensy.ReadyStatsAt -and -not $Teensy.ReadyMirrorAt) {
    if (($now - $Teensy.MonitorSince).TotalSeconds -gt $LinkReadyDeadlineSec) {
      $mode = if ($Teensy.SendStats) { "mirror/stats" } else { "mirror_tx" }
      Flag-TimeoutOnce $Teensy "ready_mirror" ("TEENSY {0} not seen within {1}s of port open" -f $mode, $LinkReadyDeadlineSec)
    }
  }
}

function Drain-Port {
  param([System.Collections.Specialized.OrderedDictionary]$Unit)

  if (-not $Unit.Port) {
    return
  }

  if ($Unit.Port.BytesToRead -le 0) {
    return
  }

  try {
    $chunk = $Unit.Port.ReadExisting()
  } catch {
    Handle-PortLoss $Unit "read" $_.Exception
    return
  }
  if ([string]::IsNullOrEmpty($chunk)) {
    return
  }

  Append-RawChunk $Unit $chunk
  $Unit.Buffer += $chunk
  $parts = $Unit.Buffer -split "`r`n|`n|`r"
  if ($Unit.Buffer -notmatch "(`r`n|`n|`r)$") {
    $Unit.Buffer = $parts[-1]
    $parts = if ($parts.Length -gt 1) { $parts[0..($parts.Length - 2)] } else { @() }
  } else {
    $Unit.Buffer = ""
  }

  foreach ($line in $parts) {
    Process-Line $Unit $line
  }
}

function Write-Summary {
  param(
    [System.Collections.Specialized.OrderedDictionary]$Air,
    [System.Collections.Specialized.OrderedDictionary]$Gnd,
    [System.Collections.Specialized.OrderedDictionary]$Teensy
  )

  $summary = [ordered]@{
    started_at = $sessionStart.ToString("o")
    ended_at = (Get-Date).ToString("o")
    config = [ordered]@{
      air_port = $Air.PortName
      gnd_port = $Gnd.PortName
      teensy_port = $Teensy.PortName
      baud = $Baud
      stats_delay_ms = $StatsDelayMs
      stats_retry_ms = $StatsRetryMs
      port_retry_ms = $PortRetryMs
      ap_ready_deadline_sec = $ApReadyDeadlineSec
      wifi_ready_deadline_sec = $WifiReadyDeadlineSec
      link_ready_deadline_sec = $LinkReadyDeadlineSec
      stat_stall_ms = $StatStallMs
      teensy_stats = $TeensyStats.IsPresent
    }
    units = @()
  }

  foreach ($unit in @($Air, $Gnd, $Teensy)) {
    if (-not $unit.Active) {
      continue
    }

    $summary.units += [ordered]@{
      unit = $unit.Name
      port = $unit.PortName
      boots = $unit.BootCount
      disconnects = $unit.DisconnectCount
      reconnects = $unit.ReconnectCount
      startup_timeouts = $unit.StartupTimeoutCount
      stalls = $unit.StallCount
      last_seq = $unit.LastSeq
      last_t_us = $unit.LastTus
      ready_ap_at = if ($unit.ReadyApAt) { $unit.ReadyApAt.ToString("o") } else { $null }
      ready_wifi_at = if ($unit.ReadyWifiAt) { $unit.ReadyWifiAt.ToString("o") } else { $null }
      ready_air_link_at = if ($unit.ReadyAirLinkAt) { $unit.ReadyAirLinkAt.ToString("o") } else { $null }
      ready_teensy_link_at = if ($unit.ReadyTeensyLinkAt) { $unit.ReadyTeensyLinkAt.ToString("o") } else { $null }
      ready_mirror_at = if ($unit.ReadyMirrorAt) { $unit.ReadyMirrorAt.ToString("o") } else { $null }
      ready_stats_at = if ($unit.ReadyStatsAt) { $unit.ReadyStatsAt.ToString("o") } else { $null }
      waiting_on = $unit.WaitingOn
      last_wifi_status = $unit.LastWifiStatus
    }
  }

  $summaryPath = Join-Path $sessionDir "summary.json"
  $summary | ConvertTo-Json -Depth 6 | Set-Content -Path $summaryPath -Encoding ASCII
  Write-EventLog "INFO" "MON" "summary" ("wrote {0}" -f $summaryPath)
}

$air = New-UnitState "AIR" $AirPort $true
$gnd = New-UnitState "GND" $GndPort $true
$teensy = New-UnitState "TEENSY" $TeensyPort $TeensyStats.IsPresent

try {
  Write-EventLog "INFO" "MON" "session_start" ("logs={0}" -f $sessionDir)

  while ($true) {
    foreach ($unit in @($air, $gnd, $teensy)) {
      Ensure-UnitPort $unit
      Send-StatsIfDue $unit
      Drain-Port $unit
      Check-Stall $unit
    }

    Check-Deadlines $air $gnd $teensy

    if ($RunSeconds -gt 0) {
      if (((Get-Date) - $sessionStart).TotalSeconds -ge $RunSeconds) {
        break
      }
    }

    Start-Sleep -Milliseconds $LoopSleepMs
  }
} finally {
  try {
    Write-Summary $air $gnd $teensy
  } catch {
    Write-Host ("Failed to write summary: {0}" -f $_.Exception.Message)
  }

  foreach ($unit in @($air, $gnd, $teensy)) {
    Close-UnitPort $unit
  }

  try {
    $script:EventWriter.Dispose()
  } catch {
  }
}
