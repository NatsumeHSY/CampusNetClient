$ErrorActionPreference = 'Stop'

$xamlPath = Join-Path $PSScriptRoot '..\client\MainWindow.xaml'
$cppPath = Join-Path $PSScriptRoot '..\client\MainWindow.xaml.cpp'
$headerPath = Join-Path $PSScriptRoot '..\client\MainWindow.xaml.h'
$xaml = Get-Content -Raw -LiteralPath $xamlPath
$cpp = Get-Content -Raw -LiteralPath $cppPath
$header = Get-Content -Raw -LiteralPath $headerPath

if ($xaml -notmatch 'Title="广航海校园网自动登录"') {
    throw 'The main window title must use the current product name.'
}

if ($xaml -notmatch '<TextBlock Text="广航海校园网自动登录"') {
    throw 'The main page heading must use the current product name.'
}

if ($xaml -notmatch '<Border x:Name="ServiceInstallStatusBadge"') {
    throw 'The Windows Service card must show an installation-status badge.'
}

if ($xaml -notmatch '<TextBlock x:Name="ServiceInstallStatusText"') {
    throw 'The service installation-status badge must expose its text.'
}

if ($cpp -notmatch 'ServiceInstallStatusText\(\)\.Text') {
    throw 'The service installation-status badge must update with service state.'
}

foreach ($installationLabel in @('未安装', '已安装', '状态未知')) {
    if ($cpp -notmatch [regex]::Escape($installationLabel)) {
        throw "The service installation-status mapping is missing: $installationLabel"
    }
}

if ($cpp -notmatch 'ServiceInstallationName\(ManagedServiceState state\)') {
    throw 'The service installation-status mapping function is missing.'
}

$installationMapping = [regex]::Match(
    $cpp, 'const wchar_t\* ServiceInstallationName\(ManagedServiceState state\)\s*\{[\s\S]*?\n\}')
if ($installationMapping.Value -notmatch 'case ManagedServiceState::Missing:\s*return L"未安装";') {
    throw 'A missing service must be labeled 未安装.'
}
if ($installationMapping.Value -notmatch 'case ManagedServiceState::Stopped:[\s\S]*?case ManagedServiceState::StopPending:\s*return L"已安装";') {
    throw 'All present service runtime states must be labeled 已安装.'
}
if ($installationMapping.Value -notmatch 'case ManagedServiceState::Failed:\s*return L"状态未知";') {
    throw 'A failed service query must be labeled 状态未知.'
}

if ($xaml -notmatch 'x:Name="StatusIndicator"\s+Grid\.Column="0"') {
    throw 'StatusIndicator must occupy the left status column.'
}

if ($xaml -notmatch 'x:Name="StatusIndicator"[\s\S]*?VerticalAlignment="Center"') {
    throw 'StatusIndicator must be vertically centered.'
}

if ($xaml -notmatch 'x:Name="StatusIndicator"[\s\S]*?Margin="0,4,0,0"') {
    throw 'StatusIndicator must include the visual alignment offset.'
}

if ($xaml -notmatch '<Canvas Width="180"\s+HorizontalAlignment="Right"[\s\S]*?<Image x:Name="StatusIllustration"') {
    throw 'StatusIllustration must occupy the right side of the run-status card.'
}

if ($xaml -notmatch 'x:Name="StatusText"[\s\S]*?VerticalAlignment="Center"') {
    throw 'StatusText must be vertically centered with StatusIndicator.'
}

if ($xaml -match 'x:Name="StatusIndicator"\s+Grid\.Column="2"') {
    throw 'StatusIndicator must not remain in the right status column.'
}

if ($xaml -match '<ProgressRing\s+x:Name="StatusProgress"') {
    throw 'The old status progress ring must be removed from the status row.'
}

$loginActionRow = [regex]::Match(
    $xaml,
    '<StackPanel Orientation="Horizontal" Spacing="10">[\s\S]*?<Button x:Name="LoginButton"[\s\S]*?<ProgressRing x:Name="LoginProgressRing"[\s\S]*?<Button Content="刷新状态"[\s\S]*?</StackPanel>').Value
if (-not $loginActionRow) {
    throw 'The login action row must show a native progress ring beside the login button.'
}
if ($loginActionRow -notmatch 'LoginProgressRing[\s\S]*?IsActive="False"[\s\S]*?Visibility="Collapsed"') {
    throw 'The login progress ring must start inactive and hidden.'
}

$loginFailureBar = [regex]::Match(
    $xaml, '<InfoBar x:Name="LoginFailureInfoBar"[\s\S]*?/>').Value
if (-not $loginFailureBar -or
    $loginFailureBar -notmatch 'IsOpen="False"' -or
    $loginFailureBar -notmatch 'Severity="Error"') {
    throw 'Login failures must be shown in a native, initially hidden error InfoBar.'
}

if ($xaml -notmatch '<Storyboard\s+x:Key="StatusIndicatorBreathing"') {
    throw 'StatusIndicatorBreathing storyboard is missing.'
}

if ($xaml -notmatch 'Storyboard\.TargetName="StatusIndicator"[\s\S]*RepeatBehavior="Forever"') {
    throw 'StatusIndicator breathing animation must target the indicator and repeat forever.'
}

if ($xaml -match 'x:Name="FallbackText"') {
    throw 'The obsolete WebView2 fallback text must be removed from the status card.'
}

$statusTextBlock = [regex]::Match(
    $xaml, '<TextBlock x:Name="StatusText"[\s\S]*?/>').Value
if ($statusTextBlock -match 'Text="服务状态：') {
    throw 'The run-status card must not show the 服务状态 prefix.'
}

foreach ($name in @('LocalIpText', 'LocalMacText', 'ComputerNameText')) {
    if ($xaml -notmatch ('x:Name="' + $name + '"')) {
        throw "$name is missing from the status card."
    }
}

if ($header -notmatch 'RefreshLocalNetworkInfo\(\)') {
    throw 'MainWindow must expose a local network information refresh method.'
}

if ($cpp -notmatch 'GetAdaptersAddresses') {
    throw 'Local network information must be read from Windows adapter data.'
}

if ($cpp -notmatch 'GetBestInterface') {
    throw 'Local network information must follow Windows primary-route selection.'
}

if ($cpp -notmatch 'FirstGatewayAddress') {
    throw 'Local network information must require an adapter gateway before selecting it.'
}

if ($cpp -notmatch 'IsVirtualAdapter') {
    throw 'Local network information must explicitly reject virtual adapters.'
}

foreach ($virtualMarker in @('WSL', 'Hyper-V', 'VMware', 'Virtual')) {
    if ($cpp -notmatch $virtualMarker) {
        throw "Virtual adapter marker is missing from the selection filter: $virtualMarker"
    }
}

foreach ($escapedText in @(
        '\u5DF2\u8FDE\u63A5\u5230\u670D\u52A1',
        '\u672A\u8FDE\u63A5\u5230\u670D\u52A1',
        '\u6B63\u5728\u8FDE\u63A5\u5230\u670D\u52A1')) {
    if ($cpp -notmatch [regex]::Escape($escapedText)) {
        throw "Service status text is missing: $escapedText"
    }
}

if ($cpp -match '\\u670D\\u52A1\\u72B6\\u6001\\uFF1A') {
    throw 'The run-status implementation must not prepend the 服务状态 prefix.'
}

Write-Output 'MainWindowUiLayoutTests: PASS'
