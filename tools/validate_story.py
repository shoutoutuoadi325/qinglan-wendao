#!/usr/bin/env python3
"""Check bilingual narrative references and explicit ending invariants."""

import json
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
ENDING_FLAGS = {"ending_open", "ending_seal", "ending_shared"}
SHARED_FLAGS = {"sq_spillway_verified", "sq_allocation_agreed", "sq_handover_accepted"}

# --- AUD-10 scene/stage location routing whitelist --------------------------
# Anchors live in C++ (not data), so the whitelist is mirrored here with the
# exact per-file sources:
# - BOOTSTRAP_ANCHORS: game/Source/QinglanAscensionEditor/QinglanContentBootstrap.cpp
#   BootstrapMaps() anchor arrays (HubAnchors/BambooAnchors/AshenAnchors/
#   ArenaAnchors) author every edit-time ATargetPoint.
# - RUNTIME_ANCHORS: game/Source/QinglanAscension/Core/QinglanInteractionSpawner.cpp
#   SynthesizeHubGateAnchors() spawns these hub gates beside hub.bamboo_gate.
# - AUD10_CANONICAL_ANCHORS: docs/dev/completion-audit-todo.md AUD-10 canonical
#   routing keys still pending bootstrap anchor authoring on the C++ side.
# QinglanInteractable.cpp (BeginSceneAtAnchor/FindObjectiveProofAtAnchor,
# SlashFormOfAnchor) matches dot and slash forms interchangeably, so "/" is
# normalized to "." before lookup; side-quest stages keep their legacy
# "<map>/<station>" spellings, accepted as aliases of sq.<map>.<station>
# anchors with the "sq." prefix stripped. lore/barks "location" values are
# presentation categories, not routing keys, and are not checked here.
BOOTSTRAP_ANCHORS = frozenset({
    # L_Hub
    "hub.courtyard", "hub.training_yard", "hub.cultivation_shrine", "hub.shrine",
    "hub.workbench", "hub.lamp_workbench", "hub.bamboo_gate", "hub.handover_table",
    "hub.archive_shelf", "hub.archive_table", "hub.courier_board",
    "hub.inspection_board", "hub.water_table", "hub.stonewright_crate",
    "hub.old_table", "hub.training_mat",
    # L_BambooHollow
    "bamboo.room_0", "bamboo.rooms_1_to_5", "bamboo.room_1_2_guided",
    "bamboo.room_5_safe",
    # L_AshenVerge
    "ashen.room_0", "ashen.approach_1_2", "ashen.archive_3", "ashen.vent_4",
    "ashen.boss_5", "ashen.release_antechamber",
    # L_TribulationArena
    "trial.foundation_arena", "trial.golden_core_arena", "trial.decision_platform",
    "trial.quiet_governor_platform", "trial.preparation_shrine",
    # Side-quest safe stations
    "sq.bamboo.old_bell_safe", "sq.bamboo.picnic_shelf_safe",
    "sq.bamboo.spillway_safe", "sq.bamboo.wheel_safe",
    "sq.ashen.archive_safe", "sq.ashen.orchard_safe", "sq.ashen.vent_room_safe",
})
RUNTIME_ANCHORS = frozenset({"hub.ashen_gate", "hub.trial_gate"})
AUD10_CANONICAL_ANCHORS = frozenset({"hub.ledger_table", "hub.correspondence_table"})
KNOWN_ANCHORS = BOOTSTRAP_ANCHORS | RUNTIME_ANCHORS | AUD10_CANONICAL_ANCHORS
SQ_LEGACY_ALIASES = frozenset(
    anchor[len("sq."):] for anchor in BOOTSTRAP_ANCHORS if anchor.startswith("sq.")
)

# --- AUD-11 objective proof / confirmation guard shapes ---------------------
# Canonical gameplay-proof event names are lowercase dot/underscore ids
# ("record.opened.route_letter"); a bare single segment ("shell_defeated") is
# also accepted where it matches an existing narrative trigger name.
EVENT_NAME_RE = re.compile(r"^[a-z][a-z0-9_]*(?:\.[a-z0-9_]+)*$")
TERM_FIELDS = {
    "event": {"kind", "event"},
    "event_count": {"kind", "event", "count"},
    "ordered": {"kind", "steps"},
    "delivery": {"kind", "quest"},
    "time_advance": {"kind", "game_hours"},
    "flag": {"kind", "flag"},
}


