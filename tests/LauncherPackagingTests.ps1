$ErrorActionPreference = 'Stop'

if ($PSVersionTable.PSVersion.Major -lt 7) {
    throw 'LauncherPackagingTests requires PowerShell 7 or later.'
}

$script:Failures = [System.Collections.Generic.List[string]]::new()

function Add-CheckFailure([string]$Message) {
    $script:Failures.Add($Message)
}

function Read-RequiredText([string]$Path, [string]$Label) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        Add-CheckFailure "$Label is missing: $Path"
        return ''
    }

    return [System.IO.File]::ReadAllText($Path)
}

function Assert-Match([string]$Text, [string]$Pattern, [string]$Message) {
    if ($Text -notmatch $Pattern) {
        Add-CheckFailure $Message
    }
}

$repoRoot = Split-Path -Parent $PSScriptRoot
$workspaceRoot = Split-Path -Parent $repoRoot
$readmePath = Join-Path $repoRoot 'README.md'
$launcherProjectPath = Join-Path $repoRoot 'launcher\CampusNetLauncher.vcxproj'
$launcherSourcePath = Join-Path $repoRoot 'launcher\LauncherMain.cpp'
$launcherResourcePath = Join-Path $repoRoot 'launcher\Launcher.rc'
$launcherResourceHeaderPath = Join-Path $repoRoot 'launcher\resource.h'
$launcherManifestPath = Join-Path $repoRoot 'launcher\launcher.manifest'
$clientManifestPath = Join-Path $repoRoot 'client\app.manifest'
$packageRoot = Join-Path $workspaceRoot 'build\package\Release'

$readme = Read-RequiredText $readmePath 'README'
$requiredSections = @(
    '项目简介',
    '发布包结构',
    '运行方法',
    '系统要求',
    '服务管理与自动登录',
    '系统托盘与单实例',
    '构建方法',
    '常见问题'
)
foreach ($section in $requiredSections) {
    $heading = '(?m)^#{1,3}\s+' + [regex]::Escape($section) + '\s*$'
    if (-not [regex]::IsMatch($readme, $heading)) {
        Add-CheckFailure "README is missing the '$section' section."
    }
}
Assert-Match $readme '不提供命令行启动接口' 'README must state that the launcher has no CLI startup interface.'
Assert-Match $readme 'WebView2 Runtime' 'README must document the WebView2 Runtime requirement.'

$launcherProjectText = Read-RequiredText $launcherProjectPath 'Launcher project'
$launcherSource = Read-RequiredText $launcherSourcePath 'Launcher source'
$launcherResource = Read-RequiredText $launcherResourcePath 'Launcher icon resource'
$launcherResourceHeader = Read-RequiredText $launcherResourceHeaderPath 'Launcher resource header'
$launcherManifest = Read-RequiredText $launcherManifestPath 'Launcher manifest'
$clientManifest = Read-RequiredText $clientManifestPath 'Client manifest'

try {
    [xml]$projectXml = $launcherProjectText
    $subsystem = $projectXml.SelectSingleNode("//*[local-name()='Link']/*[local-name()='SubSystem']")
    if ($null -eq $subsystem -or $subsystem.InnerText.Trim() -ne 'Windows') {
        Add-CheckFailure 'Launcher project must link with the Windows GUI subsystem.'
    }
} catch {
    Add-CheckFailure "Launcher project is not valid XML: $($_.Exception.Message)"
}

Assert-Match $launcherSource '\bint\s+WINAPI\s+wWinMain\s*\(' 'Launcher must use the GUI wWinMain entry point.'
Assert-Match $launcherSource 'ShellExecuteW\s*\(\s*nullptr\s*,\s*L"open"\s*,\s*clientPath\.c_str\(\)\s*,\s*nullptr' 'Launcher must open the client without forwarding CLI arguments.'
if (-not $launcherResource.Contains('IDI_CAMPUSNETLAUNCHER ICON "..\\client\\assets\\CampusNetClient.ico"')) {
    Add-CheckFailure 'Launcher must embed the existing client icon.'
}
Assert-Match $launcherResourceHeader '(?m)^#define\s+IDI_CAMPUSNETLAUNCHER\s+\d+\s*$' 'Launcher icon resource ID must be defined.'
if (-not (Test-Path -LiteralPath (Join-Path $repoRoot 'client\assets\CampusNetClient.ico') -PathType Leaf)) {
    Add-CheckFailure 'The shared CampusNetClient.ico asset is missing.'
}

foreach ($manifest in @(
        @{ Text = $clientManifest; Level = 'requireAdministrator'; Label = 'Client' },
        @{ Text = $launcherManifest; Level = 'asInvoker'; Label = 'Launcher' }
    )) {
    try {
        [xml]$manifestXml = $manifest.Text
        $executionLevel = $manifestXml.SelectSingleNode("//*[local-name()='requestedExecutionLevel']")
        if ($null -eq $executionLevel -or $executionLevel.GetAttribute('level') -ne $manifest.Level) {
            Add-CheckFailure "$($manifest.Label) manifest must request $($manifest.Level)."
        }
    } catch {
        Add-CheckFailure "$($manifest.Label) manifest is not valid XML: $($_.Exception.Message)"
    }
}

if (-not (Test-Path -LiteralPath $packageRoot -PathType Container)) {
    Add-CheckFailure "Release package directory is missing: $packageRoot"
} else {
    $rootNames = @(Get-ChildItem -LiteralPath $packageRoot -Force | ForEach-Object Name | Sort-Object)
    $expectedRootNames = @('CampusNetLauncher.exe', 'bin') | Sort-Object
    if (Compare-Object -ReferenceObject $expectedRootNames -DifferenceObject $rootNames) {
        Add-CheckFailure "Release root must contain only CampusNetLauncher.exe and bin; found: $($rootNames -join ', ')"
    }

    $requiredPayload = @(
        'CampusNetClient.exe',
        'CampusNetService.exe',
        'WebView2Loader.dll',
        'resources.pri',
        'Microsoft.WindowsAppRuntime.Insights.Resource.dll',
        'Microsoft.UI.Xaml.Controls.dll',
        'App.xbf',
        'assets\status\service-connected.png',
        'assets\status\service-disconnected.png'
    )
    foreach ($relativePath in $requiredPayload) {
        $payloadPath = Join-Path $packageRoot ('bin\' + $relativePath)
        if (-not (Test-Path -LiteralPath $payloadPath -PathType Leaf)) {
            Add-CheckFailure "Release bin payload is missing: bin\$relativePath"
        }
    }
}

$launcherIntermediate = Join-Path $workspaceRoot 'build\obj\CampusNetLauncher\Release'
if (-not (Test-Path -LiteralPath $launcherIntermediate -PathType Container)) {
    Add-CheckFailure "Launcher intermediate output must stay under build\obj: $launcherIntermediate"
}

if ($script:Failures.Count -gt 0) {
    $details = $script:Failures | ForEach-Object { " - $_" }
    throw "LauncherPackagingTests: FAIL`n$($details -join [Environment]::NewLine)"
}

Write-Output 'LauncherPackagingTests: PASS'
