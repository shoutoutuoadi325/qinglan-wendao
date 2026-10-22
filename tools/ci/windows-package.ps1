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

# Limit compile concurrency to leave memory for the cooker on a 16 GiB runner.
$build = Join-Path $engine 'Engine/Build/BatchFiles/Build.bat'
& $build QinglanAscensionEditor Win64 Development "-Project=$project" -WaitMutex -NoUBA -MaxParallelActions=2 2>&1 |
    Tee-Object (Join-Path $reports 'editor-build.log')
if ($LASTEXITCODE) { throw "Win64 editor build failed ($LASTEXITCODE)." }
$uat = Join-Path $engine 'Engine/Build/BatchFiles/RunUAT.bat'
& $uat BuildCookRun "-project=$project" -noP4 -platform=Win64 -clientconfig=Shipping -build -cook -stage -pak -iostore -compressed -archive "-archivedirectory=$package" -unattended -nosplash -utf8output -nullrhi '-ubtargs=-NoUBA -MaxParallelActions=2' 2>&1 |
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
    # The staged client lives under <StagedRoot>\QinglanAscension\Binaries\Win64
    $stagedRoot = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $clients[0].FullName))
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
