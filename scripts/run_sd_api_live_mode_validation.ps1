param(
  [string]$AirComPort = "COM7",
  [int]$Baud = 115200,
  [int]$StepTimeoutSeconds = 30,
  [string]$LogPath = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function New-Timestamp {
  (Get-Date).ToString("yyyyMMdd_HHmmss")
}

function New-LogTimestamp {
  (Get-Date).ToString("HH:mm:ss.fff")
}

if (-not $LogPath) {
  $stamp = New-Timestamp
  $LogPath = Join-Path $PSScriptRoot ("sd_api_live_validation_{0}.log" -f $stamp)
}

"SD API live-mode validation log" | Set-Content -Path $LogPath

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

$script:Context = @{
  port = $null
  partial = ""
  lines = [System.Collections.Generic.List[string]]::new()
}

function Add-Line {
  param([string]$Line)
  if ([string]::IsNullOrWhiteSpace($Line)) { return }
  $script:Context.lines.Add($Line)
  Write-LogLine "AIR" $Line
}

function Poll-Port {
  $chunk = $script:Context.port.ReadExisting()
  if (-not $chunk) { return }
  $script:Context.partial += $chunk
  while ($true) {
    $m = [regex]::Match($script:Context.partial, "^(.*?)(`r`n|`n|`r)")
    if (-not $m.Success) { break }
    $line = $m.Groups[1].Value.Trim()
    $script:Context.partial = $script:Context.partial.Substring($m.Length)
    Add-Line $line
  }
}

function Wait-ForMatch {
  param(
    [ScriptBlock]$Predicate,
    [int]$TimeoutSeconds = $StepTimeoutSeconds
  )

  $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
  $startIndex = $script:Context.lines.Count
  while ((Get-Date) -lt $deadline) {
    Poll-Port
    for ($i = $startIndex; $i -lt $script:Context.lines.Count; $i++) {
      $line = $script:Context.lines[$i]
      $result = & $Predicate $line
      if ($null -ne $result) {
        return $result
      }
    }
    $startIndex = $script:Context.lines.Count
    Start-Sleep -Milliseconds 50
  }
  throw "Timed out waiting for expected AIR response"
}

function Invoke-CommandAndWait {
  param(
    [string]$Command,
    [ScriptBlock]$Predicate,
    [int]$TimeoutSeconds = $StepTimeoutSeconds
  )

  Write-LogLine "CMD" $Command
  $script:Context.port.WriteLine($Command)
  return Wait-ForMatch -Predicate $Predicate -TimeoutSeconds $TimeoutSeconds
}

function Assert-True {
  param(
    [bool]$Condition,
    [string]$Message
  )
  if (-not $Condition) {
    throw $Message
  }
}

function Parse-KeyValueLine {
  param([string]$Line)
  $map = @{}
  foreach ($m in [regex]::Matches($Line, "([A-Za-z0-9_]+)=([^ ]+)")) {
    $map[$m.Groups[1].Value] = $m.Groups[2].Value
  }
  return $map
}

function Wait-ForJsonArray {
  param([int]$TimeoutSeconds = $StepTimeoutSeconds)
  return Wait-ForMatch -TimeoutSeconds $TimeoutSeconds -Predicate {
    param($line)
    $trimmed = $line.Trim()
    if (-not ($trimmed.StartsWith("[") -and $trimmed.EndsWith("]"))) {
      return $null
    }
    try {
      return $trimmed | ConvertFrom-Json
    } catch {
      return $null
    }
  }
}

function Request-StorageLine {
  param([string]$Command)
  return Invoke-CommandAndWait -Command $Command -Predicate {
    param($line)
    if ($line.StartsWith("SDPROBE ") -or $line.StartsWith("SDSTATE ") -or $line.StartsWith("SDWRITE ")) {
      return $line
    }
    return $null
  }
}

function Request-LogStatusLine {
  return Invoke-CommandAndWait -Command "logstat" -Predicate {
    param($line)
    if ($line.StartsWith("AIRLOG enabled=")) {
      return $line
    }
    return $null
  }
}

function Request-FileList {
  param([string]$SortCommand = "logfiles name asc")
  Write-LogLine "CMD" $SortCommand
  $script:Context.port.WriteLine($SortCommand)
  return Wait-ForJsonArray
}

function New-RenamedLogName {
  param([string]$OriginalName)

  $stem = [System.IO.Path]::GetFileNameWithoutExtension($OriginalName)
  $ext = [System.IO.Path]::GetExtension($OriginalName)
  $candidate = "{0}_ren.tlog" -f $stem
  if ($candidate.Length -gt 47) {
    $maxStem = 47 - "_ren".Length - $ext.Length
    if ($maxStem -lt 1) {
      $maxStem = 1
    }
    $candidate = "{0}_ren{1}" -f $stem.Substring(0, [Math]::Min($stem.Length, $maxStem)), $ext
  }
  return $candidate
}

$createdLog = $null
$renamedLog = $null
$createdCsv = $null

try {
  $script:Context.port = Open-SerialPort -Name $AirComPort -BaudRate $Baud
  Write-LogLine "INFO" ("Starting SD API live-mode validation on {0}" -f $AirComPort)
  Start-Sleep -Milliseconds 600
  Poll-Port

  Write-LogLine "INFO" "Forcing idle console state"
  $script:Context.port.WriteLine("x")
  Start-Sleep -Milliseconds 200
  Poll-Port

  $probeLine = Request-StorageLine -Command "sdprobe"
  $probe = Parse-KeyValueLine $probeLine
  Assert-True ($probe["status"] -eq "ok") "sdprobe did not report status=ok"

  if ($probe["mounted"] -ne "1" -or $probe["ready"] -ne "1") {
    $mountLine = Invoke-CommandAndWait -Command "sdmount" -Predicate {
      param($line)
      if ($line.StartsWith("SDMOUNT ")) { return $line }
      return $null
    }
    $mount = Parse-KeyValueLine $mountLine
    Assert-True ($mount["ok"] -eq "1") "sdmount failed"
    $probeLine = Request-StorageLine -Command "sdstate"
    $probe = Parse-KeyValueLine $probeLine
  }

  Assert-True ($probe["mounted"] -eq "1") "SD backend is not mounted"
  Assert-True ($probe["ready"] -eq "1") "SD backend is not ready"
  Write-LogLine "INFO" ("Precheck ok mounted={0} ready={1} files={2} time_state={3}" -f $probe["mounted"], $probe["ready"], $probe["files"], $probe["time_state"])

  $logStatusLine = Request-LogStatusLine
  $logStatus = Parse-KeyValueLine $logStatusLine
  Assert-True ($logStatus["active"] -eq "0") "Recording is active; live-mode SD validation requires idle logger state"

  $initialList = Request-FileList
  Write-LogLine "INFO" ("Initial file count={0}" -f @($initialList).Count)

  $sdWriteLine = Invoke-CommandAndWait -Command "sdwrite" -Predicate {
    param($line)
    if ($line.StartsWith("SDWRITE ok=")) { return $line }
    return $null
  } -TimeoutSeconds 45
  $sdWrite = Parse-KeyValueLine $sdWriteLine
  Assert-True ($sdWrite["ok"] -eq "1") "sdwrite failed"
  $createdLog = $sdWrite["file"]
  Assert-True (-not [string]::IsNullOrWhiteSpace($createdLog)) "sdwrite did not report a created file"
  Write-LogLine "INFO" ("Created test log {0}" -f $createdLog)

  $postWriteList = Request-FileList
  Assert-True (($postWriteList | Where-Object { $_.name -eq $createdLog }).Count -ge 1) "Created file is not present in file list"

  $renamedLog = New-RenamedLogName -OriginalName $createdLog
  $renameLine = Invoke-CommandAndWait -Command ("sdrename {0} {1}" -f $createdLog, $renamedLog) -Predicate {
    param($line)
    if ($line.StartsWith("SDRENAME ok=")) { return $line }
    return $null
  }
  $rename = Parse-KeyValueLine $renameLine
  Assert-True ($rename["ok"] -eq "1") "sdrename failed"
  Write-LogLine "INFO" ("Renamed test log to {0}" -f $renamedLog)

  $postRenameList = Request-FileList
  Assert-True (($postRenameList | Where-Object { $_.name -eq $renamedLog }).Count -ge 1) "Renamed file is not present in file list"
  Assert-True (($postRenameList | Where-Object { $_.name -eq $createdLog }).Count -eq 0) "Original file still appears after rename"

  $csvLine = Invoke-CommandAndWait -Command ("csvfile {0}" -f $renamedLog) -Predicate {
    param($line)
    if ($line.StartsWith("AIRCSV FILE ok=")) { return $line }
    return $null
  } -TimeoutSeconds 60
  $csvResult = Parse-KeyValueLine $csvLine
  Assert-True ($csvResult["ok"] -eq "1") "csvfile failed"
  $createdCsv = [System.IO.Path]::ChangeExtension($renamedLog, ".csv")
  Write-LogLine "INFO" ("Exported CSV {0}" -f $createdCsv)

  $postCsvList = Request-FileList
  Assert-True (($postCsvList | Where-Object { $_.name -eq $renamedLog }).Count -ge 1) "Renamed log missing after CSV export"

  $deleteCsvLine = Invoke-CommandAndWait -Command ("sddelete {0}" -f $createdCsv) -Predicate {
    param($line)
    if ($line.StartsWith("SDDELETE ok=")) { return $line }
    return $null
  }
  $deleteCsv = Parse-KeyValueLine $deleteCsvLine
  Assert-True ($deleteCsv["ok"] -eq "1") "Deleting generated CSV failed"

  $deleteLogLine = Invoke-CommandAndWait -Command ("sddelete {0}" -f $renamedLog) -Predicate {
    param($line)
    if ($line.StartsWith("SDDELETE ok=")) { return $line }
    return $null
  }
  $deleteLog = Parse-KeyValueLine $deleteLogLine
  Assert-True ($deleteLog["ok"] -eq "1") "Deleting renamed log failed"

  $finalList = Request-FileList
  Assert-True (($finalList | Where-Object { $_.name -eq $renamedLog }).Count -eq 0) "Renamed log still appears after delete"
  Assert-True (($finalList | Where-Object { $_.name -eq $createdCsv }).Count -eq 0) "Generated CSV still appears after delete"

  $ejectLine = Invoke-CommandAndWait -Command "sdeject" -Predicate {
    param($line)
    if ($line.StartsWith("SDEJECT ok=")) { return $line }
    return $null
  }
  $eject = Parse-KeyValueLine $ejectLine
  Assert-True ($eject["ok"] -eq "1") "sdeject failed"

  $mountLine = Invoke-CommandAndWait -Command "sdmount" -Predicate {
    param($line)
    if ($line.StartsWith("SDMOUNT ok=")) { return $line }
    return $null
  }
  $mount = Parse-KeyValueLine $mountLine
  Assert-True ($mount["ok"] -eq "1") "sdmount after eject failed"

  $finalProbeLine = Request-StorageLine -Command "sdprobe"
  $finalProbe = Parse-KeyValueLine $finalProbeLine
  Assert-True ($finalProbe["status"] -eq "ok") "Final sdprobe did not report status=ok"
  Assert-True ($finalProbe["mounted"] -eq "1") "Final sdprobe did not report mounted=1"
  Assert-True ($finalProbe["ready"] -eq "1") "Final sdprobe did not report ready=1"

  Write-LogLine "RESULT" ("ok=1 created={0} renamed={1} csv={2} log={3}" -f $createdLog, $renamedLog, $createdCsv, $LogPath)
}
finally {
  if ($script:Context.port) {
    try {
      if ($script:Context.port.IsOpen) {
        $script:Context.port.Close()
      }
    } catch {}
    $script:Context.port.Dispose()
  }
}
