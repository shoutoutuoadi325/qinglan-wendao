# Free Tools, Licenses, and Public Assets

## 1. Mandatory constraint

Every external game engine and required development plugin must permit free use by an individual game developer for this project. No paid-only engine seat or plugin may become a required dependency. A free download, free trial, editor-only license, or free noncommercial demo is not automatically sufficient to ship a game.

## 2. Unreal decision

Unreal Engine 5.8 is selected for realistic 3D presentation, Niagara, mature animation tooling, and Windows/Mac support. Epic offers free game-development access. Its EULA distinguishes royalty products, products without direct revenue, and seat exceptions; personal/indie eligibility can depend on revenue. The completely free, non-monetized project is expected to have no attributable game revenue. Record the applicable seat exception before use. If monetization or eligibility changes, reassess first; do not promise unconditional zero fees forever. [License overview](https://www.unrealengine.com/license) and [EULA, sections 3, 4, and 6](https://www.unrealengine.com/eula/unreal).

No engine EULA has been accepted or engine installation performed as part of writing this package. Obtain the engine through the owner's Epic account under its applicable terms during M0. Keep the engine separate from the public repository. Free-to-play pricing is a product decision; it does not make Unreal itself open-source.

## 3. Tool policy

| Tool category | Preferred approach | Constraint |
| --- | --- | --- |
| Game engine | Unreal Engine game-development license | Validate free eligibility and current terms |
| Effects | Built-in Niagara and original materials | No paid-only VFX plugin |
| 3D authoring | Blender or equivalent free tool | Output and imported assets have separate rights |
| Texture authoring | Original textures and suitable free tooling | No mandatory subscription |
| Audio | Original recordings/composition and free editing tools | Samples/music require shipping rights |
| Fonts | Redistributable Latin/CJK fonts, e.g. an OFL-licensed family | Keep exact font version and license; verify embedding |
| Source control | Git; LFS only where necessary | Check repository storage/bandwidth before large uploads |
| Packaging | Official GDK / platform build tools | Licensing, signing, account charges are tracked separately |

This table defines allowed choices rather than claiming every possible version has been audited. Before adopting a specific tool/version, enter its source URL, license, free eligibility, supported platforms, and obligations in the manifest.

## 4. Public Git and asset permissions

The repository contains original planning and examples. Public visibility is separate from an open-source license. Do not choose a permissive project license on the owner's behalf without recording the choice. Until then, original content remains all rights reserved; participation follows ordinary GitHub rights and the owner's explicit permissions.

Unreal engine source, restricted SDK source, Fab/marketplace editable assets, paid packs, model textures with unknown provenance, and signing material must not be committed by default. Some licenses allow a cooked game but forbid source redistribution. Git LFS is still public distribution and does not solve this restriction. [Fab's licensing page](https://www.fab.com/eula).

For a restricted but usable free asset, keep its source outside public Git and record approved acquisition/import steps. Provide original placeholder substitutes so the gameplay project remains reviewable and runnable by a contributor. For CC0/original assets, store provenance even when attribution is not mandatory. For attribution licenses, include the required credit and license text. Reject unknown provenance.

## 5. Asset ledger

`assets/asset-manifest.csv` begins with a header only; no third-party asset has been acquired. Each row must record:

`asset_id, category, source_url, author, license_id, license_url, acquired_version, free_use_verified, game_distribution_allowed, source_redistribution_allowed, attribution, public_path, notes`.

Fields use `yes`, `no`, or `pending` where applicable. Only rows with verified public source permission may map to public asset files. Keep purchase evidence or personal receipts privately if ever needed; do not post them in public issues.

## 6. Release notices

Generate credits from the actual manifest. Include the engine trademark/copyright notices required by the accepted EULA, relevant runtime library notices, fonts, music, models, and textures. Check the precise notice wording against the accepted engine version at release; do not fabricate third-party credits before assets are selected.

Prepare an end-user license that satisfies required engine-disclaimer obligations and the owner's intended game rights. This planning package is not the final EULA. No game asset, trademark clearance, or market-specific legal approval is implied by the current repository.

## 7. Change control

Any proposal to add ads, donations linked to in-game benefits, paid DLC, subscriptions, paid tools, or a third-party SDK requires a new product decision and license review. The current design includes none. Trusted Apple distribution may have account costs; the free engine requirement remains independent of those platform costs.
