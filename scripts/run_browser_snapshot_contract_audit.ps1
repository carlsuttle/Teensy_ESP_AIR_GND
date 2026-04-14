param(
  [string]$WsUrl = "ws://192.168.4.1/ws",
  [int]$TimeoutMs = 10000,
  [switch]$SendRefresh,
  [string]$LogPath = ""
)

$ErrorActionPreference = "Stop"

function New-Timestamp {
  (Get-Date).ToString("yyyyMMdd_HHmmss")
}

function New-LogTimestamp {
  (Get-Date).ToString("HH:mm:ss.fff")
}

if (-not $LogPath) {
  $LogPath = Join-Path $PSScriptRoot ("browser_snapshot_contract_audit_{0}.log" -f (New-Timestamp))
}

"Browser snapshot contract audit" | Set-Content -Path $LogPath

function Write-LogLine {
  param(
    [string]$Source,
    [string]$Text
  )
  $line = "[{0}] [{1}] {2}" -f (New-LogTimestamp), $Source, $Text
  Write-Host $line
  Add-Content -Path $LogPath -Value $line
}

function Get-UnexpectedKeys {
  param(
    [hashtable]$Object,
    [string[]]$AllowedKeys
  )
  $allowed = [System.Collections.Generic.HashSet[string]]::new([string[]]$AllowedKeys)
  $unexpected = New-Object System.Collections.Generic.List[string]
  foreach ($key in $Object.Keys) {
    if (-not $allowed.Contains([string]$key)) {
      [void]$unexpected.Add([string]$key)
    }
  }
  return @($unexpected)
}

$allowedSnapshotKeys = @(
  "type",
  "schema_id",
  "schema_version",
  "ws_seq",
  "seq",
  "source_t_us",
  "replay_source_seq",
  "replay_source_t_us",
  "fresh",
  "age_ms",
  "radio_rtt_ms",
  "drop",
  "len_err",
  "unknown_msg",
  "state_gap",
  "state_rewind",
  "replay_active",
  "replay_paused",
  "replay_file_open",
  "replay_at_eof",
  "replay_teensy_seen",
  "replay_session_id",
  "replay_records_sent",
  "replay_records_total",
  "replay_last_error",
  "replay_last_command",
  "replay_current_file",
  "recording_active",
  "recording_busy",
  "recording_session_id",
  "recording_bytes_written",
  "gps_itow_ms",
  "gps_fix_type",
  "gps_num_sv",
  "lat_1e7",
  "lon_1e7",
  "hMSL_mm",
  "gSpeed_mms",
  "headMot_1e5deg",
  "hAcc_mm",
  "sAcc_mms",
  "gps_calendar_valid",
  "gps_year",
  "gps_month",
  "gps_day",
  "gps_hour",
  "gps_min",
  "gps_sec",
  "roll_deg",
  "pitch_deg",
  "yaw_deg",
  "mag_heading_deg",
  "baro_temp_c",
  "baro_press_hpa",
  "baro_alt_m",
  "baro_vsi_mps",
  "fusion_gain",
  "fusion_accel_rej",
  "fusion_mag_rej",
  "fusion_recovery_period",
  "flags",
  "raw_present_mask"
)

$allowedHelloKeys = @("type", "snapshot_hz", "controls")
$allowedAckKeys = @("type", "op", "req_id", "ok", "code", "detail")
$allowedFilesKeys = @(
  "type",
  "req_id",
  "ok",
  "refresh_requested",
  "complete",
  "refresh_inflight",
  "revision",
  "total_files",
  "stored_files",
  "page_offset",
  "page_limit",
  "has_prev",
  "has_next",
  "truncated",
  "last_update_ms",
  "files"
)
$allowedFileEntryKeys = @("name", "size_bytes")
$allowedStorageKeys = @(
  "type",
  "req_id",
  "ok",
  "refresh_requested",
  "known",
  "revision",
  "last_update_ms",
  "media_state",
  "mounted",
  "backend_ready",
  "media_present",
  "busy",
  "init_hz",
  "free_bytes",
  "total_bytes",
  "file_count",
  "time_state",
  "time_source",
  "gps_calendar_present",
  "gps_time_valid",
  "system_time_set",
  "system_time_utc_s",
  "time_last_set_age_ms",
  "time_sync_count",
  "record_prefix",
  "next_record_name"
)

$summary = [ordered]@{
  ws_url = $WsUrl
  log_path = $LogPath
  hello_seen = $false
  snapshot_seen = $false
  files_seen = $false
  storage_seen = $false
  ack_seen = $false
  snapshot_unexpected = @()
  files_unexpected = @()
  file_entry_unexpected = @()
  storage_unexpected = @()
  ack_unexpected = @()
  hello_unexpected = @()
  ok = $true
}

$ws = [System.Net.WebSockets.ClientWebSocket]::new()
$cts = [System.Threading.CancellationTokenSource]::new()

