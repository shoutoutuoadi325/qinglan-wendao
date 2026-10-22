# Metal rendering and hero-palette evidence (2026-10-21)

Scope: real Metal-RHI frames from the **packaged Shipping client**, captured
while the owner's display session was locked. They establish rendering on
Metal and verify the repaired hero palette material. They are not a substitute
for the interactive playthrough, performance capture or the DX12 capture.

## Session and configuration

- Host: MacBook Air `Mac16,12`, Apple M4, 32 GB, macOS 27.0.1 (26A434).
- Engine: UE 5.8.3 `396c9f059903aed5fec78ecd3d437a40c6415368`; project
  `fix/audit-repairs` at `8fe547a` plus the repair working tree.
- Package: `.local/acceptance/20261021T114346.991305Z/package/Mac/QinglanAscension-Mac-Shipping.app`
  (client `24d31c23…`, pak `51dd0db1…`, ucas `43aad57f…`; full hashes in
  `packaging/evidence/mac-shipping-2026-10-21.txt`).
- RHI: Metal (`SF_METAL_SM5`/`SF_METAL_SM6` shader archives in the package;
  editor logs `METAL_SM6`).
- Resolution/quality: 1280x720 windowed; first-launch detected preset (Low,
  30 FPS cap, dynamic resolution on).
- Display state: locked (`IOConsoleLocked=true`,
  `CGSSessionScreenIsLocked=true`) — see
  [mac-display-locked-2026-10-21.md](mac-display-locked-2026-10-21.md).

## Packaged Shipping captures

Command shape (no `-UserDir`: a sandboxed app cannot write a path outside its
container; captures land in the package container's `Saved/Screenshots`):

```sh
QinglanAscension-Mac-Shipping -windowed -Res=1280x720w -nosplash \
  -QinglanShot=<id> -QinglanShotDelay=<seconds> [-QinglanTrial=90]
QinglanAscension-Mac-Shipping -windowed -Res=1280x720w -nosplash \
  -QinglanHandShot=<id> -QinglanHandShotDelay=6
```

1. `rendered/metal-shipping-hub-2026-10-21.png`
   `-QinglanAutoSlot=0 -QinglanShot=mac-pkg-hub -QinglanShotDelay=20`: after the
   QA slot confirmation settles, the hub courtyard with the upright, fully
   palette-textured hero and the HUD. This is the Mac hub frame for AUD-59;
   the slot confirmation is a capture aid, not an input-verification claim.

2. `rendered/metal-shipping-profile-2026-10-21.png`
   Delay 8 s from hub BeginPlay: the 3-slot cold-start Profile screen over the
   hub map (AUD-01 frontend) on Metal.

3. `rendered/metal-shipping-trial-telegraphs-2026-10-21.png`
   `-QinglanTrial=90`, delay 40 s (≈37 s into the trial): two overlapping
   lightning-strike ground telegraph discs and a spawned wave enemy in one
   frame, with the trial timer, vitals and skill line. This is the AUD-42
   Metal-backend composition on the packaged client. The hero now shows the
   palette skin (tan skin, brown hair, blue shirt/trousers) and the arena is
   textured.

4. `rendered/metal-shipping-hand-near-2026-10-21.png` (35 cm) and
   `rendered/metal-shipping-hand-normal-2026-10-21.png` (150 cm)
   A `SceneCapture2D` close-up of the hero's sword hand (`sword_base`), from a
   three-quarter front-right view. The hand shows the tan skin band and the
   blue clothing band with no palette bleed: the AUD-55 hand UV/palette check.

## Palette/material repair

The hero rendered as a grey blockout before this pass. `-game` logs showed:

```text
M_QinglanSurface: Failed to compile Material for platform SF_METAL_SM6,
  Default Material will be used in game.
  (Node TextureSampleParameter2D) Sampler type is Linear Grayscale, should be
  Color for /Engine/EngineResources/DefaultTexture.DefaultTexture
MIC_HumanCharacter missing usage flag SkeletalMesh! Default Material will be used in game.
```

Repairs in `QinglanContentBootstrap`:
- `M_QinglanSurface` Roughness/AO samplers now default to a linear grayscale
  engine texture (`BaseFlattenGrayscaleMap`, fallback `Gray`) instead of the
  color `DefaultTexture`, so the material compiles on Metal.
- The master and `MIC_HumanCharacter` set `MATUSAGE_SkeletalMesh` (plus
  StaticMesh/InstancedStaticMeshes on the parent) so skeletal shader maps are
  cooked and the pawn does not fall back to the default material.

After regeneration and packaging, the packaged Metal frames above show the
palette skin and textured world.

Note: the QA trial hook starts the combat trial without beginning the CH03_S02
story scene, so the HUD objective line shows an earlier scene's objective
(fully localized). That is a capture-hook nuance, not a packaged localization
failure.

## Developer `-game` comparison frames

- `rendered/metal-editor-game-profile-2026-10-21.png` — cold-start screen.
- `rendered/metal-editor-game-trial-telegraphs-2026-10-21.png` — the earlier
  grey-material trial composition (pre-fix), retained for comparison.

The QA hooks write `<ProjectSavedDir>/Screenshots/` (sandbox-safe); the old
`/tmp` path was denied.

## Still open

- Real-input slot selection and interactive playthrough, defeat/retreat/retry
  and boundary kills (AUD-01/02/58); the hub frame above used the QA auto-slot
  aid because keystrokes do not reach the app while the display is locked.
- Performance capture (AUD-55) on real hub/combat/trial routes.
- DX12/Windows capture (AUD-42's other backend, AUD-59 Win64).

## Image hashes (SHA-256)

```text
846f93f3bb0c92709b9f7a4a2627377f226eb9db69c6a57a9ff61483f6a4c115  metal-shipping-hub-2026-10-21.png
2364cfb3611a642871a738a2ac4167a336679d0977f7d23274dc8bb727338e80  metal-shipping-profile-2026-10-21.png
fef8d0d7460a32566fda786d835267ba1f868765136e2464e62328b36f230531  metal-shipping-trial-telegraphs-2026-10-21.png
5b6f1c2154777db756fb5dccd18782f07c79bec2e611f9282b219f7e7b88a558  metal-shipping-hand-near-2026-10-21.png
4a4bfa406af46924168a58386e99430cf5a2cfbd8df413bb4ba767c14dd2c97a  metal-shipping-hand-normal-2026-10-21.png
864848d629f690097c77cb258b975da2a74c5ebb63889ef86229e61bc25ee316  metal-editor-game-profile-2026-10-21.png
3feb26079fb3b76154d3078a901ed6a79be8504f49e5e275651f212ffc9128df  metal-editor-game-trial-telegraphs-2026-10-21.png
```
