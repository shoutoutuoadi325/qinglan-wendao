# Localization Copy Baseline

English and Simplified Chinese files share stable keys and named placeholders. They contain initial menu/HUD/tutorial/error/store copy and technique names, not the final complete game's text inventory. Import into the approved Unreal localization pipeline during LOC-001.

Chinese values are intentionally confined to localization resources. Developer comments, docs, logs, and commits stay English. Both files may contain native language labels for the language picker.

Run `python3 tools/validate_repository.py` to check key and placeholder parity. Semantic quality, font coverage, localized number formatting, and layout still require human review in both packaged builds.
