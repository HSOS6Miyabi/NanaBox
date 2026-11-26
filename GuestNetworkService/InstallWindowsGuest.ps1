# Run in the Windows guest as administrator with Python 3.12+ installed for all users.
param([Parameter(Mandatory=$true)][string]$PythonPath)
$ErrorActionPreference = 'Stop'
$PythonPath = (Resolve-Path -LiteralPath $PythonPath).Path
& $PythonPath -c "import socket; assert hasattr(socket, 'AF_HYPERV')"
if ($LASTEXITCODE -ne 0) { throw 'Python 3.12+ with Hyper-V socket support is required' }
$dir = Join-Path $env:ProgramFiles 'NanaBox Guest Network Service'
New-Item -ItemType Directory -Path $dir -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'gns.py') -Destination $dir -Force
$script = Join-Path $dir 'gns.py'
$action = New-ScheduledTaskAction -Execute $PythonPath -Argument ('"{0}" serve' -f $script)
$trigger = New-ScheduledTaskTrigger -AtStartup
$principal = New-ScheduledTaskPrincipal -UserId 'SYSTEM' -LogonType ServiceAccount -RunLevel Highest
$settings = New-ScheduledTaskSettingsSet -RestartCount 3 -RestartInterval (New-TimeSpan -Minutes 1) -ExecutionTimeLimit ([TimeSpan]::Zero) -MultipleInstances IgnoreNew
Register-ScheduledTask -TaskName 'NanaBox Guest Network Service' -Action $action -Trigger $trigger -Principal $principal -Settings $settings -Force | Out-Null
Start-ScheduledTask -TaskName 'NanaBox Guest Network Service'
