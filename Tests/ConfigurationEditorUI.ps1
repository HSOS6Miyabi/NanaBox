param(
    [Parameter(Mandatory=$true)][string]$Executable,
    [string]$OutputDirectory = (Join-Path $env:TEMP ('NanaBox-Editor-' + [guid]::NewGuid()))
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$marker = 'NanaBox editor ' + [guid]::NewGuid().ToString('N')
# Exercise spaces and non-ASCII characters without depending on script encoding.
$file = Join-Path $OutputDirectory ($marker + ' ' + [char]0x914D + [char]0x7F6E + '.7b')
$initial = @{NanaBox=@{Type='VirtualMachine';Version=1;Name='Editor test';GuestType='Windows';ProcessorCount=2;MemorySize=2048;Gpu=@{AssignmentMode='Disabled'}}} | ConvertTo-Json -Depth 10
[IO.File]::WriteAllText($file, $initial, (New-Object Text.UTF8Encoding($true)))
$before = Get-FileHash -LiteralPath $file
$process = Start-Process -FilePath (Resolve-Path $Executable).Path -ArgumentList ('--edit-config "' + $file + '"') -PassThru
$editor = $null
try {
    $root = [System.Windows.Automation.AutomationElement]::RootElement
    $pidCondition = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::ProcessIdProperty,[int]$process.Id)
    $buttonCondition = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::AutomationIdProperty,'OpenButton')
    $until = [DateTime]::UtcNow.AddSeconds(45)
    $button = $null
    do {
        if ($process.HasExited) { throw "Settings process exited: $($process.ExitCode)" }
        foreach ($window in $root.FindAll([System.Windows.Automation.TreeScope]::Children,$pidCondition)) {
            $button = $window.FindFirst([System.Windows.Automation.TreeScope]::Descendants,$buttonCondition)
            if ($button) { break }
        }
        if ($button) { break }
        Start-Sleep -Milliseconds 250
    } while ([DateTime]::UtcNow -lt $until)
    if (-not $button) { throw 'Open configuration button not found' }
    $pattern = $button.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern)
    $pattern.Invoke()
    $until = [DateTime]::UtcNow.AddSeconds(40)
    do {
        $editor = Get-Process -Name notepad -ErrorAction SilentlyContinue | Where-Object {
            $_.MainWindowTitle -like "*$marker*"
        } | Select-Object -First 1
        if ($editor) { break }
        Start-Sleep -Milliseconds 300
    } while ([DateTime]::UtcNow -lt $until)
    if (-not $editor) {
        $titles = (Get-Process -Name notepad -ErrorAction SilentlyContinue | Select-Object -ExpandProperty MainWindowTitle) -join '; '
        throw "Notepad did not open the requested configuration. Window titles: $titles"
    }
    if ((Get-FileHash -LiteralPath $file).Hash -ne $before.Hash) { throw 'Opening the editor modified the configuration' }
    Write-Host "Opened the requested configuration in Notepad: $($editor.MainWindowTitle)"
} finally {
    if ($editor -and -not $editor.HasExited) { [void]$editor.CloseMainWindow() }
    if (-not $process.HasExited) {
        [void]$process.CloseMainWindow()
        if (-not $process.WaitForExit(5000)) { $process.Kill() }
    }
}
