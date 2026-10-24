<#
.SYNOPSIS
Saves and restores compiled Unreal Engine Editor binaries and receipts
between GitHub Actions workflow jobs.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('save', 'restore')]
    [string]$Action,

    [string]$ArchiveDir = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false

$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$engine = $env:QINGLAN_ENGINE_ROOT
if (-not $engine -or -not (Test-Path $engine)) {
    $engine = Join-Path $env:RUNNER_TEMP 'qinglan-engine'
}
if (-not (Test-Path $engine)) { throw "Engine root not found: $engine" }

if ([string]::IsNullOrWhiteSpace($ArchiveDir)) {
    $ArchiveDir = Join-Path $env:RUNNER_TEMP 'editor-cache'
}
New-Item -ItemType Directory -Force $ArchiveDir | Out-Null

$engineArchive = Join-Path $ArchiveDir 'editor-engine.7z'
$gameArchive = Join-Path $ArchiveDir 'editor-game.7z'

if ($Action -eq 'save') {
    Write-Host "=========================================================="
    Write-Host "   Saving compiled Editor binaries and receipts           "
    Write-Host "=========================================================="
    Write-Host "Engine: $engine"
    Write-Host "Output: $ArchiveDir"

    # 1. Archive Engine/Binaries, Engine/Build/Receipts, and plugin binaries ONLY.
    # NOTE: Never archive Intermediate directories (.obj, .pch) - they are 40+ GB
    # and not needed by UnrealEditor-Cmd.exe cooker or RunUAT.
    Push-Location $engine
    try {
        $engItems = @()
        if (Test-Path 'Engine/Binaries/Win64') { $engItems += 'Engine/Binaries/Win64' }
        if (Test-Path 'Engine/Build/Receipts') { $engItems += 'Engine/Build/Receipts' }

        # Collect plugin binaries
        if (Test-Path 'Engine/Plugins') {
            Get-ChildItem 'Engine/Plugins' -Recurse -Directory -Filter 'Win64' |
                Where-Object { $_.FullName -match '[\\/]Binaries[\\/]Win64$' } |
                ForEach-Object {
                    $engItems += [IO.Path]::GetRelativePath($engine, $_.FullName).Replace('\', '/')
                }
        }

        Write-Host "Found $($engItems.Count) Engine directories to archive."
        $listFile = Join-Path $ArchiveDir 'engine-items.txt'
        $engItems | Set-Content -LiteralPath $listFile -Encoding utf8
        Write-Host "Compressing Engine binaries into $engineArchive (fast, 2 threads)..."
        # -mx=1: fastest compression, -mmt=2: limit to 2 threads to prevent starving runner process
        & 7z a -t7z -mx=1 -mmt=2 $engineArchive "@$listFile"
        if ($LASTEXITCODE) { throw "7z Engine archive creation failed ($LASTEXITCODE)." }
        Remove-Item -LiteralPath $listFile -Force -ErrorAction SilentlyContinue
    }
    finally {
        Pop-Location
    }

    # 2. Archive game project Binaries and Receipts
    Push-Location $repo
    try {
        $gameItems = @()
        if (Test-Path 'game/Binaries/Win64') { $gameItems += 'game/Binaries/Win64' }
        if (Test-Path 'game/Build/Receipts') { $gameItems += 'game/Build/Receipts' }
        if (Test-Path 'game/Plugins') {
            Get-ChildItem 'game/Plugins' -Recurse -Directory -Filter 'Win64' |
                Where-Object { $_.FullName -match '[\\/]Binaries[\\/]Win64$' } |
                ForEach-Object {
                    $gameItems += [IO.Path]::GetRelativePath($repo, $_.FullName).Replace('\', '/')
                }
        }

        Write-Host "Found $($gameItems.Count) Game items to archive."
        if ($gameItems.Count -gt 0) {
            $listFile = Join-Path $ArchiveDir 'game-items.txt'
            $gameItems | Set-Content -LiteralPath $listFile -Encoding utf8
            Write-Host "Compressing Game items into $gameArchive..."
            & 7z a -t7z -mx=1 -mmt=2 $gameArchive "@$listFile"
            if ($LASTEXITCODE) { throw "7z Game archive creation failed ($LASTEXITCODE)." }
            Remove-Item -LiteralPath $listFile -Force -ErrorAction SilentlyContinue
        } else {
            # Create a small marker archive if no game items
            & 7z a -t7z -mx=1 -mmt=2 $gameArchive (Join-Path $repo 'game/QinglanAscension.uproject')
        }
    }
    finally {
        Pop-Location
    }

    $engSizeMB = [math]::Round(((Get-Item $engineArchive).Length / 1MB), 2)
    $gameSizeMB = [math]::Round(((Get-Item $gameArchive).Length / 1MB), 2)
    Write-Host "Save complete: engine archive = $engSizeMB MB, game archive = $gameSizeMB MB"
}
elseif ($Action -eq 'restore') {
    Write-Host "=========================================================="
    Write-Host "   Restoring compiled Editor binaries and receipts        "
    Write-Host "=========================================================="
    if (-not (Test-Path $engineArchive)) { throw "Missing engine archive: $engineArchive" }

    $targetEngine = $engine.TrimEnd('\', '/')
    Write-Host "Extracting $engineArchive to $targetEngine..."
    & 7z x -y $engineArchive "-o$targetEngine"
    if ($LASTEXITCODE) { throw "Extraction of $engineArchive failed ($LASTEXITCODE)." }

    if (Test-Path $gameArchive) {
        $targetRepo = $repo.TrimEnd('\', '/')
        Write-Host "Extracting $gameArchive to $targetRepo..."
        & 7z x -y $gameArchive "-o$targetRepo"
        if ($LASTEXITCODE) { throw "Extraction of $gameArchive failed ($LASTEXITCODE)." }
    }

    # Verify key binaries
    $editorExe = Join-Path $engine 'Engine/Binaries/Win64/UnrealEditor.exe'
    $editorCmd = Join-Path $engine 'Engine/Binaries/Win64/UnrealEditor-Cmd.exe'
    if (-not (Test-Path $editorExe) -and -not (Test-Path $editorCmd)) {
        throw "Verification failed: Neither UnrealEditor.exe nor UnrealEditor-Cmd.exe found at $engine/Engine/Binaries/Win64 after restore."
    }
    $scwExe = Join-Path $engine 'Engine/Binaries/Win64/ShaderCompileWorker.exe'
    $pakExe = Join-Path $engine 'Engine/Binaries/Win64/UnrealPak.exe'
    if (Test-Path $scwExe) {
        Write-Host "Verification: ShaderCompileWorker.exe is present."
    } else {
        Write-Warning "ShaderCompileWorker.exe not found at $engine/Engine/Binaries/Win64."
    }
    if (Test-Path $pakExe) {
        Write-Host "Verification: UnrealPak.exe is present."
    } else {
        Write-Warning "UnrealPak.exe not found at $engine/Engine/Binaries/Win64."
    }
    Write-Host "Verification succeeded: Editor binaries are present."
}
