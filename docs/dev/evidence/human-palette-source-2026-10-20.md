# Human palette source inspection (2026-10-20)

Scope: source FBX/PNG inspection and an importer recipe repair for AUD-55.
This is not a rendered acceptance or a performance measurement. No committed
Content binary was changed by this inspection.

## Source and reproduction

The archive is the existing `quat_animated_human` entry in
[`assets/asset-manifest.csv`](../../../assets/asset-manifest.csv):
[Animated Human by Quaternius](https://opengameart.org/sites/default/files/Animated%20Human%20by%20%40Quaternius_0.zip).
It was downloaded on 2026-10-20 into the ignored local raw-asset directory.

Run from the repository root:

```sh
python3 tools/inspect_human_palette.py \
  '.local/assets-raw/quaternius/extracted/Animated Human by @Quaternius'
```

The standard-library-only reader inspects the binary FBX's skin-cluster
connections, vertices weighted above 0.25 to each hand/finger bone, polygon
UV indices, and unfiltered source PNG pixels. It rejects unsupported FBX/PNG
layouts and returns nonzero if any inspected hand cluster does not sample
the source skin band. Full output is in
[`human-palette-source-2026-10-20.json`](human-palette-source-2026-10-20.json).

## Findings

- The PNG is 32 by 32 pixels with five horizontal flat-colour bands. Its
  skin colour is RGBA `(255, 209, 159, 255)` in rows 0 through 5.
- The PNG MD5 is `4716ed6f497d7be72ed9aa041e5e87ab`. This matches the source
  MD5 recorded in the committed `T_Human_Clothed` import metadata.
- All 14 nonempty hand/finger skin clusters (both sides) use UV coordinates
  `(0.4722139835, 0.8659640551)`, `(0.4965360165, 0.8659640551)`, and
  `(0.4965360165, 0.8902860880)`. With the FBX V convention, all are inside
  the skin band. The original hand UVs do not require a speculative remap.
- The committed texture registry reports `TF_Default`, `TC_Default`, and
  wrap addressing. The importer left mip generation and sampling at their
  surface-texture defaults. A five-band lookup is susceptible to palette
  mixing under mip averaging; this is a confirmed recipe risk, not proof
  that it caused every visual issue in the previous screenshots.

## Repair and remaining acceptance

`QinglanMeshImport` now explicitly applies nearest filtering, no mipmaps,
uncompressed colour, sRGB, clamp addressing, and no streaming after texture
import, then saves those settings. Applying them after import also covers
the Interchange importer, which can bypass factory options.
`Qinglan.Content.HeroSkinBinding` now checks these settings and one-to-one
UV tiling alongside the existing texture binding assertions.

The engine must compile the change, regenerate the texture, run the extended
test, and produce a close-up of both hands at near and normal play distances.
The source reader cannot establish that the imported mesh retained its UVs
or that the final material rendered correctly. AUD-55 remains open until
those captures and the target-hardware performance evidence exist.
