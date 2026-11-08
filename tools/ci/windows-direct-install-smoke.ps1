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
        # The authored screenshot mode exits only after the game world has started.
        # NullRHI checks startup on the hosted runner without claiming DX12 rendering.
        $userPath = Join-Path $work "startup-$language"
        $gameArguments = '-nullrhi -nosound -unattended -nosplash -QinglanShot=install-smoke -QinglanShotDelay=2 -UserDir="' + $userPath + '"'
        $game = Start-Process -FilePath $exe -WorkingDirectory $installed -ArgumentList $gameArguments -PassThru
        if (-not $game.WaitForExit(120000)) {
            Stop-Process -Id $game.Id -Force
            throw 'Installed Shipping game did not exit through the startup capture mode.'
        }
        $game.Refresh()
        if ($game.ExitCode -ne 0) { throw "Installed Shipping startup failed ($($game.ExitCode))." }
        $uninstaller = Join-Path $installed 'unins000.exe'
        $uninstallLog = Join-Path $reports "uninstall-$language.log"
        $uninstallArguments = '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /LOG="' + $uninstallLog + '"'
        $uninstall = Start-Process -FilePath $uninstaller -ArgumentList $uninstallArguments -Wait -PassThru
        if ($uninstall.ExitCode -ne 0 -or (Test-Path -LiteralPath $exe)) { throw 'Uninstall did not remove the game.' }
        if (-not (Test-Path -LiteralPath $sentinel) -or
            (Get-FileHash -LiteralPath $sentinel -Algorithm SHA256).Hash -ne $sentinelHash) {
            throw 'Uninstall changed the separate saved-game sentinel.'
        }
        $checks.Add(@{
            language = $language
            install_exit_code = $process.ExitCode
            installed_files_verified = $verified
            shortcuts_verified = $true
            shipping_nullrhi_startup_exit_code = $game.ExitCode
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
    Remove-Item -LiteralPath $sentinel -Force -ErrorAction SilentlyContinue
    if (Test-Path -LiteralPath $work) { Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue }
}
