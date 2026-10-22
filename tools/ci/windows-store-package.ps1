<#
.SYNOPSIS
Packages a staged Win64 Shipping game for Microsoft Store submission.
.DESCRIPTION
Builds:
- GDK MSIXVC package (.msixvc + .ekb) using makepkg.exe.
  Acquires public GDK tools if not preinstalled on the runner.
- Standard Windows MSIX package (.msix) using Windows SDK MakeAppx.exe.
  Uses the preinstalled Windows SDK on GitHub-hosted runners.
- Validates the package using makepkg validate.
- Generates submission evidence and Partner Center upload instructions.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$StagedPath,

    [Parameter(Mandatory = $true)]
    [string]$OutputPath,

    [ValidateSet('both', 'msixvc', 'msix')]
    [string]$Format = 'both',

    [string]$IdentityName = '',
    [string]$PublisherId = '',
    [string]$PublisherDisplayName = '',
    [string]$StoreId = '',
    [string]$Version = '0.1.0.0',
    [string]$ReportsPath = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false

$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$StagedPath = [IO.Path]::GetFullPath($StagedPath)
$OutputPath = [IO.Path]::GetFullPath($OutputPath)
if ([string]::IsNullOrWhiteSpace($ReportsPath)) {
    $ReportsPath = Join-Path $OutputPath 'reports'
}
$ReportsPath = [IO.Path]::GetFullPath($ReportsPath)

New-Item -ItemType Directory -Force $OutputPath, $ReportsPath | Out-Null

Write-Host "=========================================================="
Write-Host "   Qinglan: Microsoft Store Package Builder               "
Write-Host "=========================================================="
Write-Host "Staged path:  $StagedPath"
Write-Host "Output path:  $OutputPath"
Write-Host "Target format:$Format"

# 1. Locate the staged executable
$candidates = @(Get-ChildItem -Path $StagedPath -Recurse -File -Filter 'QinglanAscension*.exe' |
    Where-Object { $_.FullName -match '\\Binaries\\Win64\\' })
