"""Catalog keys built at runtime must exist in both shipped source locales."""
import json
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]


class RuntimeLocalizationTests(unittest.TestCase):
    def test_every_equipment_effect_description_is_bilingual(self):
        source = (ROOT/'game/Source/QinglanAscension/Progression/QinglanEconomyLibrary.cpp').read_text()
        keys = set(re.findall(r'"((?:passive|talisman)\.[a-z_]+\.desc)"', source))
        self.assertEqual(len(keys), 14)
        for locale in ('en-US', 'zh-Hans'):
            table = json.loads((ROOT/'localization'/f'{locale}.json').read_text())
            for key in keys:
                with self.subTest(locale=locale, key=key):
                    self.assertTrue(table.get(key, '').strip(), key)

    def test_every_production_blessing_name_and_description_is_bilingual(self):
        source = (ROOT/'game/Source/QinglanAscension/Abilities/QinglanBlessingLibrary.cpp').read_text()
        ids = re.findall(r'Make\("(b_[a-z_]+)"', source)
        self.assertEqual(len(set(ids)), 12)
        for locale in ('en-US', 'zh-Hans'):
            table = json.loads((ROOT/'localization'/f'{locale}.json').read_text())
            for blessing in ids:
                for slot in ('name', 'desc'):
                    key = f'blessing.{blessing}.{slot}'
                    with self.subTest(locale=locale, key=key):
                        self.assertTrue(table.get(key, '').strip(), key)

    def test_backup_patterns_have_the_same_named_arguments_in_both_languages(self):
        expected = {'save.progress_preview': {'stage', 'rank'}, 'save.backup_restored': {'date', 'progress'}}
        for locale in ('en-US', 'zh-Hans'):
            table = json.loads((ROOT/'localization'/f'{locale}.json').read_text())
            for key, arguments in expected.items():
                with self.subTest(locale=locale, key=key):
                    self.assertEqual(set(re.findall(r'\{([^}]+)\}', table[key])), arguments)


if __name__ == '__main__':
    unittest.main()
