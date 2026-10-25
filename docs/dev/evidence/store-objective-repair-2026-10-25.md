# Store objective progression repair

## Certification evidence

Product: Qinglan - Path of Ascension (`9NGLSM7P5CMB`).
The 2026-10-25 certification report identifies requirement 10.1.2.10:
an objective cannot be completed on a Microsoft Surface Laptop running
Windows build 26100.9457. The report supplies no particular objective,
screenshot, or recording. Its report ID is
`6ac0d21d-46f9-42b0-b0b0-a29bb4075282`.

The submitted 0.1.0.0 gameplay matches commit
`26f8474fbdf9249810c391022ca536930e41f592`. A fresh local native game session
on 2026-10-25 reproduces a blank starting task display: "No active
objective" after creating an empty profile. The local screenshot is
`.local/certification-repro/baseline-no-objective.png`.

A separate packaged Mac Shipping session reproduced two further symptoms
through ordinary keyboard input: after ringing the opening bell, the HUD
changes to the unrelated "Read both returned signatures" objective; after
finishing the six training lines, the large dialogue panel remains visible
while movement is already enabled. Captures are
`.local/certification-repro/baseline-bell-wrong-objective.png` and
`.local/certification-repro/baseline-training-panel-stale.png`. These are Mac
runtime observations; the certification report does not identify its Windows
objective precisely.

## Changes

- Track the earliest unfinished available main scene independently of the
  dialogue playback cursor. Refresh objectives on a new profile and reload.
- Show an actionable bilingual next step and a world destination marker with
  distance. Training steps follow actual missing gameplay proof. Document
  targets come from the physical inspection catalog, including repeated reads
  of separate entries sharing one alias.
- Give realm transitions visible frames. Walking through the current exit
  invokes the same room entry, encounter and blessing rules as interaction.
  A locked room boundary retains sideways and backward movement.
- Require scene prerequisites before automatic guarded completion, and reject
  a premature departure event before the sanctuary route has been read.
- In an active expedition, keep reachable chapter guidance for older profiles
  that used the premature departure path. This does not grant their missing
  sanctuary objectives or rewrite earned progress.
- Route the gamepad A button to menu/choice confirmation while those views own
  input, retaining dodge in gameplay and Space's ending acknowledgment behavior.
- Close finished scene subtitles independently of objective completion. Keep
  document/notice content independent, and present gameplay notices through
  the ordinary Shipping HUD instead of the engine debug-message fallback.
- Give physical document panels an explicit Back/Pause dismissal path. Handle
  Escape through the pause input action once, rather than competing raw-key
  and Enhanced Input bindings.
- Refresh only the two localization tables before Windows tests and cooking;
  preserve technique assets and their animation references. Rebuild current
  project Editor modules instead of executing older cached project DLLs.
- Give generated combat montages a valid default section. The previous assets
  stopped on their first tick because they had no section. Match sword montage
  playback and notify timing to the committed cut's phase durations, normalize
  the inherited imported hand scale to a 90 cm blade in the grip direction,
  and query the full blade segment as well as its endpoint motion.

## Verification record

Repository/story/reading-edition contracts and Python tooling checks pass.
The changed C++ translation units pass syntax checking with the native UE
headers, generated headers, module PCH and warning settings. Full compile,
named native regressions, player-input traversal and the replacement Windows
package are tracked separately in the current build logs. These checks do not
by themselves identify which objective the reviewer attempted.

The current Mac Editor links successfully after disabling unity for the
Editor module's file-local importer and fixture helpers. The native report
`.local/certification-repro/focused-20261025T143234Z/index.json` passes strict
registration/result validation: 100 named Success entries, comprising 98
regular regressions and two explicitly opted-out campaign journeys, with no
failures or not-run entries. Fifteen tests record warnings. The standing
animated training test uses a possessed pawn, real world and skeletal ticks,
non-overlapping 110 cm standing separation, and actual attack inputs; all
three independent swings each yield one accepted target hit and one proof.
It also verifies the real blade's 90 cm length after inherited bone scaling.

The late catalog fixture explicitly interrupts a pending conversation by
importing its snapshot to isolate unread-document destination selection.
It resolves the authored choice before asserting scene completion; document
reads alone do not imply choice completion.

New native regression registrations cover tracking across dialogue/reload,
proof-specific guidance, physical inspection destinations, premature gate
rejection, visible exits, walking transitions and encounter/dialogue locks,
and gamepad confirmation versus gameplay dodge. Their fixtures document which
events or positions are supplied by the test rather than real player input.
