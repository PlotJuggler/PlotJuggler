#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Validate the production PJ4 Chinese catalog and its lupdate source coverage.

No build tree or third-party Python package is required. Use the same Qt lupdate
as the application build, for example:
  python3 tests/check_translations_4x.py --lupdate .qt/6.12.0/gcc_64/bin/lupdate

--update-ts refreshes the catalog before validation. Exit status: 1 when a catalog
entry would break at runtime (placeholders, markup, plural forms); 77 when the
catalog only lags the sources (unfinished, missing or stale entries), which Qt
shows as English text; 0 when complete. CTest reports 77 as a skip, so a UI
change never needs a translation to pass CI. Coverage means Qt-extractable
messages, not every string literal or plugin UI.
"""
from __future__ import annotations

import argparse
from collections import Counter
from html.parser import HTMLParser
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
CATALOG = ROOT / 'pj_app/translations/plotjuggler4_zh_CN.ts'
EXCLUDED_PARTS = {'tests', 'tools', 'demo', 'demos', 'wasm_probe', 'docs', 'translations'}
SOURCE_SUFFIXES = {'.cpp', '.h', '.hpp', '.ui'}
PLACEHOLDERS = re.compile(r'%(?:L?[1-9][0-9]?|L?n)')
VOID_TAGS = {'area', 'base', 'br', 'col', 'embed', 'hr', 'img', 'input', 'link', 'meta', 'param', 'source', 'track', 'wbr'}
RICH_TAGS = {'a', 'b', 'blockquote', 'body', 'code', 'dd', 'div', 'dl', 'dt', 'em', 'font', 'h1', 'h2', 'h3',
             'h4', 'h5', 'h6', 'head', 'html', 'i', 'li', 'ol', 'p', 'pre', 'small', 'span', 'strong', 'sub',
             'sup', 'table', 'tbody', 'td', 'th', 'thead', 'tr', 'u', 'ul'} | VOID_TAGS
MessageKey = tuple[str, str, str, bool]
EXIT_INCOMPLETE = 77


def production_sources() -> tuple[list[Path], list[Path]]:
    """Mirror the module boundary without pulling demos or generated trees in."""
    sources, includes = [], set()
    for module in sorted(ROOT.glob('pj_*')):
        if not module.is_dir():
            continue
        for current, directories, files in os.walk(module):
            directories[:] = sorted(d for d in directories if not d.startswith('.') and d not in EXCLUDED_PARTS
                                    and not d.startswith('build'))
            directory = Path(current)
            if directory.name in {'src', 'include'}:
                includes.add(directory)
            sources.extend(directory / name for name in sorted(files) if Path(name).suffix in SOURCE_SUFFIXES)
    return sorted(sources), sorted(includes)


def find_lupdate(explicit: str | None) -> str:
    """Prefer the pinned Qt toolchain; never silently skip source verification."""
    if explicit:
        executable = shutil.which(explicit) or str(Path(explicit).resolve())
        if not Path(executable).is_file():
            raise ValueError(f'lupdate not found: {explicit}')
        return executable
    candidates = []
    if os.environ.get('LUPDATE'):
        candidates.append(os.environ['LUPDATE'])
    for prefix_name in ('QT_ROOT', 'QTDIR'):
        if os.environ.get(prefix_name):
            candidates.append(str(Path(os.environ[prefix_name]) / 'bin/lupdate'))
    versions = ROOT / 'versions.env'
    match = re.search(r'^PJ_QT_VERSION=["\']?([^\s"\']+)', versions.read_text(encoding='utf-8'), re.MULTILINE)
    if match:
        candidates.extend(str(p) for p in sorted((ROOT / '.qt' / match.group(1)).glob('*/bin/lupdate')))
    candidates.extend(('lupdate6', 'lupdate-qt6', 'pyside6-lupdate', 'lupdate'))
    for candidate in candidates:
        executable = shutil.which(candidate)
        if executable:
            return executable
    raise ValueError('Qt lupdate is required for source coverage; pass --lupdate /path/to/lupdate')


def extract(lupdate: str, output: Path, scratch: Path) -> None:
    """Use Qt's C++/UI parser with include roots so QObject contexts stay exact."""
    sources, includes = production_sources()
    response_file = scratch / 'sources.list'
    # lupdate response files take one source path per line, without shell quotes.
    # Repository-relative paths also keep a checkout's spaces out of the list.
    response_file.write_text('\n'.join(str(p.relative_to(ROOT)) for p in sources) + '\n', encoding='utf-8')
    command = [lupdate, '@' + str(response_file), *('-I' + str(p) for p in includes), '-no-obsolete',
               '-locations', 'relative', '-source-language', 'en_US', '-target-language', 'zh_CN',
               '-disable-heuristic', 'sametext', '-disable-heuristic', 'similartext',
               '-ts', str(output)]
    result = subprocess.run(command, cwd=ROOT, text=True, encoding='utf-8', capture_output=True, check=False)
    if result.returncode:
        raise ValueError(f'lupdate failed ({result.returncode}):\n{result.stdout}{result.stderr}')
    if result.stderr.strip():
        # A missing class or dynamic tr() warning can mean a runtime-context mismatch.
        raise ValueError(f'lupdate reported extraction warnings:\n{result.stderr.strip()}')


