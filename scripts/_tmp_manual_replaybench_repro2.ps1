Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
. 'c:\Users\dell\Platformio\esp32_crsf_telemetry\Teensy_ESP_AIR_GND\scripts\air_serial_control_common.ps1'
function Open-TextPort { param([string]$Name,[int]$BaudRate) Open-ControlSerialPort -Name $Name -BaudRate $BaudRate }
function Read-TextPort { param([System.IO.Ports.SerialPort]$Port,[string]$Label,[string]$LogPath) $chunk=$Port.ReadExisting(); if(-not $chunk){ return }; foreach($line in ($chunk -split "`r?`n")){ $t=$line.Trim(); if($t){ $msg = ('[{0}] [{1}] {2}' -f (Get-Date).ToString('HH:mm:ss.fff'), $Label, $t); Write-Host $msg; Add-Content -Path $LogPath -Value $msg } } }
function Drain-Both { param($AirCtx,$TeensyPort,[string]$LogPath,[int]$Ms) $deadline=(Get-Date).AddMilliseconds($Ms); while((Get-Date) -lt $deadline){ Read-ControlPort -Context $AirCtx; Read-TextPort -Port $TeensyPort -Label 'TEENSY' -LogPath $LogPath; Start-Sleep -Milliseconds 25 } }
function Send-AirText { param($AirCtx,[string]$Cmd) Write-ControlLog -Context $AirCtx -Source 'CMD' -Text $Cmd; $AirCtx.Port.WriteLine($Cmd) }
function Send-TeensyText { param([System.IO.Ports.SerialPort]$Port,[string]$Cmd,[string]$LogPath) $msg = ('[{0}] [CMDT] {1}' -f (Get-Date).ToString('HH:mm:ss.fff'), $Cmd); Write-Host $msg; Add-Content -Path $LogPath -Value $msg; $Port.WriteLine($Cmd) }
$log='c:\Users\dell\Platformio\esp32_crsf_telemetry\Teensy_ESP_AIR_GND\scripts\manual_replaybench_repro2_'+(Get-Date).ToString('yyyyMMdd_HHmmss')+'.log'
'AIR+TEENSY manual replaybench repro 2' | Set-Content $log
$airPort = Open-ControlSerialPort -Name 'COM7' -BaudRate 115200
$airCtx = New-ControlContext -Port $airPort -LogPath $log
$teensy = Open-TextPort -Name 'COM10' -BaudRate 115200
try {
  Drain-Both $airCtx $teensy $log 1500; $airCtx.Lines.Clear()
  Send-AirText $airCtx 'bench on'; Drain-Both $airCtx $teensy $log 800
  Send-AirText $airCtx 'quiet on'; Drain-Both $airCtx $teensy $log 800
  Send-TeensyText $teensy 'quiet on' $log; Drain-Both $airCtx $teensy $log 800
  Send-TeensyText $teensy 'resetloopperf' $log; Drain-Both $airCtx $teensy $log 600
  $airCtx.Lines.Clear(); Send-AirText $airCtx 'tapi replaybench 5000 50 48'
  $deadline=(Get-Date).AddSeconds(12); while((Get-Date) -lt $deadline){ Read-ControlPort -Context $airCtx; Read-TextPort -Port $teensy -Label 'TEENSY' -LogPath $log; if(($airCtx.Lines | Select-Object -Last 20) -match '^TAPI REPLAYBENCH RESULT') { break }; Start-Sleep -Milliseconds 25 }
  $airCtx.Lines.Clear(); foreach($request in @(@{ category='state'; action='get' },@{ category='recording'; action='status' },@{ category='replay'; action='status' },@{ category='file'; action='storage_status' },@{ category='fusion'; action='get' })) { $resp = Invoke-AirControlRequest -Context $airCtx -Request $request -TimeoutMs 5000; Assert-ControlCompletedOk -Response $resp -Label ('manual {0}/{1}' -f $request.category,$request.action); Drain-Both $airCtx $teensy $log 250 }
  Send-TeensyText $teensy 'resetloopperf' $log; Drain-Both $airCtx $teensy $log 600
  $airCtx.Lines.Clear(); Send-AirText $airCtx 'tapi replaybench 5000 50 48'
  $deadline=(Get-Date).AddSeconds(18); $gotSecond=$false; while((Get-Date) -lt $deadline){ Read-ControlPort -Context $airCtx; Read-TextPort -Port $teensy -Label 'TEENSY' -LogPath $log; if(($airCtx.Lines | Select-Object -Last 50) -match '^TAPI REPLAYBENCH RESULT') { $gotSecond=$true; break }; Start-Sleep -Milliseconds 25 }
  if(-not $gotSecond){ Write-ControlLog -Context $airCtx -Source 'INFO' -Text 'second replaybench appears hung after resetloopperf'; Send-TeensyText $teensy 'showsource' $log; Drain-Both $airCtx $teensy $log 2500; throw 'second replaybench did not complete' }
  Write-ControlLog -Context $airCtx -Source 'DONE' -Text 'second replaybench completed'
}
finally { try { if($teensy.IsOpen){ $teensy.Close() } } catch {}; try { if($airCtx.Port.IsOpen){ $airCtx.Port.Close() } } catch {} }
