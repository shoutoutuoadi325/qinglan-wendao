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
    [string]$Version = '0.1.0.0',
    # The legacy switch name is accepted for callers, but only engine tools
    # may be reused. Project editor modules must match this checkout.
    [Alias('SkipEditorBuild')]
    [switch]$UseCachedEditorTools
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
    <bAllowLTCG>false</bAllowLTCG>
  </BuildConfiguration>
</Configuration>
'@
foreach ($dir in $ubtDirs) {
    if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Force $dir | Out-Null }
    $ubtXml | Set-Content (Join-Path $dir 'BuildConfiguration.xml') -Encoding utf8
}

$build = Join-Path $engine 'Engine/Build/BatchFiles/Build.bat'

if (-not $UseCachedEditorTools) {
    Write-Host "=========================================================="
    Write-Host "   Compiling QinglanAscensionEditor (Win64 Development)   "
    Write-Host "=========================================================="
    & $build QinglanAscensionEditor Win64 Development "-Project=$project" -WaitMutex -NoUBA -MaxParallelActions=3 2>&1 |
        Tee-Object (Join-Path $reports 'editor-build.log')
    if ($LASTEXITCODE) { throw "Win64 editor build failed ($LASTEXITCODE)." }

    Write-Host "`n=========================================================="
    Write-Host "   Compiling Tools: ShaderCompileWorker (Win64 Development)"
    Write-Host "=========================================================="
    & $build ShaderCompileWorker Win64 Development "-Project=$project" -WaitMutex -NoUBA -MaxParallelActions=3 2>&1 |
        Tee-Object -Append (Join-Path $reports 'editor-build.log')
    if ($LASTEXITCODE) { throw "ShaderCompileWorker build failed ($LASTEXITCODE)." }

    Write-Host "`n=========================================================="
    Write-Host "   Compiling Tools: UnrealPak (Win64 Development)         "
    Write-Host "=========================================================="
    & $build UnrealPak Win64 Development "-Project=$project" -WaitMutex -NoUBA -MaxParallelActions=3 2>&1 |
        Tee-Object -Append (Join-Path $reports 'editor-build.log')
    if ($LASTEXITCODE) { throw "UnrealPak build failed ($LASTEXITCODE)." }
} else {
    foreach ($tool in @('ShaderCompileWorker.exe', 'UnrealPak.exe', 'UnrealEditor-Cmd.exe')) {
        if (-not (Test-Path (Join-Path $engine "Engine/Binaries/Win64/$tool"))) {
            throw "Restored editor tools are missing $tool."
        }
    }
    Write-Host 'Reusing cached engine tools and restored Editor binaries; skipping redundant editor rebuild.'
}

$editor = Join-Path $engine 'Engine/Binaries/Win64/UnrealEditor-Cmd.exe'
Write-Host "`n==> Importing current bilingual text before tests and Shipping cook..."
# UE can discover existing generated tables partially loaded and save stale
# keys. Start these two disposable generated assets fresh; retain technique
# assets and their authored montage/cue references.
$textAssets = @('ST_Game.uasset', 'DT_Localization.uasset') | ForEach-Object {
    Join-Path $repo "game/Content/Qinglan/Data/$_"
}
foreach ($asset in $textAssets) {
    if (Test-Path -LiteralPath $asset) { Remove-Item -LiteralPath $asset -Force }
}
& $editor $project '-run=QinglanAscensionEditor.QinglanDataImport' `
    "-data=$(Join-Path $repo 'data')" '-localizationonly' `
    -unattended -nop4 -nosplash -nullrhi -stdout -FullStdOutLogOutput 2>&1 |
    Tee-Object (Join-Path $reports 'localization-import.log')
if ($LASTEXITCODE) { throw "Bilingual text import failed ($LASTEXITCODE)." }
foreach ($asset in $textAssets) {
    if (-not (Test-Path -LiteralPath $asset) -or (Get-Item -LiteralPath $asset).Length -eq 0) {
        throw "Bilingual text import produced no asset: $asset"
    }
}

Write-Host "`n==> Running current Qinglan regression tests before Shipping cook..."
$testReport = Join-Path $reports 'automation'
$previousCampaignSetting = $env:QINGLAN_RUN_CAMPAIGN
try {
    # Heavy journey specs run separately in native acceptance. This Windows
    # gate covers every regular registration, including fresh-start regressions.
    $env:QINGLAN_RUN_CAMPAIGN = '0'
    & $editor $project '-ExecCmds=Automation RunTests Qinglan.' `
        '-TestExit=Automation Test Queue Empty' "-ReportExportPath=$testReport" `
        "-abslog=$(Join-Path $reports 'automation-engine.log')" `
        '-ini:Engine:[/Script/AutomationController.AutomationControllerSettings]:bPruneLogsOnSuccess=False' `
        -unattended -nop4 -nosplash -nullrhi -stdout -FullStdOutLogOutput 2>&1 |
        Tee-Object (Join-Path $reports 'automation.log')
    if ($LASTEXITCODE) { throw "Windows regression test process failed ($LASTEXITCODE)." }
} finally {
    $env:QINGLAN_RUN_CAMPAIGN = $previousCampaignSetting
}
# UE process exit zero alone is insufficient: require every source-discovered
# test to finish successfully, and reject absent/partial/skipped reports.
& python (Join-Path $PSScriptRoot 'validate_windows_automation.py') `
    -Project (Join-Path $repo 'game') -Report (Join-Path $testReport 'index.json')
if ($LASTEXITCODE) { throw 'Windows regression report validation failed.' }

Write-Host "`n=========================================================="
Write-Host "   Compiling QinglanAscension (Win64 Shipping)            "
Write-Host "=========================================================="
& $build QinglanAscension Win64 Shipping "-Project=$project" -WaitMutex -NoUBA -MaxParallelActions=3 2>&1 |
    Tee-Object (Join-Path $reports 'shipping-build.log')
if ($LASTEXITCODE) { throw "Win64 Shipping build failed ($LASTEXITCODE)." }

Write-Host "`n=========================================================="
Write-Host "   Cooking, Staging, and Archiving Win64 Shipping         "
Write-Host "=========================================================="
$uat = Join-Path $engine 'Engine/Build/BatchFiles/RunUAT.bat'
& $uat BuildCookRun "-project=$project" -noP4 -platform=Win64 -clientconfig=Shipping `
    -cook -stage -pak -iostore -compressed -archive "-archivedirectory=$package" `
    -unattended -nosplash -utf8output -nullrhi -nodebuginfo 2>&1 |
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
