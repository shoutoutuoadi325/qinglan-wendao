#!/usr/bin/env python3
"""Validate the planning baseline without engine or third-party dependencies."""

from pathlib import Path
import csv
import json
import re
import sys
import xml.etree.ElementTree as ET
from urllib.parse import unquote
from validate_story import validate_story


ROOT = Path(__file__).resolve().parents[1]
ERRORS = []


def require(condition, message):
    if not condition:
        ERRORS.append(message)


def read_json(relative):
    path = ROOT / relative
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        ERRORS.append(f"{relative}: {error}")
        return None


def validate_text_and_links():
    ignored = {".git", "__pycache__", "private", "build", "dist", "output"}
    count = 0
    for path in ROOT.rglob("*"):
        relative = path.relative_to(ROOT)
        if not path.is_file() or ignored.intersection(relative.parts):
            continue
        if path.suffix not in {".md", ".py", ".json", ".xml", ".yml", ".csv"}:
            continue
        text = path.read_text(encoding="utf-8")
        count += 1
        if relative.parts[0] != "localization":
            require(not re.search(r"[\u3400-\u4dbf\u4e00-\u9fff]", text),
                    f"{relative}: Chinese text belongs in localization resources")
        if path.suffix != ".md":
            continue
        for target in re.findall(r"\]\(([^)]+)\)", text):
            target = target.split("#", 1)[0]
            if not target or re.match(r"^[a-zA-Z][a-zA-Z0-9+.-]*:", target):
                continue
            target = unquote(target.strip("<>"))
            require((path.parent / target).exists(), f"{relative}: broken link {target}")
    return count


def validate_localization():
    en = read_json("localization/en-US.json")
    zh = read_json("localization/zh-Hans.json")
    if not isinstance(en, dict) or not isinstance(zh, dict):
        require(False, "Locale files must contain objects")
        return {}
    require(en.keys() == zh.keys(), "Locale keys differ")
    for key in en.keys() & zh.keys():
        a, b = en[key], zh[key]
        require(isinstance(a, str) and bool(a.strip()), f"Empty English value: {key}")
        require(isinstance(b, str) and bool(b.strip()), f"Empty Chinese value: {key}")
        if isinstance(a, str) and isinstance(b, str):
            placeholders = r"\{([a-zA-Z_][a-zA-Z0-9_]*)\}"
            require(sorted(re.findall(placeholders, a)) == sorted(re.findall(placeholders, b)),
                    f"Placeholder mismatch: {key}")
    return en


def validate_examples(strings):
    data = read_json("data/examples/techniques.json")
    realm = read_json("data/examples/realm.json")
    save = read_json("data/examples/save.json")
    if not all(isinstance(x, dict) for x in (data, realm, save)):
        require(False, "Example files must contain objects")
        return
    for label, value in (("techniques", data), ("realm", realm), ("save", save)):
        require(value.get("schema_version") == 1, f"Unsupported {label} schema")
    rows = data.get("techniques", [])
    require(isinstance(rows, list) and len(rows) == 8, "Expected eight technique examples")
    ids = [row.get("id") for row in rows if isinstance(row, dict)]
    require(len(ids) == len(set(ids)) == 8, "Technique IDs must be unique")
    for row in rows:
        if not isinstance(row, dict):
            require(False, "Technique entries must be objects")
            continue
        require(bool(re.fullmatch(r"[a-z][a-z0-9_]*", row.get("id", ""))), "Invalid technique ID")
        require(row.get("name_key") in strings, f"Missing technique localization: {row.get('id')}")
        require(row.get("element") in {"Metal", "Wood", "Water", "Fire", "Earth", "Lightning"},
                f"Invalid element: {row.get('id')}")
        for field in ("energy_cost", "cooldown_seconds", "power_coefficient", "flat_damage"):
            number = row.get(field)
            require(type(number) in (int, float) and 0 <= number < 10000,
                    f"Invalid {field}: {row.get('id')}")
    rooms = realm.get("rooms", [])
    require([room.get("index") for room in rooms] == list(range(7)), "Room indices must be 0–6")
    require([room.get("kind") for room in rooms] ==
            ["entrance", "combat", "combat", "shrine", "elite", "boss", "exit"], "Unexpected room sequence")
    require(sum(room.get("spirit_shards", 0) for room in rooms) == 180, "Bamboo base reward must total 180")
    require(realm.get("name_key") in strings, "Missing realm localization")
    require(realm.get("max_concurrent_enemies") == 8, "Encounter cap differs from design")
    require(save.get("content_version") == data.get("content_version"), "Example content versions differ")
    require(save.get("cultivation") == {"stage": "qi_refining", "rank": 0}, "Example must represent a new profile")
    for name, count in save.get("inventory", {}).items():
        require(type(count) is int and 0 <= count <= 2**63 - 1, f"Invalid inventory count: {name}")
    unlocked = save.get("unlocked_techniques", [])
    equipped = save.get("equipped_techniques", [])
    require(set(unlocked).issubset(ids), "Unknown unlocked technique")
    require(set(equipped).issubset(unlocked) and len(equipped) <= 4, "Invalid loadout")
    require(save.get("checkpoint") is None, "New profile example must start in sanctuary")


def validate_packaging_and_manifest():
    path = ROOT / "packaging/windows/MicrosoftGame.config.template.xml"
    try:
        root = ET.parse(path).getroot()
        require(root.tag == "Game", "Windows template root must be Game")
        forbidden = {"MSAAppId", "TitleId", "ExtendedAttributeList"}
        require(not forbidden.intersection(node.tag for node in root.iter()),
                "Offline Windows template must omit Xbox-services fields")
        languages = {node.attrib.get("Language") for node in root.findall("Resources/Resource")}
        require(languages == {"en-US", "zh-Hans"}, "Windows template must declare both UI locales")
        require("REPLACE_FROM_PARTNER_CENTER" in path.read_text(), "Planning identity must remain a placeholder")
    except (OSError, ET.ParseError) as error:
        ERRORS.append(f"Windows template: {error}")
    with (ROOT / "assets/asset-manifest.csv").open(newline="", encoding="utf-8") as stream:
        reader = csv.DictReader(stream)
        require("source_redistribution_allowed" in (reader.fieldnames or []), "Asset ledger lacks redistribution field")
        for row in reader:
            if row.get("public_path"):
                require(row.get("source_redistribution_allowed") == "yes",
                        f"Public asset lacks permission: {row.get('asset_id')}")


def main():
    count = validate_text_and_links()
    strings = validate_localization()
    validate_examples(strings)
    validate_packaging_and_manifest()
    story_errors, story_stats = validate_story()
    ERRORS.extend(story_errors)
    if ERRORS:
        print("Planning validation failed:")
        for error in ERRORS:
            print(f"- {error}")
        return 1
    print(f"Planning validation passed: {count} text files, {len(strings)} keys per locale, "
          "8 techniques, 7 rooms, save example, asset ledger, and packaging template.")
    print("This does not validate Unreal gameplay, GPU performance, or Store certification.")
    print("Narrative contracts: " + json.dumps(story_stats, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
