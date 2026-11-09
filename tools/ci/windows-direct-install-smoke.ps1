[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$PackagePath,
    [Parameter(Mandatory)][string]$ReportsPath
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if (-not $IsWindows) { throw 'Installer smoke checks require Windows.' }
$package = (Resolve-Path -LiteralPath $PackagePath).Path
$evidence = Get-Content (Join-Path $package 'direct-package-evidence.json') -Raw | ConvertFrom-Json
$setup = Join-Path $package "Qinglan-$($evidence.version)-Windows-x64-Setup.exe"
$reports = [IO.Path]::GetFullPath($ReportsPath)
New-Item -ItemType Directory -Force -Path $reports | Out-Null
$work = Join-Path $env:RUNNER_TEMP ('qinglan-install-test-' + [Guid]::NewGuid().ToString('N'))
$saveDirectory = Join-Path $env:LOCALAPPDATA 'QinglanAscension/Saved/SaveGames'
New-Item -ItemType Directory -Force -Path $saveDirectory | Out-Null
$sentinel = Join-Path $saveDirectory ('installer-test-' + [Guid]::NewGuid().ToString('N') + '.sav')
[IO.File]::WriteAllText($sentinel, 'Qinglan installer save preservation sentinel')
$sentinelHash = (Get-FileHash -LiteralPath $sentinel -Algorithm SHA256).Hash
$checks = [Collections.Generic.List[object]]::new()
$shell = New-Object -ComObject WScript.Shell
try {
    foreach ($language in @('english', 'chinesesimplified')) {
        $installed = Join-Path $work $language
        $installLog = Join-Path $reports "install-$language.log"
        $arguments = '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /SP- /TASKS=desktopicon /LANG=' + $language + ' /DIR="' + $installed + '" /LOG="' + $installLog + '"'
        $process = Start-Process -FilePath $setup -ArgumentList $arguments -Wait -PassThru
        if ($process.ExitCode -notin @(0, 3010)) { throw "Installation failed: $language ($($process.ExitCode))." }
        $verified = 0
        foreach ($file in $evidence.files) {
            # The prerequisite bootstrapper is embedded for extraction at install time.
            if ($file.path -like 'Prerequisites/*') { continue }
            $path = Join-Path $installed $file.path
            if (-not (Test-Path -LiteralPath $path -PathType Leaf) -or
                (Get-Item -LiteralPath $path).Length -ne $file.bytes -or
                (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $file.sha256) {
                throw "Installed file differs from the verified payload: $($file.path)"
            }
            $verified++
        }
        $exe = [IO.Path]::GetFullPath((Join-Path $installed 'QinglanAscension/Binaries/Win64/QinglanAscension-Win64-Shipping.exe'))
        foreach ($shortcut in @(
            (Join-Path ([Environment]::GetFolderPath('CommonPrograms')) 'Qinglan - Path of Ascension/Qinglan - Path of Ascension.lnk'),
            (Join-Path ([Environment]::GetFolderPath('CommonDesktopDirectory')) 'Qinglan - Path of Ascension.lnk')
        )) {
            if (-not (Test-Path -LiteralPath $shortcut)) { throw "Installer shortcut missing: $shortcut" }
            $link = $shell.CreateShortcut($shortcut)
            if ($link.TargetPath -ne $exe -or $link.WorkingDirectory -ne $installed) {
                throw "Installer shortcut has the wrong launch path: $shortcut"
            }
        }
        # A fresh profile frontend pauses world timers until a slot is selected.
        # Use the existing QA aid to confirm a real menu row, then the authored
        # screenshot mode exits after hub startup. NullRHI does not test DX12.
        $userPath = Join-Path $work "startup-$language"
        New-Item -ItemType Directory -Force -Path $userPath | Out-Null
        $gameArguments = '-nullrhi -nosound -unattended -nosplash -QinglanAutoSlot=0 -QinglanShot=install-smoke -QinglanShotDelay=2 -UserDir="' + $userPath + '"'
        $game = Start-Process -FilePath $exe -WorkingDirectory $installed -ArgumentList $gameArguments -PassThru -RedirectStandardOutput (Join-Path $reports "startup-$language.stdout.log") -RedirectStandardError (Join-Path $reports "startup-$language.stderr.log")
        if (-not $game.WaitForExit(120000)) {
            Stop-Process -Id $game.Id -Force
            throw 'Installed Shipping game did not exit through the startup capture mode.'
        }
        $game.Refresh()
        if ($game.ExitCode -ne 0) { throw "Installed Shipping startup failed ($($game.ExitCode))." }
        $profiles = @(Get-ChildItem -LiteralPath $userPath -Recurse -File -Filter 'QinglanProfile_0.sav')
        if ($profiles.Count -ne 1) { throw 'Cold startup did not create the selected profile in its isolated user directory.' }
        $profile = $profiles[0]
        $profileBytes = [IO.File]::ReadAllBytes($profile.FullName)
        # v0.1.3 uses container version 1 and CurrentSchemaVersion 2.
        if ($profileBytes.Length -le 28 -or [BitConverter]::ToUInt32($profileBytes, 0) -ne 0x514C5341 -or
            [BitConverter]::ToInt32($profileBytes, 4) -ne 1 -or [BitConverter]::ToInt32($profileBytes, 8) -ne 2 -or
            [BitConverter]::ToInt32($profileBytes, 12) -lt 2 -or [BitConverter]::ToInt32($profileBytes, 16) -ne ($profileBytes.Length - 28)) {
            throw 'Cold startup did not complete profile creation and the hub arrival autosave.'
        }
        $profileHash = (Get-FileHash -LiteralPath $profile.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        Copy-Item -LiteralPath $profile.FullName -Destination (Join-Path $reports "startup-$language-profile.sav")
        $uninstaller = Join-Path $installed 'unins000.exe'
        $uninstallLog = Join-Path $reports "uninstall-$language.log"
        $uninstallArguments = '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /LOG="' + $uninstallLog + '"'
        $uninstall = Start-Process -FilePath $uninstaller -ArgumentList $uninstallArguments -Wait -PassThru
        if ($uninstall.ExitCode -ne 0 -or (Test-Path -LiteralPath $exe)) { throw 'Uninstall did not remove the game.' }
        if (-not (Test-Path -LiteralPath $sentinel) -or
            (Get-FileHash -LiteralPath $sentinel -Algorithm SHA256).Hash -ne $sentinelHash) {
            throw 'Uninstall changed the separate saved-game sentinel.'
        }
        if (-not (Test-Path -LiteralPath $profile.FullName) -or
            (Get-FileHash -LiteralPath $profile.FullName -Algorithm SHA256).Hash.ToLowerInvariant() -ne $profileHash) {
            throw 'Uninstall changed the profile created by the installed game.'
        }
        $checks.Add(@{
            language = $language
            install_exit_code = $process.ExitCode
            installed_files_verified = $verified
            shortcuts_verified = $true
            shipping_nullrhi_startup_exit_code = $game.ExitCode
            cold_start_profile_verified = $true
            startup_profile_bytes = $profileBytes.Length
            startup_profile_sequence = [BitConverter]::ToInt32($profileBytes, 12)
            startup_profile_schema = [BitConverter]::ToInt32($profileBytes, 8)
            startup_profile_sha256 = $profileHash
            startup_profile_preserved = $true
            uninstall_exit_code = $uninstall.ExitCode
            saved_game_sentinel_preserved = $true
        })
    }
    @{
        status = 'passed'
        source_project_commit = $evidence.source_project_commit
        source_run_id = $evidence.source_run_id
        version = $evidence.version
        checks = $checks.ToArray()
        dx12_rendering_tested = $false
    } | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $reports 'install-smoke-evidence.json') -Encoding utf8
    Write-Host 'Both installer languages, file hashes, shortcuts, Shipping startup, uninstall and save preservation passed.'
} finally {
    if (Test-Path -LiteralPath $work) {
        Get-ChildItem -LiteralPath $work -Directory -Filter 'startup-*' | ForEach-Object {
            Copy-Item -LiteralPath $_.FullName -Destination $reports -Recurse -Force
        }
    }
    Remove-Item -LiteralPath $sentinel -Force -ErrorAction SilentlyContinue
    if (Test-Path -LiteralPath $work) { Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue }
}
