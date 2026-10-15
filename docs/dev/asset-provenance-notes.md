# Asset provenance notes (AUD-56 supplement)

Supporting notes for `assets/asset-manifest.csv`, which this branch populated
from zero records to 42 rows. Nothing here overrides
[asset policy](../10-licensing-and-assets.md); this file records how each row
was evidenced, decisions about column semantics, and the outstanding unknowns.

## Scope and method

Rows were derived from what the content pipeline actually consumes:

- `QinglanMeshImport.cpp` — five Quaternius packs (raw root
  `assets-raw/quaternius/extracted`) imported to `/Game/Qinglan/{Characters,Weapons,Environments}`.
- `QinglanContentBootstrap.cpp` — 20 PolyHaven surface sets imported to
  `/Game/Qinglan/Materials` (MIC) + `/Game/Qinglan/Textures` (import target).
- `assets-raw/polyhaven/hdris` — 8 HDRIs acquired by the fetch script but not
  wired to any commandlet yet; recorded anyway so the acquisition is tracked.
- `QinglanAudioBootstrap.cpp` — 8 wav files (4 music, 4 SFX) with no
  discoverable origin; recorded as unknown (see below).

Evidence trail used per source:

| Source | Evidence |
| --- | --- |
| Quaternius Animated Human / Animated Monsters | `License.txt` inside each archive states CC0 1.0 Universal (local, authoritative) |
| Quaternius Knight / Medieval Weapons / Nature vol.3 | No license file inside the archives. License verified as CC0 on the OpenGameArt content pages (checked 2026-10-15): `lowpoly-animated-knight`, `lowpoly-medieval-weapons`, `lowpoly-nature-pack` |
| PolyHaven textures/HDRIs | `polyhaven.com/license` states all content is CC0 (checked 2026-10-15); per-asset authors extracted from the locally cached bulk listings `ph_textures.json` / `ph_hdris.json`; exact files came from `dl.polyhaven.org` per `fetch-polyhaven-direct.py` |
| Acquisition URLs/dates | `fetch-quaternius-loop.sh` (zip URLs, 2026-10-12) and `fetch-polyhaven-direct.py` (CDN patterns, 2026-10-13) in the parent directory, cross-checked against file mtimes in `assets-raw` |
| Audio | No fetch script, no log entry, no license file, no source tags in the vorbis comments of the `.ogg` variants (only `ENCODER=libsndfile` / libVorbis vendor strings) — unknown |

## Column semantics decisions

- The original 13-column header from `docs/10-licensing-and-assets.md` was kept
  unchanged; no extra columns were needed.
- `acquired_version` carries fetch date plus archive vintage where the pack's
  internal timestamps indicate one (the packs are not versioned upstream).
- `attribution` is `no` for CC0 rows (not required), followed by a courtesy
  credit string the release credits screen can lift verbatim.
- `public_path` is the committed `/Game/...` location of the derived uasset.
  Raw third-party sources stay outside the repository in `assets-raw/` per the
  policy on public Git distribution; CC0 would permit source redistribution,
  but that is a choice, not an obligation.
- `pending` is used only for the audio rows, where verification could not be
  performed for lack of any source record.

## License conclusions

- **Quaternius (5 packs):** CC0 1.0 Universal. Free use, cooked distribution,
  and source redistribution are all permitted; attribution not required.
  Two packs prove this locally via `License.txt`; the other three are proven
  by their OpenGameArt pages. TODO: mirror those page licenses into
  `assets-raw` as local text files so the evidence survives link rot.
- **PolyHaven (21 texture sets, 8 HDRIs):** CC0. Same conclusions as above.
  One texture set (`cobblestone_04`) is acquired but unused; 8 HDRIs are
  acquired but not yet imported. Neither blocks anything.
- **Audio (8 files):** unknown — see below.

## Unknown items (resolve before any distribution)

All eight audio files imported by `QinglanAudioBootstrap` have unknown
provenance, and derived `SW_*` uassets are committed under
`game/Content/Qinglan/Audio/`:

- `music/music_boss_epic_boss_battle_loop.wav`
- `music/music_combat_alt_prepare_your_swords.wav` (+ `.ogg` variant)
- `music/music_combat_fast_fight_loop.wav`
- `music/music_hub_calm_orient_peace_valley.wav` (+ `.ogg` variant)
- `sfx/sfx_sword_swing.wav`
- `sfx/sfx_hit_impact.wav`
- `sfx/sfx_technique_release.wav`
- `sfx/sfx_telegraph_warn.wav`

No fetch script, download log, purchase record, embedded metadata, or license
file exists for any of them. Per the policy's reject-unknown-provenance rule
they are a release blocker until one of these happens:

1. The original source and license are identified and recorded (then update
   the manifest rows in place), or
2. The files are replaced with tracked CC0/owned equivalents and the
   `SW_*` uassets re-imported, or
3. They are removed along with their uassets.

Whoever acquired them should also record the acquisition command in a committed
script or log so this class of gap cannot recur.

## Non-rows (for future auditors)

- No third-party fonts are imported yet (UI uses the engine default font
  chain), so no font rows exist; the OFL-family requirement in the policy is
  still pending on future font adoption.
- `fetch-polyhaven.py` lists 20 models (katana, lanterns, gates, boulders...)
  that were never downloaded — only the API cache `ph_models.json` exists —
  so no model rows are recorded. Add rows if they are ever fetched.
- Engine-provided content (`/Engine/BasicShapes/Cube`, `Cylinder`, default
  fonts) is governed by the Unreal Engine EULA, not this ledger.
