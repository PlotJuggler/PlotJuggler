# Interface translations

The application uses Qt translation catalogs for UI text. A fresh profile stays
in English, independently of the operating-system locale. Preferences offers
English and Simplified Chinese; accepting a selection applies it on the next
launch, while Cancel leaves the saved preference unchanged.

`plotjuggler4 --language zh_CN` and `plotjuggler4 --language en_US` override only
the current session. An unsupported or invalid stored language falls back to
English source text without rewriting the setting. Interface language selection
does not change `QLocale`, data parsing, numeric formatting, serialized IDs,
plugin protocols, or user-authored text.

## Catalog and build

`pj_app/src/AppLanguage.{h,cpp}` owns the startup translator for the application's
UI lifetime. `pj_app/cmake/PjAppLanguage.cmake` compiles
`pj_app/translations/plotjuggler4_zh_CN.ts`, merges Qt's standard widget catalog,
and embeds the result as a resource. Qt LinguistTools is required at build time;
no translation download or loose runtime catalog is needed. The matching Qt
translations component must provide `qtbase_zh_CN.qm`; configuration fails
explicitly if it is missing. Standard widget/button translations come from Qt,
while the application catalog supplies supplementary file-dialog/icon-provider
keys verified against the pinned Qt catalog. Disambiguation comments are part of their keys.
Both relative and absolute Qt translation directories are supported, including
split-prefix installations. The path regression test configures and builds the
catalog under both layouts and checks the explicit missing-catalog failure.

The translation check extracts production `.cpp`, `.h`, `.hpp`, and `.ui`
messages from `pj_*` modules, excluding tests, tools, `demo`/`demos`, `wasm_probe`, docs, and generated
build trees. It checks extraction contexts, duplicates, placeholders, Chinese
plural forms, rich-text structure/attributes, missing messages, and stale
entries. Extraction coverage is not a claim that every visible string or
third-party plugin is translated. Plugin-supplied labels, registry descriptions,
and artwork are outside this contribution.

To update the source catalog intentionally, run:

```sh
python3 tests/check_translations_4x.py --lupdate /path/to/Qt/bin/lupdate --update-ts
```

Translate new unfinished entries in Qt Linguist, preserving placeholders,
markup, shortcuts, and stable technical identifiers. Ordinary builds do not
rewrite the tracked catalog. With the full build configured, the equivalent
explicit update target is `pj_app_lupdate`.

## Focused verification

Use the Qt version pinned by `versions.env`:

```sh
python3 tests/test_check_translations_4x.py
python3 tests/check_translations_4x.py --lupdate /path/to/Qt/bin/lupdate
cmake -S pj_app/tests/i18n -B build-i18n -DCMAKE_PREFIX_PATH=/path/to/Qt
cmake --build build-i18n
ctest --test-dir build-i18n --output-on-failure
```

Both the full application build and the focused project register the ten Python
validator regressions as `TranslationValidator4x`. `TranslationCatalog4x` runs
real source extraction with the pinned Qt tool and fails on any extraction
warning, missing key, or stale entry. No production source file is omitted to
work around parser failures.

The focused suite exercises the production catalog, English default and
fallback, saved settings, session overrides, translator lifetime, repeated
startup, restart-only behavior, Qt standard buttons, and numeric-locale
invariance. Full application tests additionally cover Preferences accept/cancel
and browser persistence's bounded language allowlist. The Appearance page wraps
wide rows and scrolls at the supported minimum window size so translated labels
and the restart notice remain accessible. The focused suite also
constructs real standard buttons and file-icon providers through repeated
English/Chinese sessions. Real non-native file dialogs run where their native
QtProject settings can be redirected to a temporary directory (Linux). That
case explicitly skips Windows/macOS to avoid altering shared native settings;
direct catalog lookups and the other widget checks still run there.
The focused suite is not a full application build, Windows validation, browser
validation, or visual QA. After configuring the full application, build
`pj_app`, `pj_app_gui_tests`, and `pj_app_unit_tests`, then run the relevant
integration checks:

```sh
ctest --test-dir /path/to/full-build --output-on-failure -R 'PreferencesLanguageTest|BrowserPersistencePolicy|AppLanguageTest|QtFileDialogLanguageTest|TranslationCatalog4x|TranslationValidator4x|QtTranslationPaths'
```

Check Preferences in both languages at 560×400 and 420×300 as well. At the
smaller size, all Appearance controls must remain reachable by scrolling.

## Contribution boundary and provenance

The original contribution was based on official `main-4.x` commit
`0345ec513bde7f05ad3148548ac30401f2b6f7a7`. It now integrates official
`b9117bfbc31effacacac150656383516a2430dfe`, preserving the current asynchronous
dataset merge, load diagnostics, browser replay, lazy browser file reads,
file-picker hints, clock/time-offset controls, derived-marker preferences, and
MCP controls. The integrated upstream also pins SDK 0.40; this contribution does
not change MCP access policy or start the server from language tests. Its initial language infrastructure,
translation strings, and focused tests were extracted from
[`reliable-ly0411/PlotJuggler-zh-CN` commit `0c73d762`](https://github.com/reliable-ly0411/PlotJuggler-zh-CN/commit/0c73d7620ea60f2fec171f55314c46c72820723d);
the stricter catalog validator comes from that fork's `203d29a8` snapshot.
The nine Qt file-dialog/icon-provider corrections and direct-lookup regression
cases are adapted from downstream commit `ba8155fae58f0e029468347e534336c6fe017f98`.
New catalog entries reuse matching context/source/comment/plural keys from
downstream commit `88a6025ec330c743f6ce78e1f5a46a3cb55adbc6`, then retain only
the messages extracted from this upstream candidate. English remains the
upstream default.

No fork branding, release/installer workflow, update routing, BIN loader,
bundled-plugin translation overrides, marketplace metadata translation policy,
new splash artwork, or Windows executable/console packaging changes are part
of this contribution. Those concerns must be reviewed separately. The scope is
application language selection, Qt-extractable UI strings, the Chinese catalog,
and their regression checks.

## Qt extraction compatibility

Qt 6.12's C++ extractor crashes on the function-local `using PJ::CurveTreeView`
in `derivedGlyphColor`. Fully qualified enum names preserve the compiled C++
behavior and let the complete source file remain in extraction. Explicit
`QCoreApplication::translate` calls in the preset catalog and Scene3D passes
keep the runtime lookup contexts visible to the extractor. Deferred model
error literals use `QT_TRANSLATE_NOOP` with that same runtime context.

The browser file-picker implementation uses `BrowserPickedFile` and its separate
`browser_picked_file.js` library for lazy file reads. `FileSelectionService.cpp`
remains identical to the integrated upstream; all production C++ sources remain
in Qt extraction. The catalog covers the extracted browser-read diagnostics and
MCP status, prompt, and Preferences messages. Direct runtime-lookup regressions
exercise these messages in Chinese and English without starting an MCP server.
