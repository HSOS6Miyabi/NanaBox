param(
    [Parameter(Mandatory=$true)][string]$Executable,
    [string]$GuestToolExecutable = (Join-Path $PSScriptRoot '..\GuestTool\nanabox-guest-tool.exe'),
    [Parameter(Mandatory=$true)][string]$WindowsConfig,
    [Parameter(Mandatory=$true)][string]$LinuxConfig,
    [Parameter(Mandatory=$true)][string]$TestAddress,
    [Parameter(Mandatory=$true)][ValidateRange(1,65535)][int]$TestPort,
    [Parameter(Mandatory=$true)][string]$WindowsGpuPattern,
    [Parameter(Mandatory=$true)][string]$LinuxGpuPattern
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'ModalUI.ps1')
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
$Executable = (Resolve-Path $Executable).Path
$GuestToolExecutable = (Resolve-Path -LiteralPath $GuestToolExecutable).Path
$root = [System.Windows.Automation.AutomationElement]::RootElement
$desc = [System.Windows.Automation.TreeScope]::Descendants
$children = [System.Windows.Automation.TreeScope]::Children
function Find-Element($process,[string]$id) {
    $pidCondition = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::ProcessIdProperty, [int]$process.Id)
    $condition = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::AutomationIdProperty, $id)
    $until = [DateTime]::UtcNow.AddSeconds(60)
    do {
        if ($process.HasExited) { throw "VM app exited unexpectedly: $($process.ExitCode)" }
        foreach ($w in $root.FindAll($children,$pidCondition)) {
            $e = $w.FindFirst($desc,$condition)
            if ($null -ne $e -and -not $e.Current.IsOffscreen) { return $e }
        }
        Start-Sleep -Milliseconds 250
    } while ([DateTime]::UtcNow -lt $until)
    throw "Could not locate $id"
}
function Invoke-UI($process,[string]$id) {
    $e = Find-Element $process $id
    $p = $e.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern)
    $p.Invoke()
}
function Toggle-UI($process,[string]$id) {
    $e = Find-Element $process $id
    $p = $e.GetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern)
    $p.Toggle()
}
function Wait-VMState([string]$name,[string]$state,[int]$seconds=120) {
    $deadline = [DateTime]::UtcNow.AddSeconds($seconds)
    do {
        $raw = & hcsdiag.exe list 2>&1 | Out-String
        if ($LASTEXITCODE -ne 0) { throw "hcsdiag failed: $raw" }
        $found = $raw -match [regex]::Escape($name)
        if ($state -eq 'Present' -and $found) { return }
        if ($state -eq 'Absent' -and -not $found) { return }
        Start-Sleep -Seconds 1
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "HCS VM $name did not become $state"
}
function Query-VM([string]$name) {
    $output = Join-Path $env:TEMP ('NanaBox-HCS-' + [guid]::NewGuid().ToString('N') + '.json')
    try {
        $query = Start-Process -FilePath $Executable -ArgumentList ('--query-vm "' + $name + '" "' + $output + '"') -PassThru -Wait
        if ($query.ExitCode -ne 0 -or -not (Test-Path $output)) { throw "HCS property query failed: $($query.ExitCode)" }
        return Get-Content -LiteralPath $output -Raw | ConvertFrom-Json
    } finally { Remove-Item -LiteralPath $output -ErrorAction SilentlyContinue }
}
function Wait-ExactState([string]$name,[string]$state) {
    $until = [DateTime]::UtcNow.AddSeconds(30)
    do {
        $vm = Query-VM $name
        if ([string]$vm.State -eq $state) { return $vm }
        Start-Sleep -Milliseconds 250
    } while ([DateTime]::UtcNow -lt $until)
    throw "Expected HCS state $state, got $($vm.State)"
}
function Wait-NewRuntime([string]$name,[string]$previous,$process) {
    $until = [DateTime]::UtcNow.AddMinutes(4)
    do {
        if ($process.HasExited) { throw 'VM app exited during restart' }
        try {
            $vm = Query-VM $name
            if ($vm.State -eq 'Running' -and $vm.RuntimeId -ne $previous) { return $vm }
        } catch {}
        Start-Sleep -Seconds 1
    } while ([DateTime]::UtcNow -lt $until)
    throw 'Restart did not produce a new running HCS runtime'
}
function Wait-Exit($process,[string]$name,[string]$operation) {
    Wait-VMState $name Absent 180
    if (-not $process.WaitForExit(30000)) { throw "App did not exit after $operation" }
}
function Guest-Request([string]$runtimeId,[string]$resource,$settings) {
    $file = Join-Path $env:TEMP ('NanaBox-GNS-' + [guid]::NewGuid().ToString('N') + '.json')
    try {
        @{Version=1; ResourceType=$resource; RequestType='Query'; Settings=$settings} | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $file -Encoding UTF8
        $response = & $GuestToolExecutable --send $runtimeId $file
        if ($LASTEXITCODE -ne 0) { throw "Guest request $resource failed: $response" }
        $reply = $response | ConvertFrom-Json
        if (-not $reply.Success) { throw "Guest request $resource failed: $($reply.Error)" }
        return $reply.Data
    } finally { Remove-Item -LiteralPath $file -ErrorAction SilentlyContinue }
}
function Wait-Mirrored($process) {
    $until = [DateTime]::UtcNow.AddMinutes(3)
    do {
        if ($process.HasExited) { throw 'VM exited before network synchronization' }
        $process.Refresh()
        if ($process.MainWindowTitle -like '*Mirrored: Synchronized*') { return }
        Start-Sleep -Seconds 1
    } while ([DateTime]::UtcNow -lt $until)
    throw "Mirrored networking did not synchronize: $($process.MainWindowTitle)"
}
function Wait-GpuShares([string]$runtimeId,[string]$guestType,$cfg) {
    $until = [DateTime]::UtcNow.AddMinutes(4)
    $last = 'Guest share service has not responded.'
    do {
        try {
            $status = Guest-Request $runtimeId 'Plan9Shares' @{}
            $last = $status | ConvertTo-Json -Depth 12 -Compress
            if ($guestType -eq 'Windows') {
                $expected = @($cfg.VirtualSmbShares | Where-Object {$_})
                $missing = @($expected | Where-Object {
                    $share = $_
                    -not @($status.VirtualSmbShares | Where-Object {
                        $_.Name -ceq $share.Name -and $_.Mounted -and [bool]$_.ReadOnly -eq [bool]$share.ReadOnly
                    }).Count
                })
                if ($status.Backend -eq 'VirtualSmb' -and $status.NativeTransportActive -and
                    $status.Mounted -and @($status.Packages | Where-Object {$_}).Count -gt 0 -and
                    -not @($status.Errors | Where-Object {$_}).Count -and
                    -not @($status.ShareErrors | Where-Object {$_}).Count -and -not $missing.Count -and
                    @($status.Devices | Where-Object {$_.State -eq 'Restarted'}).Count -gt 0) {
                    Write-Host "Windows native GPU and Virtual SMB share report: $last"
                    return
                }
            } else {
                $drivers = @($status.Shares | Where-Object {$_.Name -eq 'NanaBox.HostDrivers' -and $_.Mounted -and $_.ReadOnly -and $_.MountPoint -eq '/usr/lib/wsl/drivers'})
                $libraries = @($status.Shares | Where-Object {$_.Name -eq 'NanaBox.HostLxssLib' -and $_.Mounted -and $_.ReadOnly -and $_.MountPoint -eq '/usr/lib/wsl/lib'})
                if ($drivers.Count -eq 1 -and $libraries.Count -eq 1) {
                    Write-Host "Linux native GPU share report: $last"
                    return
                }
            }
        } catch { $last = $_.Exception.Message }
        Start-Sleep -Seconds 2
    } while ([DateTime]::UtcNow -lt $until)
    throw "$guestType GPU share initialization failed: $last"
}
function Test-Guest([string]$path,[string]$guestType,[string]$gpuPattern) {
    $path = (Resolve-Path -LiteralPath $path).Path
    $cfg = (Get-Content -LiteralPath $path -Raw | ConvertFrom-Json).NanaBox
    if ($cfg.Name -notmatch '^NanaBox-CI-[A-Za-z0-9_.-]+$') { throw 'Test VM name must start with NanaBox-CI- and use safe identifier characters' }
    if (-not $cfg.MirroredNetworking) { throw 'Hardware test guest must have mirrored networking enabled' }
    if (-not $cfg.Gpu -or $cfg.Gpu.AssignmentMode -eq 'Disabled' -or -not $cfg.Gpu.EnableHostDriverStore) {
        throw 'Hardware test guest must have GPU-PV and host driver sharing enabled'
    }
    if ($cfg.GuestType -ne $guestType) { throw "Expected $guestType guest" }
    if ($cfg.SaveStateFile) { throw 'Begin with a powered-off test VM without saved state' }
    if ((& hcsdiag.exe list | Out-String) -match [regex]::Escape($cfg.Name)) { throw "VM already running: $($cfg.Name)" }
    $process = Start-Process -FilePath $Executable -ArgumentList ('"' + $path + '"') -PassThru
    try {
        Wait-VMState $cfg.Name Present
        $vm = Wait-ExactState $cfg.Name 'Running'
        Wait-Mirrored $process
        Wait-GpuShares $vm.RuntimeId $guestType $cfg
        $guest = Guest-Request $vm.RuntimeId 'Diagnostics' @{}
        if ($guest.OperatingSystem -ne $guestType) { throw "Expected guest OS $guestType" }
        $gpu = $guest | Select-Object Gpu,Dxg | ConvertTo-Json -Depth 8
        if ($gpu -notmatch $gpuPattern) { throw "Expected GPU not found: $gpu" }
        $hostAddresses = @(Get-NetIPAddress | Where-Object { $_.IPAddress -notmatch '^(127\.|169\.254\.|::1$|fe80:)' } | Select-Object -ExpandProperty IPAddress)
        if (-not @($guest.Addresses | Where-Object { $_ -in $hostAddresses }).Count) { throw 'No mirrored guest address matches a host address' }
        $connection = Guest-Request $vm.RuntimeId 'Connectivity' @{Address=$TestAddress;Port=$TestPort}
        if ($connection.LocalAddress -notin $hostAddresses) { throw "TCP connection did not use a mirrored address: $($connection.LocalAddress)" }
        Write-Host "$guestType GPU and mirrored TCP connectivity verified"
        Toggle-UI $process 'PauseVirtualMachineButton'
        $null = Wait-ExactState $cfg.Name 'Paused'
        Toggle-UI $process 'PauseVirtualMachineButton'
        $null = Wait-ExactState $cfg.Name 'Running'
        Invoke-UI $process 'VirtualMachineSettingsButton'
        $null = Find-Element $process 'ApplyButton'
        # Close only the settings window, without applying any changes.
        $p = Find-Element $process 'ApplyButton'
        $walk = [System.Windows.Automation.TreeWalker]::RawViewWalker
        while ($p -and $p.Current.ControlType -ne [System.Windows.Automation.ControlType]::Window) { $p = $walk.GetParent($p) }
        if (-not $p) { throw 'Settings window not found' }
        $p.GetCurrentPattern([System.Windows.Automation.WindowPattern]::Pattern).Close()
        Invoke-UI $process 'RestartVirtualMachineButton'
        $vm = Wait-NewRuntime $cfg.Name $vm.RuntimeId $process
        Wait-Mirrored $process
        Wait-GpuShares $vm.RuntimeId $guestType $cfg
        $connection = Guest-Request $vm.RuntimeId 'Connectivity' @{Address=$TestAddress;Port=$TestPort}
        if ($connection.LocalAddress -notin $hostAddresses) { throw 'Mirrored connectivity failed after restart' }

        Toggle-UI $process 'PauseVirtualMachineButton'
        $null = Wait-ExactState $cfg.Name 'Paused'
        Invoke-UI $process 'PowerMenu'
        Invoke-UI $process 'SaveAction'
        Wait-Exit $process $cfg.Name 'save state'
        $saved = (Get-Content -LiteralPath $path -Raw | ConvertFrom-Json).NanaBox.SaveStateFile
        if (-not $saved) { throw 'Saved state was not recorded' }
        $savedPath = if ([IO.Path]::IsPathRooted($saved)) { $saved } else { Join-Path (Split-Path -Parent $path) $saved }
        if (-not (Test-Path -LiteralPath $savedPath -PathType Leaf)) { throw 'Saved state file is missing' }

        $process = Start-Process -FilePath $Executable -ArgumentList ('"' + $path + '"') -PassThru
        Wait-VMState $cfg.Name Present
        $vm = Wait-ExactState $cfg.Name 'Running'
        Wait-Mirrored $process
        Wait-GpuShares $vm.RuntimeId $guestType $cfg
        $guest = Guest-Request $vm.RuntimeId 'Diagnostics' @{}
        if (($guest | Select-Object Gpu,Dxg | ConvertTo-Json -Depth 8) -notmatch $gpuPattern) { throw 'GPU was not restored' }
        $connection = Guest-Request $vm.RuntimeId 'Connectivity' @{Address=$TestAddress;Port=$TestPort}
        if ($connection.LocalAddress -notin $hostAddresses) { throw 'Mirrored connectivity failed after restore' }
        if ((Get-Content -LiteralPath $path -Raw | ConvertFrom-Json).NanaBox.SaveStateFile) { throw 'Saved state was not consumed' }
        if (Test-Path -LiteralPath $savedPath) { throw 'Consumed saved state file was not removed' }

        Invoke-UI $process 'PowerMenu'
        Invoke-UI $process 'ShutdownAction'
        Wait-Exit $process $cfg.Name 'shutdown'

        $process = Start-Process -FilePath $Executable -ArgumentList ('"' + $path + '"') -PassThru
        Wait-VMState $cfg.Name Present
        $null = Wait-ExactState $cfg.Name 'Running'
        Invoke-UI $process 'PowerMenu'
        Invoke-ModalAction $process.Id 'PowerOffAction' { Invoke-NativeDialogButton $process.Id 6 }
        Wait-Exit $process $cfg.Name 'forced power off'
        Write-Host "$guestType GPU, networking and complete power lifecycle passed"
    } finally {
        if (-not $process.HasExited) {
            # Only this test process owns the disposable HCS compute system.
            Stop-Process -Id $process.Id -Force
        }
    }
}
if (-not (Get-Command hcsdiag.exe -ErrorAction SilentlyContinue)) { throw 'HCS tools are required' }
$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Run the interactive runner elevated' }
Test-Guest $WindowsConfig Windows $WindowsGpuPattern
Test-Guest $LinuxConfig Linux $LinuxGpuPattern
