# Acquire a licensed engine inside this disposable runner. Never publish it.
[CmdletBinding()]
param([ValidateSet('archive', 'source')][string]$Source = 'archive')
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$root = Join-Path $env:RUNNER_TEMP 'qinglan-engine'
$reports = Join-Path $env:RUNNER_TEMP 'qinglan-win64/reports'
New-Item -ItemType Directory -Force $reports | Out-Null
if (Test-Path $root) { throw 'Engine destination already exists; use a fresh runner.' }
$freeGiB = (Get-PSDrive -Name ([IO.Path]::GetPathRoot($root).Substring(0, 1))).Free / 1GB

if ($Source -eq 'archive') {
    if (-not $env:UE_WINDOWS_ARCHIVE_URL -or -not $env:UE_WINDOWS_ARCHIVE_SHA256) {
        throw 'Missing UE_WINDOWS_ARCHIVE_URL secret or UE_WINDOWS_ARCHIVE_SHA256 variable. See docs/dev/github-windows-build.md. No Windows game has been built.'
    }
    $uri = [uri]$env:UE_WINDOWS_ARCHIVE_URL
    if ($uri.Scheme -ne 'https' -or $uri.UserInfo) { throw 'Use an HTTPS engine download URL without embedded user/password.' }
    if ($env:UE_WINDOWS_ARCHIVE_SHA256 -notmatch '^[a-fA-F0-9]{64}$') { throw 'Engine archive SHA-256 must contain 64 hexadecimal characters.' }
    if ($freeGiB -lt 80) { throw "Only $([math]::Round($freeGiB, 1)) GiB free; archive builds require at least 80 GiB before download." }
    $archive = Join-Path $env:RUNNER_TEMP 'qinglan-engine.zip'
    # Do not print the signed URL or downloader exception text into public logs.
    try { Invoke-WebRequest -Uri $uri -OutFile $archive -MaximumRedirection 5 }
    catch { throw 'Licensed engine download failed. Check URL expiry and access; URL and response were omitted from logs.' }
    if ((Get-FileHash $archive -Algorithm SHA256).Hash -ne $env:UE_WINDOWS_ARCHIVE_SHA256) { throw 'Engine archive SHA-256 mismatch.' }
    New-Item -ItemType Directory $root | Out-Null
    & 7z x $archive "-o$root" -y -bso0 -bsp0
    if ($LASTEXITCODE) { throw "Engine extraction failed ($LASTEXITCODE)." }
    Remove-Item $archive
    # Archive format: Engine/... immediately at the root, without an outer UE folder.
} else {
    if (-not $env:UE_SOURCE_TOKEN) {
        throw 'Missing UE_SOURCE_TOKEN secret with read access to EpicGames/UnrealEngine. The repository GITHUB_TOKEN cannot access Epic source. No Windows game has been built.'
    }
    if ($freeGiB -lt 110) { throw "Only $([math]::Round($freeGiB, 1)) GiB free; the experimental source route requires 110 GiB before checkout. Use a smaller installed-engine archive instead." }
    $lock = Get-Content (Join-Path $repo 'tools/build/versions.lock') -Raw
    if ($lock -notmatch '(?m)^# engine_commit\s*=\s*([0-9a-f]{40})\s*$') { throw 'Engine commit is missing from versions.lock.' }
    $revision = $Matches[1]
    # Askpass reads the secret from the environment, never from a command argument,
    # URL, Git config, or on-disk token file. Disable inherited credential helpers.
    $askpass = Join-Path $env:RUNNER_TEMP 'qinglan-askpass.cmd'
    '@echo off', 'echo %UE_SOURCE_TOKEN%' | Set-Content $askpass -Encoding ascii
    $env:GIT_ASKPASS = $askpass
    $env:GIT_TERMINAL_PROMPT = '0'
    try {
        & git init -q $root
        if ($LASTEXITCODE) { throw 'Engine git init failed.' }
        & git -C $root remote add origin https://x-access-token@github.com/EpicGames/UnrealEngine.git
        if ($LASTEXITCODE) { throw 'Engine remote setup failed.' }
        & git -c credential.helper= -C $root fetch --depth 1 origin $revision
        if ($LASTEXITCODE) { throw 'Cannot fetch the locked Epic engine commit. Check Epic account access and token permissions.' }
        & git -C $root checkout --detach FETCH_HEAD
        if ($LASTEXITCODE) { throw 'Engine checkout failed.' }
    } finally {
        Remove-Item $askpass -ErrorAction SilentlyContinue
        Remove-Item Env:GIT_ASKPASS -ErrorAction SilentlyContinue
        Remove-Item Env:UE_SOURCE_TOKEN -ErrorAction SilentlyContinue
    }
    # Call GitDependencies directly: Setup.bat pauses on failure, unsuitable for CI.
    # Keep IOS inputs: UAT compiles its IOS automation assembly even for Win64.
    Push-Location $root
    try {
        & ./Engine/Binaries/DotNET/GitDependencies/win-x64/GitDependencies.exe --force --no-cache --exclude=Mac --exclude=Linux --exclude=LinuxArm64 --exclude=Android --exclude=TVOS --exclude=VisionOS --exclude=HoloLens --exclude=Win32 --exclude=Samples --exclude=Templates --exclude=FeaturePacks
        if ($LASTEXITCODE) { throw "Engine dependency sync failed ($LASTEXITCODE)." }
    } finally { Pop-Location }

    # Clean up massive git metadata and unneeded files to reclaim disk space on disposable CI runner
    $gitDir = Join-Path $root '.git'
    if (Test-Path $gitDir) {
        cmd.exe /c "rd /s /q `"$gitDir`"" 2>&1 | Out-Null
        if (Test-Path $gitDir) {
            Get-ChildItem -Path $gitDir -Recurse -Force -ErrorAction SilentlyContinue | ForEach-Object {
                if ($_.Attributes -band [IO.FileAttributes]::ReadOnly) {
                    $_.Attributes = $_.Attributes -band (-bnot [IO.FileAttributes]::ReadOnly)
                }
            }
            Remove-Item -Recurse -Force $gitDir -ErrorAction SilentlyContinue
        }
    }
    foreach ($extra in @('Samples', 'Templates', 'Engine/Documentation')) {
        $extraPath = Join-Path $root $extra
        if (Test-Path $extraPath) {
            cmd.exe /c "rd /s /q `"$extraPath`"" 2>&1 | Out-Null
            Remove-Item -Recurse -Force $extraPath -ErrorAction SilentlyContinue
        }
    }
}

$versionPath = Join-Path $root 'Engine/Build/Build.version'
$version = Get-Content $versionPath -Raw | ConvertFrom-Json
if ("$($version.MajorVersion).$($version.MinorVersion).$($version.PatchVersion)" -ne '5.8.3') { throw 'This project requires Unreal Engine 5.8.3.' }
foreach ($relative in @('Engine/Build/BatchFiles/Build.bat', 'Engine/Build/BatchFiles/RunUAT.bat')) {
    if (-not (Test-Path (Join-Path $root $relative))) { throw "Engine input is missing: $relative" }
}
if ($Source -eq 'archive' -and -not (Test-Path (Join-Path $root 'Engine/Binaries/Win64/UnrealEditor-Cmd.exe'))) { throw 'Archive has no precompiled Win64 editor.' }
@{ source = $Source; build_version = $version; source_commit = $(if ($Source -eq 'source') { $revision } else { $null }); scope = 'Engine prepared only; no game build or runtime validation' } |
    ConvertTo-Json -Depth 6 | Set-Content (Join-Path $reports 'engine.json')
"QINGLAN_ENGINE_ROOT=$root" | Out-File $env:GITHUB_ENV -Append -Encoding utf8
