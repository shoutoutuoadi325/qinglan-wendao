# Mac interactive hub evidence (2026-10-21)

Real keyboard input, packaged Shipping client, Metal, 1280x720 windowed on the
owner Mac (M4, macOS 27.0.1). The display was unlocked for this pass.

Package: `.local/acceptance/20261021T114346.991305Z/package/Mac/QinglanAscension-Mac-Shipping.app`
(client `506c20f5…`). Input was delivered with macOS System Events key codes
(Return = 36, W = 13) after focusing the game window.

## Sequence and captures

1. Cold start shows the 3-slot Profile screen; slot 1 reads
   `Saved expedition - load`, slots 2/3 are empty:
   `rendered/interactive-mac-profile-2026-10-21.png`.
2. Return loads slot 1. The hub renders with the palette-textured hero and the
   HUD; the hub autosave is written (`QinglanProfile_0.sav`):
   `rendered/interactive-mac-hub-load-2026-10-21.png`.
3. Holding W for 1.2 s advances the character across the courtyard:
   `rendered/interactive-mac-hub-walk-2026-10-21.png`.
4. `kill -9` at the hub boundary. The save files were byte-identical before and
   after the kill:
   `QinglanProfile_0.sav` sha256 `e0d03970d4dfe1e3fd4f74e9a3b83407b2a1a99bf78fbdc87ede5b500fdede1c`;
   `QinglanProfile_0.bak1` sha256 `dda33a55947b6129cb2ab1a52d097e0b07d9f7077ec223f108ff2605e3faa1ba`.
5. Relaunch, Return on slot 1: the hub loads again and writes a new autosave
   (`QinglanProfile_0.sav` sha256 `ab0b3d72…` at 18:22, expected to differ in
   play-time/timestamp metadata):
   `rendered/interactive-mac-hub-relaunch-2026-10-21.png`.

## Scope

This verifies the Mac keyboard path for cold start → slot load → hub render →
hub autosave → hub-boundary SIGKILL/relaunch. It is **not** a complete
CH00–CH07 playthrough: no gamepad is attached, and realm/trial/ending
boundaries, defeat/retreat/retry and the full 30-scene/24-stage route remain
open. `screencapture` worked only while the display was unlocked.

## Image hashes (SHA-256)

```text
171143ba15f963a8e335e934482a5b5e0544c9bf1ac9b9bd78a7c806f641b93d  interactive-mac-profile-2026-10-21.png
457fd754d3c950eef3af47c843cee18037291ab381a06b938786f26546db4531  interactive-mac-hub-load-2026-10-21.png
add36e66a13473d8fcd95d69b8d694f81c002cf521090090dd68ef0d88996c75  interactive-mac-hub-walk-2026-10-21.png
fd28e2d23716387a012598cfa714c23012cbb7e11a6d47466083a30339fa96ec  interactive-mac-hub-relaunch-2026-10-21.png
```

## Second keyboard session (2026-10-22)

A second packaged-client session loaded slot 1 and wrote a fresh hub autosave.
The save hash changed from `81ba55b0…` to `28125769…` at 09:17:08 local; the
boundary screenshot is
`.local/rendered/playthrough/shots/boundary-20261022T011708Z.png` (hub, "No
active objective"). The session lasted about one minute and did not reach a
chapter boundary, so it adds hub-boundary evidence only.
