<#
.SYNOPSIS
  Register (or replace) the daily D0 maintenance task for the current Windows user.

.DESCRIPTION
  Runs `pythonw.exe -m hy_recorder maint --config <Ops>\maint.json` from <Ops>\tool with the interpreter in
  <Ops>\venv, every day at -At (local time) and, as a catch-up, a few minutes after every logon. The task runs
  only while the user is logged on (no stored password). StartWhenAvailable makes a missed run happen as soon as
  the machine is back; MultipleInstances=IgnoreNew and the 6 h minimum interval in maint.json keep the two
  triggers from doing the work twice. pythonw has no console window: everything the run knows is in
  <Ops>\logs (latest.txt, status.json, maint-*.log) and in the task's last result (0 ok, 1 WARN, 2 CRIT).

  Remove with:  Unregister-ScheduledTask -TaskName HengYuan-D0-Maintenance -Confirm:$false
  See recorder/MAINTENANCE.md for the layout and what to do with each finding.
#>
param(
    [Parameter(Mandatory = $true)][string]$Ops,
    [string]$At = '06:00',
    [string]$TaskName = 'HengYuan-D0-Maintenance'
)
$ErrorActionPreference = 'Stop'

$py = Join-Path $Ops 'venv\Scripts\pythonw.exe'
$tool = Join-Path $Ops 'tool'
$cfg = Join-Path $Ops 'maint.json'
foreach ($p in @($py, $tool, $cfg)) {
    if (-not (Test-Path $p)) { throw "missing: $p" }
}

$user = [System.Security.Principal.WindowsIdentity]::GetCurrent().Name
$action = New-ScheduledTaskAction -Execute $py -Argument "-m hy_recorder maint --config `"$cfg`"" -WorkingDirectory $tool
$daily = New-ScheduledTaskTrigger -Daily -At $At
# -User matters: a logon trigger for "any user" needs administrator rights to register, one for this user does not
$logon = New-ScheduledTaskTrigger -AtLogOn -User $user
$logon.Delay = 'PT5M'
$settings = New-ScheduledTaskSettingsSet -StartWhenAvailable -MultipleInstances IgnoreNew `
    -ExecutionTimeLimit (New-TimeSpan -Hours 3) -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries
$principal = New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Limited

Register-ScheduledTask -TaskName $TaskName -Action $action -Trigger @($daily, $logon) -Settings $settings `
    -Principal $principal -Force `
    -Description 'HengYuan D0 recorder: daily pull, host probe and checks (see recorder/MAINTENANCE.md)' | Out-Null

Get-ScheduledTask -TaskName $TaskName | Get-ScheduledTaskInfo | Select-Object TaskName, NextRunTime, LastTaskResult