def location_anchor_error(owner, location):
    if not isinstance(location, str) or not location.strip():
        return f"{owner}: scene/stage requires a location anchor key"
    key = location.strip().replace("/", ".")
    if key.startswith("record."):
        # record.* anchors are data-driven (lore.json record stations).
        return None
    if key in KNOWN_ANCHORS or key in SQ_LEGACY_ALIASES:
        return None
    return f"{owner}: location {location!r} is not a known map anchor"


def event_name_error(name):
    if not isinstance(name, str) or not EVENT_NAME_RE.match(name):
        return f"invalid event name {name!r} (expected lowercase dot/underscore id)"
    return None


def validate_proof_terms(owner, terms, errors, side_quest_ids, produced_flags):
    for index, term in enumerate(terms):
        label = f"{owner} term[{index}]"
        if not isinstance(term, dict):
            errors.append(f"{label}: term must be an object")
            continue
        kind = term.get("kind")
        if kind not in TERM_FIELDS:
            errors.append(f"{label}: unknown proof kind {kind!r}")
            continue
        if set(term) != TERM_FIELDS[kind]:
            errors.append(
                f"{label}: {kind} term fields must be exactly {sorted(TERM_FIELDS[kind])}"
            )
            continue
        if kind in ("event", "event_count"):
            error = event_name_error(term["event"])
            if error:
                errors.append(f"{label}: {error}")
            if kind == "event_count":
                count = term["count"]
                if isinstance(count, bool) or not isinstance(count, int) or count < 1:
                    errors.append(f"{label}: count must be an integer >= 1")
        elif kind == "ordered":
            steps = term["steps"]
            if not isinstance(steps, list) or not steps:
                errors.append(f"{label}: ordered steps must be a non-empty list")
                continue
            if len(set(steps)) != len(steps):
                errors.append(f"{label}: ordered steps contain duplicates")
            for step in steps:
                error = event_name_error(step)
                if error:
                    errors.append(f"{label}: {error}")
        elif kind == "delivery":
            if term["quest"] not in side_quest_ids:
                errors.append(f"{label}: delivery references unknown quest {term['quest']!r}")
        elif kind == "time_advance":
            hours = term["game_hours"]
            if isinstance(hours, bool) or not isinstance(hours, (int, float)) or hours < 0:
                errors.append(f"{label}: game_hours must be a number >= 0")
        elif kind == "flag":
            flag = term["flag"]
            if not isinstance(flag, str) or not flag:
                errors.append(f"{label}: flag must be a non-empty string")
            elif flag not in produced_flags:
                errors.append(f"{label}: flag {flag!r} has no producer")


def validate_proof(owner, container, errors, side_quest_ids, produced_flags):
    """Validate an AUD-11 {"all_of": [terms]} proof/guard container in place."""
    if not isinstance(container, dict) or set(container) != {"all_of"}:
        errors.append(f"{owner}: proof must be an object with a single 'all_of' list")
        return
    terms = container["all_of"]
    if not isinstance(terms, list) or not terms:
        errors.append(f"{owner}: proof.all_of must be a non-empty list")
        return
    validate_proof_terms(owner, terms, errors, side_quest_ids, produced_flags)


def validate_story_routing(errors, registries, produced_flags):
    """AUD-10/AUD-11 structural checks over scenes and side-quest stages."""
    side_quest_ids = {
        quest.get("id") for quest in registries.get("side-quests", {}).get("quests", [])
    }
    for label, registry in registries.items():
        owners = [(f"{label}/{scene.get('id', 'scene')}", scene)
                  for scene in registry.get("scenes", [])]
        for quest in registry.get("quests", []):
            owners.extend(
                (f"{label}/{stage.get('id', 'stage')}", stage)
                for stage in quest.get("stages", [])
            )
        for owner, node in owners:
            error = location_anchor_error(owner, node.get("location"))
            if error:
                errors.append(error)
            note = node.get("location_note")
            if note is not None and (not isinstance(note, str) or not note.strip()):
                errors.append(f"{owner}: location_note must preserve the prose location")
            for objective in node.get("objectives", []):
                if "proof" in objective:
                    validate_proof(
                        f"{owner}/{objective.get('id', 'objective')}",
                        objective["proof"],
                        errors,
                        side_quest_ids,
                        produced_flags,
                    )
            guard = node.get("confirmation_guard")
            if guard is not None:
                if isinstance(guard, str):
                    errors.append(
                        f"{owner}: confirmation_guard must be structured "
                        "{'all_of': [...]} with the prose moved to confirmation_guard_note"
                    )
                else:
                    validate_proof(
                        f"{owner}/confirmation_guard", guard, errors, side_quest_ids, produced_flags
                    )
                    prose = node.get("confirmation_guard_note")
                    if not isinstance(prose, str) or not prose.strip():
                        errors.append(
                            f"{owner}: structured confirmation_guard requires "
                            "confirmation_guard_note prose"
                        )


