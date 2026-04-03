param(
  [string]$AirComPort = "COM7",
  [int]$Baud = 115200,
  [int]$TimeoutSeconds = 90
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

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
  $port.ReadTimeout = 200
  $port.WriteTimeout = 1000
  $port.NewLine = "`r`n"
  $port.DtrEnable = $true
  $port.RtsEnable = $true
  $port.Open()
  Start-Sleep -Milliseconds 300
  return $port
}

function Read-Chunk {
  param([System.IO.Ports.SerialPort]$Port)
  try {
    return $Port.ReadExisting()
  } catch {
    return ""
  }
}

$stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$artifact = Join-Path $PSScriptRoot ("time_service_validation_{0}.log" -f $stamp)
$port = $null

try {
  $port = Open-SerialPort -Name $AirComPort -BaudRate $Baud
  $sw = [System.IO.StreamWriter]::new($artifact, $false, [System.Text.UTF8Encoding]::new($false))

  try {
    Start-Sleep -Milliseconds 500
    $null = Read-Chunk -Port $port
    $port.WriteLine("x")
    Start-Sleep -Milliseconds 100
    $null = Read-Chunk -Port $port
    $port.WriteLine("timeselftest")

    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    $text = ""
    while ((Get-Date) -lt $deadline) {
      $chunk = Read-Chunk -Port $port
      if (![string]::IsNullOrEmpty($chunk)) {
        $text += $chunk
        $sw.Write($chunk)
        $sw.Flush()
        $resultMatches = [regex]::Matches($text, "TIMETEST RESULT ok=(\d+)")
        if ($resultMatches.Count -ge 2) {
          break
        }
      }
      Start-Sleep -Milliseconds 100
    }

    $resultMatches = [regex]::Matches($text, "TIMETEST RESULT ok=(\d+)")
    if ($resultMatches.Count -lt 2) {
      throw "Timed out waiting for complete TIMETEST results"
    }

    $ok = [int]$resultMatches[$resultMatches.Count - 1].Groups[1].Value
    Write-Host ("artifact: {0}" -f $artifact)
    Write-Host ("timeselftest_ok: {0}" -f $ok)
    if ($ok -ne 1) {
      throw "TIMETEST RESULT reported failure"
    }
  }
  finally {
    $sw.Dispose()
  }
}
finally {
  if ($port) {
    try { if ($port.IsOpen) { $port.Close() } } catch {}
    $port.Dispose()
  }
}
