[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$PackageBinDir,

    [string]$OutputPath,
    [string]$WixPath = 'wix.exe',
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$versionPath = Join-Path $repoRoot 'version.props'
if (-not (Test-Path -LiteralPath $versionPath -PathType Leaf)) {
    throw "Central version file is missing: $versionPath"
}
[xml]$props = Get-Content -Raw -LiteralPath $versionPath
$versionNodes = $props.SelectNodes('/Project/PropertyGroup/CampusNetSemVer')
if ($null -eq $versionNodes -or $versionNodes.Count -ne 1) {
    throw 'version.props must define exactly one CampusNetSemVer value.'
}
$semanticVersion = [string]$versionNodes[0].InnerText
$numeric = '(?:0|[1-9][0-9]*)'
$preIdentifier = '(?:0|[1-9][0-9]*|[0-9A-Za-z-]*[A-Za-z-][0-9A-Za-z-]*)'
$buildIdentifier = '[0-9A-Za-z-]+'
$semVerPattern = "\A$numeric\.$numeric\.$numeric(?:-$preIdentifier(?:\.$preIdentifier)*)?(?:\+$buildIdentifier(?:\.$buildIdentifier)*)?\z"
if ($semanticVersion.Length -gt 128 -or $semanticVersion -cnotmatch $semVerPattern) {
    throw "Central version is not a valid SemVer 2.0.0 value: '$semanticVersion'"
}

$core = ($semanticVersion -split '[-+]', 2)[0]
$parts = @($core.Split('.'))
$major = [uint32]$parts[0]
$minor = [uint32]$parts[1]
$build = [uint32]$parts[2]
if ($major -gt 255 -or $minor -gt 255 -or $build -gt 65535) {
    throw "SemVer '$semanticVersion' cannot be represented by Windows Installer."
}
$msiVersion = "$major.$minor.$build"

$packageRoot = (Resolve-Path -LiteralPath $PackageBinDir).Path
$sourcePath = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot 'CampusNetClient.wxs')).Path
$requiredFiles = @('CampusNetClient.exe', 'CampusNetService.exe', 'CampusNetUpdater.exe')
foreach ($name in $requiredFiles) {
    if (-not (Test-Path -LiteralPath (Join-Path $packageRoot $name) -PathType Leaf)) {
        throw "Required MSI payload file is missing: $name"
    }
}

$buildRoot = Join-Path (Split-Path -Parent $repoRoot) 'build'
$defaultOutput = Join-Path (Join-Path $buildRoot 'package') "CampusNetClient-$semanticVersion.msi"
if ([string]::IsNullOrWhiteSpace($OutputPath)) { $OutputPath = $defaultOutput }
$OutputPath = [IO.Path]::GetFullPath($OutputPath)
$outputDirectory = [IO.Path]::GetDirectoryName($OutputPath)
[IO.Directory]::CreateDirectory($outputDirectory) | Out-Null
$layoutPath = "$OutputPath.files.json"
if ((Test-Path -LiteralPath $OutputPath) -and -not $Force) {
    throw "MSI already exists; select another OutputPath or pass -Force: $OutputPath"
}

$payloadRoot = Join-Path $outputDirectory ('.msi-stage-' + [guid]::NewGuid().ToString('N'))
$wxsPath = Join-Path $payloadRoot 'CampusNetClient.generated.wxs'
$versionFile = Join-Path $payloadRoot 'version.json'

function ConvertTo-XmlText([string]$value) {
    return [System.Security.SecurityElement]::Escape($value)
}

