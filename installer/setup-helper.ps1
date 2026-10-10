# Called by the Net Mouse installer (elevated): settings folder, migration, firewall, service, autostart.
param(
  [Parameter(Mandatory)][ValidateSet('prepare', 'firewall', 'service', 'stop-service', 'uninstall')]
  [string]$Action,
  [string]$Exe = '',
  [string]$Role = '',
  [string]$Autostart = '1'
)

$ErrorActionPreference = 'Stop'
$ServiceName = 'NetMouse'
$RuleName = 'Net Mouse'
$LegacyTaskName = 'Net Mouse'
$RunKey = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Run'
$DataDir = Join-Path $env:ProgramData 'NetMouse'
$Config = Join-Path $DataDir 'netmouse.ini'

function Set-ConfigValue([string]$Key, [string]$Value) {
  $lines = @()
  if (Test-Path $Config) { $lines = @(Get-Content $Config | Where-Object { $_ -notmatch "^$Key=" }) }
  $lines += "$Key=$Value"
  Set-Content -Path $Config -Value $lines -Encoding ascii
}

function Stop-NetMouseService {
  $svc = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
  if ($svc -and $svc.Status -ne 'Stopped') {
    Stop-Service -Name $ServiceName -Force -ErrorAction SilentlyContinue
    $svc.WaitForStatus('Stopped', [TimeSpan]::FromSeconds(15))
  }
}

switch ($Action) {
  'prepare' {
    # Settings, key and logs of the service: SYSTEM and Administrators may change them, users may read
    # the settings (the private key gets its own, stricter permissions from the agent).
    New-Item -ItemType Directory -Force $DataDir | Out-Null
    icacls $DataDir /inheritance:r /grant:r '*S-1-5-18:(OI)(CI)F' '*S-1-5-32-544:(OI)(CI)F' '*S-1-5-32-545:(OI)(CI)RX' | Out-Null

    # First install of the service version: take over the settings of the earlier per-user version
    # (most recently used one), so this PC keeps its id, key, pairings and screen positions.
    if (-not (Test-Path $Config)) {
      $old = Get-ChildItem 'C:\Users\*\AppData\Roaming\NetMouse\netmouse.ini' -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
      if ($old) {
        Copy-Item $old.FullName $Config
        $key = [IO.Path]::ChangeExtension($old.FullName, '.key')
        if (Test-Path $key) { Copy-Item $key (Join-Path $DataDir 'netmouse.key') }
        Write-Output "Migrated settings from $($old.FullName)"
      }
    }
    if ($Role) { Set-ConfigValue 'role' $Role }
    # The per-user autostart task of earlier versions is replaced by the service.
    Unregister-ScheduledTask -TaskName $LegacyTaskName -Confirm:$false -ErrorAction SilentlyContinue
  }
  'firewall' {
    Get-NetFirewallRule -DisplayName $RuleName -ErrorAction SilentlyContinue | Remove-NetFirewallRule
    New-NetFirewallRule -DisplayName $RuleName -Description 'Net Mouse discovery and input (encrypted UDP multicast)' `
      -Direction Inbound -Program $Exe -Protocol UDP -Action Allow -Profile Any | Out-Null
  }
  'service' {
    $auto = $Autostart -eq '1'
    $binPath = "`"$Exe`" --service"
    Stop-NetMouseService
    if (Get-Service -Name $ServiceName -ErrorAction SilentlyContinue) {
      sc.exe config $ServiceName binPath= $binPath start= $(if ($auto) { 'auto' } else { 'demand' }) | Out-Null
    } else {
      New-Service -Name $ServiceName -BinaryPathName $binPath -DisplayName 'Net Mouse' `
        -StartupType $(if ($auto) { 'Automatic' } else { 'Manual' }) | Out-Null
    }
    sc.exe description $ServiceName 'Shares this PC''s mouse and keyboard with other PCs, or lets another PC control this one (Net Mouse).' | Out-Null
    sc.exe failure $ServiceName reset= 86400 actions= restart/2000/restart/5000/restart/15000 | Out-Null
    Start-Service -Name $ServiceName
    # The control app (tray icon) for everyone who signs in.
    if ($auto) {
      Set-ItemProperty -Path $RunKey -Name 'Net Mouse' -Value "`"$Exe`""
    } else {
      Remove-ItemProperty -Path $RunKey -Name 'Net Mouse' -ErrorAction SilentlyContinue
    }
  }
  'stop-service' {
    Stop-NetMouseService
  }
  'uninstall' {
    Stop-NetMouseService
    if (Get-Service -Name $ServiceName -ErrorAction SilentlyContinue) { sc.exe delete $ServiceName | Out-Null }
    Remove-ItemProperty -Path $RunKey -Name 'Net Mouse' -ErrorAction SilentlyContinue
    Unregister-ScheduledTask -TaskName $LegacyTaskName -Confirm:$false -ErrorAction SilentlyContinue
    Get-NetFirewallRule -DisplayName $RuleName -ErrorAction SilentlyContinue | Remove-NetFirewallRule
  }
}
