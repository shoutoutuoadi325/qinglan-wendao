# Real checkpoint and process-restart evidence

This supports AUD-01/02/58 in the
[remaining acceptance runbook](remaining-acceptance-runbook.md). A valid save
or a passing serializer test does not establish that a relaunched game
actually resumed it. Keep the before/after gameplay screenshots or clips,
the real process termination record, and the corresponding disk snapshots.

Use the isolated `-UserDir` belonging to the actual interactive Shipping run.
The save files are under its `Saved/SaveGames/` directory. Keep the same user
directory, selected slot and package when relaunching. Record the exact
boundary (entrance, room reward, exit, hub, trial preparation or final
decision), chapter/scene, realm/room, visible currencies/vitals and pending UI.

## Preserve the real files

At each boundary, after the committed state is visible, preserve the primary
and existing backups in a new evidence directory. Record each file's byte
count and SHA256, together with the application PID and package hash. Stop
only the identified game process; do not use a broad process-name kill.
For a forced termination, record the actual `SIGKILL` and repeat the primary
hash immediately afterwards, before starting a new process. Suspending that
identified process briefly with `SIGSTOP` before the first snapshot can
prevent an unrelated later autosave from racing the hash/kill interval.
It does not replace the subsequent `SIGKILL` or restart check.

Never edit, repair or synthesize the acceptance profile. If a backup is
recovered, preserve the original files and report that recovery explicitly.
An unexpected hash change before relaunch requires investigation; it is not
automatically proof that loading failed. A successful hub-arrival autosave
can legitimately change the sequence/checksum after relaunch.

## Read a preserved snapshot without loading a profile session

After rebuilding the editor module, the read-only commandlet exports the
actual verified save object:

```bash
source tools/build/local-env.sh
EDITOR_CMD="$QINGLAN_ENGINE_ROOT/Engine/Binaries/Mac/UnrealEditor-Cmd"
# If this engine uses an app bundle instead, use the path selected by
# tools/build/common.sh::qinglan_editor_tool.
"$EDITOR_CMD" "$QINGLAN_PROJECT_DIR/QinglanAscension.uproject" \
  -run=QinglanAscensionEditor.QinglanSaveInspect \
  '-input=/absolute/path/to/the/preserved/checkpoint.sav' \
  '-output=/absolute/path/to/a/new/checkpoint.json' \
  "-DDC=$QINGLAN_DDC" -unattended -nop4 -nosplash -nullrhi
```

Choose a new output filename each time. The tool refuses existing outputs,
including the original save path. It does not call `LoadProfile`, initialize
a gameplay session, recover backups or write any save. It verifies the
production container/checksum and state invariants, then exports:

- The original container's SHA1, byte count and exact FNV-1a-64 checksum.
- Sequence, profile, checkpoint room/realm, RNG streams, committed rooms,
  pending blessing offer, player attributes and earned loot.
- Permanent inventory, transaction IDs, and complete reflected narrative
  state, including scene/choice/ending/courier data.

All reflected 64-bit integers are decimal **strings** so large balances and
checksums cannot be rounded through JSON's floating-point representation.
Older supported schemas may migrate in memory; preserve the input bytes and
record the source build/schema when interpreting that export. The original
container is never rewritten by inspection. Keep the separate SHA256 from
the file snapshot as well; none of these hashes authenticates player behavior.

## Verify the fresh process

Relaunch the same Shipping artifact and select the same slot through the
Profile screen. Capture the actual resumed location, pending reward/choice,
vitals and progression. Preserve the resulting save and inspect its copy
again after the relevant load/arrival save has finished.

Compare the state required at that specific boundary: checkpoint room/seed/
streams, loot, committed-room and transaction ledgers, pending offers,
scene/choice/ending flags and balances. Record any expected gameplay changes
individually, such as the hub refill or elapsed regeneration; do not hide
differences by removing whole state sections from the comparison. Verify
that a completed-room reward is not paid again through real input and the
resulting balances/ledgers. A JSON export is disk-state evidence and does
not by itself prove loaded pawn state, travel, UI recovery or no reward replay.

The synthetic `Qinglan.Persistence.ReadOnlySaveInspection` case verifies
read-only export, checkpoint fields, exact large integers and refusal of
corrupt/overwriting requests. It is tooling regression coverage, not one of
the required real process-kill/playthrough records.
