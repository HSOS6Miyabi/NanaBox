param([Parameter(Mandatory=$true)][string]$Executable)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'ModalUI.ps1')
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
$process = Start-Process -FilePath (Resolve-Path $Executable).Path -ArgumentList '--test-quickstart' -PassThru
$root = [System.Windows.Automation.AutomationElement]::RootElement
$pidCondition = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::ProcessIdProperty,[int]$process.Id)
$desc = [System.Windows.Automation.TreeScope]::Descendants
$children = [System.Windows.Automation.TreeScope]::Children
function Find-Control([string]$Id,[switch]$Optional) {
    $until = [DateTime]::UtcNow.AddSeconds(30)
    $condition = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::AutomationIdProperty,$Id)
    do {
        if ($process.HasExited) { throw "Quick start exited: $($process.ExitCode)" }
        foreach ($handle in [NanaBoxNativeDialogs]::Windows($process.Id,$false)) {
            if (-not [NanaBoxNativeDialogs]::IsWindowEnabled($handle)) { continue }
            try {
                $w = [System.Windows.Automation.AutomationElement]::FromHandle($handle)
                $e = $w.FindFirst($desc,$condition)
                if ($null -ne $e -and -not $e.Current.IsOffscreen) { return $e }
            } catch [System.Windows.Automation.ElementNotAvailableException] {}
        }
        if ($Optional) { return $null }
        Start-Sleep -Milliseconds 200
    } while ([DateTime]::UtcNow -lt $until)
    throw "UI control unavailable: $Id"
}
function Invoke-Control([string]$Id) {
    Write-Host "Invoke: $Id"
    (Find-Control $Id).GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
}
function Find-OwnedWindowContaining([string]$Id) {
    $deadline = [DateTime]::UtcNow.AddSeconds(30)
    $condition = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::AutomationIdProperty,$Id)
    do {
        foreach ($handle in [NanaBoxNativeDialogs]::Windows($process.Id,$true)) {
            if (-not [NanaBoxNativeDialogs]::IsWindowEnabled($handle)) { continue }
            try {
                $window = [System.Windows.Automation.AutomationElement]::FromHandle($handle)
                $element = $window.FindFirst($desc,$condition)
                if ($null -ne $element) { return $handle }
            } catch [System.Windows.Automation.ElementNotAvailableException] {}
        }
        Start-Sleep -Milliseconds 150
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "Owned window not found: $Id"
}
function Wait-NativeWindowClosed([IntPtr]$handle,[string]$Id) {
    $deadline = [DateTime]::UtcNow.AddSeconds(30)
    while ([NanaBoxNativeDialogs]::IsWindow($handle)) {
        if ([DateTime]::UtcNow -ge $deadline) { throw "Dialog did not close: $Id ($handle)" }
        Start-Sleep -Milliseconds 100
    }
}
function Close-WindowContaining([string]$Id) {
    $handle = Find-OwnedWindowContaining $Id
    if (-not [NanaBoxNativeDialogs]::RequestClose($handle)) { throw "Cannot close owned window: $Id" }
    Wait-NativeWindowClosed $handle $Id
}
try {
    Invoke-ModalAction $process.Id 'QuickStartLaunchVirtualMachineButton' { Invoke-NativeDialogButton $process.Id 2 }
    Invoke-ModalAction $process.Id 'QuickStartVirtualMachineSettingsButton' { Invoke-NativeDialogButton $process.Id 2 }
    Invoke-Control 'QuickStartCreateVirtualMachineButton'
    Close-WindowContaining 'NameTextBox'
    Invoke-Control 'QuickStartCreateVirtualHardDiskButton'
    $null = Find-Control 'CreateButton'
    Invoke-ModalAction $process.Id 'FileNameBrowseButton' { Invoke-NativeDialogButton $process.Id 2 }
    Invoke-Control 'CreateButton'
    $message = Find-Control 'DiskValidationMessage'
    if ([string]::IsNullOrWhiteSpace($message.Current.Name)) { throw 'Missing disk validation message' }
    $diskWindow = Find-OwnedWindowContaining 'CreateButton'
    Invoke-Control 'CancelButton'
    Wait-NativeWindowClosed $diskWindow 'CreateButton'
    Invoke-Control 'QuickStartResizeVirtualHardDiskButton'
    $null = Find-Control 'ResizeButton'
    Invoke-ModalAction $process.Id 'FileNameBrowseButton' { Invoke-NativeDialogButton $process.Id 2 }
    Invoke-Control 'ResizeButton'
    $message = Find-Control 'DiskValidationMessage'
    if ([string]::IsNullOrWhiteSpace($message.Current.Name)) { throw 'Missing disk validation message' }
    $diskWindow = Find-OwnedWindowContaining 'ResizeButton'
    Invoke-Control 'CancelButton'
    Wait-NativeWindowClosed $diskWindow 'ResizeButton'
    Invoke-ModalAction $process.Id 'QuickStartCompactVirtualHardDiskButton' { Invoke-NativeDialogButton $process.Id 2 }
    Invoke-Control 'QuickStartAboutButton'
    $null = Find-Control 'AboutGitHubButton'
    $aboutWindow = Find-OwnedWindowContaining 'AboutCancelButton'
    Invoke-Control 'AboutCancelButton'
    Wait-NativeWindowClosed $aboutWindow 'AboutCancelButton'
    Write-Host 'Verified all seven quick start actions, file picker cancellation and invalid disk input.'
} finally {
    if (-not $process.HasExited) {
        [void]$process.CloseMainWindow()
        if (-not $process.WaitForExit(5000)) { Stop-Process -Id $process.Id -Force }
    }
}
