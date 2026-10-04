# Called by the Net Mouse installer (elevated) to manage the firewall rule and the logon task.
param(
  [Parameter(Mandatory)][ValidateSet('firewall', 'autostart', 'remove-autostart', 'uninstall')]
  [string]$Action,
  [string]$Exe = '',
  [string]$Role = ''
)

$ErrorActionPreference = 'Stop'
$TaskName = 'Net Mouse'
$RuleName = 'Net Mouse'
$UsersSid = 'S-1-5-32-545'  # BUILTIN\Users; the SID works on every Windows language

function Remove-Autostart {
  Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false -ErrorAction SilentlyContinue
}

function Remove-Firewall {
  Get-NetFirewallRule -DisplayName $RuleName -ErrorAction SilentlyContinue | Remove-NetFirewallRule
}

switch ($Action) {
  'firewall' {
    Remove-Firewall
    New-NetFirewallRule -DisplayName $RuleName -Description 'Net Mouse discovery and pointer data (UDP multicast)' `
      -Direction Inbound -Program $Exe -Protocol UDP -Action Allow -Profile Any | Out-Null
  }
  'autostart' {
    Remove-Autostart
    if ($Role) {
      $taskAction = New-ScheduledTaskAction -Execute $Exe -Argument "--default-role $Role"
    } else {
      $taskAction = New-ScheduledTaskAction -Execute $Exe
    }
    $trigger = New-ScheduledTaskTrigger -AtLogOn
    # Runs in the session of whoever logs on. Highest privileges lets a slave control
    # administrator windows; standard users simply get a normal token.
    $principal = New-ScheduledTaskPrincipal -GroupId $UsersSid -RunLevel Highest
    $settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
      -ExecutionTimeLimit ([TimeSpan]::Zero) -MultipleInstances IgnoreNew
    Register-ScheduledTask -TaskName $TaskName -Action $taskAction -Trigger $trigger -Principal $principal `
      -Settings $settings -Description 'Starts Net Mouse at logon' -Force | Out-Null
  }
  'remove-autostart' {
    Remove-Autostart
  }
  'uninstall' {
    Remove-Autostart
    Remove-Firewall
  }
}