function Get-Sha256([string]$path) {
    return (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
}

try {
    [IO.Directory]::CreateDirectory($payloadRoot) | Out-Null
    $localVersion = [ordered]@{
        schemaVersion = 1
        version = $semanticVersion
        channel = 'msi'
    }
    [IO.File]::WriteAllText(
        $versionFile,
        ((ConvertTo-Json -InputObject $localVersion -Compress) + [Environment]::NewLine),
        [Text.UTF8Encoding]::new($false))

    $files = [ordered]@{}
    $seenPaths = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $paths = [Collections.Generic.List[string]]::new()
    foreach ($directory in (Get-ChildItem -LiteralPath $packageRoot -Recurse -Directory -Force)) {
        $attributes = [IO.File]::GetAttributes($directory.FullName)
        if (($attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Reparse-point directory is not allowed in the MSI payload: $($directory.FullName)"
        }
    }
    foreach ($file in (Get-ChildItem -LiteralPath $packageRoot -Recurse -File | Sort-Object FullName)) {
        $attributes = [IO.File]::GetAttributes($file.FullName)
        if (($attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Reparse-point file is not allowed in the MSI payload: $($file.FullName)"
        }
        if ($file.Extension -in @('.pdb', '.ilk', '.iobj', '.ipdb', '.exp', '.lib')) {
            continue
        }
        $relative = [IO.Path]::GetRelativePath($packageRoot, $file.FullName)
        if ($relative -eq 'version.json') {
            continue
        }
        $target = Join-Path $payloadRoot $relative
        [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($target)) | Out-Null
        Copy-Item -LiteralPath $file.FullName -Destination $target
        $manifestPath = $relative -replace '\\', '/'
        if (-not $seenPaths.Add($manifestPath)) {
            throw "Case-insensitive duplicate MSI payload path: $manifestPath"
        }
        $files[$manifestPath] = Get-Sha256 $target
        $paths.Add($relative)
    }
    if (-not $seenPaths.Add('version.json')) {
        throw 'Generated MSI version.json collides with a payload file.'
    }
    $files['version.json'] = Get-Sha256 $versionFile
    $paths.Add('version.json')

    $builder = [Text.StringBuilder]::new()
    [void]$builder.AppendLine('<?xml version="1.0" encoding="UTF-8"?>')
    [void]$builder.AppendLine('<Wix xmlns="http://wixtoolset.org/schemas/v4/wxs">')
    [void]$builder.AppendLine('  <Fragment>')
    [void]$builder.AppendLine('    <ComponentGroup Id="ClientPayload" Directory="INSTALLFOLDER">')
    $componentIndex = 0
    foreach ($relative in $paths | Sort-Object) {
        if ($relative -in @('CampusNetClient.exe', 'CampusNetService.exe', 'CampusNetUpdater.exe', 'version.json')) {
            continue
        }
        $relativeDirectory = [IO.Path]::GetDirectoryName($relative)
        $attributes = if ([string]::IsNullOrWhiteSpace($relativeDirectory)) {
            ''
        } else {
            ' Subdirectory="' + (ConvertTo-XmlText ($relativeDirectory -replace '/', '\')) + '"'
        }
        $source = ConvertTo-XmlText (Join-Path $payloadRoot $relative)
        [void]$builder.AppendLine(('      <Component Id="RuntimePayload{0:D4}" Guid="*"{1}>' -f $componentIndex, $attributes))
        [void]$builder.AppendLine(('        <File Source="{0}" KeyPath="yes" />' -f $source))
        [void]$builder.AppendLine('      </Component>')
        $componentIndex++
    }
    [void]$builder.AppendLine('    </ComponentGroup>')
    [void]$builder.AppendLine('  </Fragment>')
    [void]$builder.AppendLine('</Wix>')
    [IO.File]::WriteAllText($wxsPath, $builder.ToString(), [Text.UTF8Encoding]::new($false))

    $namespaceBytes = [Guid]::Parse('{9B7E1B47-0F50-4F63-A1FC-6A3B8E2D5A17}').ToByteArray()
    $nameBytes = [Text.Encoding]::UTF8.GetBytes("CampusNetClient|x64|$msiVersion")
    $hashInput = [byte[]]::new($namespaceBytes.Length + $nameBytes.Length)
    [Array]::Copy($namespaceBytes, 0, $hashInput, 0, $namespaceBytes.Length)
    [Array]::Copy($nameBytes, 0, $hashInput, $namespaceBytes.Length, $nameBytes.Length)
    $sha = [Security.Cryptography.SHA256]::Create()
    try { $guidBytes = $sha.ComputeHash($hashInput)[0..15] } finally { $sha.Dispose() }
    $guidBytes[7] = ($guidBytes[7] -band 0x0F) -bor 0x50
    $guidBytes[8] = ($guidBytes[8] -band 0x3F) -bor 0x80
    $productCode = '{' + ([Guid]::new([byte[]]$guidBytes).ToString().ToUpperInvariant()) + '}'

    & $WixPath build $sourcePath $wxsPath -arch x64 `
        -d "ProductVersion=$msiVersion" `
        -d "ProductCode=$productCode" `
        -d "PackageBinDir=$packageRoot" `
        -d "VersionFile=$versionFile" `
        -o $OutputPath
    if ($LASTEXITCODE -ne 0) {
        throw "WiX build failed with exit code $LASTEXITCODE."
    }

    $layout = [ordered]@{
        schemaVersion = 1
        version = $semanticVersion
        channel = 'msi'
        files = $files
    }
    [IO.File]::WriteAllText(
        $layoutPath,
        ((ConvertTo-Json -InputObject $layout -Depth 5) + [Environment]::NewLine),
        [Text.UTF8Encoding]::new($false))
    Write-Output "MSI created: $OutputPath"
    Write-Output "MSI file inventory: $layoutPath"
} finally {
    if (Test-Path -LiteralPath $payloadRoot) {
        $resolvedStage = [IO.Path]::GetFullPath($payloadRoot)
        if (-not $resolvedStage.StartsWith([IO.Path]::GetFullPath($outputDirectory), [StringComparison]::OrdinalIgnoreCase)) {
            throw "Refusing to remove staging path outside the package output directory: $resolvedStage"
        }
        Remove-Item -LiteralPath $resolvedStage -Recurse -Force
    }
}
