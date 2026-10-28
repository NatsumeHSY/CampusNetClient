[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$PortablePackageRoot,

    [Parameter(Mandatory = $true)]
    [string]$PortableArchive,

    [Parameter(Mandatory = $true)]
    [string]$MsiPackage,

    [Parameter(Mandatory = $true)]
    [string]$MsiFileInventory,

    [Parameter(Mandatory = $true)]
    [string]$ReleaseTag,

    [string]$ReleaseNotes = '',
    [string]$PublishedAt = ([DateTime]::UtcNow.ToString("yyyy-MM-dd'T'HH:mm:ss'Z'")),
    [string]$OutputPath,
    [string]$PortableAssetName,
    [string]$MsiAssetName,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$propsPath = Join-Path $repoRoot 'version.props'
if (-not (Test-Path -LiteralPath $propsPath -PathType Leaf)) {
    throw "Central version file is missing: $propsPath"
}
[xml]$props = Get-Content -Raw -LiteralPath $propsPath
$version = [string]$props.SelectSingleNode('/Project/PropertyGroup/CampusNetSemVer').InnerText
$numeric = '(?:0|[1-9][0-9]*)'
$preIdentifier = '(?:0|[1-9][0-9]*|[0-9A-Za-z-]*[A-Za-z-][0-9A-Za-z-]*)'
$buildIdentifier = '[0-9A-Za-z-]+'
$semVerPattern = "\A$numeric\.$numeric\.$numeric(?:-$preIdentifier(?:\.$preIdentifier)*)?(?:\+$buildIdentifier(?:\.$buildIdentifier)*)?\z"
if ($version.Length -gt 128 -or $version -cnotmatch $semVerPattern) {
    throw "Central version is not a valid SemVer 2.0.0 value: '$version'"
}
if ($ReleaseTag -notmatch '\A[A-Za-z0-9_.-]{1,128}\z') {
    throw 'ReleaseTag contains characters that are unsafe in a GitHub release URL.'
}
try {
    $timestamp = [DateTime]::ParseExact(
        $PublishedAt, "yyyy-MM-dd'T'HH:mm:ss'Z'", [Globalization.CultureInfo]::InvariantCulture,
        [Globalization.DateTimeStyles]::AssumeUniversal -bor [Globalization.DateTimeStyles]::AdjustToUniversal)
} catch {
    throw "PublishedAt must be canonical UTC in yyyy-MM-ddTHH:mm:ssZ form: $PublishedAt"
}
if ($ReleaseNotes.Length -gt 32768) { throw 'ReleaseNotes exceeds 32 KiB.' }

$portableRoot = (Resolve-Path -LiteralPath $PortablePackageRoot).Path
$portableZip = (Resolve-Path -LiteralPath $PortableArchive).Path
$msiPath = (Resolve-Path -LiteralPath $MsiPackage).Path
$inventoryPath = (Resolve-Path -LiteralPath $MsiFileInventory).Path
foreach ($required in @('CampusNetLauncher.exe', 'bin\CampusNetClient.exe',
                        'bin\CampusNetService.exe', 'bin\CampusNetUpdater.exe',
                        'bin\version.json')) {
    if (-not (Test-Path -LiteralPath (Join-Path $portableRoot $required) -PathType Leaf)) {
        throw "Portable package root is missing: $required"
    }
}
if ([string]::IsNullOrWhiteSpace($PortableAssetName)) { $PortableAssetName = "CampusNetClient-$version-portable.zip" }
if ([string]::IsNullOrWhiteSpace($MsiAssetName)) { $MsiAssetName = "CampusNetClient-$version.msi" }
foreach ($assetName in @($PortableAssetName, $MsiAssetName)) {
    if ($assetName -notmatch '\A[A-Za-z0-9_.-]{1,180}\z') {
        throw "Release asset name is unsafe: $assetName"
    }
}

function Get-Sha256([string]$path) {
    return (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
}
function Get-RelativeFileMap([string]$root) {
    $map = [ordered]@{}
    $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($directory in (Get-ChildItem -LiteralPath $root -Recurse -Directory -Force)) {
        $attributes = [IO.File]::GetAttributes($directory.FullName)
        if (($attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Reparse-point directory is not allowed in a release inventory: $($directory.FullName)"
        }
    }
    foreach ($file in (Get-ChildItem -LiteralPath $root -Recurse -File | Sort-Object FullName)) {
        $attributes = [IO.File]::GetAttributes($file.FullName)
        if (($attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Reparse-point file is not allowed in release inventory: $($file.FullName)"
        }
        if ($file.Extension -in @('.pdb', '.ilk', '.iobj', '.ipdb', '.exp', '.lib')) {
            continue
        }
        $relative = [IO.Path]::GetRelativePath($root, $file.FullName) -replace '\\', '/'
        if ($relative.StartsWith('/') -or $relative -match '(^|/)\.\.?(/|$)' -or
            -not $seen.Add($relative)) {
            throw "Unsafe or duplicate release file path: $relative"
        }
        if ($map.Count -ge 1024) { throw 'Release file inventory exceeds 1024 files.' }
        $map[$relative] = Get-Sha256 $file.FullName
    }
    return $map
}

Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$portableFiles = Get-RelativeFileMap $portableRoot
if (-not $portableFiles.Contains('CampusNetLauncher.exe') -or
    -not $portableFiles.Contains('bin/version.json')) {
    throw 'Portable package file inventory is incomplete.'
}
$zip = [IO.Compression.ZipFile]::OpenRead($portableZip)
try {
    $zipNames = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($entry in $zip.Entries) {
        if ($entry.FullName.EndsWith('/') -or -not $zipNames.Add($entry.FullName)) {
            throw "Portable ZIP has a duplicate or directory-only entry: $($entry.FullName)"
        }
    }
    if ($zipNames.Count -ne $portableFiles.Count) {
        throw 'Portable ZIP contents do not match the package directory file count.'
    }
    foreach ($name in $portableFiles.Keys) {
        if (-not $zipNames.Contains($name)) { throw "Portable ZIP is missing package file: $name" }
    }
} finally {
    $zip.Dispose()
}

$portableLocal = Get-Content -Raw -LiteralPath (Join-Path $portableRoot 'bin\version.json') | ConvertFrom-Json
if ($portableLocal.version -cne $version -or $portableLocal.channel -cne 'portable') {
    throw 'Portable local version.json does not match the central version.'
}
$inventory = Get-Content -Raw -LiteralPath $inventoryPath | ConvertFrom-Json -AsHashtable
if ($inventory.schemaVersion -ne 1 -or $inventory.version -cne $version -or
    $inventory.channel -cne 'msi' -or $inventory.files.Count -eq 0 -or
    $inventory.files.Count -gt 1024) {
    throw 'MSI file inventory is malformed or does not match the central version.'
}
$msiFiles = [ordered]@{}
$seenMsi = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($entry in $inventory.files.GetEnumerator()) {
    $path = [string]$entry.Key
    $digest = [string]$entry.Value
    if ($path.StartsWith('/') -or $path -match '(^|/)\.\.?(/|$)' -or
        $path -match '^[A-Za-z]:' -or -not $seenMsi.Add($path) -or
        $digest -cnotmatch '\A[0-9a-f]{64}\z') {
        throw "MSI file inventory contains an unsafe path or SHA-256: $path"
    }
    $msiFiles[$path] = $digest
}
if (-not $msiFiles.Contains('CampusNetClient.exe') -or
    -not $msiFiles.Contains('CampusNetUpdater.exe') -or
    -not $msiFiles.Contains('CampusNetService.exe') -or
    -not $msiFiles.Contains('version.json')) {
    throw 'MSI inventory does not include the client, service, updater, and version record.'
}

$baseUrl = "https://github.com/NatsumeHSY/CampusNetClient/releases/download/$ReleaseTag"
$portableUrl = "$baseUrl/$PortableAssetName"
$msiUrl = "$baseUrl/$MsiAssetName"
$record = [ordered]@{
    schemaVersion = 1
    version = $version
    publishedAt = $timestamp.ToUniversalTime().ToString("yyyy-MM-dd'T'HH:mm:ss'Z'")
    releaseNotes = $ReleaseNotes
    artifacts = [ordered]@{
        portable = [ordered]@{
            url = $portableUrl
            sizeBytes = ([IO.FileInfo]::new($portableZip)).Length
            sha256 = Get-Sha256 $portableZip
            files = $portableFiles
        }
        msi = [ordered]@{
            url = $msiUrl
            sizeBytes = ([IO.FileInfo]::new($msiPath)).Length
            sha256 = Get-Sha256 $msiPath
            files = $msiFiles
        }
    }
}

if ([string]::IsNullOrWhiteSpace($OutputPath)) { $OutputPath = Join-Path (Split-Path -Parent $repoRoot) 'version.json' }
$OutputPath = [IO.Path]::GetFullPath($OutputPath)
if ((Test-Path -LiteralPath $OutputPath) -and -not $Force) {
    throw "Update manifest already exists; choose another OutputPath or pass -Force: $OutputPath"
}
$outputDirectory = [IO.Path]::GetDirectoryName($OutputPath)
[IO.Directory]::CreateDirectory($outputDirectory) | Out-Null
$temporaryPath = Join-Path $outputDirectory ('.' + [IO.Path]::GetFileName($OutputPath) + '.' + [guid]::NewGuid().ToString('N') + '.tmp')
try {
    [IO.File]::WriteAllText(
        $temporaryPath,
        ((ConvertTo-Json -InputObject $record -Depth 8) + [Environment]::NewLine),
        [Text.UTF8Encoding]::new($false))
    [IO.File]::Move($temporaryPath, $OutputPath, $Force)
} finally {
    if (Test-Path -LiteralPath $temporaryPath) { Remove-Item -LiteralPath $temporaryPath -Force }
}
Write-Output "Remote update manifest created: $OutputPath"