if (-not $candidates.Count) {
    $candidates = @(Get-ChildItem -Path $StagedPath -Recurse -File -Filter '*.exe' |
        Where-Object { $_.Name -notlike '*CrashReportClient*' })
}
if (-not $candidates.Count) {
    throw "No Win64 executable found inside staged path: $StagedPath"
}
$clientExe = $candidates[0]
$exeRelative = [IO.Path]::GetRelativePath($StagedPath, $clientExe.FullName).Replace('/', '\')
Write-Host "Detected executable: $exeRelative"

# 2. Stage shell visual assets
$assetsSrc = Join-Path $repoRoot 'packaging/windows/assets'
$assetsDst = Join-Path $StagedPath 'Assets'
New-Item -ItemType Directory -Force $assetsDst | Out-Null

$requiredAssets = @('Square150x150Logo.png', 'Square44x44Logo.png', 'StoreLogo.png', 'SplashScreenImage.png')
foreach ($assetName in $requiredAssets) {
    $srcFile = Join-Path $assetsSrc $assetName
    if (-not (Test-Path -LiteralPath $srcFile -PathType Leaf)) {
        throw "Missing required visual asset: $srcFile"
    }
    Copy-Item -LiteralPath $srcFile -Destination (Join-Path $assetsDst $assetName) -Force
}
Write-Host "Shell visual assets staged to $assetsDst"

# 3. Resolve identity values
if ([string]::IsNullOrWhiteSpace($IdentityName)) {
    $IdentityName = if ($env:STORE_IDENTITY_NAME) { $env:STORE_IDENTITY_NAME } else { 'QinglanCommunity.QinglanPathofAscension' }
}
if ([string]::IsNullOrWhiteSpace($PublisherId)) {
    $PublisherId = if ($env:STORE_PUBLISHER_ID) { $env:STORE_PUBLISHER_ID } else { 'CN=QinglanCommunity' }
}
if ([string]::IsNullOrWhiteSpace($PublisherDisplayName)) {
    $PublisherDisplayName = if ($env:STORE_PUBLISHER_DISPLAY_NAME) { $env:STORE_PUBLISHER_DISPLAY_NAME } else { 'Qinglan Community' }
}
if ([string]::IsNullOrWhiteSpace($StoreId)) {
    $StoreId = if ($env:STORE_PRODUCT_ID) { $env:STORE_PRODUCT_ID } elseif ($env:STORE_ID) { $env:STORE_ID } else { '' }
}
if ([string]::IsNullOrWhiteSpace($Version)) {
    $Version = if ($env:STORE_PACKAGE_VERSION) { $env:STORE_PACKAGE_VERSION } else { '0.1.0.0' }
}

$isTestIdentity = ($PublisherId -eq 'CN=QinglanCommunity' -or $IdentityName -eq 'QinglanCommunity.QinglanPathofAscension')
Write-Host "Identity Name:          $IdentityName"
Write-Host "Publisher:              $PublisherId"
Write-Host "Publisher Display Name: $PublisherDisplayName"
Write-Host "Package Version:        $Version"
Write-Host "Store ID:               $(if ($StoreId) { $StoreId } else { '(none - draft mode)' })"
Write-Host "Is Test Identity:       $isTestIdentity"

$packagesCreated = [Collections.Generic.List[PSObject]]::new()
$warnings = [Collections.Generic.List[string]]::new()

# Helper: Find or acquire makepkg.exe
function Get-MakePkgTool {
    $potentialPaths = @()
    if ($env:GameDK) {
        $potentialPaths += (Join-Path $env:GameDK 'bin\makepkg.exe')
    }
    if ($env:GDK_PATH) {
        $potentialPaths += (Join-Path $env:GDK_PATH 'makepkg.exe')
        $potentialPaths += (Join-Path $env:GDK_PATH 'bin\makepkg.exe')
    }
    $potentialPaths += 'C:\Program Files (x86)\Microsoft GDK\bin\makepkg.exe'

    foreach ($p in $potentialPaths) {
        if (Test-Path -LiteralPath $p -PathType Leaf) {
            return $p
        }
    }
    $cmd = Get-Command 'makepkg.exe' -ErrorAction SilentlyContinue
    if ($cmd) {
        return $cmd.Source
    }

    # Attempt automated acquisition of Microsoft Public GDK
    Write-Host "makepkg.exe not found on system. Attempting to acquire Microsoft Public GDK..."
    $gdkDest = Join-Path $env:RUNNER_TEMP 'microsoft-gdk'
    $localMakePkg = Join-Path $gdkDest 'bin\makepkg.exe'
    if (Test-Path -LiteralPath $localMakePkg -PathType Leaf) {
        return $localMakePkg
    }

    $downloadUrl = if ($env:GDK_ARCHIVE_URL) {
        $env:GDK_ARCHIVE_URL
    } else {
        'https://github.com/microsoft/GDK/releases/download/October-2025-Update-5-v2510.5.6322/GDK_2510.5.6322.zip'
    }

    $gdkZip = Join-Path $env:RUNNER_TEMP 'gdk.zip'
    try {
        Write-Host "Downloading Microsoft GDK from $downloadUrl ..."
        Invoke-WebRequest -Uri $downloadUrl -OutFile $gdkZip -MaximumRedirection 5
        New-Item -ItemType Directory -Force $gdkDest | Out-Null
        Write-Host "Extracting GDK tools via 7z..."
        & 7z x $gdkZip "-o$gdkDest" "bin/*" -r -y -bso0 -bsp0
        if (Test-Path -LiteralPath $localMakePkg -PathType Leaf) {
            Write-Host "Successfully extracted makepkg.exe at $localMakePkg"
            return $localMakePkg
        }
        & 7z x $gdkZip "-o$gdkDest" "*.exe" "*.dll" -r -y -bso0 -bsp0
        $found = @(Get-ChildItem -Path $gdkDest -Recurse -File -Filter 'makepkg.exe')
        if ($found.Count -gt 0) {
            Write-Host "Successfully found makepkg.exe at $($found[0].FullName)"
            return $found[0].FullName
        }
    } catch {
        Write-Warning "Could not automatically download or extract GDK: $($_.Exception.Message)"
    } finally {
        if (Test-Path -LiteralPath $gdkZip -PathType Leaf) {
            Remove-Item -LiteralPath $gdkZip -Force -ErrorAction SilentlyContinue
        }
    }

    return $null
}

# Helper: Find MakeAppx.exe in Windows SDK
function Get-MakeAppxTool {
    $kits = Get-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots' -ErrorAction SilentlyContinue
    if ($null -ne $kits -and $kits.PSObject.Properties['KitsRoot10']) {
        $sdkRoot = $kits.KitsRoot10
        $binRoot = Join-Path $sdkRoot 'bin'
        if (Test-Path -LiteralPath $binRoot -PathType Container) {
            $candidates = @(Get-ChildItem -LiteralPath $binRoot -Directory |
                Where-Object { $_.Name -match '^10\.0\.\d+\.0$' } |
                Sort-Object Name -Descending)
            foreach ($version in $candidates) {
                $makeappx = Join-Path $version.FullName 'x64/makeappx.exe'
                if (Test-Path -LiteralPath $makeappx -PathType Leaf) {
                    return $makeappx
                }
            }
        }
    }
    $cmd = Get-Command 'makeappx.exe' -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    return $null
}

# 4. Process GDK MSIXVC packaging
if ($Format -in @('both', 'msixvc')) {
    Write-Host "`n--- [GDK MSIXVC Packaging] ---"
    $makepkg = Get-MakePkgTool

    if (-not $makepkg) {
        $msg = "makepkg.exe is unavailable. GDK MSIXVC packaging skipped."
        if ($Format -eq 'msixvc') {
            throw $msg
        } else {
            Write-Warning $msg
            $warnings.Add($msg)
        }
    } else {
        Write-Host "Using makepkg at: $makepkg"

        # Generate MicrosoftGame.config
        $xmlLines = [Collections.Generic.List[string]]::new()
        $xmlLines.Add('<?xml version="1.0" encoding="utf-8"?>')
        $xmlLines.Add('<!-- Generated by Qinglan Microsoft Store build automation -->')
        $xmlLines.Add('<Game configVersion="1">')
        $xmlLines.Add("  <Identity Name=""$IdentityName"" Publisher=""$PublisherId"" Version=""$Version"" />")
        if (-not [string]::IsNullOrWhiteSpace($StoreId)) {
            $xmlLines.Add("  <StoreId>$StoreId</StoreId>")
        }
        $xmlLines.Add('  <Resources>')
        $xmlLines.Add('    <Resource Language="en-US" />')
        $xmlLines.Add('    <Resource Language="zh-Hans" />')
        $xmlLines.Add('  </Resources>')
        $xmlLines.Add('  <ExecutableList>')
        $xmlLines.Add("    <Executable Name=""$exeRelative"" Id=""Game"" TargetDeviceFamily=""PC"" />")
        $xmlLines.Add('  </ExecutableList>')
        $xmlLines.Add('  <ShellVisuals DefaultDisplayName="Qinglan: Path of Ascension"')
        $xmlLines.Add("    PublisherDisplayName=""$PublisherDisplayName""")
        $xmlLines.Add('    StoreLogo="Assets\StoreLogo.png"')
        $xmlLines.Add('    Square150x150Logo="Assets\Square150x150Logo.png"')
        $xmlLines.Add('    Square44x44Logo="Assets\Square44x44Logo.png"')
        $xmlLines.Add('    Description="An offline cultivation adventure."')
        $xmlLines.Add('    BackgroundColor="#17212A"')
        $xmlLines.Add('    SplashScreenImage="Assets\SplashScreenImage.png" />')
        $xmlLines.Add('  <DesktopRegistration>')
        $xmlLines.Add('    <ProcessorArchitecture>x64</ProcessorArchitecture>')
        $xmlLines.Add('  </DesktopRegistration>')
        $xmlLines.Add('</Game>')
        $gameConfig = $xmlLines -join "`n"

        $gameConfigFile = Join-Path $StagedPath 'MicrosoftGame.config'
        [IO.File]::WriteAllText($gameConfigFile, $gameConfig, [Text.UTF8Encoding]::new($false))
        Write-Host "Created $gameConfigFile"

        $msixvcDir = Join-Path $OutputPath 'msixvc'
        New-Item -ItemType Directory -Force $msixvcDir | Out-Null

        Write-Host "Running makepkg pack /d '$StagedPath' /pd '$msixvcDir' /pc ..."
        & $makepkg pack /d "$StagedPath" /pd "$msixvcDir" /pc 2>&1 |
            Tee-Object (Join-Path $ReportsPath 'makepkg-pack.log')
        if ($LASTEXITCODE -ne 0) {
            throw "makepkg pack failed with exit code $LASTEXITCODE."
        }

        Write-Host "Running makepkg validate /d '$StagedPath' /pc ..."
        & $makepkg validate /d "$StagedPath" /pc 2>&1 |
            Tee-Object (Join-Path $ReportsPath 'makepkg-validate.log')

        # Check produced artifacts
        $msixvcFiles = @(Get-ChildItem -Path $msixvcDir -File | Where-Object { $_.Extension -in @('.msixvc', '.ekb') })
        foreach ($f in $msixvcFiles) {
            $packagesCreated.Add([PSObject]@{
                format = 'msixvc'
                name = $f.Name
                path = [IO.Path]::GetRelativePath($OutputPath, $f.FullName).Replace('\', '/')
                bytes = $f.Length
                sha256 = (Get-FileHash $f.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
            })
            Write-Host "Created GDK package asset: $($f.Name) ($([math]::Round($f.Length / 1MB, 2)) MB)"
        }
    }
}

# 5. Process Windows SDK MSIX packaging
if ($Format -in @('both', 'msix')) {
    Write-Host "`n--- [Windows SDK MSIX Packaging] ---"
    $makeappx = Get-MakeAppxTool

    if (-not $makeappx) {
        $msg = "MakeAppx.exe not found in Windows SDK. MSIX packaging skipped."
        if ($Format -eq 'msix') {
            throw $msg
        } else {
            Write-Warning $msg
            $warnings.Add($msg)
        }
    } else {
        Write-Host "Using MakeAppx at: $makeappx"

        # Generate AppxManifest.xml
        $appxLines = [Collections.Generic.List[string]]::new()
        $appxLines.Add('<?xml version="1.0" encoding="utf-8"?>')
        $appxLines.Add('<!-- Generated by Qinglan Microsoft Store build automation -->')
        $appxLines.Add('<Package xmlns="http://schemas.microsoft.com/appx/manifest/foundation/windows10"')
        $appxLines.Add('         xmlns:uap="http://schemas.microsoft.com/appx/manifest/uap/windows10"')
        $appxLines.Add('         xmlns:rescap="http://schemas.microsoft.com/appx/manifest/foundation/windows10/restrictedcapabilities">')
        $appxLines.Add("  <Identity Name=""$IdentityName""")
        $appxLines.Add("            Publisher=""$PublisherId""")
        $appxLines.Add("            Version=""$Version""")
        $appxLines.Add('            ProcessorArchitecture="x64" />')
        $appxLines.Add('  <Properties>')
        $appxLines.Add('    <DisplayName>Qinglan: Path of Ascension</DisplayName>')
        $appxLines.Add("    <PublisherDisplayName>$PublisherDisplayName</PublisherDisplayName>")
        $appxLines.Add('    <Logo>Assets\StoreLogo.png</Logo>')
        $appxLines.Add('  </Properties>')
        $appxLines.Add('  <Resources>')
        $appxLines.Add('    <Resource Language="en-US" />')
        $appxLines.Add('    <Resource Language="zh-Hans" />')
        $appxLines.Add('  </Resources>')
        $appxLines.Add('  <Dependencies>')
        $appxLines.Add('    <TargetDeviceFamily Name="Windows.Desktop" MinVersion="10.0.19041.0" MaxVersionTested="10.0.22621.0" />')
        $appxLines.Add('  </Dependencies>')
        $appxLines.Add('  <Capabilities>')
        $appxLines.Add('    <rescap:Capability Name="runFullTrust" />')
        $appxLines.Add('  </Capabilities>')
        $appxLines.Add('  <Applications>')
        $appxLines.Add('    <Application Id="Game"')
        $appxLines.Add("                 Executable=""$exeRelative""")
        $appxLines.Add('                 EntryPoint="Windows.FullTrustApplication">')
        $appxLines.Add('      <uap:VisualElements DisplayName="Qinglan: Path of Ascension"')
        $appxLines.Add('                          Description="An offline cultivation adventure."')
        $appxLines.Add('                          BackgroundColor="#17212A"')
        $appxLines.Add('                          Square150x150Logo="Assets\Square150x150Logo.png"')
        $appxLines.Add('                          Square44x44Logo="Assets\Square44x44Logo.png">')
        $appxLines.Add('        <uap:SplashScreen Image="Assets\SplashScreenImage.png" BackgroundColor="#17212A" />')
        $appxLines.Add('      </uap:VisualElements>')
        $appxLines.Add('    </Application>')
        $appxLines.Add('  </Applications>')
        $appxLines.Add('</Package>')
        $appxManifest = $appxLines -join "`n"

        $appxManifestFile = Join-Path $StagedPath 'AppxManifest.xml'
        [IO.File]::WriteAllText($appxManifestFile, $appxManifest, [Text.UTF8Encoding]::new($false))
        Write-Host "Created $appxManifestFile"

        $msixDir = Join-Path $OutputPath 'msix'
        New-Item -ItemType Directory -Force $msixDir | Out-Null
        $msixTarget = Join-Path $msixDir "QinglanAscension-$Version.msix"

        Write-Host "Running MakeAppx pack /d '$StagedPath' /p '$msixTarget' /nv ..."
        & $makeappx pack /d "$StagedPath" /p "$msixTarget" /nv 2>&1 |
            Tee-Object (Join-Path $ReportsPath 'makeappx-pack.log')
        if ($LASTEXITCODE -ne 0) {
            throw "MakeAppx pack failed with exit code $LASTEXITCODE."
        }

        if (Test-Path -LiteralPath $msixTarget -PathType Leaf) {
            $msixItem = Get-Item $msixTarget
            $packagesCreated.Add([PSObject]@{
                format = 'msix'
                name = $msixItem.Name
                path = [IO.Path]::GetRelativePath($OutputPath, $msixItem.FullName).Replace('\', '/')
                bytes = $msixItem.Length
                sha256 = (Get-FileHash $msixItem.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
            })
            Write-Host "Created MSIX package asset: $($msixItem.Name) ($([math]::Round($msixItem.Length / 1MB, 2)) MB)"
        }
    }
}

# 6. Copy root manifest references to output package directory
$configSrc = Join-Path $StagedPath 'MicrosoftGame.config'
if (Test-Path -LiteralPath $configSrc -PathType Leaf) {
    Copy-Item $configSrc (Join-Path $OutputPath 'MicrosoftGame.config') -Force
}
$manifestSrc = Join-Path $StagedPath 'AppxManifest.xml'
if (Test-Path -LiteralPath $manifestSrc -PathType Leaf) {
    Copy-Item $manifestSrc (Join-Path $OutputPath 'AppxManifest.xml') -Force
}

# 7. Write evidence and Partner Center upload instructions
$commit = (& git -C $repoRoot rev-parse HEAD).Trim()
$evidence = [ordered]@{
    timestamp_utc = [DateTime]::UtcNow.ToString('o')
    project_commit = $commit
    identity_name = $IdentityName
    publisher_id = $PublisherId
    publisher_display_name = $PublisherDisplayName
    store_id = $StoreId
    version = $Version
    format = $Format
    is_test_identity = $isTestIdentity
    packages = @($packagesCreated.ToArray())
    warnings = @($warnings.ToArray())
}
$evidence | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $OutputPath 'store-package-evidence.json')

$instructionLines = [Collections.Generic.List[string]]::new()
$instructionLines.Add('================================================================================')
$instructionLines.Add(' Microsoft Store (Partner Center) Package Upload Instructions')
$instructionLines.Add('================================================================================')
$instructionLines.Add("Product:                Qinglan: Path of Ascension")
$instructionLines.Add("Package Version:        $Version")
$instructionLines.Add("Package Identity Name:  $IdentityName")
$instructionLines.Add("Publisher ID:           $PublisherId")
$instructionLines.Add("Is Test Identity:       $isTestIdentity")
$instructionLines.Add('')
$instructionLines.Add('Route 1: GDK PC Game (Recommended for Games)')
$instructionLines.Add('--------------------------------------------------------------------------------')
$instructionLines.Add('1. Sign in to Microsoft Partner Center (https://partner.microsoft.com/).')
$instructionLines.Add('2. Select your Game product under "Apps & games".')
$instructionLines.Add('3. Navigate to "Game setup" and verify:')
$instructionLines.Add("   - Package/Identity Name matches '$IdentityName'")
$instructionLines.Add("   - Publisher ID matches '$PublisherId'")
$instructionLines.Add('4. Navigate to "Packages" in the submission checklist.')
$instructionLines.Add('5. Upload the generated .msixvc and .ekb files located in the "msixvc/" folder.')
$instructionLines.Add('6. Partner Center will run automated ingestion checks.')
$instructionLines.Add('')
$instructionLines.Add('Route 2: Desktop MSIX Application')
$instructionLines.Add('--------------------------------------------------------------------------------')
$instructionLines.Add('1. In Partner Center, open your MSI/EXE or MSIX product.')
$instructionLines.Add('2. Navigate to "Packages".')
$instructionLines.Add('3. Upload the .msix file from the "msix/" folder.')
$instructionLines.Add('')
$instructionLines.Add('Identity Configuration Notes:')
$instructionLines.Add('--------------------------------------------------------------------------------')
if ($isTestIdentity) {
    $instructionLines.Add('WARNING: This package was built with test default credentials.')
    $instructionLines.Add('To create a final store-ready package matching your Partner Center reservation:')
    $instructionLines.Add('Set the following GitHub repository secrets or workflow inputs:')
    $instructionLines.Add('  - STORE_IDENTITY_NAME: Your Partner Center Package/Identity Name')
    $instructionLines.Add('  - STORE_PUBLISHER_ID:  Your Partner Center Publisher ID (e.g. CN=...)')
    $instructionLines.Add('  - STORE_PRODUCT_ID:    Your Partner Center 12-char Product/Store ID')
    $instructionLines.Add('  - STORE_PACKAGE_VERSION: Build version (e.g. 0.1.0.0)')
} else {
    $instructionLines.Add("SUCCESS: Configured with official Partner Center Identity: $IdentityName")
}
$instructionLines.Add('================================================================================')
$instructions = $instructionLines -join "`n"

$instructions | Set-Content (Join-Path $OutputPath 'PARTNER-CENTER-UPLOAD-INSTRUCTIONS.txt')

Write-Host "`n==> Microsoft Store packaging complete!"
Write-Host "Total packages created: $($packagesCreated.Count)"
foreach ($pkg in $packagesCreated) {
    Write-Host " - [$($pkg.format)] $($pkg.name) ($([math]::Round($pkg.bytes / 1MB, 2)) MB)"
}

if ($env:GITHUB_STEP_SUMMARY) {
    $credNote = if ($isTestIdentity) { 'Yes (set STORE_IDENTITY_NAME and STORE_PUBLISHER_ID for retail submission)' } else { 'No (configured with custom credentials)' }
    $summary = [Collections.Generic.List[string]]::new()
    $summary.Add('### Microsoft Store Packages Built')
    $summary.Add('')
    $summary.Add("- **Target format:** $Format")
    $summary.Add("- **Identity Name:** $IdentityName")
    $summary.Add("- **Publisher ID:** $PublisherId")
    $summary.Add("- **Version:** $Version")
    $summary.Add("- **Test credentials:** $credNote")
    $summary.Add('')
    $summary.Add('| Format | File | Size (MB) | SHA-256 |')
    $summary.Add('| --- | --- | --- | --- |')
    foreach ($pkg in $packagesCreated) {
        $mb = [math]::Round($pkg.bytes / 1MB, 2)
        $shaPrefix = $pkg.sha256.Substring(0, 16)
        $summary.Add("| $($pkg.format) | $($pkg.name) | $mb | $shaPrefix... |")
    }
    foreach ($w in $warnings) {
        $summary.Add("- Warning: $w")
    }
    [IO.File]::AppendAllText($env:GITHUB_STEP_SUMMARY, (($summary -join "`n") + "`n"), [Text.UTF8Encoding]::new($false))
}
