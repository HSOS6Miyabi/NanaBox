# Invoke a control whose handler waits for a modal dialog, while the caller
# locates and confirms the dialog using UI Automation.
function Invoke-ModalAction([int]$processId,[string]$id,[scriptblock]$Confirm) {
    Write-Host "Invoke modal action: $id"
    $job = Start-Job -ArgumentList $processId,$id -ScriptBlock {
        param([int]$targetProcessId,[string]$automationId)
        $ErrorActionPreference = 'Stop'
        Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
        $root = [System.Windows.Automation.AutomationElement]::RootElement
        $pidCondition = New-Object System.Windows.Automation.PropertyCondition(
            [System.Windows.Automation.AutomationElement]::ProcessIdProperty, $targetProcessId)
        $idCondition = New-Object System.Windows.Automation.PropertyCondition(
            [System.Windows.Automation.AutomationElement]::AutomationIdProperty, $automationId)
        $deadline = [DateTime]::UtcNow.AddSeconds(20)
        do {
            foreach ($window in $root.FindAll([System.Windows.Automation.TreeScope]::Children,$pidCondition)) {
                $element = $window.FindFirst([System.Windows.Automation.TreeScope]::Descendants,$idCondition)
                if ($null -eq $element) { continue }
                $scroll = $null
                if ($element.TryGetCurrentPattern([System.Windows.Automation.ScrollItemPattern]::Pattern,[ref]$scroll)) {
                    $scroll.ScrollIntoView()
                }
                $element.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
                return
            }
            Start-Sleep -Milliseconds 200
        } while ([DateTime]::UtcNow -lt $deadline)
        throw "Modal action control unavailable: $automationId"
    }
    try {
        & $Confirm
        if (-not (Wait-Job -Job $job -Timeout 30)) {
            throw "Modal action did not complete after confirmation: $id"
        }
        Receive-Job -Job $job -ErrorAction Stop
        if ($job.State -ne 'Completed') {
            throw "Modal action failed: $id ($($job.State))"
        }
    } finally {
        Remove-Job -Job $job -Force
    }
}
