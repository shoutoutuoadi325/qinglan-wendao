# Windows Packaging & Microsoft Store Configuration

This directory contains the manifest template, shell visual assets, and specifications for packaging Qinglan for the **Microsoft Store** (via the GDK MSIXVC and standard MSIX pathways).

## Visual Assets (`assets/`)

Microsoft Store and `MicrosoftGame.config` require shell visual assets:
- `StoreLogo.png`: 100x100 PNG (Store listing / icon)
- `Square150x150Logo.png`: 150x150 PNG (Windows Start menu medium tile)
- `Square44x44Logo.png`: 44x44 PNG (Taskbar and app list icon)
- `SplashScreenImage.png`: 1920x1080 PNG (Launch splash screen)

These assets are staged alongside the loose game contents by `tools/ci/windows-store-package.ps1` during the packaging process.

## Manifest Template (`MicrosoftGame.config.template.xml`)

The XML file is a validated planning template conforming to Microsoft GDK `configVersion="1"`:
- Bilingual UI locale declarations (`en-US` and `zh-Hans`).
- Explicit PC device family target: `<Executable ... TargetDeviceFamily="PC" />`.
- Shell visual assets mapped to `Assets\*.png`.
- Offline game architecture: Xbox Live and multiplayer services (`MSAAppId`, `TitleId`, `ExtendedAttributeList`) are deliberately omitted to avoid unwanted Xbox network sign-in prompts.
- Real identities (`Identity Name`, `Publisher`, `StoreId`) are injected at build time from GitHub Actions secrets / workflow inputs.

## Automated Packaging via GitHub Actions

See [docs/dev/microsoft-store-publishing.md](../../docs/dev/microsoft-store-publishing.md) for the end-to-end publishing runbook using GitHub's free Windows runner.
