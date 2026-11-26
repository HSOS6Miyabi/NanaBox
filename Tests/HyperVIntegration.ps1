param(
    [Parameter(Mandatory=$true)][string]$Executable,
    [Parameter(Mandatory=$true)][string]$WindowsConfig,
    [Parameter(Mandatory=$true)][string]$LinuxConfig
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
$Executable = (Resolve-Path $Executable).Path
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
function Test-Guest([string]$path,[string]$guestType) {
    $path = (Resolve-Path -LiteralPath $path).Path
    $cfg = (Get-Content -LiteralPath $path -Raw | ConvertFrom-Json).NanaBox
    if ($cfg.Name -notlike 'NanaBox-CI-*') { throw 'Test VM name must start with NanaBox-CI-' }
    if ($cfg.GuestType -ne $guestType) { throw "Expected $guestType guest" }
    if ((& hcsdiag.exe list | Out-String) -match [regex]::Escape($cfg.Name)) { throw "VM already running: $($cfg.Name)" }
    $process = Start-Process -FilePath $Executable -ArgumentList ('"' + $path + '"') -PassThru
    try {
        Wait-VMState $cfg.Name Present
        Toggle-UI $process 'PauseVirtualMachineButton'
        Start-Sleep -Seconds 2
        Toggle-UI $process 'PauseVirtualMachineButton'
        Invoke-UI $process 'VirtualMachineSettingsButton'
        $null = Find-Element $process 'ApplyButton'
        # Close only the settings window, without applying any changes.
        $p = Find-Element $process 'ApplyButton'
        $walk = [System.Windows.Automation.TreeWalker]::RawViewWalker
        while ($p -and $p.Current.ControlType -ne [System.Windows.Automation.ControlType]::Window) { $p = $walk.GetParent($p) }
        if (-not $p) { throw 'Settings window not found' }
        $p.GetCurrentPattern([System.Windows.Automation.WindowPattern]::Pattern).Close()
        Invoke-UI $process 'PowerMenu'
        Invoke-UI $process 'ShutdownAction'
        Wait-VMState $cfg.Name Absent 180
        if (-not $process.WaitForExit(30000)) { throw 'App did not exit after shutdown' }
        Write-Host "$guestType power and running settings UI passed"
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
Test-Guest $WindowsConfig Windows
Test-Guest $LinuxConfig Linux
