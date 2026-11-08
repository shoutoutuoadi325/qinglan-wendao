# Visual upgrade evidence (2026-11-08)

This change replaces the player's short-sleeved blockout silhouette with a cultivator costume and introduces original sanctuary architecture, rock forms, bamboo branches/leaves and stone lanterns. It is a visual upgrade with a stylized character face; it does not establish AAA production quality.

## Assets and rendering

- The player costume has 55,508 triangles, eight independent material slots, a small repeating weave normal and the original Human skeleton. Skin, cloth, hair, leather, jade and bronze have separate material responses. The head adds lips, eyelids, nostrils and swept hair; the hands have separate skinned digits. The edited palette retains original center-column samples and adds original accents in unused columns.
- The six original environment meshes include swept eaves, tile courses, column/bracket joinery, rails, eroded rock geometry and bamboo leaf blades. The map generator instances repeated decorations, authors ordinary reduction LODs and culls distant groups. Two small side pavilions frame the hall approach. Decorations have no collision or navigation influence; the authored walking floors and hall walls remain gameplay geometry.
- Surface textures use centimetre-scale world projection, with imported Poly Haven DirectX normals, roughness and ambient occlusion. Character palette UVs retain their separate mapping. Dominant-axis projection can show seams on curved rocks; actual captures determine whether a more expensive triplanar material is needed.
- The four maps share fixed exposure, soft directional lighting, sky capture, restrained bloom and realm-specific bounded fog. Low quality retains solid material fallbacks and reduces the number of authored instances.
- The original nature FBX sources were independently imported into Blender to verify their material partitions. Tree1 and Tree2 use leaf slot 0/trunk slot 1; Tree4 reverses that order. Source material names select the correct foliage and bark, with a verified per-mesh fallback. The [partition evidence](evidence/visual-upgrade/tree-material-slots.json) records material names, face counts and bounds.
- Surface sets without a separate AO file extract the red channel of the packed ARM source into a real grayscale AO texture. This preserves the LinearGrayscale sampler contract instead of binding a Masks texture to it.

The [asset ledger](../../assets/asset-manifest.csv) records original materials and the CC0 Quaternius derivative. Official normal-map acquisition and API MD5 verification are recorded privately under .local/assets-raw/provenance/surface-normals-2026-11-08.json.

## Reproduction

Blender 4.5.4 LTS produces the tracked FBX sources with:

    blender --background --python tools/art/generate_hero.py
    blender --background --python tools/art/build_environment.py -- --render

The hero generator requires the original licensed Human source under .local/assets-raw/quaternius/extracted/. Environment generation is original; its optional studio preview uses the ledger-listed raw Poly Haven textures. FBX imports bind those material slots to the generated Unreal PBR assets.

After obtaining the locked Unreal engine, run:

    source tools/build/local-env.sh
    bash tools/build/build-game.sh
    bash tools/build/regenerate-content.sh

The regeneration wrapper requires the new character, all costume materials, weave normal and six environment meshes to exist as game assets.

## Validation

The independent [FBX rig comparison](evidence/visual-upgrade/hero-rig-verification.json) checks both imported source files: all 48 exported bone names, parent links, world matrices and head/tail positions match, with maximum difference 0. [Model inventory](evidence/visual-upgrade/hero-model.json) records geometry and material parameters. The [hero studio preview](evidence/visual-upgrade/hero-preview.png), [face close-up](evidence/visual-upgrade/hero-face-preview.png) and [sanctuary studio preview](../../assets/original/environment/sanctuary-lookdev.png) are Blender asset look development, not gameplay screenshots.

The original Human FBX has an invalid bind pose, and Unreal Interchange uses its evaluated animation T0 instead. Its source Blend is saved at Idle frame 77. The costume exports those same evaluated Model defaults while retaining the unchanged rest BindPose/Cluster matrices and no baked animations. Unreal's T0 import skins the costume geometry to those defaults. A clean native import passed all 49 bone names, parent indices and local reference transforms; an existing costume package must be recreated because legacy reimport retains its previous mesh reference pose. The regeneration wrapper now recreates this generated costume. [FBX default-pose evidence](evidence/visual-upgrade/hero-fbx-reference-pose-verification.json) separately records the small Blender-version evaluation differences.

Repository validation and the nine build-wrapper tests pass. Native editor compilation and all six regeneration commandlets passed. After the final scene revision, the full Qinglan automation suite executed 165 named tests: 130 successes, 35 successes with warnings, zero failures and zero skipped tests. This includes the saved binding/CPU skinning contract, real standing sword hits, hall navigation and map collision checks. Five frames from each original Human animation take were sampled against the actual built costume geometry. The [automation summary](evidence/visual-upgrade/automation-summary.json) records the final report hash and counts. No Windows DX12 evidence is established on this Mac host.

