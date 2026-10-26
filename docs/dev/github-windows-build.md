# GitHub Actions Windows & Microsoft Store Build Pipeline

## What is configured

`.github/workflows/windows-build.yml` runs on the standard **windows-2022 x64**
hosted runner. Standard public-repository runners are 100% free; larger paid
runners are not used.

The workflow supports two execution modes:

- **preflight**: Validates the runner's CPU, memory, storage volumes, Visual C++
  toolchains (MSVC v143), Windows 10/11 SDK, committed game content, PowerShell
  syntax across `tools/ci/*.ps1`, and repository contracts. Produces a diagnostic
  JSON report (`windows-preflight-<run_id>`).
- **package**: Prepares licensed Unreal Engine 5.8.3, compiles the editor module,
  and executes Win64 Shipping cook, stage, pak, and IoStore archiving. It then
  automatically invokes `tools/ci/windows-store-package.ps1` to produce both the
  loose Win64 test package and the **Microsoft Store submission package**
  (`.msixvc` + `.ekb` and `.msix`).

Artifacts generated:
1. `Qinglan-MicrosoftStore-Package-<run_id>`: Ready-to-upload store packages (`.msixvc`
   via GDK and `.msix` via Windows SDK), `MicrosoftGame.config`, hash evidence, and
   upload runbook instructions.
2. `Qinglan-Win64-Shipping-<run_id>`: Raw staged Win64 Shipping binaries and pak files
   for direct manual testing on Windows.
3. `windows-build-diagnostics-<run_id>`: Detailed build and packaging logs.

## Engine access

GitHub runners do not preinstall Unreal Engine. The repository's default
`GITHUB_TOKEN` cannot access Epic's private engine repository.

Configure engine acquisition under **Settings → Secrets and variables → Actions**:

### Installed engine archive (recommended)

Use an authorized Windows installed-engine ZIP with `Engine/` directly at the archive
root.

- Secret `UE_WINDOWS_ARCHIVE_URL`: HTTPS URL to download the engine ZIP.
- Variable `UE_WINDOWS_ARCHIVE_SHA256`: Complete SHA-256 hash of the ZIP.

### Locked engine source (experimental)

- Secret `UE_SOURCE_TOKEN`: GitHub Personal Access Token with read access to
  `EpicGames/UnrealEngine`.

## Microsoft Store identity configuration

Configure your official Microsoft Partner Center credentials before submission:

| Setting | Type | Description |
| --- | --- | --- |
| `STORE_IDENTITY_NAME` | Secret or Variable | Package/Identity Name from Partner Center Product Identity |
| `STORE_PUBLISHER_ID` | Secret | Publisher ID (e.g., `CN=XXXXXXXX-XXXX-...`) |
| `STORE_PUBLISHER_DISPLAY_NAME` | Variable | Publisher display name shown in store |
| `STORE_PRODUCT_ID` | Variable | 12-character Product/Store ID (optional in draft) |
| `GDK_ARCHIVE_URL` | Secret (optional) | Custom GDK archive URL (public GDK is downloaded automatically if omitted) |

If these credentials are not set, the workflow builds using safe test default
credentials (`QinglanCommunity.QinglanPathofAscension`), allowing you to test the
build pipeline before completing Partner Center registration.

For the comprehensive Microsoft Store submission guide, see
[docs/dev/microsoft-store-publishing.md](microsoft-store-publishing.md).
