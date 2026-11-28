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
        foreach ($w in $root.FindAll($children,$pidCondition)) {
            $e = $w.FindFirst($desc,$condition)
            if ($null -ne $e) { return $e }
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
function Wait-Closed([string]$Id) {
    $until = [DateTime]::UtcNow.AddSeconds(30)
    do {
        if ($null -eq (Find-Control $Id -Optional)) { return }
        Start-Sleep -Milliseconds 200
    } while ([DateTime]::UtcNow -lt $until)
    throw "Dialog did not close: $Id"
}
function Close-WindowContaining([string]$Id) {
    $null = Find-Control $Id
    $condition = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::AutomationIdProperty,$Id)
    foreach ($w in $root.FindAll($children,$pidCondition)) {
        if ($null -ne $w.FindFirst($desc,$condition)) {
            $w.GetCurrentPattern([System.Windows.Automation.WindowPattern]::Pattern).Close()
            Wait-Closed $Id
            return
        }
    }
    throw "Window not found: $Id"
}
try {
    Invoke-ModalAction $process.Id 'QuickStartLaunchVirtualMachineButton' { Invoke-NativeDialogButton $process.Id 2 }
    Invoke-ModalAction $process.Id 'QuickStartVirtualMachineSettingsButton' { Invoke-NativeDialogButton $process.Id 2 }
    Invoke-Control 'QuickStartCreateVirtualMachineButton'
    Close-WindowContaining 'NameTextBox'
    Invoke-Control 'QuickStartCreateVirtualHardDiskButton'
    $null = Find-Control 'CreateButton'
    Invoke-ModalAction $process.Id 'FileNameBrowseButton' { Invoke-NativeDialogButton $process.Id 2 }
    Invoke-ModalAction $process.Id 'CreateButton' { Invoke-OwnedDialogControl $process.Id 'MessageConfirmButton' }
    Invoke-Control 'CancelButton'
    Wait-Closed 'CreateButton'
    Invoke-Control 'QuickStartResizeVirtualHardDiskButton'
    $null = Find-Control 'ResizeButton'
    Invoke-ModalAction $process.Id 'FileNameBrowseButton' { Invoke-NativeDialogButton $process.Id 2 }
    Invoke-ModalAction $process.Id 'ResizeButton' { Invoke-OwnedDialogControl $process.Id 'MessageConfirmButton' }
    Invoke-Control 'CancelButton'
    Wait-Closed 'ResizeButton'
    Invoke-ModalAction $process.Id 'QuickStartCompactVirtualHardDiskButton' { Invoke-NativeDialogButton $process.Id 2 }
    Invoke-Control 'QuickStartAboutButton'
    $null = Find-Control 'AboutGitHubButton'
    Invoke-Control 'AboutCancelButton'
    Wait-Closed 'AboutCancelButton'
    Write-Host 'Verified all seven quick start actions, file picker cancellation and invalid disk input.'
} finally {
    if (-not $process.HasExited) {
        [void]$process.CloseMainWindow()
        if (-not $process.WaitForExit(5000)) { Stop-Process -Id $process.Id -Force }
    }
}
