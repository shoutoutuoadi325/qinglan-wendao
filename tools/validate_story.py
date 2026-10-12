#!/usr/bin/env python3
"""Check bilingual narrative references and explicit ending invariants."""

import json
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
ENDING_FLAGS = {"ending_open", "ending_seal", "ending_shared"}
SHARED_FLAGS = {"sq_spillway_verified", "sq_allocation_agreed", "sq_handover_accepted"}


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
