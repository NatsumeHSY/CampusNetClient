[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('portable', 'msi')]
    [string]$Channel,

    [Parameter(Mandatory = $true)]
    [string]$OutputPath
)

$ErrorActionPreference = 'Stop'

$propsPath = Join-Path (Split-Path -Parent $PSScriptRoot) 'version.props'
if (-not (Test-Path -LiteralPath $propsPath -PathType Leaf)) {
    throw "Central version file is missing: $propsPath"
}

try {
    [xml]$props = Get-Content -Raw -LiteralPath $propsPath
} catch {
    throw "Could not parse central version file: $($_.Exception.Message)"
}

$versionNodes = $props.SelectNodes('/Project/PropertyGroup/CampusNetSemVer')
if ($null -eq $versionNodes -or $versionNodes.Count -ne 1) {
    throw 'version.props must define exactly one CampusNetSemVer value.'
}
$version = [string]$versionNodes[0].InnerText
$numeric = '(?:0|[1-9][0-9]*)'
$preIdentifier = '(?:0|[1-9][0-9]*|[0-9A-Za-z-]*[A-Za-z-][0-9A-Za-z-]*)'
$buildIdentifier = '[0-9A-Za-z-]+'
$semVerPattern = "\A$numeric\.$numeric\.$numeric(?:-$preIdentifier(?:\.$preIdentifier)*)?(?:\+$buildIdentifier(?:\.$buildIdentifier)*)?\z"
if ($version.Length -gt 128 -or $version -cnotmatch $semVerPattern) {
    throw "Central version is not a valid SemVer 2.0.0 value: '$version'"
}

$record = [ordered]@{
    schemaVersion = 1
    version = $version
    channel = $Channel
}
$json = ConvertTo-Json -InputObject $record -Compress
$fullOutputPath = [System.IO.Path]::GetFullPath($OutputPath)
$outputDirectory = [System.IO.Path]::GetDirectoryName($fullOutputPath)
if ([string]::IsNullOrWhiteSpace($outputDirectory)) {
    throw 'OutputPath must include a parent directory.'
}
[System.IO.Directory]::CreateDirectory($outputDirectory) | Out-Null
$temporaryPath = Join-Path $outputDirectory ('.' + [System.IO.Path]::GetFileName($fullOutputPath) + '.' + [guid]::NewGuid().ToString('N') + '.tmp')
try {
    [System.IO.File]::WriteAllText(
        $temporaryPath,
        $json + [Environment]::NewLine,
        [System.Text.UTF8Encoding]::new($false))
    [System.IO.File]::Move($temporaryPath, $fullOutputPath, $true)
} finally {
    if (Test-Path -LiteralPath $temporaryPath) {
        Remove-Item -LiteralPath $temporaryPath -Force
    }
}