Repeatable real-game art captures use the existing screenshot mode with the optional art view, an isolated QA user directory and a confirmed QA profile:

    -QinglanAutoSlot=0 -QinglanShot=visual-hall -QinglanShotDelay=30 -QinglanArtView=hall
    -QinglanAutoSlot=0 -QinglanShot=visual-hero -QinglanShotDelay=30 -QinglanArtView=hero
    -QinglanAutoSlot=0 -QinglanShot=visual-bamboo -QinglanShotDelay=30 -QinglanArtView=bamboo

The Bamboo view must run in the Bamboo map. Art views exclude the Slate UI from their screenshots. Capture both the normal third-person combat camera and art cameras; art cameras alone cannot verify animated costume intersections, encounter readability or gameplay frame rate. Qinglan.Content.SanctuaryArtContract, Qinglan.Content.HubHallNavigation, Qinglan.Content.MapCollisionContract and the Human source-animation tests cover generated assets and preserved gameplay contracts.

The first native pass revealed two integration bugs that source previews could not catch: static hall decorations reverted to their default collision profile when loaded, and costume bounds changed the socket scale. Authored decorations now save the explicit NoCollision profile, and the player normalizes against the canonical Human bounds. Legacy import also left rest-pose bone attributes in MeshDescription despite updating the rendering mesh to T0; the importer now commits the evaluated bone attributes with the geometry. The failed checks passed after these fixes.

Native scene review also replaced sine-based rock ripples with a three-dimensional erosion field. Distant mountains use a continuous height field with multiple asymmetric summits and fractal erosion rather than enlarged foreground rocks or a single smooth cone. Their instance rings are 260 m from the hub/arena center and 450 m from the Bamboo/Ashen center, with a small deterministic radius variation. The saved-map contract checks every distant instance against the actual walking-floor volumes. Bamboo room borders now use lower rocks with varied proportions and room-facing foliage so the rocks do not hide the groves. Distant masses have their own restrained blue-grey material to reduce repetitive texture contrast.

## Rendered evidence and local delivery

All images below were rendered at 1600 × 900 through Metal after the final resource regeneration. The gameplay view includes the normal UI; the art-review cameras exclude UI and preserve the regular player camera during interactive play.

| View | Executable | Evidence |
| --- | --- | --- |
| Normal hub gameplay | Mac Shipping | [Screenshot](evidence/visual-upgrade/metal-shipping-gameplay.png) |
| Cultivator close-up | Mac Shipping | [Screenshot](evidence/visual-upgrade/metal-shipping-hero.png) |
| Hall architecture | Editor in game mode | [Screenshot](evidence/visual-upgrade/metal-editor-hall.png) |
| Bamboo room borders | Editor in game mode | [Screenshot](evidence/visual-upgrade/metal-editor-bamboo.png) |

The fresh Mac Shipping BuildCookRun succeeded. Final gameplay and hero capture runs both produced images and exited with code 0. Each run used a new QA directory inside the macOS app sandbox container so profile selection and screenshot writes could complete without touching existing player saves. Shipping suppresses normal diagnostic logging; its runtime evidence is the actual image and successful exit.

The complete staged app is copied to `.local/releases/visual-upgrade-2026-11-08/QinglanAscension-Mac-Shipping.app`. The adjacent `Qinglan-VisualUpgrade-Mac-arm64.zip` contains the complete app and needs no Unreal installation to play. All 30 file hashes in the released app match the staged app, archive CRC validation passes, and deep/strict codesign verification passes. The [package summary](evidence/visual-upgrade/package-summary.json) records the archive SHA256 and every app file hash. This is a local Apple-silicon delivery, not a published release.

An isolated `UnrealEditor -game` Metal run captured 900 CSV frames after all cook, build and other game processes ended. With 1600 × 900 output, Epic settings, dynamic resolution enabled and the existing 30 FPS cap, frames 300–899 in the stationary hub view averaged 33.35 ms; p95 was 34.93 ms and p99 was 35.34 ms. Mean GPU time was 17.79 ms. The [performance summary](evidence/visual-upgrade/performance-summary.json) records settings, sampling bounds and the raw CSV hash. This establishes a roughly 30 FPS capped editor-game hub sample on this M4 Pro, not a Shipping benchmark, fixed internal render resolution, or worst-case combat result.

The reviewed images retain a stylized face, thin bamboo foliage, faceted distant mountains and older blockout props/flat room floors. These limitations are visible in the evidence. The change supplies integrated assets and rendering improvements; commercial AAA quality still requires a cohesive art direction, production character sculpt/textures, terrain composition and further vegetation work.
