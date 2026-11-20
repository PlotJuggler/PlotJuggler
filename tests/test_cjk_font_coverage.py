# SPDX-License-Identifier: MPL-2.0
"""Coverage regressions use the actual committed fonts, not requested counts."""
import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('cjk', ROOT / 'scripts/make_cjk_font_subset.py')
cjk = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cjk)


class FontCoverageTest(unittest.TestCase):
    def test_actual_catalogs(self):
        cjk.check_coverage(cjk.catalog_text(), [cjk.LATIN, cjk.OUTPUT, cjk.SYMBOL_OUTPUT])

    def test_traditional_regression(self):
        self.assertTrue({ord(c) for c in '創擊補'} <= cjk.cmap(cjk.OUTPUT))

    def test_menu_separator_uses_bundled_symbol(self):
        self.assertIn(ord('▸'), cjk.cmap(cjk.SYMBOL_OUTPUT))
        cjk.check_coverage('外掛程式設定 ▸ Preferences', [cjk.LATIN, cjk.OUTPUT, cjk.SYMBOL_OUTPUT])

    def test_missing_catalog_character_fails(self):
        with self.assertRaisesRegex(ValueError, 'U\\+0378'):
            cjk.check_coverage('\u0378', [cjk.LATIN, cjk.OUTPUT, cjk.SYMBOL_OUTPUT])

    def test_no_blanket_punctuation_range_requirement(self):
        # U+FF00 is unassigned, and is not demanded merely by a broad range.
        self.assertNotIn(0xFF00, cjk.required_codepoints(cjk.catalog_text()))
        cjk.check_coverage('中文\n\t', [cjk.OUTPUT])


if __name__ == '__main__':
    unittest.main()
