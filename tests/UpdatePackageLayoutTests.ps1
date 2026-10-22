[CmdletBinding()]
param(
    [string]$PortablePackageRoot = (Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'build\package\Release'),
    [string]$PortableArchive = (Join-Path (Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'build\package') 'CampusNetClient-0.1.0-portable.zip'),
    [string]$MsiPackage = (Join-Path (Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'build\package') 'CampusNetClient-0.1.0.msi'),
    [string]$MsiFileInventory = (Join-Path (Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'build\package') 'CampusNetClient-0.1.0.msi.files.json'),
    [string]$RemoteManifest = (Join-Path (Split-Path -Parent $PSScriptRoot) 'version.json')
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path -LiteralPath $PortablePackageRoot).Path
$archivePath = (Resolve-Path -LiteralPath $PortableArchive).Path
$msiPath = (Resolve-Path -LiteralPath $MsiPackage).Path
$inventoryPath = (Resolve-Path -LiteralPath $MsiFileInventory).Path
$manifestPath = (Resolve-Path -LiteralPath $RemoteManifest).Path
$repoRoot = Split-Path -Parent $PSScriptRoot
[xml]$props = Get-Content -Raw -LiteralPath (Join-Path $repoRoot 'version.props')
$version = [string]$props.SelectSingleNode('/Project/PropertyGroup/CampusNetSemVer').InnerText

foreach ($required in @('CampusNetLauncher.exe', 'bin\CampusNetClient.exe',
                        'bin\CampusNetService.exe', 'bin\CampusNetUpdater.exe',
                        'bin\version.json')) {
    if (-not (Test-Path -LiteralPath (Join-Path $root $required) -PathType Leaf)) {
        throw "Portable package is missing required file: $required"
    }
}
$rootNames = @(Get-ChildItem -LiteralPath $root -Force | ForEach-Object Name | Sort-Object)
if (($rootNames -join '|') -cne 'bin|CampusNetLauncher.exe') {
    throw "Portable package root has unexpected entries: $($rootNames -join ', ')"
}
$local = Get-Content -Raw -LiteralPath (Join-Path $root 'bin\version.json') | ConvertFrom-Json
if ($local.schemaVersion -ne 1 -or $local.version -cne $version -or $local.channel -cne 'portable') {
    throw 'Portable package version.json does not match the central version.'
}

Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
function Get-StreamSha256([IO.Stream]$stream) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return ([Convert]::ToHexString($sha.ComputeHash($stream))).ToLowerInvariant() }
    finally { $sha.Dispose() }
}
function Get-FileSha256([string]$path) {
    return (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
}
$expectedPortable = @{}
foreach ($file in (Get-ChildItem -LiteralPath $root -Recurse -File | Sort-Object FullName)) {
    if ($file.Extension -in @('.pdb', '.ilk', '.iobj', '.ipdb', '.exp', '.lib')) {
        continue
    }
    $relative = [IO.Path]::GetRelativePath($root, $file.FullName) -replace '\\', '/'
    $expectedPortable[$relative] = Get-FileSha256 $file.FullName
}
$zip = [IO.Compression.ZipFile]::OpenRead($archivePath)
try {
    $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($entry in $zip.Entries) {
        if (-not $seen.Add($entry.FullName) -or $entry.FullName.StartsWith('/') -or
            $entry.FullName -match '(^|/)\.\.?(/|$)' -or
            [IO.Path]::GetExtension($entry.FullName) -in @('.pdb', '.ilk', '.iobj', '.ipdb', '.exp', '.lib')) {
            throw "Portable ZIP contains a duplicate, unsafe, or excluded entry: $($entry.FullName)"
        }
        if (-not $expectedPortable.ContainsKey($entry.FullName)) {
            throw "Portable ZIP contains an entry absent from the package: $($entry.FullName)"
        }
        $stream = $entry.Open()
        try { $actualHash = Get-StreamSha256 $stream } finally { $stream.Dispose() }
        if ($actualHash -cne $expectedPortable[$entry.FullName]) {
            throw "Portable ZIP file hash differs from the package: $($entry.FullName)"
        }
    }
    if ($seen.Count -ne $expectedPortable.Count) {
        throw 'Portable ZIP does not contain every package file exactly once.'
    }
} finally {
    $zip.Dispose()
}

$inventory = Get-Content -Raw -LiteralPath $inventoryPath | ConvertFrom-Json -AsHashtable
if ($inventory.schemaVersion -ne 1 -or $inventory.version -cne $version -or
    $inventory.channel -cne 'msi' -or $inventory.files.Count -eq 0 -or
    -not $inventory.files.Contains('CampusNetClient.exe') -or
    -not $inventory.files.Contains('CampusNetService.exe') -or
    -not $inventory.files.Contains('CampusNetUpdater.exe') -or
    -not $inventory.files.Contains('version.json')) {
    throw 'MSI installation inventory is invalid or incomplete.'
}
if ((Get-FileSha256 $msiPath) -notmatch '\A[0-9a-f]{64}\z' -or (Get-Item -LiteralPath $msiPath).Length -le 0) {
    throw 'MSI package is empty or has an invalid SHA-256.'
}

$manifest = Get-Content -Raw -LiteralPath $manifestPath | ConvertFrom-Json -AsHashtable
if ($manifest.schemaVersion -ne 1 -or $manifest.version -cne $version -or
    $manifest.artifacts.portable.url -notmatch '^https://github\.com/NatsumeHSY/CampusNetClient/releases/download/' -or
    $manifest.artifacts.msi.url -notmatch '^https://github\.com/NatsumeHSY/CampusNetClient/releases/download/') {
    throw 'Remote manifest schema, version, or fixed GitHub release URLs are invalid.'
}
foreach ($channel in @('portable', 'msi')) {
    $artifact = $manifest.artifacts[$channel]
    $asset = if ($channel -eq 'portable') { $archivePath } else { $msiPath }
    if ($artifact.sizeBytes -ne (Get-Item -LiteralPath $asset).Length -or
        $artifact.sha256 -cne (Get-FileSha256 $asset) -or
        $artifact.files.Count -eq 0) {
        throw "$channel remote manifest artifact does not match the built package."
    }
}
foreach ($name in $expectedPortable.Keys) {
    if (-not $manifest.artifacts.portable.files.Contains($name) -or
        $manifest.artifacts.portable.files[$name] -cne $expectedPortable[$name]) {
        throw "Portable file hash in remote manifest does not match: $name"
    }
}
foreach ($path in $inventory.files.Keys) {
    if (-not $manifest.artifacts.msi.files.Contains($path) -or
        $manifest.artifacts.msi.files[$path] -cne $inventory.files[$path]) {
        throw "MSI file hash in remote manifest does not match: $path"
    }
}

Write-Output 'UpdatePackageLayoutTests: PASS'
