#!/usr/bin/env python3
"""Render complete bilingual review editions from authoritative story resources."""

import argparse
import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DESTINATION = ROOT / "localization/story/reading"


def resources(locale):
    merged = {}
    for path in sorted((ROOT / f"localization/story/{locale}").glob("*.json")):
        merged.update(json.loads(path.read_text(encoding="utf-8")))
    return merged


def quote(text):
    return "\n".join("> " + line if line else ">" for line in text.splitlines())


def render_books():
    en, zh = resources("en-US"), resources("zh-Hans")
    books = {}

    def begin(title):
        return [f"# {title}", "", "Generated bilingual reading edition. Authoritative copy is in the locale JSON files; production staging and conditions are in the corresponding English scripts. No playable runtime is implied.", ""]

    def show(lines, key, seen, label=None):
        if key in seen:
            return
        seen.add(key)
        lines.extend([f"### {label or key}", "", f"Key: `{key}`", "", "**English**", "", quote(en[key]), "", "**Simplified Chinese**", "", quote(zh[key]), ""])

    def block(lines, scene, seen):
        lines.extend([f"## {scene['id']} — {en.get(scene.get('title_key'), scene.get('location', ''))}", "", f"Location: {scene.get('location', '')}", ""])
        if scene.get("title_key"):
            show(lines, scene["title_key"], seen, "Scene title")
        for row in scene.get("script", []):
            conditions = row.get("when_all", [])
            excluded = row.get("when_none", [])
            if conditions or excluded:
                lines.extend([f"Conditions: require {conditions}; exclude {excluded}.", ""])
            speaker_key = row.get("speaker_key")
            name = en.get(speaker_key, row.get("speaker", "Text"))
            if speaker_key in zh:
                name += " / " + zh[speaker_key]
            source_key = row.get("source_kind_key")
            if source_key in en:
                name += " — " + en[source_key] + " / " + zh[source_key]
            show(lines, row["text_key"], seen, name)
        for choice in scene.get("choices", []):
            lines.extend([f"#### Choice `{choice['id']}`", ""])
            if choice.get("prerequisites"):
                lines.extend([f"Requires: {choice['prerequisites']}.", ""])
            show(lines, choice["label_key"], seen, "Player choice")
            for key in choice.get("response_keys", []):
                show(lines, key, seen, "Selected response")
        for row in scene.get("objectives", []):
            show(lines, row["text_key"], seen, "Objective")
        for key in scene.get("player_text_keys", []):
            if not key.startswith("story.speaker."):
                show(lines, key, seen, "Journal, inspection, or contextual text")

    for name in ("main-early", "main-late"):
        registry = json.loads((ROOT / f"data/story/{name}.json").read_text())
        for chapter in registry["chapters"]:
            seen = set()
            lines = begin(f"{chapter['id']} — {en[chapter['title_key']]}")
            show(lines, chapter["title_key"], seen, "Chapter title")
            show(lines, chapter["summary_key"], seen, "Chapter summary")
            for scene in registry["scenes"]:
                if scene["chapter"] == chapter["id"]:
                    block(lines, scene, seen)
            books[chapter["id"] + ".md"] = "\n".join(lines).rstrip() + "\n"

    side = json.loads((ROOT / "data/story/side-quests.json").read_text())
    for quest in side["quests"]:
        seen = set()
        lines = begin(f"{quest['id']} — {en[quest['title_key']]}")
        show(lines, quest["title_key"], seen, "Quest title")
        show(lines, quest["summary_key"], seen, "Quest summary")
        for stage in quest["stages"]:
            block(lines, stage, seen)
        for key in quest.get("epilogue_keys", []):
            show(lines, key, seen, "Ending-specific retrospective")
        for key in quest["player_text_keys"]:
            if not key.startswith("story.speaker."):
                show(lines, key, seen)
        books[quest["id"] + ".md"] = "\n".join(lines).rstrip() + "\n"

    lore = json.loads((ROOT / "data/story/lore.json").read_text())
    lines = begin("Environmental Records — Complete Bilingual Copy")
    seen = set()
    for record in lore["records"]:
        lines.extend([f"## {record['id']} — {en[record['title_key']]}", "", f"Location: {record['location']}", ""])
        for field in ("title_key", "date_key", "body_key", "inspect_key"):
            show(lines, record[field], seen)
    books["LORE.md"] = "\n".join(lines).rstrip() + "\n"
    barks = json.loads((ROOT / "data/story/barks.json").read_text())
    lines = begin("Contextual Lines — Complete Bilingual Copy")
    seen = set()
    for bark in barks["barks"]:
        show(lines, bark["text_key"], seen, f"{bark['id']} — {bark['speaker']}, {bark['location']}")
    books["BARKS.md"] = "\n".join(lines).rstrip() + "\n"
    return books


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true", help="Reject stale generated editions")
    options = parser.parse_args()
    books = render_books()
    DESTINATION.mkdir(parents=True, exist_ok=True)
    stale = []
    for name, text in books.items():
        path = DESTINATION / name
        if options.check:
            if not path.exists() or path.read_text(encoding="utf-8") != text:
                stale.append(name)
        else:
            path.write_text(text, encoding="utf-8")
    if stale:
        print("Stale bilingual reading editions: " + ", ".join(stale))
        return 1
    print(f"{'Checked' if options.check else 'Generated'} {len(books)} complete bilingual reading editions.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