def catalog_messages(root: ET.Element) -> dict[MessageKey, ET.Element]:
    """Index by Qt's runtime lookup key and reject ambiguous duplicate entries."""
    result = {}
    for context in root.findall('context'):
        name = context.findtext('name', '')
        for message in context.findall('message'):
            translation = message.find('translation')
            if translation is not None and translation.get('type') in {'obsolete', 'vanished'}:
                continue
            key = (name, message.findtext('source', ''), message.findtext('comment', ''), message.get('numerus') == 'yes')
            if key in result:
                raise ValueError(f'duplicate active entry: {name}: {key[1]!r}')
            result[key] = message
    return result


class RichTextShape(HTMLParser):
    """Compare tags/attributes and check nesting only for actual Qt rich text.

    Some diagnostic messages mention literal XML tokens such as <root> without a
    closing tag. Preserve those tokens, but do not mistake them for broken HTML.
    """
    def __init__(self) -> None:
        super().__init__(convert_charrefs=False)
        self.tokens: Counter = Counter()
        self.stack: list[str] = []
        self.errors: list[str] = []

    def handle_starttag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        self.tokens[('start', tag, tuple(sorted(attrs)))] += 1
        if tag in RICH_TAGS and tag not in VOID_TAGS:
            self.stack.append(tag)

    def handle_startendtag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        self.tokens[('start', tag, tuple(sorted(attrs)))] += 1
        if tag not in VOID_TAGS:
            self.tokens[('end', tag)] += 1

    def handle_endtag(self, tag: str) -> None:
        self.tokens[('end', tag)] += 1
        if tag in RICH_TAGS and tag not in VOID_TAGS:
            if not self.stack or self.stack[-1] != tag:
                self.errors.append(f'unmatched closing tag </{tag}>')
            else:
                self.stack.pop()


def rich_shape(text: str) -> RichTextShape:
    parser = RichTextShape()
    parser.feed(text)
    parser.close()
    return parser


def check_translation(source: str, translated: str) -> list[str]:
    """Protect substitutions, links, attributes, and rich-text structure."""
    errors = []
    if not translated.strip():
        return ['empty translation']
    expected, actual = Counter(PLACEHOLDERS.findall(source)), Counter(PLACEHOLDERS.findall(translated))
    if expected != actual:
        errors.append(f'placeholder mismatch: {dict(expected)} != {dict(actual)}')
    source_shape, translated_shape = rich_shape(source), rich_shape(translated)
    if source_shape.tokens != translated_shape.tokens:
        errors.append('rich-text/XML tag or attribute mismatch')
    # Allow intentional source fragments, but never introduce broken markup.
    if not source_shape.errors and not source_shape.stack:
        if translated_shape.errors or translated_shape.stack:
            errors.append('unbalanced translated rich text')
    return errors


