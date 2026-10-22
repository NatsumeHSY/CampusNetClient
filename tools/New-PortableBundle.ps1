[CmdletBinding()]
param(
    [string]$PackageRoot = (Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'build\package\Release'),
    [string]$OutputPath,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$propsPath = Join-Path $repoRoot 'version.props'
[xml]$props = Get-Content -Raw -LiteralPath $propsPath
$version = [string]$props.SelectSingleNode('/Project/PropertyGroup/CampusNetSemVer').InnerText
if ([string]::IsNullOrWhiteSpace($version)) { throw 'Central version is empty.' }
$package = (Resolve-Path -LiteralPath $PackageRoot).Path
foreach ($required in @('CampusNetLauncher.exe', 'bin\CampusNetClient.exe',
                        'bin\CampusNetService.exe', 'bin\CampusNetUpdater.exe',
                        'bin\version.json')) {
    if (-not (Test-Path -LiteralPath (Join-Path $package $required) -PathType Leaf)) {
        throw "Portable package is missing required file: $required"
    }
}
$localVersion = Get-Content -Raw -LiteralPath (Join-Path $package 'bin\version.json') | ConvertFrom-Json
if ($localVersion.version -cne $version -or $localVersion.channel -cne 'portable') {
    throw 'Portable version.json does not match version.props and the portable channel.'
}
if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    $OutputPath = Join-Path (Join-Path (Split-Path -Parent $repoRoot) 'build\package') `
        "CampusNetClient-$version-portable.zip"
}
$OutputPath = [IO.Path]::GetFullPath($OutputPath)
$outputDirectory = [IO.Path]::GetDirectoryName($OutputPath)
[IO.Directory]::CreateDirectory($outputDirectory) | Out-Null
if ((Test-Path -LiteralPath $OutputPath) -and -not $Force) {
    throw "Portable archive already exists; choose another path or pass -Force: $OutputPath"
}

Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
if ((Test-Path -LiteralPath $OutputPath) -and $Force) {
    Remove-Item -LiteralPath $OutputPath -Force
}
$archive = [IO.Compression.ZipFile]::Open($OutputPath, [IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($directory in (Get-ChildItem -LiteralPath $package -Recurse -Directory -Force)) {
        $attributes = [IO.File]::GetAttributes($directory.FullName)
        if (($attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Reparse-point directory is not allowed in the portable package: $($directory.FullName)"
        }
    }
    foreach ($file in (Get-ChildItem -LiteralPath $package -Recurse -File | Sort-Object FullName)) {
        $attributes = [IO.File]::GetAttributes($file.FullName)
        if (($attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Reparse-point file is not allowed in the portable package: $($file.FullName)"
        }
        if ($file.Extension -in @('.pdb', '.ilk', '.iobj', '.ipdb', '.exp', '.lib')) {
            continue
        }
        $relative = [IO.Path]::GetRelativePath($package, $file.FullName) -replace '\\', '/'
        if ($relative.StartsWith('/') -or $relative -match '(^|/)\.\.?(/|$)') {
            throw "Unsafe portable archive path: $relative"
        }
        [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
            $archive, $file.FullName, $relative,
            [IO.Compression.CompressionLevel]::Optimal)
    }
} finally {
    $archive.Dispose()
}

$check = [IO.Compression.ZipFile]::OpenRead($OutputPath)
try {
    $names = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($entry in $check.Entries) {
        if (-not $names.Add($entry.FullName) -or
            $entry.FullName.StartsWith('/') -or
            $entry.FullName -match '(^|/)\.\.?(/|$)' -or
            [IO.Path]::GetExtension($entry.FullName) -in @('.pdb', '.ilk', '.iobj', '.ipdb', '.exp', '.lib')) {
            throw "Portable archive validation failed for entry: $($entry.FullName)"
        }
    }
    if ($names.Count -lt 5 -or -not $names.Contains('CampusNetLauncher.exe') -or
        -not $names.Contains('bin/CampusNetClient.exe') -or
        -not $names.Contains('bin/CampusNetUpdater.exe')) {
        throw 'Portable archive does not contain the expected package layout.'
    }
} finally {
    $check.Dispose()
}
Write-Output "Portable archive created: $OutputPath"
