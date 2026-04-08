Set-StrictMode -Version Latest

function New-ControlTimestamp {
  (Get-Date).ToString("HH:mm:ss.fff")
}

function New-ControlLogPath {
  param([string]$Prefix)
  $stamp = (Get-Date).ToString("yyyyMMdd_HHmmss")
  return Join-Path $PSScriptRoot ("{0}_{1}.log" -f $Prefix, $stamp)
}

function Write-ControlLog {
  param(
    [hashtable]$Context,
    [string]$Source,
    [string]$Text
  )

  $line = "[{0}] [{1}] {2}" -f (New-ControlTimestamp), $Source, $Text
  Write-Host $line
  if ($Context.LogPath) {
    Add-Content -Path $Context.LogPath -Value $line
  }
}

function Open-ControlSerialPort {
  param(
    [string]$Name,
    [int]$BaudRate
  )

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

function New-ControlContext {
  param(
    [System.IO.Ports.SerialPort]$Port,
    [string]$LogPath
  )

  if ($LogPath) {
    "AIR serial control log" | Set-Content -Path $LogPath
  }

  return @{
    Port = $Port
    Partial = ""
    Lines = [System.Collections.Generic.List[string]]::new()
    LogPath = $LogPath
  }
}

function Read-ControlPort {
  param([hashtable]$Context)

  $chunk = $Context.Port.ReadExisting()
  if (-not $chunk) { return }
  $Context.Partial += $chunk
  while ($true) {
    $m = [regex]::Match($Context.Partial, "^(.*?)(`r`n|`n|`r)")
    if (-not $m.Success) { break }
    $line = $m.Groups[1].Value.Trim()
    $Context.Partial = $Context.Partial.Substring($m.Length)
    if ([string]::IsNullOrWhiteSpace($line)) { continue }
    $Context.Lines.Add($line)
    Write-ControlLog -Context $Context -Source "AIR" -Text $line
  }
}

function Drain-ControlPort {
  param(
    [hashtable]$Context,
    [int]$DurationMs = 500
  )

  $deadline = (Get-Date).AddMilliseconds($DurationMs)
  while ((Get-Date) -lt $deadline) {
    Read-ControlPort -Context $Context
    Start-Sleep -Milliseconds 25
  }
}

function Try-ParseControlJson {
  param([string]$Line)

  $trimmed = $Line.Trim()
  if (-not ($trimmed.StartsWith("{") -and $trimmed.EndsWith("}"))) {
    return $null
  }
  try {
    return $trimmed | ConvertFrom-Json
  } catch {
    return $null
  }
}

$script:AirControlReqId = 0
function New-AirControlRequestId {
  $script:AirControlReqId++
  if ($script:AirControlReqId -le 0) { $script:AirControlReqId = 1 }
  return [uint32]$script:AirControlReqId
}

function Invoke-AirControlRequest {
  param(
    [hashtable]$Context,
    [hashtable]$Request,
    [int]$TimeoutMs = 4000
  )

  if (-not $Request.ContainsKey("req_id")) {
    $Request["req_id"] = New-AirControlRequestId
  }

  $json = $Request | ConvertTo-Json -Compress
  Write-ControlLog -Context $Context -Source "REQ" -Text $json
  $startIndex = $Context.Lines.Count
  $Context.Port.WriteLine($json)

  $deadline = (Get-Date).AddMilliseconds($TimeoutMs)
  $immediate = $null
  $final = $null
  while ((Get-Date) -lt $deadline) {
    Read-ControlPort -Context $Context
    for ($i = $startIndex; $i -lt $Context.Lines.Count; $i++) {
      $parsed = Try-ParseControlJson -Line $Context.Lines[$i]
      if ($null -eq $parsed) { continue }
      if ($parsed.type -ne "control_result") { continue }
      if ([uint32]$parsed.req_id -ne [uint32]$Request.req_id) { continue }
      if ($parsed.phase -eq "immediate" -and $null -eq $immediate) {
        $immediate = $parsed
        if ($parsed.status -ne "accepted") {
          return [pscustomobject]@{
            Request = [pscustomobject]$Request
            Immediate = $immediate
            Final = $null
          }
        }
      } elseif ($parsed.phase -eq "final") {
        $final = $parsed
        return [pscustomobject]@{
          Request = [pscustomobject]$Request
          Immediate = $immediate
          Final = $final
        }
      }
    }
    $startIndex = $Context.Lines.Count
    Start-Sleep -Milliseconds 25
  }

  throw ("Timed out waiting for AIR control response req_id={0}" -f $Request.req_id)
}

function Get-AirControlState {
  param(
    [hashtable]$Context,
    [int]$TimeoutMs = 4000
  )

  $resp = Invoke-AirControlRequest -Context $Context -Request @{
    category = "state"
    action = "get"
  } -TimeoutMs $TimeoutMs

  if ($null -eq $resp.Final) {
    throw "State request did not return a final response"
  }
  return $resp.Final.state
}

function Wait-AirControlState {
  param(
    [hashtable]$Context,
    [scriptblock]$Predicate,
    [int]$TimeoutMs = 8000,
    [int]$PollMs = 200
  )

  $deadline = (Get-Date).AddMilliseconds($TimeoutMs)
  while ((Get-Date) -lt $deadline) {
    $remainingMs = [int][Math]::Max(1000, ($deadline - (Get-Date)).TotalMilliseconds)
    $stateTimeoutMs = [Math]::Max(3000, [Math]::Min($remainingMs, 8000))
    $state = Get-AirControlState -Context $Context -TimeoutMs $stateTimeoutMs
    if (& $Predicate $state) {
      return $state
    }
    Start-Sleep -Milliseconds $PollMs
  }
  throw "Timed out waiting for AIR control state predicate"
}

function Assert-ControlAccepted {
  param(
    $Response,
    [string]$Label
  )

  if ($Response.Immediate.status -ne "accepted") {
    throw ("{0}: expected accepted, got {1}/{2}" -f $Label, $Response.Immediate.status, $Response.Immediate.code)
  }
}

function Assert-ControlCompletedOk {
  param(
    $Response,
    [string]$Label
  )

  Assert-ControlAccepted -Response $Response -Label $Label
  if ($null -eq $Response.Final) {
    throw ("{0}: missing final response" -f $Label)
  }
  if ($Response.Final.status -ne "completed_ok") {
    throw ("{0}: expected completed_ok, got {1}/{2}" -f $Label, $Response.Final.status, $Response.Final.code)
  }
}

function Assert-ControlRejectedCode {
  param(
    $Response,
    [string]$ExpectedCode,
    [string]$Label
  )

  if ($Response.Immediate.status -ne "rejected") {
    throw ("{0}: expected rejected, got {1}/{2}" -f $Label, $Response.Immediate.status, $Response.Immediate.code)
  }
  if ($Response.Immediate.code -ne $ExpectedCode) {
    throw ("{0}: expected code {1}, got {2}" -f $Label, $ExpectedCode, $Response.Immediate.code)
  }
}

function Ensure-AirControlIdle {
  param([hashtable]$Context)

  $state = Get-AirControlState -Context $Context
  if ($state.recording_active) {
    $resp = Invoke-AirControlRequest -Context $Context -Request @{
      category = "recording"
      action = "stop"
    } -TimeoutMs 4000
    Assert-ControlCompletedOk -Response $resp -Label "recording stop during idle recovery"
  }
  if ($state.replay_active -or $state.replay_file_open) {
    $resp = Invoke-AirControlRequest -Context $Context -Request @{
      category = "replay"
      action = "stop"
    } -TimeoutMs 4000
    Assert-ControlCompletedOk -Response $resp -Label "replay stop during idle recovery"
  }
  return Wait-AirControlState -Context $Context -Predicate {
    param($s)
    return (-not $s.recording_active) -and (-not $s.replay_active) -and (-not $s.replay_file_open)
  } -TimeoutMs 12000 -PollMs 250
}

function Close-ControlContext {
  param([hashtable]$Context)

  if ($null -ne $Context -and $null -ne $Context.Port) {
    try {
      if ($Context.Port.IsOpen) {
        $Context.Port.Close()
      }
    } catch {}
    $Context.Port.Dispose()
  }
}
