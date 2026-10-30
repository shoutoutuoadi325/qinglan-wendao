# VFX warning render verification — 2026-10-30

Groups 64–65 in the [gameplay audit](../gameplay-bug-audit-2026-10-29.md)
were found by actual Metal rendering after native automation passed. The
final source and resource state passes the six-step native acceptance run
`.local/acceptance/20261030T023816.180170Z/manifest.json`.

## Ground warning comparison

The before frame uses the preceding Shipping archive
`20261030T013435.815037Z`. Its low-flash warning is a white filled disc;
floor texture and skill labels lose contrast, and the decal paints the
hero's lower body white. The final three after frames use the exact client
and cooked resources in the `20261030T023816.180170Z` archive.

Client SHA-256:
`6f3b9eefae959ed464785c92d86f4be8fcd9334e98d426c148b33603c2746fb4`.
The run's `package-artifacts.json` also identifies all five cooked containers.

| Frame in rendered/ | Configuration | Observed result |
| --- | --- | --- |
| gameplay-audit-2026-10-30-trial-low-before.png | Low, low-flash enabled, Chinese | Filled white warning; washed-out hero and HUD |
| gameplay-audit-2026-10-30-trial-low-after.png | Low, low-flash enabled, Chinese | Visible complete outline, terrain texture, blue hero clothing and readable skill labels |
| gameplay-audit-2026-10-30-trial-high-after.png | High, low-flash enabled, Chinese | Same readable warning outcome |
| gameplay-audit-2026-10-30-trial-high-standard-after.png | High, low-flash disabled, Chinese | Same readable warning outcome |

Each archived client runs at 1280×720 using an isolated sandbox-contained
UserDir, the existing `-QinglanTrial=90` capture flag,
`-QinglanShotDelay=9.8`, and a unique shot name. Each exits with code 0
after its screenshot. Configurations and console logs are retained under
`.local/gameplay-qa/20261030/`; no normal player profile was used. The QA
trial bypasses admission and does not establish ordinary progression or
successful trial completion.

## Safe-sector comparison

The native standalone Editor uses rebuilt gameplay code and current materials
on Metal, with an isolated MacEditor High + low-flash configuration. Python
calls the production PlayTelegraph and PlaySafeLane methods from a transient
VFX component owned by the pawn. Both frames use a 350 cm fire disc centered
500 cm ahead of the pawn and a 270-degree green sector with 30-degree
half-width. The preview holds the cues for 60 seconds and captures after
12 seconds. It does not run the AI safe-arc attack or test warning decay.

- `rendered/gameplay-audit-2026-10-30-safe-sector-before.png`: native
  code before group 65, no green sector despite successful production calls.
- `rendered/gameplay-audit-2026-10-30-safe-sector-after.png`: rebuilt
  clockwise faces show a green sector over the orange danger fill. The
  hero, floor and HUD remain readable. The process exits with code 0.

The isolated helper and successful log are
`.local/gameplay-qa/20261030/preview_safe_sector.py` and
`safe-sector-fixed-console.log`. Earlier failed helper attempts remain as
diagnostic logs; only the successful invocation and after frame support this
result. The registered native geometry and lifetime fixtures independently
pass membership, boundary, pause and warning-expiration assertions.

## Coverage boundary

These frames certify the stated rendered presentation on this Mac
(Apple M4 Pro, pinned UE 5.8.3). They do not establish a normal boss encounter,
physical-controller operation, Windows/DX12, audio listening, sustained
combat performance, or complete interactive-playthrough acceptance.