def nodes(value):
    if isinstance(value, dict):
        yield value
        for child in value.values():
            yield from nodes(child)
    elif isinstance(value, list):
        for child in value:
            yield from nodes(child)


def text_references(value):
    found = set()
    for node in nodes(value):
        for field, item in node.items():
            if field in {"transaction_key", "commit_key", "idempotency_key"}:
                continue
            if field.endswith("_key") and isinstance(item, str):
                found.add(item)
            elif field.endswith("_keys") and isinstance(item, list):
                found.update(key for key in item if isinstance(key, str))
    return found


def validate_story():
    errors = []
    english, chinese = {}, {}
    en_dir = ROOT / "localization/story/en-US"
    zh_dir = ROOT / "localization/story/zh-Hans"
    en_files = {p.name for p in en_dir.glob("*.json")}
    zh_files = {p.name for p in zh_dir.glob("*.json")}
    if en_files != zh_files:
        errors.append("Story locale file sets differ")
    for filename in sorted(en_files & zh_files):
        try:
            en = json.loads((en_dir / filename).read_text(encoding="utf-8"))
            zh = json.loads((zh_dir / filename).read_text(encoding="utf-8"))
        except (OSError, ValueError) as error:
            errors.append(f"{filename}: {error}")
            continue
        if not isinstance(en, dict) or not isinstance(zh, dict):
            errors.append(f"{filename}: locale resource must be an object")
            continue
        if en.keys() != zh.keys():
            errors.append(f"{filename}: bilingual keys differ")
        for key in en.keys() & zh.keys():
            if key in english:
                errors.append(f"Duplicate story text key: {key}")
            a, b = en[key], zh[key]
            if not isinstance(a, str) or not a.strip() or not isinstance(b, str) or not b.strip():
                errors.append(f"Empty or invalid story text: {key}")
                continue
            pattern = r"\{([a-zA-Z_][a-zA-Z0-9_]*)\}"
            if sorted(re.findall(pattern, a)) != sorted(re.findall(pattern, b)):
                errors.append(f"Story placeholder mismatch: {key}")
            if re.search(r"\b(?:TODO|TBD|PLACEHOLDER|REPLACE_ME)\b", a):
                errors.append(f"Unfinished player text: {key}")
            english[key], chinese[key] = a, b

    registries = {}
    for path in sorted((ROOT / "data/story").glob("*.json")):
        try:
            value = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, ValueError) as error:
            errors.append(f"{path.name}: {error}")
            continue
        if not isinstance(value, dict) or value.get("schema_version") != 1:
            errors.append(f"{path.name}: unsupported story schema")
            continue
        registries[path.stem] = value

    refs = set()
    all_scenes = []
    chapters = []
    confirmations = []
    produced_flags = set()
    required_flags = set()
    for label, registry in registries.items():
        refs.update(text_references(registry))
        all_scenes.extend(registry.get("scenes", []))
        chapters.extend(registry.get("chapters", []))
        for node in nodes(registry):
            for field in ("sets_flags", "completion_flags", "sets_after_discharge"):
                produced_flags.update(node.get(field, []))
            if "collection_flag" in node:
                produced_flags.add(node["collection_flag"])
            for field in ("prerequisites", "availability", "required_flags", "when_all", "when_none", "forbidden_flags"):
                required_flags.update(node.get(field, []))
            if "player_text_keys" in node:
                declared = set(node["player_text_keys"])
                actual = text_references({k: v for k, v in node.items() if k != "player_text_keys"})
                missing = actual - declared
                if missing:
                    errors.append(f"{label}/{node.get('id', 'node')}: undeclared text keys {sorted(missing)}")
            flags = set(node.get("sets_flags", []))
            selected = flags & ENDING_FLAGS
            if len(selected) > 1:
                errors.append("A choice cannot set more than one ending")
            if selected:
                confirmations.append(node)
                action = node.get("action", {})
                action_type = action.get("type") if isinstance(action, dict) else action
                if action_type != "confirm_ending":
                    errors.append(f"Ending flag set outside confirmation: {node.get('id')}")
                if "ending_shared" in selected and not SHARED_FLAGS.issubset(node.get("prerequisites", [])):
                    errors.append("Shared confirmation lacks all three explicit prerequisites")

    validate_story_routing(errors, registries, produced_flags)

    for key in sorted(refs - english.keys()):
        errors.append(f"Missing bilingual story text: {key}")
    orphaned = english.keys() - refs
    if orphaned:
        errors.append(f"Story text absent from registry: {sorted(orphaned)}")
    if required_flags - produced_flags:
        errors.append(f"Narrative flags have no producer: {sorted(required_flags - produced_flags)}")
    scene_ids = [scene.get("id") for scene in all_scenes]
    if len(scene_ids) != len(set(scene_ids)):
        errors.append("Duplicate main scene ID")
    chapter_ids = {chapter.get("id") for chapter in chapters}
    if registries.get("main-early") and registries.get("main-late"):
        if chapter_ids != {f"CH{i:02}" for i in range(8)}:
            errors.append("Main campaign must declare CH00–CH07")
        for chapter in chapters:
            for scene in chapter.get("scene_ids", []):
                if scene not in scene_ids:
                    errors.append(f"Chapter references unknown scene: {scene}")
        for scene in all_scenes:
            if scene.get("chapter") not in chapter_ids:
                errors.append(f"Unknown scene chapter: {scene.get('id')}")
            for target in scene.get("next_scene_ids", []):
                if target not in scene_ids:
                    errors.append(f"Unknown scene transition: {scene.get('id')} → {target}")
        emitted = set().union(*(set(node.get("sets_flags", [])) & ENDING_FLAGS for node in confirmations))
        if emitted != ENDING_FLAGS:
            errors.append("Final choices must explicitly confirm all three endings")
        for confirmation in confirmations:
            action = confirmation.get("action", {})
            if set(action.get("mutually_exclusive_flags", [])) != ENDING_FLAGS:
                errors.append("Ending confirmation lacks mutual-exclusion guard")
            if not action.get("requires_warning_acknowledgment") or not action.get("requires_preview"):
                errors.append("Ending confirmation must require preview and warning acknowledgment")
        for accepted in range(8):
            available_flags = {flag for index, flag in enumerate(sorted(SHARED_FLAGS)) if accepted & (1 << index)}
            allowed = []
            for confirmation in confirmations:
                side_requirements = set(confirmation.get("prerequisites", [])) & SHARED_FLAGS
                if side_requirements.issubset(available_flags):
                    allowed.extend(set(confirmation.get("sets_flags", [])) & ENDING_FLAGS)
            expected = ENDING_FLAGS if accepted == 7 else {"ending_open", "ending_seal"}
            if set(allowed) != expected:
                errors.append(f"Invalid ending eligibility for completed agreements: {sorted(available_flags)}")
    stats = {
        "story_keys_per_locale": len(english),
        "main_chapters": len(chapters),
        "main_scenes": len(all_scenes),
        "side_quests": len(registries.get("side-quests", {}).get("quests", [])),
        "side_stages": sum(len(q.get("stages", [])) for q in registries.get("side-quests", {}).get("quests", [])),
        "lore_records": len(registries.get("lore", {}).get("records", [])),
        "contextual_barks": len(registries.get("barks", {}).get("barks", [])),
    }
    return errors, stats


if __name__ == "__main__":
    issues, counts = validate_story()
    for issue in issues:
        print(f"- {issue}")
    print(json.dumps(counts, indent=2))
    raise SystemExit(bool(issues))
