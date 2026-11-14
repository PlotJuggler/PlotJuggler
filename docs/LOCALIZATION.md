# Interface translations

The UI uses Qt translation catalogs. A fresh profile stays in English whatever
the operating-system locale. Preferences → Appearance offers English and
Simplified Chinese and Traditional Chinese (Taiwan); OK applies the choice on the next launch, Cancel leaves it.
`plotjuggler4 --language zh_CN|zh_TW|en_US` overrides only the current session and
warns on any other value. An unsupported stored language (one a newer version
saved) runs in English and is never overwritten by an unrelated OK. The
interface language never changes `QLocale`, data parsing, numeric formatting,
serialized IDs, plugin protocols or user-authored text.

## Catalog and build

- `pj_app/src/AppLanguage.{h,cpp}` owns the startup translator for the UI's lifetime.
- `pj_app/cmake/PjAppLanguage.cmake` compiles `pj_app/translations/plotjuggler4_zh_CN.ts` and `plotjuggler4_zh_TW.ts`,
  merges Qt's own `qtbase_zh_CN`/`qtbase_zh_TW` catalogs (standard buttons, dialogs) and embeds the
  result as a resource. Configuration fails if the Qt kit lacks either `qtbase_zh_CN.qm` or `qtbase_zh_TW.qm`
  (relative, absolute and split-prefix translation directories are supported:
  Nix's `qtbase` points into `qttranslations`). The path test `QtTranslationPaths` covers the layouts.
- `AppLanguage.cpp` extracts the `QFileDialog`/`QAbstractFileIconProvider` keys the
  pinned Qt catalog lacks into the application catalog; drop one when Qt carries it.
- Plugin-supplied labels, registry descriptions and artwork are not covered.

## Keeping the catalog current

A UI change never needs a translation to pass CI. Untranslated or new strings
show in English until the next refresh.

```sh
python3 tests/check_translations_4x.py --lupdate .qt/<version>/gcc_64/bin/lupdate             # check
python3 tests/check_translations_4x.py --lupdate .qt/<version>/gcc_64/bin/lupdate --update-ts # refresh
```

Then translate the new unfinished entries in Qt Linguist, keeping placeholders,
markup, mnemonics and technical identifiers. `pj_app_lupdate` is the same refresh
as a build target; ordinary builds never rewrite the catalog.

The checker exits 1 only for entries that break at runtime (placeholders,
rich-text markup, plural forms, empty or duplicate entries). A catalog that lags
the sources (missing, stale or unfinished entries, or an extraction warning)
exits 77, which CTest reports as a skipped `TranslationCatalog4x`.
`TranslationValidator4x` runs the checker's own unit tests. Extraction covers
production `.cpp/.h/.hpp/.ui` files under `pj_*`, excluding tests, tools, demos
and the WebAssembly probes.

A context must be visible to lupdate: call `tr()` on a class with `Q_OBJECT` or
`Q_DECLARE_TR_FUNCTIONS` (see `SceneText` in `pj_scene3D/widgets/rhi/rhi_scene_pass.h`),
never through a free helper function. Mark object names and stylesheets in
`.ui` files `notr="true"`. Qt 6.12's lupdate crashes on a function-local
`using` of a class name inside a function that calls `tr()`; qualify the names instead.

## Browser build

The browser has no system font fallback, so the WebAssembly build embeds
`resources/fonts/NotoSansSC/NotoSansSC-Subset.otf` (OFL-1.1) as a fallback family
after Noto Sans. It holds every character of both catalogs plus the 3,755 common
GB2312 level-1 hanzi, so Chinese series and file names render too. After a
catalog refresh that adds new characters, regenerate it with
`scripts/make_cjk_font_subset.py` (needs fontTools).

## Terminology

Use 话题 for data topics and 主题 for the interface theme. “刷新派生标记” re-runs
marker rules; source promotion means conversion to a file-backed data source.
Use 数据流 for a stream, 流式数据源 for a streaming source and 流数据插件 for its
plugin. Spell out 二维/三维 views, keep canonical type names such as
`SceneEntities`, and use 网络托管的布局 for a web-hosted layout. Chinese
quotation marks belong in visible prose, never in HTML attributes, identifiers,
placeholders or serialized keys.

Traditional Chinese is seeded from the localized stable branch for matching source keys.
Use `--catalog pj_app/translations/plotjuggler4_zh_TW.ts` with the checker to validate
or refresh it separately; the CMake refresh target updates both. Locale selection
changes presentation only; the color-map tokens stored in layouts remain English.

Desktop startup exports the canonical selected UI locale in
`PLOTJUGGLER_UI_LANGUAGE` before loading plugin libraries. This is a generic
Qt-free plugin protocol; unsupported plugins remain in English. Plugin-owned
translations are maintained separately, not in a host plugin-ID lookup table.
Marketplace names and descriptions are outside this change.
