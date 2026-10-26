# Development Language and Bilingual UI

## 1. Language policy

Development documents, source identifiers, code comments, developer-only logs, issue text, and commit messages are English. Player-facing UI supports **English and Simplified Chinese** at first release. Localized Chinese values are allowed only in dedicated localization resources; Chinese text should not be scattered through code or English planning documents.

Cultivation terminology has reviewed native Chinese equivalents in `localization/zh-Hans.json`. English uses accessible translations rather than unexplained transliteration. Traditional Chinese is not claimed as supported; a later locale requires its own editorial and layout review.

## 2. English terminology

| Concept | Canonical English | Usage |
| --- | --- | --- |
| Practice and growth | Cultivation | Permanent progression system |
| First realm | Qi Refining | Explain Qi as spiritual energy in first tooltip |
| Second realm | Foundation Establishment | Never mix with Foundation Building |
| Campaign culmination | Golden Core | Capitalized realm name |
| Magical resource | Spiritual Energy | UI may abbreviate to Energy where context is clear |
| Currency | Spirit Shards | Non-purchasable banked progression resource |
| Expedition area | Spiritual Realm | Use Realm on compact navigation controls |
| Breakthrough challenge | Heavenly Tribulation | Use Tribulation after introduction |
| Active skill | Technique | Never alternate between Skill, Spell, and Art in menus |
| Prepared item | Talisman | Refillable expedition charges |

Technique names match the combat document. Five Elements are Metal, Wood, Water, Fire, and Earth; Lightning is a special technique category. English copy uses ordinary sentences and avoids exaggerated power claims.

## 3. Localization architecture

Use stable keys such as `menu.continue`, `technique.jade_sword_arc.name`, and `error.save_failed`. UMG widgets receive `FText` from String Tables/localization assets. Never concatenate translated fragments. Use named arguments with `FText::Format`, plural-aware patterns where needed, and locale-aware numeric formatting.

The JSON files are reviewable example copy and key contracts. Runtime implementation imports them into the approved Unreal String Table/localization workflow or generates equivalent resources through a documented script. They are not automatically loaded by Unreal today.

Both locales must have identical keys and named placeholders. Missing localized text falls back to English and is flagged in development validation. Empty strings and raw keys cannot ship. Developers may log key names in English without exposing those logs to players.

## 4. Language selection

First launch selects Simplified Chinese for a matching OS language and English otherwise. Persist a user override independently of the save profile. Show the language selector before the tutorial. Both language options use their native names from resource files so the player can recover from an unfamiliar language.

Switching language refreshes visible widgets, dialogue/journal, controller prompts, formatting, and runtime-generated tooltips without restarting. If a platform-specific screen cannot switch immediately, display its limitation and re-open it with the selected locale; gameplay HUD and menus must update immediately.

## 5. Fonts and layout

Choose a redistributable Latin/CJK font with complete Simplified Chinese coverage, including realm/technique names, numbers, punctuation, and supported user-profile input. Package only licensed files and retain the exact license. Font fallback must not produce missing-glyph squares.

Use responsive widgets with wrapping and minimum/maximum widths, not fixed English-sized buttons. Test both languages at 100%, 125%, and 150% scale. English often needs more horizontal space; Chinese requires readable glyph size and line height. On Retina Macs, verify DPI scaling without doubling layout dimensions incorrectly.

Profile names are optional player input, bounded to 24 displayed characters with a Unicode-aware policy; they must not be used as filesystem names. Do not accept invisible control characters. The default label is localized and profiles retain stable internal IDs.

## 6. Copy coverage

Localize title/menu/settings, HUD labels, techniques and statuses, item descriptions, journal, tutorial, interactions, reward previews, confirmation dialogs, failure messages, save recovery, credits UI, compatibility messages, and accessibility options. Third-party legal text may retain its required original wording, with a Chinese explanation where permitted.

Store listings and important screenshot captions must be reviewed in both languages. Developer-facing build instructions stay English. The free/no-ad/no-purchase message must be accurate in both locales.

## 7. Localization acceptance

Validate key parity, placeholder parity, non-empty values, glossary consistency, fallback behavior, and font coverage. Review all user-visible strings in context in both builds. English documents can describe the Chinese locale using keys and transliteration; dedicated resource values contain the actual Chinese copy.

## 8. Complete narrative copy

The story resources now contain the complete authored dialogue, branches, documents, objectives, retries, and three endings, rather than only example prose. Use [the story index](story/README.md) and generated bilingual reading editions for review. The locale JSON files are imported into `ST_Game`/`DT_Localization` by `UQinglanDataImportCommandlet`; keep a single source of exact text in the JSON files so deterministic reading exports prevent drift.
