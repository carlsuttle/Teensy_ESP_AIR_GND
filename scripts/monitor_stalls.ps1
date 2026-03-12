param(
  [string]$AirPort = "",
  [string]$GndPort = "",
  [string]$TeensyPort = "",
  [switch]$TeensyStats,
  [string]$OutDir = ".\\soak_logs",
  [string]$GndBaseUrl = "http://192.168.4.1",
  [switch]$StateOnly,
  [string[]]$RateProfile = @(),
  [int]$ProfileSettleSec = 10,
  [int]$ProfileRunSec = 60,
  [int]$StatusPollMs = 1000,
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

if (-not $AirPort -and -not $GndPort -and -not $TeensyPort -and $RateProfile.Count -eq 0) {
  throw "Specify at least one of -AirPort, -GndPort, -TeensyPort, or -RateProfile."
}

$sessionStart = Get-Date
$sessionDir = Join-Path $OutDir ($sessionStart.ToString("yyyyMMdd-HHmmss"))
New-Item -ItemType Directory -Force -Path $sessionDir | Out-Null

$script:EventPath = Join-Path $sessionDir "events.csv"
$script:EventWriter = [System.IO.StreamWriter]::new($script:EventPath, $false, [System.Text.Encoding]::ASCII)
$script:EventWriter.AutoFlush = $true
$script:EventWriter.WriteLine("ts,level,unit,type,message")
$script:StatusPath = Join-Path $sessionDir "status.csv"
$script:StatusWriter = $null
$script:ProfileSummaryPath = Join-Path $sessionDir "profiles.csv"
$script:ProfileSummaryWriter = $null
$script:Profiles = @()
$script:CurrentProfileIndex = -1
$script:CurrentProfile = $null
$script:CurrentProfileAppliedAt = $null
$script:CurrentProfileSummary = $null
$script:NextStatusPollAt = $null
$script:ProfilesFinished = $false
$script:RestoreConfig = $null

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

function Parse-RateProfileText {
  param([string]$Text)

  if ([string]::IsNullOrWhiteSpace($Text)) {
    throw "Rate profile text is empty."
  }

  if ($Text -notmatch '^\s*(\d+)\s*/\s*(\d+)\s*$') {
    throw "Invalid -RateProfile '$Text'. Use source/ui format such as 100/20."
  }

  $sourceHz = [int]$matches[1]
  $uiHz = [int]$matches[2]
  if ($sourceHz -lt 1 -or $sourceHz -gt 400) {
    throw "source_rate_hz out of range in '$Text'. Expected 1..400."
  }
  if ($uiHz -lt 1 -or $uiHz -gt 30) {
    throw "ui_rate_hz out of range in '$Text'. Expected 1..30."
  }

  return [ordered]@{
    Name = ('{0}/{1}' -f $sourceHz, $uiHz)
    SourceHz = $sourceHz
    UiHz = $uiHz
  }
}

function Init-RateProfiles {
  if ($RateProfile.Count -eq 0) {
    return
  }

  foreach ($entry in $RateProfile) {
    foreach ($piece in ($entry -split ',')) {
      $trimmed = $piece.Trim()
      if ([string]::IsNullOrWhiteSpace($trimmed)) {
        continue
      }
      $script:Profiles += Parse-RateProfileText $trimmed
    }
  }

  $script:StatusWriter = [System.IO.StreamWriter]::new($script:StatusPath, $false, [System.Text.Encoding]::ASCII)
  $script:StatusWriter.AutoFlush = $true
  $script:StatusWriter.WriteLine(
    "ts,profile,phase,source_hz,ui_hz,air_link_fresh,seq,link_rx,state_packets,state_seq_gap,state_seq_rewind,ok,len_err,unknown_msg,drop,radio_rtt_ms,radio_rtt_avg_ms,air_rssi_dbm,air_link_age_ms,ap_clients"
  )
  $script:ProfileSummaryWriter = [System.IO.StreamWriter]::new($script:ProfileSummaryPath, $false, [System.Text.Encoding]::ASCII)
  $script:ProfileSummaryWriter.AutoFlush = $true
  $script:ProfileSummaryWriter.WriteLine(
    "profile,source_hz,ui_hz,samples,duration_s,air_link_fresh_pct,state_rx_delta,state_gap_delta,state_rewind_delta,len_err_delta,unknown_msg_delta,drop_delta,radio_rtt_avg_ms,radio_rtt_max_ms,air_rssi_avg_dbm,air_rssi_min_dbm,effective_state_hz"
  )
  $script:NextStatusPollAt = Get-Date
}

function New-ProfileSummary {
  param([System.Collections.Specialized.OrderedDictionary]$Profile)

  return [ordered]@{
    Name = $Profile.Name
    SourceHz = $Profile.SourceHz
    UiHz = $Profile.UiHz
    Samples = 0
    FreshSamples = 0
    FirstAt = $null
    LastAt = $null
    FirstStatePackets = $null
    LastStatePackets = $null
    FirstStateGap = $null
    LastStateGap = $null
    FirstStateRewind = $null
    LastStateRewind = $null
    FirstLenErr = $null
    LastLenErr = $null
    FirstUnknownMsg = $null
    LastUnknownMsg = $null
    FirstDrop = $null
    LastDrop = $null
    RadioRttTotal = 0.0
    RadioRttSamples = 0
    RadioRttMax = 0.0
    AirRssiTotal = 0.0
    AirRssiSamples = 0
    AirRssiMin = $null
  }
}

function Delta-Counter {
  param(
    [object]$First,
    [object]$Last
  )

  if ($null -eq $First -or $null -eq $Last) {
    return 0
  }

  $firstValue = [double]$First
  $lastValue = [double]$Last
  if ($lastValue -ge $firstValue) {
    return [int64]($lastValue - $firstValue)
  }
  return [int64]$lastValue
}

function Update-ProfileSummary {
  param(
    [object]$Status,
    [datetime]$Timestamp
  )

  if (-not $script:CurrentProfile) {
    return
  }
  if (-not $script:CurrentProfileSummary -or $script:CurrentProfileSummary.Name -ne $script:CurrentProfile.Name) {
    $script:CurrentProfileSummary = New-ProfileSummary $script:CurrentProfile
  }

  $summary = $script:CurrentProfileSummary
  if (-not $summary.FirstAt) {
    $summary.FirstAt = $Timestamp
    $summary.FirstStatePackets = ($Status.state_packets -as [int64])
    $summary.FirstStateGap = ($Status.state_seq_gap -as [int64])
    $summary.FirstStateRewind = ($Status.state_seq_rewind -as [int64])
    $summary.FirstLenErr = ($Status.len_err -as [int64])
    $summary.FirstUnknownMsg = ($Status.unknown_msg -as [int64])
    $summary.FirstDrop = ($Status.drop -as [int64])
  }

  $summary.Samples++
  if ($Status.air_link_fresh) {
    $summary.FreshSamples++
  }
  $summary.LastAt = $Timestamp
  $summary.LastStatePackets = ($Status.state_packets -as [int64])
  $summary.LastStateGap = ($Status.state_seq_gap -as [int64])
  $summary.LastStateRewind = ($Status.state_seq_rewind -as [int64])
  $summary.LastLenErr = ($Status.len_err -as [int64])
  $summary.LastUnknownMsg = ($Status.unknown_msg -as [int64])
  $summary.LastDrop = ($Status.drop -as [int64])

  $radioRttMs = ($Status.radio_rtt_ms -as [double])
  if ($radioRttMs -gt 0) {
    $summary.RadioRttTotal += $radioRttMs
    $summary.RadioRttSamples++
    if ($radioRttMs -gt $summary.RadioRttMax) {
      $summary.RadioRttMax = $radioRttMs
    }
  }

  if ($Status.air_rssi_valid) {
    $airRssiDbm = ($Status.air_rssi_dbm -as [double])
    $summary.AirRssiTotal += $airRssiDbm
    $summary.AirRssiSamples++
    if ($null -eq $summary.AirRssiMin -or $airRssiDbm -lt $summary.AirRssiMin) {
      $summary.AirRssiMin = $airRssiDbm
    }
  }
}

function Write-ProfileSummary {
  if (-not $script:ProfileSummaryWriter -or -not $script:CurrentProfileSummary) {
    return
  }

  $summary = $script:CurrentProfileSummary
  $durationSec = 0.0
  if ($summary.FirstAt -and $summary.LastAt) {
    $durationSec = ($summary.LastAt - $summary.FirstAt).TotalSeconds
  }
  $freshPct = if ($summary.Samples -gt 0) { [math]::Round((100.0 * $summary.FreshSamples) / $summary.Samples, 1) } else { 0.0 }
  $stateRxDelta = Delta-Counter $summary.FirstStatePackets $summary.LastStatePackets
  $stateGapDelta = Delta-Counter $summary.FirstStateGap $summary.LastStateGap
  $stateRewindDelta = Delta-Counter $summary.FirstStateRewind $summary.LastStateRewind
  $lenErrDelta = Delta-Counter $summary.FirstLenErr $summary.LastLenErr
  $unknownMsgDelta = Delta-Counter $summary.FirstUnknownMsg $summary.LastUnknownMsg
  $dropDelta = Delta-Counter $summary.FirstDrop $summary.LastDrop
  $radioRttAvgMs = if ($summary.RadioRttSamples -gt 0) {
    [math]::Round($summary.RadioRttTotal / $summary.RadioRttSamples, 2)
  } else {
    ""
  }
  $radioRttMaxMs = if ($summary.RadioRttSamples -gt 0) {
    [math]::Round($summary.RadioRttMax, 2)
  } else {
    ""
  }
  $airRssiAvgDbm = if ($summary.AirRssiSamples -gt 0) {
    [math]::Round($summary.AirRssiTotal / $summary.AirRssiSamples, 1)
  } else {
    ""
  }
  $airRssiMinDbm = if ($summary.AirRssiSamples -gt 0) {
    [math]::Round([double]$summary.AirRssiMin, 1)
  } else {
    ""
  }
  $effectiveStateHz = if ($durationSec -gt 0) {
    [math]::Round($stateRxDelta / $durationSec, 2)
  } else {
    0.0
  }

  $script:ProfileSummaryWriter.WriteLine(
    ('{0},{1},{2},{3},{4},{5},{6},{7},{8},{9},{10},{11},{12},{13},{14},{15},{16}' -f
      $summary.Name,
      $summary.SourceHz,
      $summary.UiHz,
      $summary.Samples,
      ([math]::Round($durationSec, 3)),
      $freshPct,
      $stateRxDelta,
      $stateGapDelta,
      $stateRewindDelta,
      $lenErrDelta,
      $unknownMsgDelta,
      $dropDelta,
      $radioRttAvgMs,
      $radioRttMaxMs,
      $airRssiAvgDbm,
      $airRssiMinDbm,
      $effectiveStateHz
    )
  )
  Write-EventLog "INFO" "MON" "profile_summary" (
    "profile={0} rx_hz={1} gap={2} rewind={3} rtt_avg={4} rssi_avg={5}" -f
      $summary.Name, $effectiveStateHz, $stateGapDelta, $stateRewindDelta, $radioRttAvgMs, $airRssiAvgDbm
  )
  $script:CurrentProfileSummary = $null
}

function Invoke-GndRequest {
  param(
    [string]$Method,
    [string]$Path,
    [object]$Body = $null
  )

  $uri = $GndBaseUrl.TrimEnd("/") + $Path
  try {
    if ($null -eq $Body) {
      return Invoke-RestMethod -Method $Method -Uri $uri -TimeoutSec 4
    }
    $json = $Body | ConvertTo-Json -Compress
    return Invoke-RestMethod -Method $Method -Uri $uri -Body $json -ContentType "application/json" -TimeoutSec 4
  } catch {
    Write-EventLog "WARN" "MON" "http_error" ("{0} {1}: {2}" -f $Method, $uri, $_.Exception.Message)
    return $null
  }
}

function Apply-RateProfile {
  param([System.Collections.Specialized.OrderedDictionary]$Profile)

  $body = @{
    source_rate_hz = $Profile.SourceHz
    download_rate_hz = $Profile.UiHz
    ui_rate_hz = $Profile.UiHz
    radio_state_only = [bool]$StateOnly
  }

  $null = Invoke-GndRequest "POST" "/api/config" $body
  $config = Invoke-GndRequest "GET" "/api/config"
  if (-not $config) {
    return $false
  }
  if (($config.source_rate_hz -as [int]) -ne $Profile.SourceHz -or
      ($config.download_rate_hz -as [int]) -ne $Profile.UiHz -or
      ($config.ui_rate_hz -as [int]) -ne $Profile.UiHz -or
      [bool]$config.radio_state_only -ne [bool]$StateOnly) {
    Write-EventLog "WARN" "MON" "profile_mismatch" (
      "requested={0} state_only={1} confirmed={2}/{3}/{4} state_only={5}" -f
        $Profile.Name, [int][bool]$StateOnly, $config.source_rate_hz, $config.download_rate_hz, $config.ui_rate_hz,
        [int][bool]$config.radio_state_only
    )
    return $false
  }

  $null = Invoke-GndRequest "POST" "/api/reset_counters"
  $script:CurrentProfileSummary = New-ProfileSummary $Profile
  Write-EventLog "INFO" "MON" "profile_apply" ("applied {0} state_only={1}" -f $Profile.Name, [int][bool]$StateOnly)
  return $true
}

function Poll-GndStatus {
  if (-not $script:StatusWriter -or -not $script:CurrentProfile) {
    return
  }

  if (-not $script:NextStatusPollAt -or (Get-Date) -lt $script:NextStatusPollAt) {
    return
  }

  $script:NextStatusPollAt = (Get-Date).AddMilliseconds($StatusPollMs)
  $timestamp = Get-Date
  $status = Invoke-GndRequest "GET" "/api/status"
  if (-not $status) {
    return
  }
  $phase = "settle"
  if ($script:CurrentProfileAppliedAt -and ($timestamp - $script:CurrentProfileAppliedAt).TotalSeconds -ge $ProfileSettleSec) {
    $phase = "run"
  }

  $script:StatusWriter.WriteLine(
    ('{0},{1},{2},{3},{4},{5},{6},{7},{8},{9},{10},{11},{12},{13},{14},{15},{16},{17},{18},{19}' -f
      $timestamp.ToString("o"),
      $script:CurrentProfile.Name,
      $phase,
      $script:CurrentProfile.SourceHz,
      $script:CurrentProfile.UiHz,
      ([int][bool]$status.air_link_fresh),
      ($status.seq -as [int64]),
      ($status.link_rx -as [int64]),
      ($status.state_packets -as [int64]),
      ($status.state_seq_gap -as [int64]),
      ($status.state_seq_rewind -as [int64]),
      ($status.ok -as [int64]),
      ($status.len_err -as [int64]),
      ($status.unknown_msg -as [int64]),
      ($status.drop -as [int64]),
      ($status.radio_rtt_ms -as [int64]),
      ($status.radio_rtt_avg_ms -as [int64]),
      ($status.air_rssi_dbm -as [int64]),
      ($status.air_link_age_ms -as [int64]),
      ($status.ap_clients -as [int64])
    )
  )
  if ($phase -eq "run") {
    Update-ProfileSummary $status $timestamp
  }
}

function Advance-RateProfiles {
  if ($script:Profiles.Count -eq 0 -or $script:ProfilesFinished) {
    return
  }

  $now = Get-Date
  if (-not $script:CurrentProfile) {
    $script:CurrentProfileIndex = 0
    $script:CurrentProfile = $script:Profiles[$script:CurrentProfileIndex]
    if (Apply-RateProfile $script:CurrentProfile) {
      $script:CurrentProfileAppliedAt = $now
    } else {
      $script:CurrentProfileAppliedAt = $null
    }
    return
  }

  if (-not $script:CurrentProfileAppliedAt) {
    if (Apply-RateProfile $script:CurrentProfile) {
      $script:CurrentProfileAppliedAt = $now
    }
    return
  }

  $elapsedSec = ($now - $script:CurrentProfileAppliedAt).TotalSeconds
  if ($elapsedSec -lt ($ProfileSettleSec + $ProfileRunSec)) {
    return
  }

  Write-ProfileSummary
  $script:CurrentProfileIndex++
  if ($script:CurrentProfileIndex -ge $script:Profiles.Count) {
    $script:ProfilesFinished = $true
    Write-EventLog "INFO" "MON" "profiles_done" "all rate profiles completed"
    return
  }

  $script:CurrentProfile = $script:Profiles[$script:CurrentProfileIndex]
  $script:CurrentProfileAppliedAt = $null
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
      $Unit.WaitingOn = "radio"
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
      if ($trimmed -match '^AIR READY radio ') {
        if (-not $Unit.ReadyWifiAt) {
          $Unit.ReadyWifiAt = Get-Date
          Write-EventLog "INFO" $Unit.Name "ready_radio" $trimmed
        }
        $Unit.WaitingOn = ""
      } elseif ($trimmed -match '^AIR READY gnd_link ') {
        Write-EventLog "INFO" $Unit.Name "ready_gnd_link" $trimmed
        $Unit.WaitingOn = ""
      } elseif ($trimmed -match '^AIR READY teensy_link ') {
        if (-not $Unit.ReadyTeensyLinkAt) {
          $Unit.ReadyTeensyLinkAt = Get-Date
          Write-EventLog "INFO" $Unit.Name "ready_teensy_link" $trimmed
        }
        $Unit.WaitingOn = ""
      } elseif ($trimmed -match '^AIR WAIT gnd_link ') {
        if ($Unit.WaitingOn -ne "gnd_link") {
          Write-EventLog "WARN" $Unit.Name "wait_gnd_link" $trimmed
        }
        $Unit.WaitingOn = "gnd_link"
      } elseif ($trimmed -match '^AIR WAIT radio') {
        if ($Unit.WaitingOn -ne "radio") {
          Write-EventLog "WARN" $Unit.Name "wait_radio" $trimmed
        }
        $Unit.WaitingOn = "radio"
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

  if ($Air.Active -and $Air.BootAt -and -not $Air.ReadyWifiAt) {
    if (($now - $Air.BootAt).TotalSeconds -gt $WifiReadyDeadlineSec) {
      Flag-TimeoutOnce $Air "ready_radio" ("AIR radio not ready within {0}s of AIR boot" -f $WifiReadyDeadlineSec)
    }
  }

  if ($Air.Active -and $Air.ReadyWifiAt -and $Teensy.Active -and $Teensy.ReadyStatsAt -and -not $Air.ReadyTeensyLinkAt) {
    $anchor = Later-Of $Air.ReadyWifiAt $Teensy.ReadyStatsAt
    if ($anchor -and ($now - $anchor).TotalSeconds -gt $LinkReadyDeadlineSec) {
      Flag-TimeoutOnce $Air "ready_teensy_link" ("AIR teensy link not ready within {0}s of AIR radio + Teensy stats" -f $LinkReadyDeadlineSec)
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
      gnd_base_url = $GndBaseUrl
      radio_state_only = [bool]$StateOnly
      rate_profiles = @($script:Profiles | ForEach-Object { $_.Name })
      profile_settle_sec = $ProfileSettleSec
      profile_run_sec = $ProfileRunSec
      status_poll_ms = $StatusPollMs
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
      ready_radio_at = if ($unit.ReadyWifiAt) { $unit.ReadyWifiAt.ToString("o") } else { $null }
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
  Init-RateProfiles
  Write-EventLog "INFO" "MON" "session_start" ("logs={0}" -f $sessionDir)
  if ($script:Profiles.Count -gt 0) {
    $initialConfig = Invoke-GndRequest "GET" "/api/config"
    if ($initialConfig) {
      $script:RestoreConfig = @{
        source_rate_hz = ($initialConfig.source_rate_hz -as [int])
        download_rate_hz = ($initialConfig.download_rate_hz -as [int])
        ui_rate_hz = ($initialConfig.ui_rate_hz -as [int])
        radio_state_only = [bool]$initialConfig.radio_state_only
      }
      Write-EventLog "INFO" "MON" "restore_capture" (
        "captured source={0} download={1} ui={2} state_only={3}" -f
          $script:RestoreConfig.source_rate_hz, $script:RestoreConfig.download_rate_hz, $script:RestoreConfig.ui_rate_hz,
          [int][bool]$script:RestoreConfig.radio_state_only
      )
    }
    Write-EventLog "INFO" "MON" "profiles" ("profiles={0}" -f (($script:Profiles | ForEach-Object { $_.Name }) -join " "))
  }

  while ($true) {
    Advance-RateProfiles
    Poll-GndStatus

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

    if ($script:ProfilesFinished -and $RunSeconds -le 0) {
      break
    }

    Start-Sleep -Milliseconds $LoopSleepMs
  }
} finally {
  try {
    if ($script:RestoreConfig) {
      $null = Invoke-GndRequest "POST" "/api/config" $script:RestoreConfig
      $restoredConfig = Invoke-GndRequest "GET" "/api/config"
      if ($restoredConfig) {
        Write-EventLog "INFO" "MON" "restore_config" (
          "restored source={0} download={1} ui={2} state_only={3}" -f
            $restoredConfig.source_rate_hz, $restoredConfig.download_rate_hz, $restoredConfig.ui_rate_hz,
            [int][bool]$restoredConfig.radio_state_only
        )
      } else {
        Write-EventLog "WARN" "MON" "restore_config" "restore request sent but confirmation failed"
      }
    }
    Write-ProfileSummary
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

  if ($script:StatusWriter) {
    try {
      $script:StatusWriter.Dispose()
    } catch {
    }
  }

  if ($script:ProfileSummaryWriter) {
    try {
      $script:ProfileSummaryWriter.Dispose()
    } catch {
    }
  }
}
