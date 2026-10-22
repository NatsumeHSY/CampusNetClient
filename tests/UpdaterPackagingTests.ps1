$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$projectPath = Join-Path $root 'updater\CampusNetUpdater.vcxproj'
$mainPath = Join-Path $root 'updater\UpdaterMain.cpp'
$manifestPath = Join-Path $root 'updater\Updater.manifest'
$resourcePath = Join-Path $root 'updater\Updater.rc'
$rolePath = Join-Path $root 'updater\UpdaterRole.cpp'

foreach ($path in @($projectPath, $mainPath, $manifestPath, $resourcePath, $rolePath)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Required updater packaging file is missing: $path"
    }
}

$project = Get-Content -Raw -LiteralPath $projectPath
$main = Get-Content -Raw -LiteralPath $mainPath
$manifest = Get-Content -Raw -LiteralPath $manifestPath
$resource = Get-Content -Raw -LiteralPath $resourcePath
$role = Get-Content -Raw -LiteralPath $rolePath

if ($project -notmatch '<SubSystem>Windows</SubSystem>' -or
    $project -match '<SubSystem>Console</SubSystem>') {
    throw 'Updater must link as a Windows GUI subsystem application.'
}
if ($main -notmatch '\bwWinMain\s*\(' -or
    $main -match '\b(?:int|void)\s+main\s*\(' -or
    $main -match '\b(?:argc|argv|CommandLineToArgvW)\b') {
    throw 'Updater must use wWinMain and must not expose a CLI parser.'
}
if ($manifest -notmatch 'Microsoft\.Windows\.Common-Controls' -or
    $manifest -notmatch '6\.0\.0\.0' -or
    $project -notmatch '<Manifest Include="Updater\.manifest"') {
    throw 'Updater must embed the Common Controls 6.0 activation manifest.'
}
if ($role -notmatch 'ResolveUpdaterRole' -or
    $project -notmatch 'CampusNetBuildRoot' -or
    $project -notmatch 'CampusNetPackageRoot' -or
    $project -notmatch '\\obj\\') {
    throw 'Updater role or build output layout is incomplete.'
}

$binary = Join-Path $root 'build\package\Debug\bin\CampusNetUpdater.exe'
if (Test-Path -LiteralPath $binary -PathType Leaf) {
    $bytes = [System.IO.File]::ReadAllBytes($binary)
    if ($bytes.Length -lt 0x40 -or $bytes[0] -ne 0x4d -or $bytes[1] -ne 0x5a) {
        throw 'CampusNetUpdater.exe is not a valid PE image.'
    }
    $peOffset = [BitConverter]::ToInt32($bytes, 0x3c)
    if ($peOffset -lt 0x40 -or $peOffset + 24 -gt $bytes.Length -or
        $bytes[$peOffset] -ne 0x50 -or $bytes[$peOffset + 1] -ne 0x45) {
        throw 'CampusNetUpdater.exe has an invalid PE header.'
    }
    $subsystem = [BitConverter]::ToUInt16($bytes, $peOffset + 24 + 68)
    if ($subsystem -ne 2) {
        throw "Expected Windows GUI PE subsystem 2, got $subsystem."
    }
}

'UpdaterPackagingTests: PASS'
