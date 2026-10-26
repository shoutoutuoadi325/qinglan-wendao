# Qinglan: Path of Ascension

A completely free, offline, single-player **3D xianxia action game** for Windows and Apple Silicon macOS. Explore mountain sanctuaries, master the Five Elements, and survive Heavenly Tribulations with grounded materials and cinematic, scalable spell effects.

## Download & Play (Windows & macOS)

> **Release v0.1.1.** Binaries are available for Windows 64-bit and Apple
> Silicon macOS. On macOS, the build is ad-hoc signed and Gatekeeper will
> show an initial prompt; follow the install steps below. On Windows, a
> standalone direct-play archive is available, and the Microsoft Store package
> (v0.1.1.0) is currently in the Store certification review pipeline.
> Only download from this repository's [Releases](https://github.com/shoutoutuoadi325/qinglan-wendao/releases).

| Platform | Download | Requirements |
| --- | --- | --- |
| **Windows 64-bit** (Direct) | [Releases](https://github.com/shoutoutuoadi325/qinglan-wendao/releases) (`Qinglan-0.1.1-Windows-x64.zip`) | Windows 10/11 64-bit, DirectX 12 GPU, VC++ 2015-2022 x64, ~2 GB free disk |
| **Windows** (Microsoft Store) | [Microsoft Store](https://apps.microsoft.com/detail/9NGLSM7P5CMB) | Store ID: `9NGLSM7P5CMB` (Official store package v0.1.1.0, In the Microsoft Store) |
| **macOS (Apple Silicon)** | [Releases](https://github.com/shoutoutuoadi325/qinglan-wendao/releases) (`Qinglan-0.1.1-macOS-arm64.dmg`) | macOS 14.0+, Apple Silicon (M1 or newer), ~2 GB free disk |

### Windows Installation & Play

1. Download `Qinglan-0.1.1-Windows-x64.zip` and `SHA256SUMS` from the release.
2. Verify checksum: `shasum -a 256 -c SHA256SUMS` (or use CertUtil in PowerShell: `Get-FileHash Qinglan-0.1.1-Windows-x64.zip`).
3. Extract the ZIP archive to a folder of your choice (e.g. `C:\Games\Qinglan`).
4. Run `Launch-Qinglan.bat` or `QinglanAscension\Binaries\Win64\QinglanAscension-Win64-Shipping.exe`.

**Windows Save data:** `%LOCALAPPDATA%\QinglanAscension\Saved\SaveGames\`.

### macOS Installation & Play

1. Download `Qinglan-0.1.1-macOS-arm64.dmg` and `SHA256SUMS` from the
   release into the same folder.
2. Verify the download: `shasum -a 256 -c SHA256SUMS`.
3. Open the DMG and drag **Qinglan - Path of Ascension** into **Applications**.
4. Try to open the app once, then open **System Settings > Privacy & Security**,
   click **Open Anyway** for Qinglan, authenticate, and open the app again.
5. If macOS reports the app is damaged, remove the quarantine flag for this app
   only:
   `xattr -dr com.apple.quarantine "/Applications/Qinglan - Path of Ascension.app"`.

Never run `sudo spctl --master-disable` and never disable Gatekeeper globally.
The macOS build is ad-hoc signed, not Developer ID signed or notarized; see the
[distribution guide](docs/09-distribution.md) for the release policy.

**macOS Save data:** `~/Library/Containers/io.github.shoutoutuoadi325.qinglan/Data/`.

**Current limits:** early release; the full campaign is not yet interactively
verified end to end, gamepad support is unverified, there is no auto-update,
and save compatibility across versions is not guaranteed. See the [v0.1.1 release notes](docs/releases/v0.1.1.md).

**Windows CI & Store Packaging (2026-10-26):** automated GitHub Actions CI pipeline produces both standalone Win64 Shipping and Microsoft Store MSIXVC packages. Windows regression suite is green at **100/100 tests**, GDK SubmissionValidator confirms 0 failures and 0 warnings, and package v0.1.1.0 has been certified and published to the Microsoft Store (`Status: In the Microsoft Store`).

**Project status (2026-10-20):** in active development on Linux (UE 5.8 source build). The `game/` Unreal project contains C++ implementations for combat, abilities, AI, progression, persistence, narrative, trials, UI, and a headless content pipeline (maps, materials, characters, montages, string tables, Niagara cue systems). Current recorded results: editor build green; automation **70/70**; functional new-profile journeys **30/30 main scenes** and **24/24 side stages**; clean content regeneration `rc_all=0`; Linux Shipping package `rc=0` with a verified IoStore inventory; rendered packaged boots in English, Simplified Chinese, and Low + low-flash; packaged keyboard/mouse movement, cold-start slot selection, and a real `kill -9`/relaunch at the hub boundary. **These results establish compilation, scripted subsystem behavior, packaging, and targeted rendered evidence; they do not establish a complete interactive playthrough.** Windows DX12 and Apple Silicon Metal builds/devices, a full keyboard/gamepad playthrough, and target-hardware performance captures remain open. See [docs/dev/build-and-run.md](docs/dev/build-and-run.md), [docs/dev/test-report.md](docs/dev/test-report.md), and the [owner runbook](docs/dev/remaining-acceptance-runbook.md).

**Completion audit (2026-10-14):** development is not complete. See the [reviewed repair TODO and acceptance gates](docs/dev/completion-audit-todo.md) for confirmed integration defects, unfinished player flows, and required verification; the earlier build/test report does not establish campaign or release readiness.

**Local Mac continuation (2026-10-22):** the native Apple Silicon baseline is
green after repairing the corrupted 2026-10-22 character-source batch: editor
build, content regeneration, **90/90** automation, **30/30** main and
**24/24** side journeys, and an arm64 Shipping package. The package boots
with Metal; a keyboard cold start, profile slot load, hub movement, and a real
`kill -9` / relaunch with save recovery are recorded. A full interactive
CH00-CH07 playthrough, gamepad, Windows DX12/Win64, and target-hardware
performance remain open. See [local Mac setup](docs/dev/local-mac-setup.md),
the [2026-10-22 package evidence](packaging/evidence/mac-shipping-2026-10-22.txt),
and the [owner runbook](docs/dev/remaining-acceptance-runbook.md).

## Confirmed requirements

- Windows and Apple Silicon macOS at first release.
- Windows GPU rendering through DirectX; macOS GPU rendering through Metal.
- Realistic 3D environments and spell effects, with accessible performance presets.
- Development documents, identifiers, logs, comments, and commits in English; player-facing UI in English and Simplified Chinese.
- Completely free: no advertisements, in-app purchases, paid currency, subscriptions, or paid gameplay unlocks.
- Public GitHub repository. Only original or explicitly redistributable material may be committed.
- External engines and development plugins must offer a free license usable by an individual game developer.

## Design at a glance

| Decision | First-release baseline |
| --- | --- |
| Engine | Unreal Engine 5.8 release family; exact patch locked during M0 |
| Perspective | Third-person action camera, optional target lock |
| Setting | An original Chinese-inspired mountain cultivation world |
| Core loop | Prepare at the sanctuary → explore a realm → fight and collect → return → cultivate → attempt a tribulation |
| Campaign | Eight chapters: Qi Refining → Foundation Establishment → Golden Core; three endings |
| Content | One hub, two compact realms, one tribulation arena, one playable character |
| Combat | Sword combo, dodge, four equipped techniques, two talisman slots |
| Story | Eight side quests, 24 environmental records, complete English/Simplified Chinese text |
| Replay | Seeded encounter selection and temporary blessings in authored 3D rooms |
| Distribution | Windows: Microsoft Store; Mac: direct GitHub Release (current alpha unsigned; Developer ID signing and notarization planned) |

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

Read the [narrative index](docs/story/README.md) for the complete chapter scripts, side stories, three endings, bilingual reading editions, and implementation/QA contracts. The [story bible](docs/story-bible.md) defines characters, chronology, world rules, and the central mystery. The story registry and scene playback code are imported from these sources; functional new-profile journeys now pass all 30 main scenes and 24 side stages in automation. A complete interactive/rendered playthrough on a stable display machine is still required by the audit gates.

## Repository usage

Python 3.10 or newer is sufficient to validate the planning package; no paid tool or engine installation is required for documentation work.

```sh
python3 tools/validate_repository.py
```

[Example data](data/examples/README.md) illustrates the intended runtime contracts. It is not a complete production database. The validator checks local Markdown links, development-language policy, bilingual key parity, examples, cross-references, and forbidden Xbox identity fields in the Windows packaging template.

Gameplay source has completed the repair waves recorded in the audit TODO. Remaining acceptance work is environment-bound (full interactive playthrough, controller, DX12/Metal, hand art, target-hardware performance, Win64/macOS builds); see the [owner runbook](docs/dev/remaining-acceptance-runbook.md). See [the development guide](CONTRIBUTING.md) for setup, branching, asset handling, and review requirements.

## Rights and release status

Public visibility does not grant a general reuse license. Original project material remains all rights reserved until the owner selects a redistribution license; see [NOTICE](NOTICE.md). Unreal Engine, licensed asset source files, third-party SDKs, signing keys, and store account details must not be uploaded here. The engine is separately obtained under Epic's license.

The macOS alpha binaries on GitHub Releases are unsigned, ad-hoc-signed builds distributed under the alpha `EULA.txt` included in the download. They are not Developer ID signed or notarized, and they are not Mac App Store deliverables.

The working title has not been reserved or cleared in Microsoft Store. The privacy statement is a release draft, not a claim that a game has already shipped.
