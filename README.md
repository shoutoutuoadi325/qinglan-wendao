# Qinglan: Path of Ascension

A completely free, offline, single-player **3D xianxia action game** for Windows and Apple Silicon macOS. Explore mountain sanctuaries, master the Five Elements, and survive Heavenly Tribulations with grounded materials and cinematic, scalable spell effects.

## Download & Play (Windows & macOS)

**[Release v0.1.2](https://github.com/shoutoutuoadi325/qinglan-wendao/releases/tag/v0.1.2)** is available for Windows 64-bit and Apple Silicon macOS. The Windows update is also available in the [Microsoft Store](https://apps.microsoft.com/detail/9NGLSM7P5CMB) as package **0.1.2.0**, published on October 30, 2026.

| Platform | Download | Requirements |
| --- | --- | --- |
| **Windows 64-bit** (Direct) | [Qinglan-0.1.2-Windows-x64.zip](https://github.com/shoutoutuoadi325/qinglan-wendao/releases/download/v0.1.2/Qinglan-0.1.2-Windows-x64.zip) | Windows 10/11 64-bit, DirectX 12 GPU, Microsoft Visual C++ x64 runtime, ~2 GB free disk |
| **Windows** (Microsoft Store) | [Microsoft Store](https://apps.microsoft.com/detail/9NGLSM7P5CMB) | Windows 10/11 64-bit, DirectX 12 GPU; installation and updates through the Store |
| **macOS (Apple Silicon)** | [Qinglan-0.1.2-macOS-arm64.dmg](https://github.com/shoutoutuoadi325/qinglan-wendao/releases/download/v0.1.2/Qinglan-0.1.2-macOS-arm64.dmg) | macOS 14.0+, Apple Silicon (M1 or newer), ~2 GB free disk |

Download [SHA256SUMS](https://github.com/shoutoutuoadi325/qinglan-wendao/releases/download/v0.1.2/SHA256SUMS) alongside your chosen package. It contains separate SHA-256 entries for Windows and macOS.

### What's new in v0.1.2

- More consistent projectile interactions and dodge timing.
- Improved enemy behavior, character progression, and transitions between areas.
- More reliable saving and recovery of adventure progress.
- Improved keyboard, mouse, and controller input, including menu navigation.
- Fixes for interruptions to the main story and side quests.
- Clearer interface text, danger zones, and safe-area indicators.

See the [full release notes in English and Simplified Chinese](https://github.com/shoutoutuoadi325/qinglan-wendao/releases/tag/v0.1.2).

### Windows Installation & Play

1. Download `Qinglan-0.1.2-Windows-x64.zip` and `SHA256SUMS` from the release.
2. Open PowerShell in the folder containing the downloaded files and run `Get-FileHash .\Qinglan-0.1.2-Windows-x64.zip -Algorithm SHA256`. Compare the result with the Windows ZIP entry in `SHA256SUMS`.
3. Install the [Microsoft Visual C++ x64 Redistributable](https://aka.ms/vs/17/release/vc_redist.x64.exe) if it is not already installed.
4. Extract the entire ZIP to a local folder, such as `C:\Games\Qinglan`. Keep the `Engine` and `QinglanAscension` folders together.
5. Run `Launch-Qinglan.bat` or `QinglanAscension\Binaries\Win64\QinglanAscension-Win64-Shipping.exe`.

**Windows Save data:** `%LOCALAPPDATA%\QinglanAscension\Saved\SaveGames\`.

### macOS Installation & Play

1. Download `Qinglan-0.1.2-macOS-arm64.dmg` and `SHA256SUMS` from the
   release into the same folder.
2. Open Terminal in the folder containing the downloaded files and run `shasum -a 256 Qinglan-0.1.2-macOS-arm64.dmg`. Compare the result with the macOS DMG entry in `SHA256SUMS`.
3. Open the DMG and drag **Qinglan - Path of Ascension** into **Applications**.
4. Try to open the app once, then open **System Settings > Privacy & Security**,
   click **Open Anyway** for Qinglan, authenticate, and open the app again.
5. If macOS reports the app is damaged, first confirm its checksum matches, then remove the quarantine flag for this app only:
   `xattr -dr com.apple.quarantine "/Applications/Qinglan - Path of Ascension.app"`.

The macOS download uses ad hoc signing and is not Apple-notarized, so the first launch requires the approval above. Apply it only to this app; keep system-wide Gatekeeper protection enabled.

**macOS Save data:** `~/Library/Containers/io.github.shoutoutuoadi325.qinglan/Data/`.

### Updating and keeping your saves

Close the game and back up the save folder above before updating. For a direct Windows download, extract the new version into a new folder and keep your existing save folder. On macOS, replace the app in Applications while keeping its sandbox container. Direct downloads are updated manually; Microsoft Store installations receive updates through the Store.

## Game features

- Offline, single-player cultivation adventure with local save profiles.
- Sword combat, elemental techniques, talismans, and breakthrough trials.
- English and Simplified Chinese, with keyboard, mouse, and controller input.
- Adjustable graphics settings and spell effects.
- Completely free: no advertisements, in-app purchases, subscriptions, or paid gameplay unlocks.

## Design at a glance

| Aspect | Game design |
| --- | --- |
| Perspective | Third-person action camera, optional target lock |
| Setting | An original Chinese-inspired mountain cultivation world |
| Core loop | Prepare at the sanctuary → explore a realm → fight and collect → return → cultivate → attempt a tribulation |
| Campaign | Eight chapters: Qi Refining → Foundation Establishment → Golden Core; three endings |
| Content | One hub, two compact realms, one tribulation arena, one playable character |
| Combat | Sword combo, dodge, four equipped techniques, two talisman slots |
| Story | Eight side quests, 24 environmental records, complete English/Simplified Chinese text |
| Replay | Seeded encounter selection and temporary blessings in authored 3D rooms |
| Distribution | Windows: Microsoft Store and direct GitHub download; macOS: direct GitHub download |

## Read the development package

1. [Product decisions and constraints](docs/00-product-brief.md)
2. [Game design document](docs/01-game-design.md)
3. [Combat, enemies, and camera](docs/02-combat-and-camera.md)
4. [Progression and economy](docs/03-progression-and-economy.md)
5. [Art, animation, audio, and VFX](docs/04-art-and-vfx.md)
6. [Technical architecture and save design](docs/05-technical-architecture.md)
7. [Windows, Mac, and GPU compatibility](docs/06-platform-and-performance.md)
8. [Production roadmap and development backlog](docs/07-production-plan.md)
9. [Testing and release acceptance](docs/08-quality-and-testing.md)
10. [Microsoft Store and macOS release guide](docs/09-distribution.md)
11. [Asset and tool licensing policy](docs/10-licensing-and-assets.md)
12. [Development language and bilingual UI](docs/11-english-style-and-ui.md)
13. [Decision log](docs/decisions.md) and [verified external references](docs/references.md)

## Complete narrative package

Read the [narrative index](docs/story/README.md) for chapter scripts, side stories, three endings, and bilingual reading editions. The [story bible](docs/story-bible.md) defines characters, chronology, world rules, and the central mystery. These documents contain story spoilers.

## Repository usage

Use Python 3.10 or newer to validate the documentation, localization, example data, and narrative contracts:

```sh
python3 tools/validate_repository.py
```

[Example data](data/examples/README.md) illustrates the intended runtime contracts. It is not a complete production database. The validator checks local Markdown links, development-language policy, bilingual key parity, examples, cross-references, and forbidden Xbox identity fields in the Windows packaging template.

The Unreal Engine 5.8.3 project is in `game/`. See [the development guide](CONTRIBUTING.md) for setup, branching, asset handling, and review requirements; [local Mac setup](docs/dev/local-mac-setup.md) and [Windows packaging](docs/dev/microsoft-store-publishing.md) describe the platform workflows. Detailed change evidence is kept in the [gameplay audit](docs/dev/gameplay-bug-audit-2026-10-29.md) and [acceptance runbook](docs/dev/remaining-acceptance-runbook.md).

## Rights, privacy, and support

Public visibility does not grant a general reuse license. Original project material remains all rights reserved until the owner selects a redistribution license; see [NOTICE](NOTICE.md). Unreal Engine, licensed asset source files, third-party SDKs, signing keys, and store account details must not be uploaded here. The engine is separately obtained under Epic's license.

The macOS download includes its `EULA.txt`, credits, and third-party notices. It is distributed through GitHub Releases.

Read the [Privacy Policy](PRIVACY.md), effective October 25, 2026, or its [Simplified Chinese translation](localization/privacy/zh-Hans.md). For help, use [GitHub Issues](https://github.com/shoutoutuoadi325/qinglan-wendao/issues) or email [zhiqizhangg@gmail.com](mailto:zhiqizhangg@gmail.com).
