param([string]$Archive = (Join-Path $PSScriptRoot 'Cirno.zip'))
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'CirnoPackage.ps1')
Get-CirnoArchive $Archive
$temporary = Join-Path ([IO.Path]::GetTempPath()) ('NanaBox-Cirno-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $temporary | Out-Null
try {
    Expand-CirnoPackage $Archive $temporary
    foreach ($arch in @('x64','arm64')) {
        $files = Resolve-CirnoFiles $temporary $arch
        Write-Host "$arch Mile.Cirno and Dokany verified."
    }
} finally { Remove-Item -LiteralPath $temporary -Force -Recurse }