try {
  Write-LogLine "INFO" ("connect {0}" -f $WsUrl)
  $ws.ConnectAsync([Uri]$WsUrl, $cts.Token).GetAwaiter().GetResult()
  Write-LogLine "INFO" "socket open"

  if ($SendRefresh) {
    $refresh = '{"type":"control","req_id":1,"category":"file","action":"refresh"}'
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($refresh)
    $seg = [ArraySegment[byte]]::new($bytes)
    $ws.SendAsync($seg, [System.Net.WebSockets.WebSocketMessageType]::Text, $true, $cts.Token).GetAwaiter().GetResult()
    Write-LogLine "TX" $refresh
  }

  $buffer = New-Object byte[] 8192
  $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
  while ([DateTime]::UtcNow -lt $deadline) {
    $segment = [ArraySegment[byte]]::new($buffer)
    $receiveTask = $ws.ReceiveAsync($segment, $cts.Token)
    if (-not $receiveTask.Wait(500)) {
      continue
    }
    $result = $receiveTask.Result
    if ($result.MessageType -eq [System.Net.WebSockets.WebSocketMessageType]::Close) {
      Write-LogLine "INFO" "socket closed by peer"
      break
    }
    $count = $result.Count
    while (-not $result.EndOfMessage) {
      $nextTask = $ws.ReceiveAsync([ArraySegment[byte]]::new($buffer, $count, $buffer.Length - $count), $cts.Token)
      $nextTask.Wait() | Out-Null
      $result = $nextTask.Result
      $count += $result.Count
    }
    $text = [System.Text.Encoding]::UTF8.GetString($buffer, 0, $count)
    Write-LogLine "RX" $text
    $msg = $text | ConvertFrom-Json -AsHashtable
    switch ($msg.type) {
      "hello" {
        $summary.hello_seen = $true
        $summary.hello_unexpected = Get-UnexpectedKeys -Object $msg -AllowedKeys $allowedHelloKeys
      }
      "snapshot" {
        $summary.snapshot_seen = $true
        $summary.snapshot_unexpected = Get-UnexpectedKeys -Object $msg -AllowedKeys $allowedSnapshotKeys
      }
      "files" {
        $summary.files_seen = $true
        $summary.files_unexpected = Get-UnexpectedKeys -Object $msg -AllowedKeys $allowedFilesKeys
        $entryUnexpected = New-Object System.Collections.Generic.List[string]
        foreach ($entry in @($msg.files)) {
          foreach ($key in (Get-UnexpectedKeys -Object $entry -AllowedKeys $allowedFileEntryKeys)) {
            if (-not $entryUnexpected.Contains($key)) {
              [void]$entryUnexpected.Add($key)
            }
          }
        }
        $summary.file_entry_unexpected = @($entryUnexpected)
      }
      "storage" {
        $summary.storage_seen = $true
        $summary.storage_unexpected = Get-UnexpectedKeys -Object $msg -AllowedKeys $allowedStorageKeys
      }
      "ack" {
        $summary.ack_seen = $true
        $summary.ack_unexpected = Get-UnexpectedKeys -Object $msg -AllowedKeys $allowedAckKeys
      }
    }
    if ($summary.hello_seen -and $summary.files_seen -and $summary.storage_seen -and (($summary.snapshot_seen) -or (([DateTime]::UtcNow.AddMilliseconds(1000)) -ge $deadline))) {
      if ($summary.snapshot_seen) { break }
    }
  }

  if (-not $summary.hello_seen) {
    $summary.ok = $false
    Write-LogLine "FAIL" "hello not observed"
  }
  if (-not $summary.files_seen) {
    $summary.ok = $false
    Write-LogLine "FAIL" "files not observed"
  }
  if (-not $summary.storage_seen) {
    $summary.ok = $false
    Write-LogLine "FAIL" "storage not observed"
  }
  if (-not $summary.snapshot_seen) {
    Write-LogLine "WARN" "snapshot not observed during audit window"
  }

  foreach ($bucket in @(
      @{ Name = "hello"; Value = $summary.hello_unexpected },
      @{ Name = "snapshot"; Value = $summary.snapshot_unexpected },
      @{ Name = "files"; Value = $summary.files_unexpected },
      @{ Name = "files.entry"; Value = $summary.file_entry_unexpected },
      @{ Name = "storage"; Value = $summary.storage_unexpected },
      @{ Name = "ack"; Value = $summary.ack_unexpected })) {
    if ($bucket.Value.Count -gt 0) {
      $summary.ok = $false
      Write-LogLine "FAIL" ("unexpected {0} keys: {1}" -f $bucket.Name, ($bucket.Value -join ","))
    }
  }

  Write-Host ""
  Write-Host ($summary | ConvertTo-Json -Depth 4)
}
finally {
  if ($ws.State -eq [System.Net.WebSockets.WebSocketState]::Open) {
    $ws.CloseAsync([System.Net.WebSockets.WebSocketCloseStatus]::NormalClosure, "done", [Threading.CancellationToken]::None).GetAwaiter().GetResult()
  }
  $ws.Dispose()
  $cts.Dispose()
}
