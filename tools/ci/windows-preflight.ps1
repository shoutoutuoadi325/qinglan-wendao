<#
.SYNOPSIS
Inspects a Windows build host and the committed Qinglan inputs without changing them.
.DESCRIPTION
Writes a JSON report and, in GitHub Actions, a step summary. Resource measurements
are observations, not promises that Unreal Engine fits or finishes on this host.
Use -RequireBuildTools to make missing VC tools or Windows SDK a failing result.
#>
[CmdletBinding()]
param(
    [string]$ReportPath,
    [switch]$RequireBuildTools
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
if ([string]::IsNullOrWhiteSpace($ReportPath)) {
    $reportRoot = if ($env:RUNNER_TEMP) { $env:RUNNER_TEMP } else { Join-Path $repoRoot '.local/ci' }
    $ReportPath = Join-Path $reportRoot 'windows-preflight.json'
}
$ReportPath = [IO.Path]::GetFullPath($ReportPath)
$failures = [Collections.Generic.List[string]]::new()
$warnings = [Collections.Generic.List[string]]::new()
$report = [ordered]@{
    schema_version = 1
    timestamp_utc = [DateTime]::UtcNow.ToString('o')
    scope = 'Host and committed-input inspection; no Unreal build, gameplay, DX12, or Store validation.'
    require_build_tools = [bool]$RequireBuildTools
    host = $null
    volumes = @()
    visual_studio = @()
    windows_sdks = @()
    content = $null
    warnings = @()
    failures = @()
    status = 'running'
}

function Format-GiB([double]$Bytes) {
    return [Math]::Round($Bytes / 1GB, 2)
}

if (-not $IsWindows) {
    $failures.Add('This preflight must run in Windows PowerShell 7 (pwsh).')
} else {
    try {
        $computer = Get-CimInstance -ClassName Win32_ComputerSystem
        $os = Get-CimInstance -ClassName Win32_OperatingSystem
        $architecture = [Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString()
        $report.host = [ordered]@{
            os = $os.Caption
            os_version = $os.Version
            architecture = $architecture
            logical_processors = [int]$computer.NumberOfLogicalProcessors
            memory_total_gib = Format-GiB ([double]$computer.TotalPhysicalMemory)
            memory_available_gib = Format-GiB ([double]$os.FreePhysicalMemory * 1KB)
            powershell = $PSVersionTable.PSVersion.ToString()
        }
        Write-Host ('Host: {0} {1}, {2}, {3} logical CPUs, {4} GiB RAM ({5} GiB available)' -f
            $report.host.os, $report.host.os_version, $architecture,
            $report.host.logical_processors, $report.host.memory_total_gib,
            $report.host.memory_available_gib)
        if ($architecture -ne 'X64') {
            $failures.Add("An x64 Windows host is required; detected $architecture.")
        }
    } catch {
        $failures.Add("Could not inspect Windows host via CIM: $($_.Exception.Message)")
    }

    try {
        $report.volumes = @(Get-Volume | Where-Object { $_.DriveLetter -and $_.Size -gt 0 } |
            Sort-Object DriveLetter | ForEach-Object {
                [ordered]@{
                    drive = "$($_.DriveLetter):"
                    filesystem = $_.FileSystemType.ToString()
                    size_gib = Format-GiB ([double]$_.Size)
                    free_gib = Format-GiB ([double]$_.SizeRemaining)
                }
            })
        foreach ($volume in $report.volumes) {
            Write-Host ('Disk {0}: {1} GiB free / {2} GiB total ({3})' -f
                $volume.drive, $volume.free_gib, $volume.size_gib, $volume.filesystem)
        }
    } catch {
        $warnings.Add("Could not inspect disk volumes: $($_.Exception.Message)")
    }

    try {
        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
        if (Test-Path -LiteralPath $vswhere -PathType Leaf) {
            $rawInstances = & $vswhere -all -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -format json -utf8
            if ($LASTEXITCODE -ne 0) { throw "vswhere returned exit code $LASTEXITCODE." }
            $instances = @($rawInstances -join "`n" | ConvertFrom-Json)
            foreach ($instance in $instances) {
                $msvcRoot = Join-Path $instance.installationPath 'VC/Tools/MSVC'
                if (-not (Test-Path -LiteralPath $msvcRoot -PathType Container)) { continue }
                foreach ($toolset in @(Get-ChildItem -LiteralPath $msvcRoot -Directory | Sort-Object Name -Descending)) {
                    $compiler = Join-Path $toolset.FullName 'bin/Hostx64/x64/cl.exe'
                    if (-not (Test-Path -LiteralPath $compiler -PathType Leaf)) { continue }
                    $report.visual_studio += [ordered]@{
                        installation_version = $instance.installationVersion
                        installation_path = $instance.installationPath
                        msvc_toolset = $toolset.Name
                        compiler_version = (Get-Item -LiteralPath $compiler).VersionInfo.FileVersion
                    }
                }
            }
        }
    } catch {
        $warnings.Add("Could not inspect Visual Studio VC tools: $($_.Exception.Message)")
    }
    foreach ($vs in $report.visual_studio) {
        Write-Host ('VC tools: Visual Studio {0}, MSVC {1}, compiler {2}' -f
            $vs.installation_version, $vs.msvc_toolset, $vs.compiler_version)
    }
    if ($report.visual_studio.Count -eq 0) {
        $message = 'No x64 Visual C++ compiler found through vswhere.'
        if ($RequireBuildTools) { $failures.Add($message) } else { $warnings.Add($message) }
    }

    try {
        $kits = Get-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots' -ErrorAction SilentlyContinue
        if ($null -ne $kits -and $kits.PSObject.Properties['KitsRoot10']) {
            $sdkRoot = $kits.KitsRoot10
            $includeRoot = Join-Path $sdkRoot 'Include'
            if (Test-Path -LiteralPath $includeRoot -PathType Container) {
                foreach ($version in @(Get-ChildItem -LiteralPath $includeRoot -Directory | Sort-Object Name -Descending)) {
                    if ($version.Name -notmatch '^10\.0\.\d+\.0$') { continue }
                    $requiredFiles = @(
                        (Join-Path $version.FullName 'um/Windows.h'),
                        (Join-Path $version.FullName 'shared/sdkddkver.h'),
                        (Join-Path $version.FullName 'ucrt/stdio.h'),
                        (Join-Path $sdkRoot "Lib/$($version.Name)/um/x64/kernel32.lib"),
                        (Join-Path $sdkRoot "Lib/$($version.Name)/ucrt/x64/ucrt.lib"),
                        (Join-Path $sdkRoot "bin/$($version.Name)/x64/rc.exe")
                    )
                    $missingFiles = @($requiredFiles | Where-Object { -not (Test-Path -LiteralPath $_ -PathType Leaf) })
                    $report.windows_sdks += [ordered]@{
                        version = $version.Name
                        root = $sdkRoot
                        complete_x64 = ($missingFiles.Count -eq 0)
                        missing_files = $missingFiles
                    }
                    Write-Host ('Windows SDK: {0}, x64 headers/libraries/rc complete: {1}' -f
                        $version.Name, ($missingFiles.Count -eq 0))
                }
            }
        }
    } catch {
        $warnings.Add("Could not inspect Windows SDK: $($_.Exception.Message)")
    }
    if (@($report.windows_sdks | Where-Object { $_.complete_x64 }).Count -eq 0) {
        $message = 'No complete x64 Windows 10/11 SDK found (headers, libraries, and resource compiler).'
        if ($RequireBuildTools) { $failures.Add($message) } else { $warnings.Add($message) }
    }
}

try {
    if (-not (Get-Command git -ErrorAction SilentlyContinue)) { throw 'git is not installed.' }
    $tracked = @(& git -C $repoRoot -c core.quotePath=false ls-files -- game/Content)
    if ($LASTEXITCODE -ne 0) { throw "git ls-files returned exit code $LASTEXITCODE." }
    $tracked = @($tracked | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
    if ($tracked.Count -eq 0) { throw 'No tracked Content files were found.' }
    $missing = [Collections.Generic.List[string]]::new()
    $pointers = [Collections.Generic.List[string]]::new()
    [long]$contentBytes = 0
    foreach ($relative in $tracked) {
        $asset = Join-Path $repoRoot $relative
        if (-not (Test-Path -LiteralPath $asset -PathType Leaf)) {
            $missing.Add($relative)
            continue
        }
        $stream = [IO.File]::OpenRead($asset)
        try {
            $contentBytes += $stream.Length
            $prefix = [byte[]]::new(128)
            $read = $stream.Read($prefix, 0, $prefix.Length)
            if ([Text.Encoding]::UTF8.GetString($prefix, 0, $read).StartsWith('version https://git-lfs.github.com/spec/v1')) {
                $pointers.Add($relative)
            }
        } finally {
            $stream.Dispose()
        }
    }
    $requiredMaps = @('L_Hub', 'L_BambooHollow', 'L_AshenVerge', 'L_TribulationArena')
    $missingMaps = @($requiredMaps | Where-Object {
        $relative = "game/Content/Qinglan/Maps/$_.umap"
        $relative -notin $tracked -or -not (Test-Path -LiteralPath (Join-Path $repoRoot $relative) -PathType Leaf)
    })
    $report.content = [ordered]@{
        tracked_files = $tracked.Count
        total_bytes = $contentBytes
        missing_files = @($missing.ToArray())
        lfs_pointer_files = @($pointers.ToArray())
        required_maps = $requiredMaps
        missing_maps = $missingMaps
    }
    Write-Host ('Content: {0} tracked files, {1} MiB, {2} missing, {3} LFS pointers, {4}/4 required maps present' -f
        $tracked.Count, [Math]::Round($contentBytes / 1MB, 2), $missing.Count,
        $pointers.Count, (4 - $missingMaps.Count))
    if ($missing.Count -gt 0) { $failures.Add("Tracked Content files are missing: $($missing -join ', ')") }
    if ($pointers.Count -gt 0) { $failures.Add("Content contains unresolved Git LFS pointers: $($pointers -join ', ')") }
    if ($missingMaps.Count -gt 0) { $failures.Add("Required committed maps are missing: $($missingMaps -join ', ')") }
} catch {
    $failures.Add("Could not inspect committed Content inputs: $($_.Exception.Message)")
}

$report.warnings = @($warnings.ToArray())
$report.failures = @($failures.ToArray())
$report.status = if ($failures.Count -eq 0) { 'passed' } else { 'failed' }
$reportDirectory = Split-Path -Parent $ReportPath
[void][IO.Directory]::CreateDirectory($reportDirectory)
[IO.File]::WriteAllText($ReportPath, ($report | ConvertTo-Json -Depth 8), [Text.UTF8Encoding]::new($false))
foreach ($warning in $warnings) { Write-Warning $warning }
foreach ($failure in $failures) { Write-Host "ERROR: $failure" }
Write-Host "Preflight $($report.status). JSON report: $ReportPath"

if ($env:GITHUB_STEP_SUMMARY) {
    $summary = @(
        '### Windows preflight',
        '',
        "Result: **$($report.status)**.",
        '',
        '- Inspected OS/architecture, CPU/RAM, disk volumes, VC toolsets, Windows SDK, and committed Content inputs.',
        '- Resource values are observations; this preflight does not certify sufficient build space or time.',
        '- No Unreal compilation, gameplay, DX12, GDK packaging, or Store certification was performed.'
    )
    if ($report.content) { $summary += "- Tracked Content: $($report.content.tracked_files) files; unresolved LFS pointers: $($report.content.lfs_pointer_files.Count)." }
    foreach ($failure in $failures) { $summary += "- Failure: $failure" }
    [IO.File]::AppendAllText($env:GITHUB_STEP_SUMMARY, (($summary -join "`n") + "`n"), [Text.UTF8Encoding]::new($false))
}
if ($failures.Count -gt 0) { exit 1 }
