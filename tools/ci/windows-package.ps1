# Build a Win64 Shipping package and optionally package for Microsoft Store.
[CmdletBinding()]
param(
    [switch]$StorePackage,
    [ValidateSet('both', 'msixvc', 'msix')]
    [string]$StoreFormat = 'both',
    [string]$IdentityName = '',
    [string]$PublisherId = '',
    [string]$PublisherDisplayName = '',
    [string]$StoreId = '',
    [string]$Version = '0.1.0.0'
)
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$engine = $env:QINGLAN_ENGINE_ROOT
if (-not $engine -or -not (Test-Path $engine)) { throw 'A prepared Windows engine is required.' }
$project = Join-Path $repo 'game/QinglanAscension.uproject'
$base = Join-Path $env:RUNNER_TEMP 'qinglan-win64'
$package = Join-Path $base 'package'
$reports = Join-Path $base 'reports'
New-Item -ItemType Directory -Force $package, $reports | Out-Null
& (Join-Path $PSScriptRoot 'windows-preflight.ps1') -ReportPath (Join-Path $reports 'preflight.json') -RequireBuildTools

# Configure UnrealBuildTool to omit debug info/PDBs on CI to avoid disk exhaustion and accelerate linking.
$ubtDirs = @(
    (Join-Path $env:APPDATA 'Unreal Engine/UnrealBuildTool'),
    (Join-Path $engine 'Engine/Saved/UnrealBuildTool')
)
$ubtXml = @'
<?xml version="1.0" encoding="utf-8" ?>
<Configuration xmlns="https://www.unrealengine.com/BuildConfiguration">
  <BuildConfiguration>
    <bOmitPCDebugInfoInDevelopment>true</bOmitPCDebugInfoInDevelopment>
    <bDisableDebugInfoForGeneratedCode>true</bDisableDebugInfoForGeneratedCode>
    <bUsePDBFiles>false</bUsePDBFiles>
  </BuildConfiguration>
</Configuration>
'@
foreach ($dir in $ubtDirs) {
    if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Force $dir | Out-Null }
    $ubtXml | Set-Content (Join-Path $dir 'BuildConfiguration.xml') -Encoding utf8
}

# Run build with 3 parallel actions on the 4-core runner.
$build = Join-Path $engine 'Engine/Build/BatchFiles/Build.bat'
& $build QinglanAscensionEditor Win64 Development "-Project=$project" -WaitMutex -NoUBA -MaxParallelActions=3 2>&1 |
    Tee-Object (Join-Path $reports 'editor-build.log')
if ($LASTEXITCODE) { throw "Win64 editor build failed ($LASTEXITCODE)." }
$uat = Join-Path $engine 'Engine/Build/BatchFiles/RunUAT.bat'
& $uat BuildCookRun "-project=$project" -noP4 -platform=Win64 -clientconfig=Shipping -build -cook -stage -pak -iostore -compressed -archive "-archivedirectory=$package" -unattended -nosplash -utf8output -nullrhi -nodebuginfo '-ubtargs=-NoUBA -MaxParallelActions=3' 2>&1 |
    Tee-Object (Join-Path $reports 'package.log')
if ($LASTEXITCODE) { throw "Win64 Shipping packaging failed ($LASTEXITCODE)." }
$clients = @(Get-ChildItem $package -Recurse -File -Filter 'QinglanAscension*.exe' | Where-Object { $_.FullName -match '\\Binaries\\Win64\\' })
if (-not $clients.Count) { throw 'UAT returned success without staging a Win64 client.' }
foreach ($extension in @('*.pak', '*.utoc', '*.ucas')) {
    if (-not (Get-ChildItem $package -Recurse -File -Filter $extension | Select-Object -First 1)) { throw "Package is missing $extension content." }
}
$commit = (& git -C $repo rev-parse HEAD).Trim()
if ($LASTEXITCODE) { throw 'Cannot determine project revision.' }
$files = @(Get-ChildItem $package -Recurse -File | ForEach-Object {
    @{ path = [IO.Path]::GetRelativePath($package, $_.FullName).Replace('\', '/'); bytes = $_.Length; sha256 = (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
})
@{ timestamp_utc = [DateTime]::UtcNow.ToString('o'); project_commit = $commit; platform = 'Win64'; configuration = 'Shipping'; files = $files; runtime_tested = $false; store_package = [bool]($StorePackage -or ($env:STORE_PACKAGE -eq 'true')) } |
    ConvertTo-Json -Depth 5 | Set-Content (Join-Path $package 'build-evidence.json')
@'
Win64 Shipping test package built from committed content.
Run QinglanAscension.exe from the extracted Windows package folder.
DX12 gameplay, performance, clean-machine prerequisites, and install/update remain unverified.
'@ | Set-Content (Join-Path $package 'BUILD-STATUS.txt')
if ($env:GITHUB_STEP_SUMMARY) {
    "## Win64 test package built`nProject: $commit`n`nBuild/cook/staging passed. DX12 runtime and Microsoft Store certification were not tested." | Out-File $env:GITHUB_STEP_SUMMARY -Append
}

# Optional / Default: Produce Microsoft Store submission package
if ($StorePackage -or ($env:STORE_PACKAGE -eq 'true')) {
    Write-Host "`n==> Packaging for Microsoft Store..."
    $storeOut = Join-Path $base 'store-package'
    # Locate the staged root containing Engine and QinglanAscension
    $stagedRoot = Split-Path -Parent $clients[0].FullName
    while ($stagedRoot -and -not (Test-Path (Join-Path $stagedRoot 'Engine')) -and (Split-Path -Parent $stagedRoot) -ne $stagedRoot) {
        $stagedRoot = Split-Path -Parent $stagedRoot
    }
    if (-not $stagedRoot -or -not (Test-Path (Join-Path $stagedRoot 'Engine'))) {
        $stagedRoot = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $clients[0].FullName)))
    }
    & (Join-Path $PSScriptRoot 'windows-store-package.ps1') `
        -StagedPath $stagedRoot `
        -OutputPath $storeOut `
        -Format $StoreFormat `
        -IdentityName $IdentityName `
        -PublisherId $PublisherId `
        -PublisherDisplayName $PublisherDisplayName `
        -StoreId $StoreId `
        -Version $Version `
        -ReportsPath $reports
}
