<#
.SYNOPSIS
Builds QinglanAscensionEditor (Win64 Development) and saves the compiled
binaries and receipts to a portable cache archive.
#>
[CmdletBinding()]
param(
    [string]$ArchiveDir = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false

$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$engine = $env:QINGLAN_ENGINE_ROOT
if (-not $engine -or -not (Test-Path $engine)) { throw 'A prepared Windows engine is required.' }
$project = Join-Path $repo 'game/QinglanAscension.uproject'
$reports = Join-Path $env:RUNNER_TEMP 'qinglan-win64/reports'
New-Item -ItemType Directory -Force $reports | Out-Null

if ([string]::IsNullOrWhiteSpace($ArchiveDir)) {
    $ArchiveDir = Join-Path $env:RUNNER_TEMP 'editor-cache'
}

# 1. Configure UnrealBuildTool to omit debug info/PDBs and disable LTCG
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

Write-Host "=========================================================="
Write-Host "   Compiling QinglanAscensionEditor (Win64 Development)   "
Write-Host "=========================================================="
$build = Join-Path $engine 'Engine/Build/BatchFiles/Build.bat'
& $build QinglanAscensionEditor Win64 Development "-Project=$project" -WaitMutex -NoUBA -MaxParallelActions=3 2>&1 |
    Tee-Object (Join-Path $reports 'editor-build.log')
if ($LASTEXITCODE) { throw "Win64 editor build failed ($LASTEXITCODE)." }

# 2. Reclaim intermediate disk space before archiving
Write-Host "`n==> Freeing bulky intermediate compiler artifacts before archiving..."
Remove-Item -Recurse -Force (Join-Path $engine 'Engine/Intermediate/Build') -ErrorAction SilentlyContinue
Remove-Item -Recurse -Force (Join-Path $repo 'game/Intermediate/Build') -ErrorAction SilentlyContinue

# 3. Package the compiled binaries and receipts
Write-Host "`n==> Archiving compiled Editor cache..."
& (Join-Path $PSScriptRoot 'windows-editor-cache.ps1') -Action save -ArchiveDir $ArchiveDir
Write-Host "Editor build and caching complete!"
