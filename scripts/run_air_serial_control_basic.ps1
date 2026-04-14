param(
  [string]$AirComPort = "COM7",
  [int]$Baud = 115200,
  [string]$LogPath = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

. "$PSScriptRoot\air_serial_control_common.ps1"

if (-not $LogPath) {
  $LogPath = New-ControlLogPath -Prefix "air_serial_control_basic"
}

$context = $null
$passed = 0
$failed = 0
$replayPauseTestFile = $null
$replayPauseTestRecordsTotal = 0

function Invoke-Check {
  param(
    [string]$Label,
    [scriptblock]$Action
  )

  try {
    & $Action
    $script:passed++
    Write-ControlLog -Context $script:context -Source "CHECK" -Text ("PASS {0}" -f $Label)
  } catch {
    $script:failed++
    Write-ControlLog -Context $script:context -Source "CHECK" -Text ("FAIL {0} :: {1}" -f $Label, $_.Exception.Message)
    throw
  }
}

function Get-ReplayStatus {
  param(
    [hashtable]$Context,
    [int]$TimeoutMs = 3000
  )

  $resp = Invoke-AirControlRequest -Context $Context -Request @{
    category = "replay"
    action = "status"
  } -TimeoutMs $TimeoutMs
  Assert-ControlCompletedOk -Response $resp -Label "replay status"
  return $resp.Final.replay
}

function Wait-ReplayStatus {
  param(
    [hashtable]$Context,
    [scriptblock]$Predicate,
    [int]$TimeoutMs = 6000,
    [int]$PollMs = 150
  )

  $deadline = (Get-Date).AddMilliseconds($TimeoutMs)
  while ((Get-Date) -lt $deadline) {
    $replay = Get-ReplayStatus -Context $Context -TimeoutMs 3000
    if (& $Predicate $replay) {
      return $replay
    }
    Start-Sleep -Milliseconds $PollMs
  }
  throw "Timed out waiting for replay status predicate"
}

try {
  $port = Open-ControlSerialPort -Name $AirComPort -BaudRate $Baud
  $context = New-ControlContext -Port $port -LogPath $LogPath
  Drain-ControlPort -Context $context -DurationMs 800

  Invoke-Check -Label "idle_precheck" -Action {
    $idle = Ensure-AirControlIdle -Context $context
    if ($idle.mode -notin @("idle", "recording_busy", "replay_busy")) {
      throw ("unexpected precheck mode={0}" -f $idle.mode)
    }
  }

  $initialFusion = $null
  Invoke-Check -Label "fusion_get_initial" -Action {
    $resp = Invoke-AirControlRequest -Context $context -Request @{
      category = "fusion"
      action = "get"
    } -TimeoutMs 4000
    Assert-ControlCompletedOk -Response $resp -Label "fusion get initial"
    if (-not $resp.Final.has_fusion_settings) {
      throw "fusion get initial returned no fusion settings payload"
    }
    $script:initialFusion = $resp.Final.fusion
  }

  Invoke-Check -Label "recording_start" -Action {
    $resp = Invoke-AirControlRequest -Context $context -Request @{
      category = "recording"
      action = "start"
    } -TimeoutMs 4000
    Assert-ControlCompletedOk -Response $resp -Label "recording start"
    $state = Wait-AirControlState -Context $context -Predicate {
      param($s)
      return $s.recording_active
    } -TimeoutMs 6000 -PollMs 250
    if (-not $state.sd_ready) {
      throw "recording started but sd_ready is false"
    }
  }

  Start-Sleep -Milliseconds 1200

  Invoke-Check -Label "recording_status" -Action {
    $resp = Invoke-AirControlRequest -Context $context -Request @{
      category = "recording"
      action = "status"
    } -TimeoutMs 3000
    Assert-ControlCompletedOk -Response $resp -Label "recording status"
    if (-not $resp.Final.recording.active) {
      throw "recording status did not report active=true"
    }
  }

  Invoke-Check -Label "recording_stop" -Action {
    $resp = Invoke-AirControlRequest -Context $context -Request @{
      category = "recording"
      action = "stop"
    } -TimeoutMs 4000
    Assert-ControlCompletedOk -Response $resp -Label "recording stop"
    [void](Wait-AirControlState -Context $context -Predicate {
      param($s)
      return (-not $s.recording_active) -and (-not $s.recording_busy)
    } -TimeoutMs 12000 -PollMs 250)
  }

  Invoke-Check -Label "storage_status" -Action {
    $resp = Invoke-AirControlRequest -Context $context -Request @{
      category = "file"
      action = "storage_status"
    } -TimeoutMs 3000
    Assert-ControlCompletedOk -Response $resp -Label "storage status"
    if (-not $resp.Final.storage.backend_ready) {
      throw "storage status backend_ready=false"
    }
  }

  Invoke-Check -Label "file_list_json" -Action {
    $resp = Invoke-AirControlRequest -Context $context -Request @{
      category = "file"
      action = "list_json"
      sort_key = "date"
      sort_dir = "descending"
    } -TimeoutMs 8000
    Assert-ControlCompletedOk -Response $resp -Label "file list json"
    if ($null -eq $resp.Final.files_json) {
      throw "file list json returned no files_json payload"
    }
    if (-not $resp.Final.files_json.truncated) {
      throw "file list json expected truncated=true for temporary capped response"
    }
  }

  Invoke-Check -Label "replay_select_pause_file" -Action {
    $resp = Invoke-AirControlRequest -Context $context -Request @{
      category = "file"
      action = "list_json"
      sort_key = "size"
      sort_dir = "descending"
    } -TimeoutMs 8000
    Assert-ControlCompletedOk -Response $resp -Label "file list json size descending"
    if ($null -eq $resp.Final.files_json -or $null -eq $resp.Final.files_json.files) {
      throw "size-sorted file list returned no files payload"
    }

    $candidate = $null
    foreach ($file in $resp.Final.files_json.files) {
      if ($null -eq $file.name) { continue }
      $sizeBytes = 0
      if ($null -ne $file.size_bytes) {
        $sizeBytes = [int64]$file.size_bytes
      }
      if ($sizeBytes -gt 1024) {
        $candidate = $file
        break
      }
    }

    if ($null -eq $candidate) {
      throw "no replay file larger than 1024 bytes was available for deterministic pause test"
    }

    $script:replayPauseTestFile = [string]$candidate.name
    Write-ControlLog -Context $context -Source "CHECK" -Text (
      "selected replay pause file name={0} size_bytes={1}" -f
      $script:replayPauseTestFile,
      [int64]$candidate.size_bytes
    )
  }

  Invoke-Check -Label "replay_start_file" -Action {
    $resp = Invoke-AirControlRequest -Context $context -Request @{
      category = "replay"
      action = "start_file"
      name = $script:replayPauseTestFile
    } -TimeoutMs 5000
    Assert-ControlCompletedOk -Response $resp -Label "replay start file"
    if (-not $resp.Final.replay.active) {
      throw "replay start file did not report active=true"
    }
    if (-not $resp.Final.replay.file_open) {
      throw "replay start file did not report file_open=true"
    }
  }

  Invoke-Check -Label "replay_wait_active" -Action {
    $replay = Wait-ReplayStatus -Context $context -Predicate {
      param($r)
      return $r.active -and (-not $r.at_eof)
    } -TimeoutMs 5000 -PollMs 100
    if ($replay.paused) {
      throw "replay wait active observed paused=true before pause request"
    }
  }

  Invoke-Check -Label "replay_pause" -Action {
    $resp = Invoke-AirControlRequest -Context $context -Request @{
      category = "replay"
      action = "pause"
    } -TimeoutMs 4000
    Assert-ControlCompletedOk -Response $resp -Label "replay pause"
    if ($resp.Final.replay.active) {
      throw "replay pause should report active=false once paused"
    }
    if (-not $resp.Final.replay.paused) {
      throw "replay pause did not report paused=true"
    }
    if ($resp.Final.replay.at_eof) {
      throw "replay pause unexpectedly reported at_eof=true"
    }
    if (-not $resp.Final.replay.file_open) {
      throw "replay pause did not report file_open=true"
    }
    if ($resp.Final.state.replay_state -eq "idle") {
      throw "replay pause reported idle state despite replay occupancy"
    }
  }

  Invoke-Check -Label "replay_status" -Action {
    $resp = Invoke-AirControlRequest -Context $context -Request @{
      category = "replay"
      action = "status"
    } -TimeoutMs 3000
    Assert-ControlCompletedOk -Response $resp -Label "replay status"
    if ($resp.Final.replay.active) {
      throw "replay status should report active=false while paused"
    }
    if (-not $resp.Final.replay.file_open) {
      throw "replay status did not report file_open=true"
    }
    if (-not $resp.Final.replay.paused) {
      throw "replay status did not report paused=true"
    }
    if ($resp.Final.replay.at_eof) {
      throw "replay status unexpectedly reported at_eof=true while paused"
    }
  }

  Invoke-Check -Label "replay_seek_relative" -Action {
    $resp = Invoke-AirControlRequest -Context $context -Request @{
      category = "replay"
      action = "seek_relative"
      delta_records = 25
    } -TimeoutMs 4000
    Assert-ControlCompletedOk -Response $resp -Label "replay seek relative"
  }

  Invoke-Check -Label "replay_stop" -Action {
    $resp = Invoke-AirControlRequest -Context $context -Request @{
      category = "replay"
      action = "stop"
    } -TimeoutMs 4000
    Assert-ControlCompletedOk -Response $resp -Label "replay stop"
    [void](Wait-AirControlState -Context $context -Predicate {
      param($s)
      return (-not $s.replay_active) -and (-not $s.replay_file_open)
    } -TimeoutMs 10000 -PollMs 250)
  }

  Invoke-Check -Label "replay_start_file_eof_case" -Action {
    $resp = Invoke-AirControlRequest -Context $context -Request @{
      category = "replay"
      action = "start_file"
      name = $script:replayPauseTestFile
    } -TimeoutMs 5000
    Assert-ControlCompletedOk -Response $resp -Label "replay start file eof case"
    $script:replayPauseTestRecordsTotal = [int]$resp.Final.replay.records_total
    if ($script:replayPauseTestRecordsTotal -le 0) {
      throw "replay start file eof case did not report a positive records_total"
    }
    [void](Wait-ReplayStatus -Context $context -Predicate {
      param($r)
      return $r.file_open
    } -TimeoutMs 4000 -PollMs 100)

    $seekResp = Invoke-AirControlRequest -Context $context -Request @{
      category = "replay"
      action = "seek_relative"
      delta_records = $script:replayPauseTestRecordsTotal
    } -TimeoutMs 4000
    Assert-ControlCompletedOk -Response $seekResp -Label "replay seek near eof"
  }

  Invoke-Check -Label "replay_pause_after_eof" -Action {
    [void](Wait-ReplayStatus -Context $context -Predicate {
      param($r)
      return (-not $r.active) -and $r.at_eof
    } -TimeoutMs 8000 -PollMs 100)

    $resp = Invoke-AirControlRequest -Context $context -Request @{
      category = "replay"
      action = "pause"
    } -TimeoutMs 4000
    Assert-ControlAccepted -Response $resp -Label "replay pause after eof"
    if ($null -eq $resp.Final) {
      throw "replay pause after eof: missing final response"
    }
    if ($resp.Final.status -ne "completed_error") {
      throw ("replay pause after eof: expected completed_error, got {0}/{1}" -f $resp.Final.status, $resp.Final.code)
    }
    if ($resp.Final.code -ne "invalid_state") {
      throw ("replay pause after eof: expected invalid_state, got {0}" -f $resp.Final.code)
    }
  }

  Invoke-Check -Label "replay_stop_after_eof" -Action {
    $resp = Invoke-AirControlRequest -Context $context -Request @{
      category = "replay"
      action = "stop"
    } -TimeoutMs 4000
    Assert-ControlCompletedOk -Response $resp -Label "replay stop after eof"
    [void](Wait-AirControlState -Context $context -Predicate {
      param($s)
      return (-not $s.replay_active) -and (-not $s.replay_file_open)
    } -TimeoutMs 10000 -PollMs 250)
  }

  $updatedFusion = @{
    gain = [Math]::Round([double]$initialFusion.gain + 0.111, 3)
    accelerationRejection = [Math]::Round([double]$initialFusion.accelerationRejection + 1.5, 3)
    magneticRejection = [Math]::Round([double]$initialFusion.magneticRejection + 1.5, 3)
    recoveryTriggerPeriod = [int]$initialFusion.recoveryTriggerPeriod + 25
  }

  Invoke-Check -Label "fusion_set" -Action {
    $resp = Invoke-AirControlRequest -Context $context -Request @{
      category = "fusion"
      action = "set"
      gain = $updatedFusion.gain
      accelerationRejection = $updatedFusion.accelerationRejection
      magneticRejection = $updatedFusion.magneticRejection
      recoveryTriggerPeriod = $updatedFusion.recoveryTriggerPeriod
    } -TimeoutMs 4000
    Assert-ControlCompletedOk -Response $resp -Label "fusion set"
  }

  Invoke-Check -Label "fusion_readback" -Action {
    $resp = Invoke-AirControlRequest -Context $context -Request @{
      category = "fusion"
      action = "get"
    } -TimeoutMs 4000
    Assert-ControlCompletedOk -Response $resp -Label "fusion get readback"
    if (-not $resp.Final.has_fusion_settings) {
      throw "fusion readback returned no payload"
    }
    $fusion = $resp.Final.fusion
    if ([Math]::Abs([double]$fusion.gain - $updatedFusion.gain) -gt 0.0001) {
      throw "fusion gain readback mismatch"
    }
    if ([Math]::Abs([double]$fusion.accelerationRejection - $updatedFusion.accelerationRejection) -gt 0.0001) {
      throw "fusion accelerationRejection readback mismatch"
    }
    if ([Math]::Abs([double]$fusion.magneticRejection - $updatedFusion.magneticRejection) -gt 0.0001) {
      throw "fusion magneticRejection readback mismatch"
    }
    if ([int]$fusion.recoveryTriggerPeriod -ne $updatedFusion.recoveryTriggerPeriod) {
      throw "fusion recoveryTriggerPeriod readback mismatch"
    }
  }

  Invoke-Check -Label "fusion_restore" -Action {
    $resp = Invoke-AirControlRequest -Context $context -Request @{
      category = "fusion"
      action = "set"
      gain = [double]$initialFusion.gain
      accelerationRejection = [double]$initialFusion.accelerationRejection
      magneticRejection = [double]$initialFusion.magneticRejection
      recoveryTriggerPeriod = [int]$initialFusion.recoveryTriggerPeriod
    } -TimeoutMs 4000
    Assert-ControlCompletedOk -Response $resp -Label "fusion restore"
  }

  Write-ControlLog -Context $context -Source "SUMMARY" -Text ("passed={0} failed={1} log={2}" -f $passed, $failed, $LogPath)
}
finally {
  if ($null -ne $context) {
    Close-ControlContext -Context $context
  }
}