def validate(root: ET.Element) -> tuple[dict[MessageKey, ET.Element], list[str], list[str], dict[str, int]]:
    """Return (messages, errors, warnings, stats): errors break at runtime, unfinished entries only warn."""
    errors, warnings = [], []
    if root.tag != 'TS' or root.get('language') != 'zh_CN' or root.get('sourcelanguage') != 'en_US':
        errors.append('catalog must be a TS file with language=zh_CN and sourcelanguage=en_US')
    messages = catalog_messages(root)
    stats = {'active': len(messages), 'translated': 0, 'unfinished': 0, 'numerus': 0, 'identity': 0}
    for key, message in messages.items():
        context, source, _comment, numerus = key
        translation = message.find('translation')
        label = f'{context}: {source!r}'
        if translation is None or translation.get('type') == 'unfinished':
            stats['unfinished'] += 1
            warnings.append(f'{label}: unfinished translation')
            continue
        if not context or not source:
            errors.append(f'{label}: empty context or source')
        forms = translation.findall('numerusform')
        if numerus:
            stats['numerus'] += 1
            if len(forms) != 1:
                errors.append(f'{label}: zh_CN requires exactly one numerus form')
            texts = [''.join(form.itertext()) for form in forms]
        else:
            if forms:
                errors.append(f'{label}: unexpected numerus forms')
            texts = [''.join(translation.itertext())]
        for translated in texts:
            errors.extend(f'{label}: {error}' for error in check_translation(source, translated))
        stats['translated'] += bool(texts) and all(text.strip() for text in texts)
        stats['identity'] += texts == [source]
    return messages, errors, warnings, stats


def compare_sources(messages: dict[MessageKey, ET.Element], expected: dict[MessageKey, ET.Element]) -> tuple[set[MessageKey], set[MessageKey]]:
    """Missing and stale keys both matter: translated text in a wrong context cannot load."""
    return set(expected) - set(messages), set(messages) - set(expected)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--lupdate', help='Qt Linguist lupdate executable (defaults to pinned Qt, then PATH)')
    parser.add_argument('--update-ts', action='store_true', help='refresh the shipped TS before checking it')
    parser.add_argument('--catalog-only', action='store_true',
                        help='check the TS only, explicitly skipping Qt source coverage (no Qt tools required)')
    args = parser.parse_args()
    if args.catalog_only and args.update_ts:
        parser.error('--catalog-only cannot be combined with --update-ts')
    try:
        lupdate = None if args.catalog_only else find_lupdate(args.lupdate)
        with tempfile.TemporaryDirectory(prefix='pj4-i18n-') as directory:
            scratch = Path(directory)
            if args.update_ts:
                extract(lupdate, CATALOG, scratch)
            root = ET.parse(CATALOG).getroot()
            messages, errors, warnings, stats = validate(root)
            expected, missing, stale = {}, set(), set()
            if not args.catalog_only:
                extracted = scratch / 'source_zh_CN.ts'
                try:
                    extract(lupdate, extracted, scratch)
                    expected = catalog_messages(ET.parse(extracted).getroot())
                except (OSError, ValueError, ET.ParseError) as error:
                    # A Qt parser quirk in someone's new source file must not block their change.
                    warnings.append(f'source coverage not checked: {error}')
                missing, stale = compare_sources(messages, expected) if expected else (set(), set())
                warnings.extend(f'missing source entry: {key[0]}: {key[1]!r}' for key in sorted(missing))
                warnings.extend(f'stale active entry: {key[0]}: {key[1]!r}' for key in sorted(stale))
        print(f"PJ4 zh_CN: {stats['translated']}/{stats['active']} active messages translated; "
              f"{stats['unfinished']} unfinished; {stats['numerus']} numerus; "
              f"{stats['identity']} unchanged identifiers/format strings.")
        if args.catalog_only:
            print('Source coverage: NOT CHECKED (--catalog-only; run with Qt lupdate for full validation).')
        elif expected:
            print(f'Source coverage: {len(expected)} Qt-extractable messages, {len(missing)} missing, {len(stale)} stale.')
        if errors:
            print('\n'.join(errors), file=sys.stderr)
            return 1
        print('Placeholders, plural forms, and rich-text tags/attributes: OK')
        if warnings:
            print('WARNING: the catalog lags the sources; these strings show in English until the next refresh '
                  '(--update-ts, then translate):', file=sys.stderr)
            print('\n'.join(warnings), file=sys.stderr)
            return EXIT_INCOMPLETE
        return 0
    except (OSError, ValueError, ET.ParseError) as error:
        print(f'translation check failed: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
