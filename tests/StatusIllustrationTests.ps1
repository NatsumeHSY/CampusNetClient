$ErrorActionPreference = 'Stop'

$clientRoot = Join-Path $PSScriptRoot '..\client'
$xamlPath = Join-Path $clientRoot 'MainWindow.xaml'
$cppPath = Join-Path $clientRoot 'MainWindow.xaml.cpp'
$assetRoot = Join-Path $clientRoot 'assets\status'
$xaml = Get-Content -Raw -LiteralPath $xamlPath
$cpp = Get-Content -Raw -LiteralPath $cppPath

if ($xaml -notmatch 'x:Name="StatusIllustration"') {
    throw 'The run-status card must expose a status illustration image.'
}

$canvasOverlay = [regex]::Match(
    $xaml,
    '<Canvas Width="180"[\s\S]*?IsHitTestVisible="False">\s*<Image x:Name="StatusIllustration"[\s\S]*?/>\s*</Canvas>').Value
if (-not $canvasOverlay) {
    throw 'The status illustration must be in a non-measuring right-side canvas overlay.'
}

if ($canvasOverlay -match '<Canvas[^>]*\bHeight=') {
    throw 'The status illustration overlay must not impose a fixed height on its parent.'
}

if ($cpp -notmatch 'SetStatusIllustration') {
    throw 'MainWindow must update the status illustration from service state.'
}

$refreshMethod = [regex]::Match(
    $cpp,
    '(?ms)^void MainWindow::RefreshStatus\(\) \{\r?\n(?<body>.*?)^\}')
if (-not $refreshMethod.Success) {
    throw 'MainWindow::RefreshStatus must remain available for refresh behavior checks.'
}
if ($refreshMethod.Groups['body'].Value -match
    'SetStatusIllustration\s*\(\s*L"service-connecting\.png"') {
    throw 'A status refresh must keep the current illustration until the fresh response arrives.'
}

foreach ($asset in @(
        'service-disconnected.png',
        'service-connecting.png',
        'service-connected.png',
        'network-unauthenticated.png',
        'network-authenticating.png',
        'network-authenticated.png',
        'network-authentication-failed.png')) {
    $path = Join-Path $assetRoot $asset
    if (-not (Test-Path -LiteralPath $path)) {
        throw "Status illustration asset is missing: $asset"
    }
}

foreach ($state in @(
        'service-disconnected',
        'service-connecting',
        'service-connected',
        'network-unauthenticated',
        'network-authenticating',
        'network-authenticated',
        'network-authentication-failed')) {
    if ($cpp -notmatch [regex]::Escape($state)) {
        throw "Status illustration state is missing from the mapping: $state"
    }
}

Write-Output 'StatusIllustrationTests: PASS'
