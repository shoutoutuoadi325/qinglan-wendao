# Qinglan: Path of Ascension

A completely free, offline, single-player **3D xianxia action game** for Windows and Apple Silicon macOS. Explore mountain sanctuaries, master the Five Elements, and survive Heavenly Tribulations with grounded materials and cinematic, scalable spell effects.

**Project status:** design and development planning. This repository does not yet contain a playable game or an Unreal project. Content counts, frame rates, and schedules below are development targets, not shipped capabilities.

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
| Campaign | Qi Refining → Foundation Establishment → Golden Core |
| Content | One hub, two compact realms, one tribulation arena, one playable character |
| Combat | Sword combo, dodge, four equipped techniques, two talisman slots |
| Replay | Seeded encounter selection and temporary blessings in authored 3D rooms |
| Distribution | Windows: Microsoft Store; Mac: signed and notarized direct download |

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

## Repository usage

Python 3.10 or newer is sufficient to validate the planning package; no paid tool or engine installation is required for documentation work.

```sh
python3 tools/validate_repository.py
```

[Example data](data/examples/README.md) illustrates the intended runtime contracts. It is not a complete production database. The validator checks local Markdown links, development-language policy, bilingual key parity, examples, cross-references, and forbidden Xbox identity fields in the Windows packaging template.

Actual gameplay development starts with the cross-platform feasibility tasks in M0. See [the development guide](CONTRIBUTING.md) for setup, branching, asset handling, and review requirements.

## Rights and release status

Public visibility does not grant a general reuse license. Original project material remains all rights reserved until the owner selects a redistribution license; see [NOTICE](NOTICE.md). Unreal Engine, licensed asset source files, third-party SDKs, signing keys, and store account details must not be uploaded here. The engine is separately obtained under Epic's license.

The working title has not been reserved or cleared in Microsoft Store. The privacy statement is a release draft, not a claim that a game has already shipped.
