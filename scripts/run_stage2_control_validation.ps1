param(
  [string]$AirComPort = "COM7",
  [string]$TeensyComPort = "COM10",
  [string]$GndComPort = "COM9",
  [int]$Baud = 115200,
  [int]$DurationMs = 150000,
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
  $stamp = New-Timestamp
  $LogPath = Join-Path $PSScriptRoot ("stage2_control_validation_{0}.log" -f $stamp)
}

"Stage 2 control validation log" | Set-Content -Path $LogPath

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
    AIR = @{ port = $null; partial = "" }
    GND = @{ port = $null; partial = "" }
    TEENSY = @{ port = $null; partial = "" }
  }
}

function Add-Line {
  param(
    [string]$Name,
    [string]$Line
  )
  if ([string]::IsNullOrWhiteSpace($Line)) { return }
  Write-LogLine $Name $Line
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

function Send-Command {
  param(
    [string]$Name,
    [string]$Command
  )
  Write-LogLine "CMD/$Name" $Command
  $ctx.ports[$Name].port.WriteLine($Command)
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

  Write-LogLine "INFO" ("Stage 2 control validation start duration_ms={0}" -f $DurationMs)
  Write-LogLine "INFO" "Expected operator sequence: idle file refresh, record start, file ops during record, record stop, safe file ops, fusion apply, backend replay start/stop while browser remains connected"

  Start-Sleep -Milliseconds 1000
  Poll-Ports

  foreach ($name in @("AIR", "GND")) {
    Send-Command -Name $name -Command "x"
  }
  Send-Command -Name "AIR" -Command "quiet off"
  Send-Command -Name "AIR" -Command "relink"
  Send-Command -Name "GND" -Command "relink"
  Send-Command -Name "AIR" -Command "stats"
  Send-Command -Name "GND" -Command "stats"
  Send-Command -Name "AIR" -Command "airstate"
  Send-Command -Name "AIR" -Command "logstat"
  Send-Command -Name "GND" -Command "replaystat"

  $events = @(
    @{ at = 30000; done = $false; name = "baseline_status"; action = { Send-Command -Name "AIR" -Command "airstate"; Send-Command -Name "AIR" -Command "logstat"; Send-Command -Name "GND" -Command "replaystat" } }
    @{ at = 65000; done = $false; name = "replay_start"; action = { Send-Command -Name "GND" -Command "replaystart"; Send-Command -Name "GND" -Command "replaystat" } }
    @{ at = 95000; done = $false; name = "replay_mid"; action = { Send-Command -Name "AIR" -Command "airstate"; Send-Command -Name "AIR" -Command "logstat"; Send-Command -Name "GND" -Command "replaystat" } }
    @{ at = 115000; done = $false; name = "replay_stop"; action = { Send-Command -Name "GND" -Command "replaystop"; Send-Command -Name "GND" -Command "replaystat" } }
    @{ at = 135000; done = $false; name = "final_status"; action = { Send-Command -Name "AIR" -Command "airstate"; Send-Command -Name "AIR" -Command "logstat"; Send-Command -Name "GND" -Command "replaystat" } }
  )

  $start = Get-Date
  while (((Get-Date) - $start).TotalMilliseconds -lt $DurationMs) {
    Poll-Ports
    $elapsed = [int](((Get-Date) - $start).TotalMilliseconds)
    foreach ($event in $events) {
      if (-not $event.done -and $elapsed -ge $event.at) {
        Write-LogLine "INFO" ("timeline {0} at {1}ms" -f $event.name, $elapsed)
        & $event.action
        $event.done = $true
      }
    }
    Start-Sleep -Milliseconds 50
  }

  Start-Sleep -Milliseconds 1000
  Poll-Ports
  Send-Command -Name "AIR" -Command "x"
  Send-Command -Name "GND" -Command "x"
  Send-Command -Name "AIR" -Command "airstate"
  Send-Command -Name "AIR" -Command "logstat"
  Send-Command -Name "GND" -Command "replaystat"
  Start-Sleep -Milliseconds 1500
  Poll-Ports
  Write-LogLine "INFO" ("Stage 2 control validation complete log={0}" -f $LogPath)
}
finally {
  Close-AllPorts
}
