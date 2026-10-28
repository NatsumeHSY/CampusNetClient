$ErrorActionPreference = 'Stop'

if ($PSVersionTable.PSVersion.Major -lt 7) {
    throw 'LocalVersionJsonTests requires PowerShell 7 or newer.'
}

$testRoot = Join-Path ([System.IO.Path]::GetTempPath()) ([guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testRoot | Out-Null
try {
    $sourceRoot = Split-Path -Parent $PSScriptRoot
    $scriptRoot = Join-Path $testRoot 'tools'
    New-Item -ItemType Directory -Path $scriptRoot | Out-Null
    Copy-Item (Join-Path $sourceRoot 'tools\Write-LocalVersionJson.ps1') $scriptRoot
    Copy-Item (Join-Path $sourceRoot 'version.props') $testRoot
    [xml]$expectedProps = Get-Content -Raw -LiteralPath (Join-Path $testRoot 'version.props')
    $expectedVersion = [string]$expectedProps.Project.PropertyGroup.CampusNetSemVer
    $script = Join-Path $scriptRoot 'Write-LocalVersionJson.ps1'

    foreach ($channel in @('portable', 'msi')) {
        $output = Join-Path $testRoot "$channel\version.json"
        & $script -Channel $channel -OutputPath $output
        $record = Get-Content -Raw -LiteralPath $output | ConvertFrom-Json
        $keys = @($record.PSObject.Properties.Name | Sort-Object)
        if (($keys -join ',') -ne 'channel,schemaVersion,version' -or
            $record.schemaVersion -ne 1 -or $record.version -ne $expectedVersion -or
            $record.channel -ne $channel) {
            throw "Unexpected local version record for channel '$channel'."
        }
    }

    $propsPath = Join-Path $testRoot 'version.props'
    $invalidProps = Get-Content -Raw -LiteralPath $propsPath
    $invalidProps = $invalidProps.Replace("<CampusNetSemVer>$expectedVersion</CampusNetSemVer>", '<CampusNetSemVer>0.1.0.0</CampusNetSemVer>')
    Set-Content -LiteralPath $propsPath -Value $invalidProps -NoNewline
    $invalidOutput = Join-Path $testRoot 'invalid\version.json'
    $failed = $false
    try {
        & $script -Channel portable -OutputPath $invalidOutput
    } catch {
        $failed = $true
    }
    if (-not $failed -or (Test-Path -LiteralPath $invalidOutput)) {
        throw 'Invalid four-component SemVer must fail without writing output.'
    }

    $failed = $false
    try {
        & $script -Channel staging -OutputPath $invalidOutput
    } catch {
        $failed = $true
    }
    if (-not $failed) {
        throw 'Unsupported channel must fail.'
    }

    'LocalVersionJsonTests: PASS'
} finally {
    Remove-Item -LiteralPath $testRoot -Recurse -Force
}
