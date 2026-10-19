# Development Guide

## Current stage

This repository contains an Unreal Engine project at `game/` (engine association 5.8) with C++ gameplay modules, authored data, generated content, and editor automation tests. The last recorded Linux Development editor build succeeded and the automation suite ran 35/35 green; see `docs/dev/evidence/`. That evidence covers compilation and scripted subsystem behavior only. Do not mark performance, licensing, rendered play, or Store compatibility verified before the corresponding evidence exists; the open audit items are tracked in `docs/dev/completion-audit-todo.md`.

## Local documentation workflow

```sh
git clone https://github.com/shoutoutuoadi325/qinglan-wendao.git
cd qinglan-wendao
python3 tools/validate_repository.py
```

Use Python 3.10+ without third-party packages for repository validation. All code comments, documentation, developer logs, issues, branches, and commit messages use English. Chinese UI copy belongs in `localization/zh-Hans.json` or its future runtime localization outputs.

## Gameplay environment

Obtain the selected released Unreal patch under the owner's applicable free license. Record Windows build tools/GDK versions and Mac Xcode/SDK versions. Build and test each platform on its own OS. Do not commit the engine or signing credentials. Do not use paid-only plugins or plugins without native Mac support.

Place the future project under `game/`. Use ASCII file/asset names, `BP_` for Blueprints, `WBP_` for widgets, `DA_` for data assets, `NS_` for Niagara systems, `M_` for materials, and `MI_` for material instances. Prefer components and data assets over large level Blueprint graphs. Comments explain non-obvious intent or constraints.

## Git and asset workflow

Branch examples: `feature/combat-foundation`, `fix/checkpoint-rewards`, `docs/platform-budgets`. Small PRs include behavior changed, validation performed, platform implications, localization, and licensing evidence for new assets. Do not claim a passing planning check validates gameplay.

Use Git LFS for original `.uasset`, `.umap`, and large art/audio source files before first binary commit. LFS storage quotas are a separate operational constraint. Source-redistribution-restricted assets stay outside public Git; document acquisition and provide placeholders. Update the asset manifest for every imported third-party asset.

The repository is public but no general reuse license has been granted. Do not copy project content into another product without the owner's permission. External contributions should clarify rights before merge; no CLA or license has been invented here.

## Verification

Run planning validation for document/example changes. Gameplay changes require the affected runtime/functional checks and packaged platform checks in the QA plan. GPU-facing changes require Windows and Metal evidence. UI changes require both languages at increased text scale.

Before publication inspect staged files for secrets, personal paths, restricted SDK/engine source, unsupported external assets, generated caches, and debug build artifacts. Use the PR template to keep review evidence concise.
