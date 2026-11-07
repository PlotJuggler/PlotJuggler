#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Focused negative cases for the catalog guard; no Qt installation required."""
import unittest
from pathlib import Path
from tempfile import TemporaryDirectory
from unittest.mock import patch
import xml.etree.ElementTree as ET

from check_translations_4x import catalog_messages, check_translation, compare_sources, production_sources, validate


class TranslationGuardTest(unittest.TestCase):
    """A broken translation must not pass merely because lrelease accepts it."""

    def catalog(self, translation: str, numerus: bool = False) -> ET.Element:
        root = ET.Element('TS', version='2.1', language='zh_CN', sourcelanguage='en_US')
        context = ET.SubElement(root, 'context')
        ET.SubElement(context, 'name').text = 'PJ::Example'
        message = ET.SubElement(context, 'message', {'numerus': 'yes'} if numerus else {})
        ET.SubElement(message, 'source').text = '%n sources' if numerus else 'Hello %1'
        message.append(ET.fromstring(translation))
        return root

    def test_production_sources_exclude_demo_and_probe_trees(self) -> None:
        with TemporaryDirectory() as directory:
            root = Path(directory)
            for subdir in ('src', 'demo', 'demos', 'wasm_probe', 'tests', 'build-fixture'):
                source = root / 'pj_example' / subdir / 'example.cpp'
                source.parent.mkdir(parents=True)
                source.write_text('void example() {}', encoding='utf-8')
            with patch('check_translations_4x.ROOT', root):
                sources, _ = production_sources()
            self.assertEqual(sources, [root / 'pj_example/src/example.cpp'])

    def test_repeated_placeholders_are_counted(self) -> None:
        self.assertTrue(check_translation('Source %1 of %2: %1', '数据源 %1 / %2'))
        self.assertFalse(check_translation('Source %1 of %2: %1', '数据源 %1 / %2：%1'))

    def test_localized_and_numerus_placeholders_stay_distinct(self) -> None:
        self.assertTrue(check_translation('%L1 / %n', '%1 / %n'))
        self.assertFalse(check_translation('%L1 / %n', '%L1 / %n'))

    def test_links_and_attributes_cannot_change(self) -> None:
        self.assertFalse(check_translation('<a href="%1">Open</a>', '<a href="%1">打开</a>'))
        self.assertTrue(check_translation('<a href="%1">Open</a>', '<a href="https://example.com">打开 %1</a>'))

    def test_void_tags_and_literal_xml_names(self) -> None:
        self.assertFalse(check_translation('Hello<br>world', '你好<br>世界'))
        self.assertFalse(check_translation('Missing <root> element', '缺少 <root> 元素'))
        self.assertTrue(check_translation('Missing <root> element', '缺少 <根> 元素'))

    def test_incorrect_nesting_fails(self) -> None:
        self.assertTrue(check_translation('<b><i>Text</i></b>', '<b><i>文字</b></i>'))

    def test_unfinished_or_empty_translation_fails(self) -> None:
        for translation in ('<translation type="unfinished">你好 %1</translation>', '<translation/>'):
            self.assertTrue(validate(self.catalog(translation))[1])

    def test_chinese_requires_one_plural_form(self) -> None:
        correct = '<translation><numerusform>%n 个数据源</numerusform></translation>'
        self.assertFalse(validate(self.catalog(correct, numerus=True))[1])
        wrong = '<translation><numerusform>%n 个</numerusform><numerusform>%n 个</numerusform></translation>'
        self.assertTrue(validate(self.catalog(wrong, numerus=True))[1])

    def test_wrong_context_is_missing_and_stale(self) -> None:
        root = self.catalog('<translation>你好 %1</translation>')
        messages = catalog_messages(root)
        root.find('context/name').text = 'PJ::Other'
        expected = catalog_messages(root)
        missing, stale = compare_sources(messages, expected)
        self.assertEqual({key[0] for key in missing}, {'PJ::Other'})
        self.assertEqual({key[0] for key in stale}, {'PJ::Example'})
        self.assertEqual(compare_sources(expected, expected), (set(), set()))

    def test_duplicate_context_key_fails(self) -> None:
        root = self.catalog('<translation>你好 %1</translation>')
        context = root.find('context')
        context.append(ET.fromstring(ET.tostring(context.find('message'))))
        with self.assertRaisesRegex(ValueError, 'duplicate active entry'):
            catalog_messages(root)


if __name__ == '__main__':
    unittest.main()
