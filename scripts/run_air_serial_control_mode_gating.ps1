param(
  [string]$AirComPort = "COM7",
  [int]$Baud = 115200,
  [string]$LogPath = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

. "$PSScriptRoot\air_serial_control_common.ps1"

if (-not $LogPath) {
  $LogPath = New-ControlLogPath -Prefix "air_serial_control_mode_gating"
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
    [void](Ensure-AirControlIdle -Context $context)
  }

  Invoke-Check -Label "recording_blocks_replay_and_file_list" -Action {
    $start = Invoke-AirControlRequest -Context $context -Request @{
      category = "recording"
      action = "start"
    } -TimeoutMs 4000
    Assert-ControlCompletedOk -Response $start -Label "recording start"
    [void](Wait-AirControlState -Context $context -Predicate { param($s) $s.recording_active } -TimeoutMs 6000 -PollMs 250)

    $replay = Invoke-AirControlRequest -Context $context -Request @{
      category = "replay"
      action = "start_latest"
    } -TimeoutMs 3000
    Assert-ControlBusyCode -Response $replay -ExpectedCode "busy_recording" -Label "replay start during recording"

    $list = Invoke-AirControlRequest -Context $context -Request @{
      category = "file"
      action = "list_page"
      offset = 0
      limit = 1
    } -TimeoutMs 3000
    Assert-ControlBusyCode -Response $list -ExpectedCode "busy_recording" -Label "file list during recording"

    $stop = Invoke-AirControlRequest -Context $context -Request @{
      category = "recording"
      action = "stop"
    } -TimeoutMs 4000
    Assert-ControlCompletedOk -Response $stop -Label "recording stop"
    [void](Wait-AirControlState -Context $context -Predicate {
      param($s)
      return (-not $s.recording_active) -and (-not $s.recording_busy)
    } -TimeoutMs 12000 -PollMs 250)
  }

  Start-Sleep -Milliseconds 1200

  Invoke-Check -Label "replay_blocks_recording_and_file_list" -Action {
    $replayStart = Invoke-AirControlRequest -Context $context -Request @{
      category = "replay"
      action = "start_latest"
    } -TimeoutMs 5000
    Assert-ControlCompletedOk -Response $replayStart -Label "replay start"
    [void](Wait-AirControlState -Context $context -Predicate {
      param($s)
      return $s.replay_active -or $s.replay_file_open
    } -TimeoutMs 6000 -PollMs 250)

    $record = Invoke-AirControlRequest -Context $context -Request @{
      category = "recording"
      action = "start"
    } -TimeoutMs 3000
    Assert-ControlBusyCode -Response $record -ExpectedCode "busy_replay" -Label "recording start during replay"

    $list = Invoke-AirControlRequest -Context $context -Request @{
      category = "file"
      action = "list_page"
      offset = 0
      limit = 1
    } -TimeoutMs 3000
    Assert-ControlBusyCode -Response $list -ExpectedCode "busy_replay" -Label "file list during replay"

    $replayStop = Invoke-AirControlRequest -Context $context -Request @{
      category = "replay"
      action = "stop"
    } -TimeoutMs 4000
    Assert-ControlCompletedOk -Response $replayStop -Label "replay stop"
    [void](Wait-AirControlState -Context $context -Predicate {
      param($s)
      return (-not $s.replay_active) -and (-not $s.replay_file_open)
    } -TimeoutMs 10000 -PollMs 250)
  }

  Write-ControlLog -Context $context -Source "SUMMARY" -Text ("passed={0} failed={1} log={2}" -f $passed, $failed, $LogPath)
}
finally {
  if ($null -ne $context) {
    Close-ControlContext -Context $context
  }
}
