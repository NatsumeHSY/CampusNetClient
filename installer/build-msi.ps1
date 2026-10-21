param(
    [Parameter(Mandatory = $true)]
    [string]$ClientBinDir,

    [Parameter(Mandatory = $true)]
    [string]$ServiceBinDir,

    [string]$ProductVersion = "0.1.0.0",
    [string]$OutputPath = "$PSScriptRoot\..\..\build\package\CampusNetClient-$ProductVersion.msi",
    [string]$WixPath = "wix.exe"
)

$ErrorActionPreference = "Stop"
$clientRoot = (Resolve-Path -LiteralPath $ClientBinDir).Path
$serviceRoot = (Resolve-Path -LiteralPath $ServiceBinDir).Path
$sourcePath = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "CampusNetClient.wxs")).Path
$outputDirectory = Split-Path -Parent $OutputPath
if ($outputDirectory -and -not (Test-Path -LiteralPath $outputDirectory)) {
    New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
}
$payloadPath = Join-Path ([IO.Path]::GetTempPath()) ("CampusNetClient-Payload-{0}.wxs" -f ([guid]::NewGuid()))

function ConvertTo-XmlText([string]$value) {
    return [System.Security.SecurityElement]::Escape($value)
}

try {
    $builder = [Text.StringBuilder]::new()
    [void]$builder.AppendLine('<?xml version="1.0" encoding="UTF-8"?>')
    [void]$builder.AppendLine('<Wix xmlns="http://wixtoolset.org/schemas/v4/wxs">')
    [void]$builder.AppendLine('  <Fragment>')
    [void]$builder.AppendLine('    <ComponentGroup Id="ClientPayload" Directory="INSTALLFOLDER">')

    $index = 0
    foreach ($file in (Get-ChildItem -LiteralPath $clientRoot -Recurse -File | Sort-Object FullName)) {
        $relative = [IO.Path]::GetRelativePath($clientRoot, $file.FullName)
        $relativeDirectory = [IO.Path]::GetDirectoryName($relative)
        $componentId = "ClientPayload{0:D4}" -f $index
        $source = ConvertTo-XmlText $file.FullName
        [void]$builder.Append(('      <Component Id="{0}"' -f $componentId))
        if ($relativeDirectory -and $relativeDirectory -ne ".") {
            $subdirectory = ConvertTo-XmlText ($relativeDirectory -replace '/', '\\')
            [void]$builder.Append((' Subdirectory="{0}"' -f $subdirectory))
        }
        [void]$builder.AppendLine('>')
        [void]$builder.AppendLine(('        <File Source="{0}" />' -f $source))
        [void]$builder.AppendLine('      </Component>')
        $index++
    }

    [void]$builder.AppendLine('    </ComponentGroup>')
    [void]$builder.AppendLine('  </Fragment>')
    [void]$builder.AppendLine('</Wix>')
    [IO.File]::WriteAllText($payloadPath, $builder.ToString(), [Text.UTF8Encoding]::new($false))

    & $WixPath build $sourcePath $payloadPath `
        -d "ProductVersion=$ProductVersion" `
        -d "ClientBinDir=$clientRoot" `
        -d "ServiceBinDir=$serviceRoot" `
        -o $OutputPath
    if ($LASTEXITCODE -ne 0) {
        throw "WiX build failed with exit code $LASTEXITCODE."
    }
}
finally {
    if (Test-Path -LiteralPath $payloadPath) {
        Remove-Item -LiteralPath $payloadPath -Force
    }
}
