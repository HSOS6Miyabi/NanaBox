param(
    [Parameter(Mandatory=$true)][string]$Executable,
    [string]$OutputDirectory = (Join-Path $env:TEMP ('NanaBox-UI-' + [guid]::NewGuid())),
    [switch]$HostIntegration
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'ModalUI.ps1')
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
Add-Type -AssemblyName System.Drawing, System.Windows.Forms
$Executable = (Resolve-Path $Executable).Path
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$config = Join-Path $OutputDirectory 'ui-test.7b'
$initial = @{NanaBox=@{Type='VirtualMachine';Version=1;Name='UI automation';GuestType='Windows';ProcessorCount=2;MemorySize=2048;Gpu=@{AssignmentMode='Disabled'}}} | ConvertTo-Json -Depth 10
[IO.File]::WriteAllText($config,$initial,(New-Object Text.UTF8Encoding($true)))

$mode = if ($HostIntegration) { '--edit-config' } else { '--edit-config-test' }
$process = Start-Process -FilePath $Executable -ArgumentList ($mode + ' "' + $config + '"') -PassThru
$root = [System.Windows.Automation.AutomationElement]::RootElement
$pidCondition = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::ProcessIdProperty, [int]$process.Id)
$desc = [System.Windows.Automation.TreeScope]::Descendants
$children = [System.Windows.Automation.TreeScope]::Children
function Find-Control([string]$Id, [switch]$Optional) {
    $until = [DateTime]::UtcNow.AddSeconds(25)
    do {
        if ($process.HasExited) { throw "NanaBox exited: $($process.ExitCode)" }
        $condition = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::AutomationIdProperty, $Id)
        foreach ($w in $root.FindAll($children,$pidCondition)) {
            $e = $w.FindFirst($desc,$condition)
            if ($null -ne $e) { return $e }
        }
        if ($Optional) { return $null }
        Start-Sleep -Milliseconds 200
    } while ([DateTime]::UtcNow -lt $until)
    throw "UI control unavailable: $Id"
}
function Pattern($element, $pattern) {
    $result = $null
    if (-not $element.TryGetCurrentPattern($pattern,[ref]$result)) {
        throw "Automation pattern unavailable: $($element.Current.AutomationId) / $pattern"
    }
    return $result
}
function Scroll($e) {
    $s = $null
    if ($e.TryGetCurrentPattern([System.Windows.Automation.ScrollItemPattern]::Pattern,[ref]$s)) { $s.ScrollIntoView() }
}
function Invoke-Control([string]$id) {
    Write-Host "Invoke: $id"
    $e = Find-Control $id
    Scroll $e
    (Pattern $e ([System.Windows.Automation.InvokePattern]::Pattern)).Invoke()
}
function Set-Text([string]$id,[string]$value) {
    $e = Find-Control $id
    Scroll $e
    (Pattern $e ([System.Windows.Automation.ValuePattern]::Pattern)).SetValue($value)
    if ((Get-Text $id) -cne $value) { throw "Value not accepted: $id" }
}
function Get-Text([string]$id) {
    return (Pattern (Find-Control $id) ([System.Windows.Automation.ValuePattern]::Pattern)).Current.Value
}
function Set-Check([string]$id,[bool]$value) {
    $p = Pattern (Find-Control $id) ([System.Windows.Automation.TogglePattern]::Pattern)
    for ($n=0; $n -lt 3; $n++) {
        if (($p.Current.ToggleState -eq [System.Windows.Automation.ToggleState]::On) -eq $value) { return }
        $p.Toggle()
    }
    throw "Toggle failed: $id"
}
function Select-Tab([string]$id) {
    $e = Find-Control $id
    (Pattern $e ([System.Windows.Automation.SelectionItemPattern]::Pattern)).Select()
}
function Select-Choice([string]$id,[string]$choice) {
    $e = Find-Control $id
    Scroll $e
    $expand = Pattern $e ([System.Windows.Automation.ExpandCollapsePattern]::Pattern)
    $expand.Expand()
    $item = Find-Control $choice
    (Pattern $item ([System.Windows.Automation.SelectionItemPattern]::Pattern)).Select()
    if ($expand.Current.ExpandCollapseState -eq [System.Windows.Automation.ExpandCollapseState]::Expanded) { $expand.Collapse() }
}
function Wait-Result([string]$id,[scriptblock]$predicate,[int]$seconds=90) {
    $until = [DateTime]::UtcNow.AddSeconds($seconds)
    do {
        try {
            $result = Get-Text $id | ConvertFrom-Json
            if ($null -ne $result -and (& $predicate $result)) { return $result }
        } catch {}
        Start-Sleep -Milliseconds 250
    } while ([DateTime]::UtcNow -lt $until)
    throw "Operation did not return the expected result: $id"
}
function Select-ByName([string]$id,[string]$prefix,[switch]$Optional) {
    $e = Find-Control $id
    Scroll $e
    $p = Pattern $e ([System.Windows.Automation.ExpandCollapsePattern]::Pattern)
    try { $p.Expand() } catch { if ($Optional) { return $false }; throw }
    $until = [DateTime]::UtcNow.AddSeconds(15)
    $type = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::ControlTypeProperty,[System.Windows.Automation.ControlType]::ListItem)
    do {
        foreach ($w in $root.FindAll($children,$pidCondition)) {
            foreach ($item in $w.FindAll($desc,$type)) {
                if ($item.Current.Name.StartsWith($prefix,[StringComparison]::OrdinalIgnoreCase)) {
                    (Pattern $item ([System.Windows.Automation.SelectionItemPattern]::Pattern)).Select()
                    if ($p.Current.ExpandCollapseState -eq [System.Windows.Automation.ExpandCollapseState]::Expanded) { $p.Collapse() }
                    return $true
                }
            }
        }
        Start-Sleep -Milliseconds 200
    } while ([DateTime]::UtcNow -lt $until)
    if ($p.Current.ExpandCollapseState -eq [System.Windows.Automation.ExpandCollapseState]::Expanded) { $p.Collapse() }
    if ($Optional) { return $false }
    throw "ComboBox item unavailable: $id / $prefix"
}
function Confirm-Change([string]$id) {
    Invoke-ModalAction $process.Id $id { Invoke-NativeDialogButton $process.Id 6 }
}
function Assert-Equal($actual,$expected,[string]$description) {
    if ($actual -ne $expected) { throw "$description : expected [$expected], got [$actual]" }
}
function Assert-Missing([string]$id) {
    if ($null -ne (Find-Control $id -Optional)) { throw "Unexpected control: $id" }
}
function Save-Config {
    $old = (Get-Item -LiteralPath $config).LastWriteTimeUtc
    Invoke-Control 'ApplyButton'
    $deadline = [DateTime]::UtcNow.AddSeconds(20)
    do {
        if ((Get-Item -LiteralPath $config).LastWriteTimeUtc -gt $old) {
            try { return (Get-Content -LiteralPath $config -Raw | ConvertFrom-Json).NanaBox } catch {}
        }
        Start-Sleep -Milliseconds 200
    } while ([DateTime]::UtcNow -lt $deadline)
    throw 'Configuration was not saved'
}
function Capture([string]$name) {
    $bounds = [System.Windows.Forms.SystemInformation]::VirtualScreen
    $bitmap = New-Object System.Drawing.Bitmap($bounds.Width,$bounds.Height)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    try {
        $graphics.CopyFromScreen($bounds.Location,[System.Drawing.Point]::Empty,$bounds.Size)
        $bitmap.Save((Join-Path $OutputDirectory ($name+'.png')))
    } finally { $graphics.Dispose(); $bitmap.Dispose() }
}
try {
    if (-not $HostIntegration) {
        Invoke-Control 'OpenButton'
        $opened = Wait-Result 'ConfigurationStatusTextBox' {param($v) $v.Success -and $v.Operation -eq 'OpenConfiguration'}
        Assert-Equal $opened.Path $config 'Opened configuration path'
        Invoke-Control 'ReferenceButton'
        $null = Wait-Result 'ConfigurationStatusTextBox' {param($v) $v.Success -and $v.Operation -eq 'Reference'}
    }
    Select-Tab 'TabGeneral'
    Set-Text 'NameTextBox' 'UI automation saved'
    Set-Text 'ProcessorCountTextBox' '4'
    Set-Text 'MemorySizeTextBox' '4096'
    Set-Text 'ProcessorLimitTextBox' '75000'
    Set-Text 'ProcessorWeightTextBox' '250'
    Set-Check 'MemoryOvercommitCheckBox' $true
    Set-Check 'DeferredCommitCheckBox' $true
    Set-Text 'StorageIopsMaximumTextBox' '1000'
    Set-Text 'StorageBandwidthMaximumTextBox' '10485760'
    Set-Check 'SecureBootCheckBox' $true
    Set-Check 'TpmCheckBox' $true
    Set-Check 'VirtualizationExtensionsCheckBox' $true
    Select-Tab 'TabGuestTools'
    if (-not $HostIntegration) {
        Invoke-Control 'GuestHeartbeatButton'
        $null = Wait-Result 'GuestToolsResult' {param($v) $v.Success -and $v.ResourceType -eq 'Heartbeat' -and $v.Data.Alive}
        Invoke-Control 'GuestInformationButton'
        $null = Wait-Result 'GuestToolsResult' {param($v) $v.Success -and $v.ResourceType -eq 'GuestInformation' -and $v.Data.HostName -eq 'mock-guest'}
        Invoke-Control 'GuestDiagnosticsButton'
        $null = Wait-Result 'GuestToolsResult' {param($v) -not $v.Success -and $v.ResourceType -eq 'Diagnostics' -and $v.Error -like '*Injected guest query failure*'}
        Invoke-Control 'GuestHeartbeatButton'
        $null = Wait-Result 'GuestToolsResult' {param($v) $v.Success -and $v.ResourceType -eq 'Heartbeat' -and $v.Data.Alive}
        Invoke-Control 'GuestClockQueryButton'
        $null = Wait-Result 'GuestToolsResult' {param($v) $v.Success -and $v.ResourceType -eq 'TimeQuery' -and $v.Data.GuestOffsetMilliseconds -eq 123}
        Invoke-ModalAction $process.Id 'GuestClockSyncButton' { Invoke-NativeDialogButton $process.Id 6 }
        $null = Wait-Result 'GuestToolsResult' {param($v) $v.Success -and $v.ResourceType -eq 'TimeSync' -and $v.Data.UnixMilliseconds -eq 1800000000123}
        $uploadPath = Join-Path $OutputDirectory 'guest-upload.bin'
        $downloadPath = Join-Path $OutputDirectory 'guest-download.bin'
        [IO.File]::WriteAllBytes($uploadPath, [byte[]](0,255,128,32,0,42))
        Set-Text 'GuestFileNameTextBox' 'exchange.bin'
        Set-Text 'GuestFilePathTextBox' $uploadPath
        Set-Check 'GuestFileOverwriteCheckBox' $false
        Invoke-ModalAction $process.Id 'GuestBrowseUploadButton' { Invoke-NativeDialogButton $process.Id 2 }
        Invoke-Control 'GuestUploadButton'
        $null = Wait-Result 'GuestToolsResult' {param($v) $v.Success -and $v.ResourceType -eq 'FileUpload' -and $v.Data.Size -eq 6}
        Invoke-Control 'GuestUploadButton'
        $null = Wait-Result 'GuestToolsResult' {param($v) $v.Success -eq $false -and $v.ResourceType -eq 'FileUpload'}
        Set-Text 'GuestFilePathTextBox' $downloadPath
        Invoke-ModalAction $process.Id 'GuestBrowseDownloadButton' { Invoke-NativeDialogButton $process.Id 2 }
        Invoke-Control 'GuestDownloadButton'
        $null = Wait-Result 'GuestToolsResult' {param($v) $v.Success -and $v.ResourceType -eq 'FileDownload' -and $v.Data.Size -eq 6}
        Assert-Equal (Get-FileHash -LiteralPath $downloadPath).Hash (Get-FileHash -LiteralPath $uploadPath).Hash 'Guest file transfer content'
        Invoke-Control 'GuestDownloadButton'
        $null = Wait-Result 'GuestToolsResult' {param($v) $v.Success -eq $false -and $v.ResourceType -eq 'FileDownload'}
        Set-Check 'GuestFileOverwriteCheckBox' $true
        Invoke-ModalAction $process.Id 'GuestDownloadButton' { Invoke-NativeDialogButton $process.Id 6 }
        $null = Wait-Result 'GuestToolsResult' {param($v) $v.Success -and $v.ResourceType -eq 'FileDownload'}
        Assert-Equal (Get-FileHash -LiteralPath $downloadPath).Hash (Get-FileHash -LiteralPath $uploadPath).Hash 'Guest file replacement'
        Set-Check 'GuestFileOverwriteCheckBox' $false
        Set-Text 'GuestFileNameTextBox' 'cancel-test.bin'
        Set-Text 'GuestFilePathTextBox' $uploadPath
        Invoke-Control 'GuestUploadButton'
        Invoke-Control 'GuestCancelButton'
        $null = Wait-Result 'GuestToolsResult' {param($v) $v.Success -eq $false -and $v.ResourceType -eq 'FileUpload' -and $v.Cancelled}
    } else {
        Invoke-Control 'GuestHeartbeatButton'
        $null = Wait-Result 'GuestToolsResult' {param($v) $v.Success -eq $false -and $v.Error}
    }
    Select-Tab 'TabSerial'
    Invoke-Control 'GenerateCom1'
    $pipe1 = Get-Text 'ComPort1TextBox'
    if ($pipe1 -notlike '\\.\pipe\NanaBox-*') { throw "Invalid pipe: $pipe1" }
    Invoke-Control 'CheckCom1'
    if (-not (Get-Text 'SerialPipeStatusTextBox')) { throw 'COM1 status was not reported' }
    Invoke-Control 'GenerateCom2'
    $pipe2 = Get-Text 'ComPort2TextBox'
    if ($pipe2 -notlike '\\.\pipe\NanaBox-*' -or $pipe1 -eq $pipe2) { throw 'COM2 pipe must be unique' }
    Invoke-Control 'CheckCom2'
    if (-not (Get-Text 'SerialPipeStatusTextBox')) { throw 'COM2 status was not reported' }
    Invoke-Control 'ClearCom2'
    Assert-Equal (Get-Text 'ComPort2TextBox') '' 'COM2 disconnected'
    Invoke-Control 'GenerateCom2'
    $pipe2 = Get-Text 'ComPort2TextBox'

    Select-Tab 'TabGpu'
    Select-Choice 'AssignmentModeComboBox' 'ChoiceListItem'
    Invoke-Control 'AddGpuDeviceButton'
    Set-Text 'GpuDeviceDeviceInterfaceLabel' 'UI-test-GPU'
    Set-Text 'GpuDevicePartitionIdLabel' '65535'
    $null = Find-Control 'HostGpuLabel'
    Set-Check 'EnableHostDriverStoreCheckBox' $true

    Select-Tab 'TabNetwork'
    $null = Find-Control 'HostNetworksComboBox'
    $null = Find-Control 'InspectNetwork'
    $null = Find-Control 'DeleteNetwork'
    $null = Find-Control 'CreateNetwork'
    Invoke-Control 'NetworkPreflightButton'
    $diagnostics = Wait-Result 'NetworkDiagnosticsTextBox' {param($v) $v.Mode -eq 'HostPreflight'}
    if ($null -eq $diagnostics.ApiAvailable -or $null -eq $diagnostics.WslDependencyAvailable -or $null -eq $diagnostics.HcnReachable) { throw 'Incomplete host diagnostic result' }
    Invoke-Control 'NetworkCopyButton'
    if (-not $HostIntegration) {
        Invoke-Control 'NetworkStatusButton'
        $null = Wait-Result 'NetworkDiagnosticsTextBox' {param($v) $v.Success -and $v.Mode -eq 'VmNetwork' -and -not $v.RetryRequested}
        Invoke-Control 'NetworkRetryButton'
        $null = Wait-Result 'NetworkDiagnosticsTextBox' {param($v) $v.Success -and $v.Mode -eq 'VmNetwork' -and $v.RetryRequested}
    } else {
        if ((Find-Control 'NetworkStatusButton').Current.IsEnabled -or (Find-Control 'NetworkRetryButton').Current.IsEnabled) { throw 'Offline network controls must be disabled' }
    }
    Set-Check 'MirroredNetworkingCheckBox' $true
    Set-Check 'MirroredAutoReservePortsCheckBox' $false
    Set-Text 'MirroredFirewallCreatorTextBox' '06b34f94-82ae-4ad9-9f35-3a2a7da8508c'
    Set-Text 'MirroredTcpPortsTextBox' '8080'
    Set-Text 'MirroredUdpPortsTextBox' '5353'
    Set-Text 'MirroredIgnoredPortsTextBox' '53'
    Set-Text 'NetworkNameTextBox' 'UI-test'
    Set-Check 'NetworkFirewallCheckBox' $true
    Invoke-Control 'AddNetworkAdapterButton'
    Set-Check 'NetworkAdapterConnectedLabel' $true
    Set-Text 'NetworkAdapterMacAddressLabel' '00-15-5D-12-34-56'
    Set-Text 'NetworkIdLabel' '85c8668e-36fe-447f-aae0-ba159a0142a7'
    Set-Text 'FirewallCreatorLabel' '06b34f94-82ae-4ad9-9f35-3a2a7da8508c'
    Set-Text 'NetworkAdapterSuggestedSwitchNameLabel' 'UI-test-legacy'
    Set-Text 'NetworkAdapterSuggestedSwitchSubnetLabel' '192.0.2.0/24'
    $null = Find-Control 'HostNetworkLabel'

    Select-Tab 'TabStorage'
    $diskPath = Join-Path $OutputDirectory 'ui-disk.vhdx'
    Set-Text 'DiskPathTextBox' $diskPath
    Set-Text 'DiskSizeTextBox' '32'
    Invoke-Control 'DiskCreate'
    $disk = Wait-Result 'DiskResultTextBox' { param($v) $v.Success -and $v.Operation -eq 'Create' }
    Assert-Equal $disk.VirtualSize (32MB) 'Created VHDX capacity'
    if (-not (Test-Path $diskPath -PathType Leaf)) { throw 'Virtual disk file missing' }
    Set-Text 'DiskSizeTextBox' '64'
    Invoke-Control 'DiskResize'
    $disk = Wait-Result 'DiskResultTextBox' { param($v) $v.Success -and $v.Operation -eq 'Resize' }
    Assert-Equal $disk.VirtualSize (64MB) 'Expanded VHDX capacity'
    Invoke-Control 'DiskCompact'
    $disk = Wait-Result 'DiskResultTextBox' { param($v) $v.Success -and $v.Operation -eq 'Compact' }
    Assert-Equal $disk.VirtualSize (64MB) 'Compacted VHDX capacity'
    Invoke-Control 'DiskInspect'
    $disk = Wait-Result 'DiskResultTextBox' { param($v) $v.Success -and $v.Operation -eq 'Inspect' }
    Assert-Equal $disk.VirtualSize (64MB) 'Inspected VHDX capacity'
    Remove-Item -LiteralPath $diskPath -Force
    $previousDiskPath = Get-Text 'DiskPathTextBox'
    Invoke-ModalAction $process.Id 'DiskBrowse' { Invoke-NativeDialogButton $process.Id 2 }
    Assert-Equal (Get-Text 'DiskPathTextBox') $previousDiskPath 'Canceled disk selection'
    Invoke-Control 'AddScsiDeviceButton'
    Set-Text 'ScsiDevicePathLabel' 'test.vhdx'
    $null = Find-Control 'ScsiDeviceTypeLabel'
    Invoke-ModalAction $process.Id 'ScsiBrowse' { Invoke-NativeDialogButton $process.Id 2 }
    Assert-Equal (Get-Text 'ScsiDevicePathLabel') 'test.vhdx' 'Canceled SCSI selection'
    Set-Text 'GuestStateFileTextBox' 'test.vmgs'
    Set-Text 'RuntimeStateFileTextBox' 'test.vmrs'

    Select-Tab 'TabHostManagement'
    Set-Text 'ManagedSwitchNameTextBox' 'UI-test'
    Select-Choice 'ManagedSwitchTypeComboBox' 'ManagedSwitchTypeComboBoxPrivate'
    Set-Text 'FirewallCreatorNameTextBox' 'UI-test'
    Select-Choice 'FirewallInboundComboBox' 'FirewallInboundComboBoxBlock'
    Select-Choice 'FirewallOutboundComboBox' 'FirewallOutboundComboBoxAllow'
    Set-Check 'FirewallLoopbackCheckBox' $true
    Set-Check 'FirewallMergeCheckBox' $true
    Set-Text 'FirewallRuleNameTextBox' 'UI-test-rule'
    Select-Choice 'FirewallDirectionComboBox' 'FirewallDirectionComboBoxInbound'
    Select-Choice 'FirewallActionComboBox' 'FirewallActionComboBoxAllow'
    Select-Choice 'FirewallProtocolComboBox' 'FirewallProtocolComboBoxTCP'
    Set-Text 'FirewallPortsTextBox' '18080'
    foreach ($id in @('SwitchListButton','SwitchCreateButton','SwitchDeleteButton','AdapterListButton','CreatorListButton','CreatorRegisterButton','FirewallInspectButton','UseCreatorButton','FirewallConfigureButton','RuleCreateButton','RuleEnableButton','RuleDisableButton','RuleDeleteButton')) {
        $null = Find-Control $id
    }
    & {
        $name = 'CI-' + [guid]::NewGuid().ToString('N')
        $managed = 'NanaBox.Managed.' + $name
        $creator = $null
        $networkCreated = $false
        try {
            Set-Text 'ManagedSwitchNameTextBox' $name
            Invoke-Control 'AdapterListButton'
            $null = Wait-Result 'HostManagementResult' {param($v) $v.Success -and $v.Operation -eq 'AdapterList'}
            Invoke-Control 'SwitchListButton'
            $null = Wait-Result 'HostManagementResult' {param($v) $v.Success -and $v.Operation -eq 'SwitchList'}
            Invoke-Control 'SwitchCreateButton'
            $null = Wait-Result 'HostManagementResult' {param($v) $v.Success -and $v.Name -eq $managed}
            if ($HostIntegration) {
                $switch = Get-VMSwitch | Where-Object Name -eq $managed
                if ($null -eq $switch -or [string]$switch.SwitchType -ne 'Private') { throw 'Switch was not created' }
            } else {
                Invoke-Control 'SwitchCreateButton'
                $null = Wait-Result 'HostManagementResult' {param($v) $v.Success -eq $false -and $v.Error -like '*already exists*'}
            }
            $null = Select-ByName 'ManagedSwitchesComboBox' ($managed+' (')
            Confirm-Change 'SwitchDeleteButton'
            $null = Wait-Result 'HostManagementResult' {param($v) $v.Success -and $v.Operation -eq 'SwitchDelete'}
            if ($HostIntegration -and (Get-VMSwitch | Where-Object Name -eq $managed)) { throw 'Switch was not removed' }
            if (Select-ByName 'ManagedSwitchesComboBox' ($managed+' (') -Optional) { throw 'Deleted switch still displayed' }

            Invoke-Control 'CreatorListButton'
            $null = Wait-Result 'HostManagementResult' {param($v) $v.Success -and $v.Operation -eq 'CreatorList'}
            Set-Text 'FirewallCreatorNameTextBox' $name
            Invoke-Control 'CreatorRegisterButton'
            $registered = Wait-Result 'HostManagementResult' {param($v) $v.Name -eq $managed -and $v.Id}
            $creator = [string]$registered.Id
            if ($HostIntegration) {
                $registeredCreator = Get-NetFirewallHyperVVMCreator | Where-Object { [guid]$_.VMCreatorId -eq [guid]$creator }
                if ($null -eq $registeredCreator) { throw 'Firewall creator registration missing' }
            }
            $null = Select-ByName 'FirewallCreatorsComboBox' ($managed+' (')
            Invoke-Control 'FirewallInspectButton'
            $null = Wait-Result 'HostManagementResult' {param($v) $null -ne $v.Settings}
            Invoke-Control 'UseCreatorButton'
            Select-Tab 'TabNetwork'
            Assert-Equal (Get-Text 'MirroredFirewallCreatorTextBox') $creator 'Selected firewall creator'
            Set-Text 'MirroredFirewallCreatorTextBox' '06b34f94-82ae-4ad9-9f35-3a2a7da8508c'
            Select-Tab 'TabHostManagement'
            Select-Choice 'FirewallInboundComboBox' 'FirewallInboundComboBoxAllow'
            Select-Choice 'FirewallOutboundComboBox' 'FirewallOutboundComboBoxBlock'
            Confirm-Change 'FirewallConfigureButton'
            $report = Wait-Result 'HostManagementResult' {param($v) @($v.Settings | Where-Object { $_.DefaultInboundAction -eq 'Allow' -and $_.DefaultOutboundAction -eq 'Block' }).Count -eq 1}
            $setting = $report.Settings[0]
            if ($HostIntegration) { $setting = Get-NetFirewallHyperVVMSetting -Name $creator }
            Assert-Equal ([string]$setting.DefaultInboundAction) 'Allow' 'Firewall inbound default'
            Assert-Equal ([string]$setting.DefaultOutboundAction) 'Block' 'Firewall outbound default'
            Invoke-Control 'RuleCreateButton'
            $report = Wait-Result 'HostManagementResult' {param($v) @($v.Rules | Where-Object Name -eq 'UI-test-rule').Count -eq 1}
            $rule = $report.Rules | Where-Object Name -eq 'UI-test-rule'
            if ($HostIntegration) { $rule = Get-NetFirewallHyperVRule -VMCreatorId $creator | Where-Object DisplayName -eq 'UI-test-rule' }
            if ($null -eq $rule -or $rule.LocalPorts -notcontains '18080') { throw 'Firewall rule missing or incorrect' }
            $null = Select-ByName 'FirewallRulesComboBox' 'UI-test-rule ('
            Invoke-Control 'RuleDisableButton'
            $null = Wait-Result 'HostManagementResult' {param($v) @($v.Rules | Where-Object {$_.Name -eq 'UI-test-rule' -and $_.Enabled -eq 'False'}).Count -eq 1}
            $null = Select-ByName 'FirewallRulesComboBox' 'UI-test-rule ('
            Invoke-Control 'RuleEnableButton'
            $null = Wait-Result 'HostManagementResult' {param($v) @($v.Rules | Where-Object {$_.Name -eq 'UI-test-rule' -and $_.Enabled -eq 'True'}).Count -eq 1}
            $null = Select-ByName 'FirewallRulesComboBox' 'UI-test-rule ('
            Confirm-Change 'RuleDeleteButton'
            $null = Wait-Result 'HostManagementResult' {param($v) $null -ne $v.Rules -and @($v.Rules | Where-Object Name -eq 'UI-test-rule').Count -eq 0}
            if ($HostIntegration -and (Get-NetFirewallHyperVRule -VMCreatorId $creator | Where-Object DisplayName -eq 'UI-test-rule')) { throw 'Firewall rule was not deleted' }

            Select-Tab 'TabNetwork'
            Set-Text 'NetworkNameTextBox' $name
            Set-Check 'NetworkFirewallCheckBox' $false
            $networkCreated = $true
            Invoke-Control 'CreateNetwork'
            $null = Wait-Result 'NetworkPropertiesTextBox' {param($v) $v.Success -and $v.Operation -eq 'Create'}
            $null = Select-ByName 'HostNetworksComboBox' ($managed+' (')
            Invoke-Control 'InspectNetwork'
            $null = Wait-Result 'NetworkPropertiesTextBox' {param($v) $v.Success -and $v.Operation -eq 'Inspect' -and $v.Name -eq $managed}
            Confirm-Change 'DeleteNetwork'
            $null = Wait-Result 'NetworkPropertiesTextBox' {param($v) $v.Success -and $v.Operation -eq 'Delete'}
            Invoke-Control 'RefreshNetworks'
            $null = Wait-Result 'NetworkPropertiesTextBox' {param($v) $v.Success -and $v.Operation -eq 'Refresh'}
            if (Select-ByName 'HostNetworksComboBox' ($managed+' (') -Optional) { throw 'HCN network was not deleted' }
            $networkCreated = $false
        } finally {
            if ($HostIntegration -and $networkCreated -and -not $process.HasExited) {
                try {
                    Select-Tab 'TabNetwork'
                    Invoke-Control 'RefreshNetworks'
                    $null = Wait-Result 'NetworkPropertiesTextBox' {param($v) $v.Success -and $v.Operation -eq 'Refresh'}
                    if (Select-ByName 'HostNetworksComboBox' ($managed+' (') -Optional) {
                        Confirm-Change 'DeleteNetwork'
                        $null = Wait-Result 'NetworkPropertiesTextBox' {param($v) $v.Success -and $v.Operation -eq 'Delete'}
                    }
                } catch { Write-Warning "HCN test network cleanup: $_" }
            }
            if ($HostIntegration) { Get-VMSwitch | Where-Object Name -eq $managed | Remove-VMSwitch -Confirm:$false }
            if ($HostIntegration -and $creator) {
                Get-NetFirewallHyperVRule -VMCreatorId $creator | Where-Object Name -like 'NanaBox.Managed.*' | Remove-NetFirewallHyperVRule -Confirm:$false
                $null = Invoke-CimMethod -Namespace root/standardcimv2 -ClassName MSFT_NetFirewallHyperVVMCreator -MethodName UnregisterHyperVVMCreator -Arguments @{VMCreatorId=$creator}
            }
        }
    }

    Select-Tab 'TabSession'
    Invoke-Control 'RefreshDriveButton'
    Invoke-Control 'RefreshDeviceButton'
    Set-Check 'RedirectKeyCombinationsCheckBox' $false
    foreach ($id in @('FullScreen','CtrlEsc','AltEsc','AltTab','AltShiftTab','AltSpace','CtrlAltDel','FocusReleaseLeft','FocusReleaseRight')) {
        Set-Text ($id+'HotkeyTextBox') '65'
    }
    foreach ($id in @('Audio','AudioCapture','Drives','Printers','Ports','SmartCards','Clipboard','Devices','POSDevices','DynamicDrives','DynamicDevices')) {
        Set-Check ('Redirect'+$id+'CheckBox') $true
    }

    Select-Tab 'TabAdvanced'
    foreach ($id in @('BaseBoardSerialNumber','ChassisSerialNumber','ChassisAssetTag','Manufacturer','ProductName','ChipsetVersion','SerialNumber','SKUNumber','Family')) {
        Set-Text ($id+'TextBox') ('Test-'+$id)
    }
    Set-Text 'UUIDTextBox' '12345678-1234-1234-1234-123456789abc'
    Set-Text 'HorizontalResolutionTextBox' '1280'
    Set-Text 'VerticalResolutionTextBox' '720'
    Set-Check 'DisableDpiScalingCheckBox' $true
    Set-Check 'DpiOverrideCheckBox' $true
    Set-Text 'DpiScaleTextBox' '175'
    Set-Check 'ContentResizingCheckBox' $false
    Set-Check 'ConnectionBarCheckBox' $false
    Set-Text 'PoliciesTextBox' 'test-policy'
    Invoke-Control 'AddPlan9ShareButton'
    Set-Text 'Plan9SharePortLabel' '564'
    Set-Text 'Plan9SharePathLabel' 'C:\Temp'
    Set-Text 'Plan9ShareNameLabel' 'test-share'
    Set-Check 'Plan9ShareReadOnlyLabel' $true
    Invoke-Control 'AddVirtualSmbShareButton'
    Set-Text 'VirtualSmbSharePathLabel' 'C:\Temp'
    Set-Text 'VirtualSmbShareNameLabel' 'test-smb'
    Set-Check 'VirtualSmbShareReadOnlyLabel' $true
    $saved = Save-Config
    Assert-Equal $saved.Name 'UI automation saved' 'Name'
    Assert-Equal $saved.ProcessorCount 4 'CPU'
    Assert-Equal $saved.MemorySize 4096 'Memory'
    Assert-Equal $saved.ProcessorLimit 75000 'CPU limit'
    Assert-Equal $saved.ProcessorWeight 250 'CPU weight'
    Assert-Equal $saved.EnableDeferredCommit $true 'Deferred memory commit'
    Assert-Equal $saved.StorageIopsMaximum 1000 'IOPS limit'
    Assert-Equal $saved.StorageBandwidthMaximum 10485760 'Storage bandwidth limit'
    Assert-Equal $saved.ComPorts.ComPort1 $pipe1 'COM1'
    Assert-Equal $saved.ComPorts.ComPort2 $pipe2 'COM2'
    Assert-Equal $saved.Gpu.AssignmentMode 'List' 'GPU mode'
    Assert-Equal $saved.Gpu.SelectedDevices[0] 'UI-test-GPU' 'Selected GPU'
    Assert-Equal $saved.MirroredNetworking $true 'Mirrored mode'
    Assert-Equal $saved.MirroredAutoReservePorts $false 'Automatic mirrored port reservations'
    Assert-Equal $saved.MirroredTcpPorts[0] '8080' 'Mirrored TCP port'
    Assert-Equal $saved.MirroredUdpPorts[0] '5353' 'Mirrored UDP port'
    Assert-Equal $saved.MirroredIgnoredPorts[0] '53' 'Excluded automatic port'
    Assert-Equal $saved.NetworkAdapters[0].NetworkId '85c8668e-36fe-447f-aae0-ba159a0142a7' 'Network'
    Assert-Equal $saved.NetworkAdapters[0].FirewallCreatorId '06b34f94-82ae-4ad9-9f35-3a2a7da8508c' 'Firewall'
    Assert-Equal $saved.ScsiDevices[0].Path 'test.vhdx' 'Disk'
    Assert-Equal $saved.Plan9Shares[0].Name 'test-share' 'Plan9'
    Assert-Equal $saved.VirtualSmbShares[0].Name 'test-smb' 'SMB'
    Assert-Equal $saved.VideoMonitor.HorizontalResolution 1280 'Resolution'
    Assert-Equal $saved.VideoMonitor.EnableDpiScalingValueOverride $true 'Display scaling override'
    Assert-Equal $saved.VideoMonitor.OverriddenDpiScalingValue 175 'Display scale'
    Assert-Equal $saved.VideoMonitor.EnableContentResizing $false 'Enhanced-session resizing'
    Assert-Equal $saved.VideoMonitor.ShowFullScreenModeConnectionBar $false 'Full-screen connection bar'
    Invoke-Control 'ReloadButton'
    Select-Tab 'TabSerial'
    Assert-Equal (Get-Text 'ComPort1TextBox') $pipe1 'COM1 reloaded'
    Invoke-Control 'ClearCom1'
    Invoke-Control 'ClearCom2'
    Select-Tab 'TabGpu'
    Invoke-Control 'GpuRemove'
    Select-Choice 'AssignmentModeComboBox' 'ChoiceMirrorItem'
    Select-Tab 'TabNetwork'
    Invoke-Control 'NetworkRemove'
    Select-Tab 'TabStorage'
    Invoke-Control 'ScsiRemove'
    Select-Tab 'TabAdvanced'
    Invoke-Control 'Plan9Remove'
    Invoke-Control 'SmbRemove'
    $saved = Save-Config
    Assert-Equal $saved.Gpu.AssignmentMode 'Mirror' 'All GPUs'
    Assert-Equal @($saved.NetworkAdapters | Where-Object {$_}).Count 0 'Network removed'
    Assert-Equal @($saved.ScsiDevices | Where-Object {$_}).Count 0 'Disk removed'
    Assert-Equal @($saved.Plan9Shares | Where-Object {$_}).Count 0 'Share removed'
    Assert-Equal @($saved.VirtualSmbShares | Where-Object {$_}).Count 0 'SMB removed'
    Select-Tab 'TabGeneral'
    Assert-Equal (Get-Text 'NameTextBox') 'UI automation saved' 'Reloaded name'
    Assert-Equal (Get-Text 'ProcessorLimitTextBox') '75000' 'Reloaded CPU limit'
    Assert-Equal (Get-Text 'ProcessorWeightTextBox') '250' 'Reloaded CPU weight'
    Assert-Equal (Get-Text 'StorageIopsMaximumTextBox') '1000' 'Reloaded IOPS limit'
    Assert-Equal (Get-Text 'StorageBandwidthMaximumTextBox') '10485760' 'Reloaded bandwidth limit'
    $external = Get-Content -LiteralPath $config -Raw | ConvertFrom-Json
    $external.NanaBox.Name = 'External edit'
    [IO.File]::WriteAllText($config,($external | ConvertTo-Json -Depth 30),(New-Object Text.UTF8Encoding($true)))
    Set-Text 'NameTextBox' 'Stale edit'
    Invoke-Control 'ApplyButton'
    $deadline = [DateTime]::UtcNow.AddSeconds(20)
    do {
        $status = Get-Text 'ConfigurationStatusTextBox'
        if ($status -like '*changed by another editor*') { break }
        Start-Sleep -Milliseconds 200
    } while ([DateTime]::UtcNow -lt $deadline)
    if ($status -notlike '*changed by another editor*') { throw "Conflict was not reported: $status" }
    Assert-Equal (Get-Content -LiteralPath $config -Raw | ConvertFrom-Json).NanaBox.Name 'External edit' 'Concurrent change preserved'
    Invoke-Control 'ReloadButton'
    Assert-Equal (Get-Text 'NameTextBox') 'External edit' 'External change reloaded'
    Set-Text 'NameTextBox' 'Recovered edit'
    Invoke-Control 'ValidateConfigurationButton'
    if ((Get-Text 'ConfigurationStatusTextBox') -notlike '*No changes have been saved*') { throw 'Validation result missing' }
    Assert-Equal (Get-Content -LiteralPath $config -Raw | ConvertFrom-Json).NanaBox.Name 'External edit' 'Validation does not change file'
    $saved = Save-Config
    Assert-Equal $saved.Name 'Recovered edit' 'Save after conflict recovery'
    Write-Host 'Settings UI round trip passed'
} catch {
    try { Capture 'failure' } catch { Write-Warning $_ }
    throw
} finally {
    if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force }
}
