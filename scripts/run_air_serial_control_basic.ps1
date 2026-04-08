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

  Invoke-Check -Label "replay_start_latest" -Action {
    $resp = Invoke-AirControlRequest -Context $context -Request @{
      category = "replay"
      action = "start_latest"
    } -TimeoutMs 5000
    Assert-ControlCompletedOk -Response $resp -Label "replay start latest"
    [void](Wait-AirControlState -Context $context -Predicate {
      param($s)
      return $s.replay_active -or $s.replay_file_open
    } -TimeoutMs 6000 -PollMs 250)
  }

  Start-Sleep -Milliseconds 1000

  Invoke-Check -Label "replay_status" -Action {
    $resp = Invoke-AirControlRequest -Context $context -Request @{
      category = "replay"
      action = "status"
    } -TimeoutMs 3000
    Assert-ControlCompletedOk -Response $resp -Label "replay status"
    if (-not $resp.Final.replay.file_open) {
      throw "replay status did not report file_open=true"
    }
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
