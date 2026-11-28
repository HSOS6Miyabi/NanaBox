param([string]$Installer)
$ErrorActionPreference = 'Stop'
$expected = '36ce01f9eedbbe6842b7fb4bedc79d7d4f71983e8aceb6877b46a07b331c7e19'
$temporary = $false
if (-not $Installer) {
    $Installer = Join-Path ([IO.Path]::GetTempPath()) ('WinFsp-' + [guid]::NewGuid().ToString('N') + '.msi')
    $temporary = $true
}
try {
    if ($temporary) {
        Invoke-WebRequest -Uri 'https://github.com/winfsp/winfsp/releases/download/v2.2/winfsp-2.2.26323.msi' -OutFile $Installer
    }
    $Installer = (Resolve-Path -LiteralPath $Installer).Path
    if ((Get-FileHash -LiteralPath $Installer -Algorithm SHA256).Hash -ne $expected) { throw 'WinFsp installer checksum mismatch.' }
    $process = Start-Process -FilePath msiexec.exe -ArgumentList @('/i',('"' + $Installer + '"'),'ADDLOCAL=ALL','/qn','/norestart') -Wait -PassThru
    if ($process.ExitCode -ne 0) { throw "WinFsp SDK installation failed: $($process.ExitCode)" }
} finally {
    if ($temporary -and (Test-Path -LiteralPath $Installer)) { Remove-Item -LiteralPath $Installer -Force }
}
