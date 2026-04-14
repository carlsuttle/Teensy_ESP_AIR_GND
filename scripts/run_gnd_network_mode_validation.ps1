param(
  [string]$GndComPort = "COM9",
  [int]$Baud = 115200,
  [int]$SerialTimeoutMs = 12000,
  [int]$WsTimeoutMs = 12000,
  [string]$LogPath = ""
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function New-Timestamp {
  (Get-Date).ToString("yyyyMMdd_HHmmss")
}

function New-LogTimestamp {
  (Get-Date).ToString("HH:mm:ss.fff")
}

if (-not $LogPath) {
  $LogPath = Join-Path $PSScriptRoot ("gnd_network_mode_validation_{0}.log" -f (New-Timestamp))
}

"GND network mode validation" | Set-Content -Path $LogPath

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

function Read-SerialLines {
  param(
    [System.IO.Ports.SerialPort]$Port,
    [ref]$Partial,
    [System.Collections.Generic.List[string]]$Lines
  )

  $chunk = $Port.ReadExisting()
  if (-not $chunk) { return }
  $Partial.Value += $chunk
  while ($true) {
    $m = [regex]::Match($Partial.Value, "^(.*?)(`r`n|`n|`r)")
    if (-not $m.Success) { break }
    $line = $m.Groups[1].Value.Trim()
    $Partial.Value = $Partial.Value.Substring($m.Length)
    if ([string]::IsNullOrWhiteSpace($line)) { continue }
    $Lines.Add($line)
    Write-LogLine "SER" $line
  }
}

function Wait-ForSerialMatch {
  param(
    [System.IO.Ports.SerialPort]$Port,
    [ref]$Partial,
    [System.Collections.Generic.List[string]]$Lines,
    [regex]$Pattern,
    [int]$TimeoutMs
  )

  $deadline = (Get-Date).AddMilliseconds($TimeoutMs)
  while ((Get-Date) -lt $deadline) {
    Read-SerialLines -Port $Port -Partial $Partial -Lines $Lines
    foreach ($line in $Lines) {
      $m = $Pattern.Match($line)
      if ($m.Success) {
        return $m
      }
    }
    Start-Sleep -Milliseconds 25
  }
  throw ("Timed out waiting for serial pattern: {0}" -f $Pattern)
}

function Receive-WebSocketMessage {
  param(
    [System.Net.WebSockets.ClientWebSocket]$WebSocket,
    [System.Threading.CancellationToken]$Token,
    [int]$BufferSize = 16384
  )

  $buffer = New-Object byte[] $BufferSize
  $segment = [ArraySegment[byte]]::new($buffer)
  $task = $WebSocket.ReceiveAsync($segment, $Token)
  $task.Wait() | Out-Null
  $result = $task.Result
  if ($result.MessageType -eq [System.Net.WebSockets.WebSocketMessageType]::Close) {
    return $null
  }

  $count = $result.Count
  while (-not $result.EndOfMessage) {
    $segment = [ArraySegment[byte]]::new($buffer, $count, $buffer.Length - $count)
    $task = $WebSocket.ReceiveAsync($segment, $Token)
    $task.Wait() | Out-Null
    $result = $task.Result
    $count += $result.Count
  }

  return [System.Text.Encoding]::UTF8.GetString($buffer, 0, $count)
}

function Send-WebSocketJson {
  param(
    [System.Net.WebSockets.ClientWebSocket]$WebSocket,
    [hashtable]$Message,
    [System.Threading.CancellationToken]$Token
  )

  $json = $Message | ConvertTo-Json -Compress
  Write-LogLine "WS-TX" $json
  $bytes = [System.Text.Encoding]::UTF8.GetBytes($json)
  $segment = [ArraySegment[byte]]::new($bytes)
  $WebSocket.SendAsync($segment, [System.Net.WebSockets.WebSocketMessageType]::Text, $true, $Token).GetAwaiter().GetResult()
}

$summary = [ordered]@{
  gnd_port = $GndComPort
  log_path = $LogPath
  mode = ""
  ws_url = ""
  hello_seen = $false
  snapshot_seen = $false
  files_seen = $false
  storage_seen = $false
  ack_seen = $false
  ack_ok = $false
}

$port = $null
$ws = $null
$cts = [System.Threading.CancellationTokenSource]::new()
$partial = ""
$lines = [System.Collections.Generic.List[string]]::new()

try {
  $port = Open-SerialPort -Name $GndComPort -BaudRate $Baud
  Write-LogLine "INFO" ("serial open {0} @ {1}" -f $GndComPort, $Baud)
  Start-Sleep -Milliseconds 250
  Read-SerialLines -Port $port -Partial ([ref]$partial) -Lines $lines
  $port.WriteLine("netstat")
  Write-LogLine "SER-TX" "netstat"

  $netstatMatch = Wait-ForSerialMatch -Port $port -Partial ([ref]$partial) -Lines $lines -Pattern ([regex]'NETSTAT mode=(?<mode>\S+) .* ws=(?<ws>ws://\S+)') -TimeoutMs $SerialTimeoutMs
  $summary.mode = $netstatMatch.Groups["mode"].Value
  $summary.ws_url = $netstatMatch.Groups["ws"].Value
  Write-LogLine "INFO" ("mode={0} ws={1}" -f $summary.mode, $summary.ws_url)

  $ws = [System.Net.WebSockets.ClientWebSocket]::new()
  $ws.ConnectAsync([Uri]$summary.ws_url, $cts.Token).GetAwaiter().GetResult()
  Write-LogLine "INFO" "websocket open"

  $refreshReqId = 9001
  Send-WebSocketJson -WebSocket $ws -Token $cts.Token -Message @{
    type = "control"
    req_id = $refreshReqId
    category = "file"
    action = "refresh"
  }

  $deadline = (Get-Date).AddMilliseconds($WsTimeoutMs)
  while ((Get-Date) -lt $deadline) {
    $remainingMs = [int][Math]::Max(250, ($deadline - (Get-Date)).TotalMilliseconds)
    $wsCts = [System.Threading.CancellationTokenSource]::new($remainingMs)
    $text = $null
    try {
      $text = Receive-WebSocketMessage -WebSocket $ws -Token $wsCts.Token
    } catch {
      continue
    } finally {
      $wsCts.Dispose()
    }

    if ($null -eq $text) {
      Write-LogLine "INFO" "websocket closed by peer"
      break
    }

    Write-LogLine "WS-RX" $text
    $msg = $text | ConvertFrom-Json -AsHashtable
    switch ($msg.type) {
      "hello" { $summary.hello_seen = $true }
      "snapshot" { $summary.snapshot_seen = $true }
      "files" { $summary.files_seen = $true }
      "storage" { $summary.storage_seen = $true }
      "ack" {
        $summary.ack_seen = $true
        if (($msg.req_id -as [int]) -eq $refreshReqId) {
          $summary.ack_ok = [bool]$msg.ok
        }
      }
    }

    if ($summary.hello_seen -and $summary.snapshot_seen -and $summary.files_seen -and $summary.storage_seen -and $summary.ack_seen) {
      break
    }
  }

  if (-not $summary.hello_seen) { throw "hello not observed" }
  if (-not $summary.snapshot_seen) { throw "snapshot not observed" }
  if (-not $summary.files_seen) { throw "files not observed" }
  if (-not $summary.storage_seen) { throw "storage not observed" }
  if (-not $summary.ack_seen) { throw "ack not observed" }
  if (-not $summary.ack_ok) { throw "refresh ack was not ok" }

  Write-LogLine "PASS" ("mode={0} ws={1} hello=1 snapshot=1 files=1 storage=1 ack_ok=1" -f $summary.mode, $summary.ws_url)
} finally {
  if ($ws -and $ws.State -eq [System.Net.WebSockets.WebSocketState]::Open) {
    $ws.CloseAsync([System.Net.WebSockets.WebSocketCloseStatus]::NormalClosure, "done", [System.Threading.CancellationToken]::None).GetAwaiter().GetResult()
  }
  if ($ws) { $ws.Dispose() }
  if ($port) { $port.Close(); $port.Dispose() }
}

$summaryJson = $summary | ConvertTo-Json -Depth 4
Write-LogLine "SUMMARY" $summaryJson
