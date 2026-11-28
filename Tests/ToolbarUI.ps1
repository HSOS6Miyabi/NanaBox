param(
    [Parameter(Mandatory=$true)][string]$Executable,
    [string]$OutputDirectory = (Join-Path $env:TEMP ('NanaBox-Toolbar-' + [guid]::NewGuid()))
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$log = Join-Path $OutputDirectory 'toolbar-commands.txt'
$exe = (Resolve-Path -LiteralPath $Executable).Path
$process = Start-Process -FilePath $exe -ArgumentList ('--test-toolbar "' + $log + '"') -PassThru
$root = [System.Windows.Automation.AutomationElement]::RootElement
$desc = [System.Windows.Automation.TreeScope]::Descendants
$children = [System.Windows.Automation.TreeScope]::Children
$pidCondition = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::ProcessIdProperty,[int]$process.Id)
$expected = New-Object 'System.Collections.Generic.List[int]'
function Find-Control([string]$id) {
    $deadline = [DateTime]::UtcNow.AddSeconds(25)
    $condition = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::AutomationIdProperty,$id)
    do {
        if ($process.HasExited) { throw "Toolbar process exited: $($process.ExitCode)" }
        foreach ($window in $root.FindAll($children,$pidCondition)) {
            foreach ($element in $window.FindAll($desc,$condition)) {
                if ($element.Current.IsEnabled -and -not $element.Current.IsOffscreen) { return $element }
            }
        }
        Start-Sleep -Milliseconds 200
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "Toolbar control unavailable: $id"
}
function Click-Control([string]$id,[switch]$Toggle) {
    $element = Find-Control $id
    Write-Host "Invoke: $id"
    if ($Toggle) {
        $pattern = $element.GetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern)
        $previous = $pattern.Current.ToggleState
        $pattern.Toggle()
        if ($pattern.Current.ToggleState -eq $previous) { throw "Toggle state unchanged: $id" }
    } else {
        $element.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
    }
}
function Read-CommandLog {
    $stream = [IO.File]::Open($log,[IO.FileMode]::Open,[IO.FileAccess]::Read,([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
    $reader = New-Object IO.StreamReader($stream)
    try { $reader.ReadToEnd() -split '\r?\n' | Where-Object { $_ -ne '' } }
    finally { $reader.Dispose() }
}
function Assert-Command([string]$id,[int]$command,[switch]$Toggle) {
    $expected.Add($command)
    Click-Control $id -Toggle:$Toggle
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    do {
        try { $actual = @(Read-CommandLog) } catch [IO.IOException] { Start-Sleep -Milliseconds 100; continue }
        if ($actual.Count -gt $expected.Count) { throw "Unexpected duplicate command from $id" }
        if ($actual.Count -eq $expected.Count) {
            if (($actual -join ',') -ne ($expected -join ',')) {
                throw "Incorrect command after $id : $($actual -join ',')"
            }
            return
        }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "No command from $id"
}
try {
    Assert-Command 'EnhancedSessionButton' 0x1002 -Toggle
    Assert-Command 'EnhancedSessionButton' 0x1001 -Toggle
    Assert-Command 'FullScreenButton' 0x1003
    Assert-Command 'PauseVirtualMachineButton' 0x1004 -Toggle
    Assert-Command 'PauseVirtualMachineButton' 0x1005 -Toggle
    Assert-Command 'RestartVirtualMachineButton' 0x1006
    Click-Control 'PowerMenu'
    Assert-Command 'ShutdownAction' 0x1011
    Click-Control 'PowerMenu'
    Assert-Command 'SaveAction' 0x1013
    Click-Control 'PowerMenu'
    Assert-Command 'PowerOffAction' 0x1012
    Assert-Command 'VirtualMachineSettingsButton' 0x1007
    Assert-Command 'RedirectKeyboardNavigationButton' 0x1008 -Toggle
    Assert-Command 'RedirectKeyboardNavigationButton' 0x1009 -Toggle
    Assert-Command 'RestoreVirtualMachineWindowSizeButton' 0x100A
    Assert-Command 'TypeClipboardTextButton' 0x1015
    Assert-Command 'CopyScreenshotButton' 0x100B
    Assert-Command 'RecordVideoButton' 0x100C -Toggle
    Assert-Command 'RecordVideoButton' 0x100C -Toggle
    Assert-Command 'CreateVirtualHardDiskButton' 0x100D
    Assert-Command 'ResizeVirtualHardDiskButton' 0x100E
    Assert-Command 'CompactVirtualHardDiskButton' 0x100F
    Assert-Command 'AboutButton' 0x1010
    Assert-Command 'SponsorButton' 0x1014
    Start-Sleep -Milliseconds 300
    $actual = @(Read-CommandLog)
    if (($actual -join ',') -ne ($expected -join ',')) { throw 'Unexpected trailing command' }
    Write-Host "Verified $($expected.Count) toolbar commands."
} finally {
    if (-not $process.HasExited) {
        [void]$process.CloseMainWindow()
        if (-not $process.WaitForExit(5000)) { Stop-Process -Id $process.Id -Force }
    }
}
