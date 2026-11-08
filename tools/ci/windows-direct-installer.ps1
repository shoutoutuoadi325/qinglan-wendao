# Package a verified Windows Shipping artifact as a desktop installer and portable ZIP.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$StagedPath,
    [Parameter(Mandatory)][string]$SourceEvidencePath,
    [Parameter(Mandatory)][string]$OutputPath,
    [Parameter(Mandatory)][ValidatePattern('^[0-9]+\.[0-9]+\.[0-9]+$')][string]$Version,
    [Parameter(Mandatory)][ValidatePattern('^[0-9a-fA-F]{40}$')][string]$ExpectedProjectCommit,
    [Parameter(Mandatory)][ValidatePattern('^[0-9]+$')][string]$SourceRunId
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if (-not $IsWindows) { throw 'Direct Windows packaging requires PowerShell 7 on Windows.' }

$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$stage = (Resolve-Path -LiteralPath $StagedPath).Path.TrimEnd('\')
$sourceEvidence = (Resolve-Path -LiteralPath $SourceEvidencePath).Path
$evidenceRoot = Split-Path -Parent $sourceEvidence
$out = [IO.Path]::GetFullPath($OutputPath)
if ($out.Equals($stage, [StringComparison]::OrdinalIgnoreCase) -or
    $out.StartsWith($stage + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw 'OutputPath must be outside the input staged tree.'
}
$clientRelative = 'QinglanAscension/Binaries/Win64/QinglanAscension-Win64-Shipping.exe'
$client = Join-Path $stage $clientRelative
foreach ($required in @($client, (Join-Path $stage 'Engine'), (Join-Path $stage 'QinglanAscension/Content/Paks'))) {
    if (-not (Test-Path -LiteralPath $required)) { throw "Required staged game path is missing: $required" }
}
foreach ($extension in @('*.pak', '*.utoc', '*.ucas')) {
    $files = @(Get-ChildItem (Join-Path $stage 'QinglanAscension/Content/Paks') -Recurse -File -Filter $extension)
    if (-not $files.Count -or @($files | Where-Object Length -eq 0).Count) {
        throw "Staged Shipping content is missing or empty: $extension"
    }
}
if ((Get-Item -LiteralPath $client).Length -eq 0) { throw 'Staged Shipping executable is empty.' }

$source = Get-Content -LiteralPath $sourceEvidence -Raw | ConvertFrom-Json
if ($source.project_commit -ne $ExpectedProjectCommit -or $source.platform -ne 'Win64' -or
    $source.configuration -ne 'Shipping' -or -not @($source.files).Count) {
    throw 'Build evidence does not match the expected Win64 Shipping project commit.'
}
# Build evidence precedes Store metadata injection. Only known Store shell files
# may be absent from it; they are omitted from direct-download packages.
$catalog = @{}
foreach ($entry in $source.files) {
    $relative = [string]$entry.path
    if ([IO.Path]::IsPathRooted($relative) -or $relative -match '(^|[/\\])\.\.([/\\]|$)' -or
        $entry.sha256 -notmatch '^[0-9a-fA-F]{64}$') { throw "Invalid source manifest entry: $relative" }
    $full = [IO.Path]::GetFullPath((Join-Path $evidenceRoot $relative))
    if (-not $full.StartsWith($evidenceRoot.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw "Source manifest escapes its artifact root: $relative"
    }
    if ($catalog.ContainsKey($full)) { throw "Duplicate source manifest entry: $relative" }
    if (-not (Test-Path -LiteralPath $full -PathType Leaf) -or
        (Get-Item -LiteralPath $full).Length -ne [long]$entry.bytes -or
        (Get-FileHash -LiteralPath $full -Algorithm SHA256).Hash -ne $entry.sha256) {
        throw "Source artifact hash or size does not match its build evidence: $relative"
    }
    $catalog[$full] = $entry
}
if (-not $catalog.ContainsKey($client)) { throw 'Shipping executable is absent from the verified build manifest.' }
$gameFiles = @()
foreach ($file in Get-ChildItem -LiteralPath $stage -Recurse -File) {
    $relative = [IO.Path]::GetRelativePath($stage, $file.FullName).Replace('\', '/')
    if ($relative -match '^(Assets/|MicrosoftGame\.config$|AppxManifest\.xml$|BUILD-STATUS\.txt$|build-evidence\.json$)') {
        continue
    }
    if (-not $catalog.ContainsKey($file.FullName)) {
        throw "Unverified file in staged game tree: $relative"
    }
    $gameFiles += $file
}

function Get-SignatureRecord([string]$Path, [switch]$MicrosoftOnly) {
    $signature = Get-AuthenticodeSignature -LiteralPath $Path
    if ($signature.Status -ne 'Valid' -or -not $signature.SignerCertificate) {
        throw "Authenticode validation failed for $Path ($($signature.Status))."
    }
    if ($MicrosoftOnly -and $signature.SignerCertificate.Subject -notmatch '(^|,\s*)O=Microsoft Corporation(,|$)') {
        throw "Prerequisite signer is not Microsoft Corporation: $($signature.SignerCertificate.Subject)"
    }
    return @{
        status = [string]$signature.Status
        signer = $signature.SignerCertificate.Subject
        certificate_thumbprint = $signature.SignerCertificate.Thumbprint
        timestamp_signer = if ($signature.TimeStamperCertificate) { $signature.TimeStamperCertificate.Subject } else { $null }
    }
}

function Get-OfficialCompiler([string]$WorkingPath) {
    $lockedVersion = '7.1.1'
    $registryPaths = @(
        'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Inno Setup 7_is1',
        'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\Inno Setup 7_is1',
        'HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Inno Setup 7_is1'
    )
    foreach ($key in $registryPaths) {
        $registration = Get-ItemProperty -LiteralPath $key -ErrorAction SilentlyContinue
        if (-not $registration -or -not $registration.PSObject.Properties['InstallLocation']) { continue }
        $candidate = Join-Path $registration.InstallLocation 'ISCC.exe'
        if (-not (Test-Path -LiteralPath $candidate -PathType Leaf)) { continue }
        $compilerVersion = (Get-Item -LiteralPath $candidate).VersionInfo
        if (($compilerVersion.FileMajorPart, $compilerVersion.FileMinorPart, $compilerVersion.FileBuildPart -join '.') -ne $lockedVersion) { continue }
        $signature = Get-AuthenticodeSignature -LiteralPath $candidate
        if ($signature.Status -eq 'Valid' -and $signature.SignerCertificate.Subject -match 'JRSoftware|Jordan Russell') {
            return @{ path = $candidate; version = $lockedVersion; source = 'signed official registry installation'; signature = (Get-SignatureRecord $candidate) }
        }
    }
    $url = 'https://github.com/jrsoftware/issrc/releases/download/is-7_1_1/innosetup-7.1.1-x64.exe'
    $expectedHash = '8a5a6711c3766a6e5fb74f280ba0e3551e719ce94538d901fdbd1f8f9920b3f0'
    $installer = Join-Path $WorkingPath 'innosetup-7.1.1-x64.exe'
    Invoke-WebRequest -Uri $url -OutFile $installer -MaximumRedirection 5 -TimeoutSec 300
    if ((Get-FileHash -LiteralPath $installer -Algorithm SHA256).Hash -ne $expectedHash) {
        throw 'Official Inno Setup installer SHA-256 mismatch.'
    }
    $installerSignature = Get-SignatureRecord $installer
    $installDirectory = Join-Path $WorkingPath 'compiler'
    $compilerLog = Join-Path $WorkingPath 'compiler-install.log'
    $arguments = '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /SP- /DIR="' + $installDirectory + '" /LOG="' + $compilerLog + '"'
    $process = Start-Process -FilePath $installer -ArgumentList $arguments -Wait -PassThru
    if ($process.ExitCode -ne 0) { throw "Official Inno Setup installation failed ($($process.ExitCode))." }
    $compiler = Join-Path $installDirectory 'ISCC.exe'
    if (-not (Test-Path -LiteralPath $compiler -PathType Leaf)) { throw 'Official installation did not produce ISCC.exe.' }
    return @{ path = $compiler; version = $lockedVersion; source = $url; installer_sha256 = $expectedHash; installer_signature = $installerSignature }
}

New-Item -ItemType Directory -Force -Path $out | Out-Null
$work = Join-Path ([IO.Path]::GetTempPath()) ('qinglan-direct-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $work | Out-Null
try {
    $payload = Join-Path $work 'payload'
    $buildOutput = Join-Path $work 'artifacts'
    New-Item -ItemType Directory -Path $payload, $buildOutput | Out-Null
    foreach ($file in $gameFiles) {
        $relative = [IO.Path]::GetRelativePath($stage, $file.FullName)
        $destination = Join-Path $payload $relative
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $destination) | Out-Null
        Copy-Item -LiteralPath $file.FullName -Destination $destination
        if ((Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash -ne $catalog[$file.FullName].sha256) {
            throw "Game payload changed during copy: $relative"
        }
    }
    $runtimeURL = 'https://aka.ms/vc14/vc_redist.x64.exe'
    $prerequisites = Join-Path $payload 'Prerequisites'
    New-Item -ItemType Directory -Path $prerequisites | Out-Null
    $runtime = Join-Path $prerequisites 'vc_redist.x64.exe'
    Invoke-WebRequest -Uri $runtimeURL -OutFile $runtime -MaximumRedirection 5 -TimeoutSec 300
    $runtimeSignature = Get-SignatureRecord $runtime -MicrosoftOnly
    $runtimeHash = (Get-FileHash -LiteralPath $runtime -Algorithm SHA256).Hash.ToLowerInvariant()
    $runtimeVersion = (Get-Item -LiteralPath $runtime).VersionInfo.FileVersion
    $compiler = Get-OfficialCompiler $work
    foreach ($language in @('Default.isl', 'Languages/ChineseSimplified.isl')) {
        if (-not (Test-Path -LiteralPath (Join-Path (Split-Path -Parent $compiler.path) $language))) {
            throw "Official compiler is missing required language resource: $language"
        }
    }

    # Preserve engine notices already present in the verified staged artifact.
    # Add accepted asset attribution from the same checkout as the packaging tools.
    $credits = [Collections.Generic.List[string]]::new()
    $credits.Add('Qinglan: Path of Ascension')
    $credits.Add('Copyright 2026 Qinglan Community. All rights reserved.')
    $credits.Add('')
    $credits.Add('Unreal Engine, Copyright 1998 - 2026, Epic Games, Inc. All rights reserved.')
    $credits.Add('Unreal and its logo are trademarks or registered trademarks of Epic Games, Inc.')
    $credits.Add('Portions of this software are copyright The FreeType Project (https://www.freetype.org). All rights reserved.')
    $credits.Add('')
    $credits.Add('Art and audio credits')
    foreach ($asset in Import-Csv -LiteralPath (Join-Path $repo 'assets/asset-manifest.csv')) {
        if ($asset.game_distribution_allowed -eq 'yes' -and $asset.public_path -ne '(none)') {
            $credits.Add("$($asset.asset_id) - $($asset.author) - $($asset.license_id)")
            $credits.Add("$($asset.source_url) | $($asset.license_url)")
        }
    }
    $credits.Add('')
    $credits.Add('Microsoft Visual C++ x64 Redistributable is provided under its Microsoft license terms.')
    $credits.Add('https://learn.microsoft.com/cpp/windows/latest-supported-vc-redist')
    $credits | Set-Content -LiteralPath (Join-Path $payload 'CREDITS.txt') -Encoding utf8
    Copy-Item -LiteralPath (Join-Path $repo 'PRIVACY.md') -Destination (Join-Path $payload 'PRIVACY.txt')
    @'
@echo off
setlocal
cd /d "%~dp0"
start "" "%~dp0QinglanAscension\Binaries\Win64\QinglanAscension-Win64-Shipping.exe"
'@ | Set-Content -LiteralPath (Join-Path $payload 'Launch-Qinglan.bat') -Encoding ascii
    @"
Qinglan: Path of Ascension
Version $Version | Windows x64

Windows 10/11 (64-bit) and a DirectX 12 GPU are required.
The game supports English and Simplified Chinese in Settings.

Installer: run Qinglan-$Version-Windows-x64-Setup.exe and follow the wizard.
It installs the required Microsoft Visual C++ runtime automatically.

Portable ZIP: extract the entire archive to a local folder. Run
Prerequisites\vc_redist.x64.exe once, then double-click Launch-Qinglan.bat.
Keep the Engine and QinglanAscension folders together.

Saved games: %LOCALAPPDATA%\QinglanAscension\Saved\SaveGames\
Updates and uninstall preserve this separate save folder. Close the game
before updating. To update a portable copy, extract the new ZIP to a new folder.

The game is free, offline and single-player. No account is required.
https://github.com/shoutoutuoadi325/qinglan-wendao
"@ | Set-Content -LiteralPath (Join-Path $payload 'INSTALL.txt') -Encoding utf8
    @{
        version = $Version
        source_run_id = $SourceRunId
        source_project_commit = $ExpectedProjectCommit.ToLowerInvariant()
        source_manifest_sha256 = (Get-FileHash -LiteralPath $sourceEvidence -Algorithm SHA256).Hash.ToLowerInvariant()
        game_executable_sha256 = $catalog[$client].sha256
        prerequisite_sha256 = $runtimeHash
    } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $payload 'PACKAGE-INFO.json') -Encoding utf8
    $payloadInventory = @(Get-ChildItem -LiteralPath $payload -Recurse -File | Sort-Object FullName | ForEach-Object {
        @{
            path = [IO.Path]::GetRelativePath($payload, $_.FullName).Replace('\', '/')
            bytes = $_.Length
            sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    })

    $compilerArgs = @('/Qp', "/DPayloadPath=$payload", "/DOutputPath=$buildOutput", "/DReleaseVersion=$Version", (Join-Path $repo 'packaging/windows/installer.iss'))
    & $compiler.path @compilerArgs
    if ($LASTEXITCODE) { throw "Inno Setup compilation failed ($LASTEXITCODE)." }
    $setupName = "Qinglan-$Version-Windows-x64-Setup.exe"
    $zipName = "Qinglan-$Version-Windows-x64.zip"
    $setup = Join-Path $buildOutput $setupName
    if (-not (Test-Path -LiteralPath $setup -PathType Leaf) -or (Get-Item -LiteralPath $setup).Length -eq 0) {
        throw 'Inno Setup did not produce a nonempty installer.'
    }
    $zip = Join-Path $buildOutput $zipName
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [IO.Compression.ZipFile]::CreateFromDirectory($payload, $zip, [IO.Compression.CompressionLevel]::Optimal, $false)
    $archive = [IO.Compression.ZipFile]::OpenRead($zip)
    try {
        if ($archive.Entries.Count -ne $payloadInventory.Count) { throw 'Portable archive inventory count mismatch.' }
        foreach ($file in $payloadInventory) {
            $entry = $archive.GetEntry($file.path)
            if (-not $entry -or $entry.Length -ne $file.bytes) { throw "Portable archive entry missing or wrong size: $($file.path)" }
            $stream = $entry.Open()
            try { $hash = (Get-FileHash -InputStream $stream -Algorithm SHA256).Hash } finally { $stream.Dispose() }
            if ($hash -ne $file.sha256) { throw "Portable archive file hash mismatch: $($file.path)" }
        }
    } finally { $archive.Dispose() }
    $artifacts = @($setup, $zip | ForEach-Object {
        @{
            name = [IO.Path]::GetFileName($_)
            bytes = (Get-Item -LiteralPath $_).Length
            sha256 = (Get-FileHash -LiteralPath $_ -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    })
    @{
        timestamp_utc = [DateTime]::UtcNow.ToString('o')
        version = $Version
        source_run_id = $SourceRunId
        source_project_commit = $ExpectedProjectCommit.ToLowerInvariant()
        source_build_manifest_sha256 = (Get-FileHash -LiteralPath $sourceEvidence -Algorithm SHA256).Hash.ToLowerInvariant()
        compiler = $compiler
        prerequisite = @{ url = $runtimeURL; version = $runtimeVersion; sha256 = $runtimeHash; authenticode = $runtimeSignature }
        app_id = '028A5C95-3510-56CD-B9D9-29F884E79E3E'
        runtime_tested = $false
        installer_compiled = $true
        portable_archive_hash_verified = $true
        files = $payloadInventory
        artifacts = $artifacts
    } | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $buildOutput 'direct-package-evidence.json') -Encoding utf8
    $artifacts | ForEach-Object { "$($_.sha256)  $($_.name)" } | Set-Content -LiteralPath (Join-Path $buildOutput 'SHA256SUMS') -Encoding ascii
    Get-ChildItem -LiteralPath $buildOutput -File | Copy-Item -Destination $out -Force
    Write-Host "Installer and verified portable archive ready in $out"
} finally {
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}
