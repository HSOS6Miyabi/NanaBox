# Mile.Cirno distribution and native Dokany driver installation.
$script:CirnoVersion = '1.3.1180.0'
$script:CirnoSha256 = '51abd62b1f85d1ec1e95cf7ada6d1a5ce4d988dd64e62bc641d99090b9c48f32'
$script:CirnoUri = 'https://github.com/ProjectMile/Mile.Cirno/releases/download/1.3.1180.0/Mile.Cirno_1.3.1180.0_Binaries.zip'

function Get-PeMachine([string]$Path) {
    $stream = [IO.File]::OpenRead($Path)
    $reader = [IO.BinaryReader]::new($stream)
    try {
        if ($stream.Length -lt 64 -or $reader.ReadUInt16() -ne 0x5a4d) { throw "Invalid executable: $Path" }
        $stream.Position = 60
        $offset = $reader.ReadUInt32()
        if ($offset -gt ($stream.Length - 6)) { throw "Invalid PE header: $Path" }
        $stream.Position = $offset
        if ($reader.ReadUInt32() -ne 0x4550) { throw "Invalid PE signature: $Path" }
        return $reader.ReadUInt16()
    } finally { $reader.Dispose(); $stream.Dispose() }
}

function Get-GuestArchitecture {
    switch ([System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString()) {
        'X64' { return 'x64' }
        'Arm64' { return 'arm64' }
        default { throw 'A native 64-bit Windows guest is required.' }
    }
}

function Get-ArchitectureMachine([string]$Architecture) {
    switch ($Architecture) {
        'x64' { return 0x8664 }
        'arm64' { return 0xaa64 }
        default { throw "Unsupported architecture: $Architecture" }
    }
}

function Assert-CirnoArchive([string]$Archive) {
    $actual = (Get-FileHash -LiteralPath $Archive -Algorithm SHA256).Hash
    if ($actual -ine $script:CirnoSha256) { throw 'Mile.Cirno package SHA-256 verification failed.' }
}

function Get-CirnoArchive([string]$Archive) {
    if (Test-Path -LiteralPath $Archive -PathType Leaf) {
        Assert-CirnoArchive $Archive
        return
    }
    $parent = Split-Path -Parent ([IO.Path]::GetFullPath($Archive))
    if (-not (Test-Path -LiteralPath $parent -PathType Container)) { throw "Missing package directory: $parent" }
    $temporary = Join-Path $parent ('.cirno-' + [guid]::NewGuid().ToString('N') + '.zip')
    try {
        Invoke-WebRequest -Uri $script:CirnoUri -UseBasicParsing -OutFile $temporary
        Assert-CirnoArchive $temporary
        Move-Item -LiteralPath $temporary -Destination $Archive
    } finally {
        if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary -Force }
    }
}

function Resolve-CirnoFiles([string]$Root,[string]$Architecture) {
    $machine = Get-ArchitectureMachine $Architecture
    $clients = @(Get-ChildItem -LiteralPath $Root -Filter 'Mile.Cirno.exe' -File -Recurse | Where-Object {
        -not ($_.Attributes -band [IO.FileAttributes]::ReparsePoint) -and (Get-PeMachine $_.FullName) -eq $machine
    })
    $drivers = @(Get-ChildItem -LiteralPath $Root -Filter 'Dokan2.sys' -File -Recurse | Where-Object {
        -not ($_.Attributes -band [IO.FileAttributes]::ReparsePoint) -and (Get-PeMachine $_.FullName) -eq $machine
    })
    if ($clients.Count -ne 1 -or $drivers.Count -ne 1) {
        throw "Expected one Mile.Cirno executable and one Dokany driver for $Architecture."
    }
    return @{ ClientDirectory = $clients[0].DirectoryName; Driver = $drivers[0].FullName }
}

function Expand-CirnoPackage([string]$Archive,[string]$Root) {
    Assert-CirnoArchive $Archive
    Expand-Archive -LiteralPath $Archive -DestinationPath $Root
}

function Install-CirnoDriver([string]$Source,[string]$Architecture,[switch]$UseExistingDriver) {
    $destination = Join-Path $env:SystemRoot 'System32\drivers\Dokan2.sys'
    $service = Get-Service -Name 'Dokan2' -ErrorAction SilentlyContinue
    if ($service) {
        if (-not (Test-Path -LiteralPath $destination -PathType Leaf)) { throw 'The installed Dokan2 service has no standard driver file.' }
        if ((Get-Item -LiteralPath $destination -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Unsafe Dokany driver path.' }
        if ((Get-PeMachine $destination) -ne (Get-ArchitectureMachine $Architecture)) { throw 'Installed Dokany driver architecture does not match Windows.' }
        if (-not $UseExistingDriver -and $Source -and
            ((Get-FileHash -LiteralPath $Source).Hash -ine (Get-FileHash -LiteralPath $destination).Hash)) {
            throw 'An unrelated Dokan2 driver is already installed. Use -UseExistingDokany only after checking compatibility.'
        }
        if ($service.Status -ne 'Running') {
            Start-Service -Name Dokan2
            (Get-Service -Name Dokan2).WaitForStatus('Running',[TimeSpan]::FromSeconds(30))
        }
        return
    }
    if ($UseExistingDriver) { throw 'Dokan2 is not installed.' }
    if (-not $Source) { throw 'Specify the Dokany driver from the matching Mile.Cirno release.' }
    if ((Get-PeMachine $Source) -ne (Get-ArchitectureMachine $Architecture)) { throw 'Wrong Dokany driver architecture.' }
    if (Test-Path -LiteralPath $destination) { throw 'An unregistered Dokany driver file already exists; it will not be overwritten.' }
    $created = $false
    $copied = $false
    try {
        Copy-Item -LiteralPath $Source -Destination $destination
        $copied = $true
        & "$env:SystemRoot\System32\sc.exe" create Dokan2 type= filesys start= auto error= normal binPath= $destination
        if ($LASTEXITCODE -ne 0) { throw 'Cannot register Dokany filesystem driver.' }
        $created = $true
        Start-Service -Name Dokan2
        (Get-Service -Name Dokan2).WaitForStatus('Running',[TimeSpan]::FromSeconds(30))
    } catch {
        if ($created) {
            $current = Get-Service -Name Dokan2 -ErrorAction SilentlyContinue
            if ($current -and $current.Status -eq 'Stopped') {
                & "$env:SystemRoot\System32\sc.exe" delete Dokan2 | Out-Null
                if ($LASTEXITCODE -eq 0 -and $copied) { Remove-Item -LiteralPath $destination -Force -ErrorAction SilentlyContinue }
            }
        } elseif ($copied -and -not (Get-Service -Name Dokan2 -ErrorAction SilentlyContinue)) {
            Remove-Item -LiteralPath $destination -Force -ErrorAction SilentlyContinue
        }
        throw
    }
}
