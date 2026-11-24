// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "BrowserPersistence.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDomDocument>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLoggingCategory>
#include <QMap>
#include <QMetaType>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <algorithm>
#include <array>
#include <chrono>
#include <utility>

#include "LoadInput.h"
#include "SecretKeys.h"
#include "pj_runtime/PluginSettingsKeys.h"

#if defined(PJ_TARGET_WASM) && defined(PJ_WASM_ENABLE_INGRESS_PROBE)
#include <emscripten/emscripten.h>

#include "pj_runtime/QSettingsBackend.h"
#endif

using namespace Qt::StringLiterals;

namespace PJ {
namespace {

#ifdef PJ_TARGET_WASM
Q_LOGGING_CATEGORY(lcBrowserPersistence, "pj.app.browserpersistence")
#endif

#if defined(PJ_TARGET_WASM) && defined(PJ_WASM_ENABLE_INGRESS_PROBE)
// clang-format off
EM_JS(void, pj_wasm_install_persistence_test_probes, (), {
  globalThis.pjWasmSeedPersistenceProbe = () => Module._pj_wasm_test_seed_browser_persistence();
  globalThis.pjWasmReportPersistenceProbe = () => Module._pj_wasm_test_report_browser_persistence();
  globalThis.pjWasmClearPersistenceProbe = () => Module._pj_wasm_test_clear_browser_persistence();
});
// clang-format on
#endif

[[maybe_unused]] constexpr auto kSessionSettingsRoot = "/tmp/plotjuggler4-settings";
[[maybe_unused]] constexpr auto kPreferencesKey = "BrowserPersistence/preferences_v1";
[[maybe_unused]] constexpr auto kRecentLayoutsKey = "BrowserPersistence/recent_generic_layouts_v1";
[[maybe_unused]] constexpr auto kPluginSettingsKey = "BrowserPersistence/plugin_settings_v1";
constexpr int kPreferencesVersion = 1;
constexpr int kRecentLayoutsVersion = 1;
constexpr int kPluginSettingsVersion = 1;
constexpr int kMaxRecentLayouts = 5;
constexpr qsizetype kMaxLayoutBytes = 256 * 1024;
constexpr qsizetype kMaxTotalLayoutBytes = 1024 * 1024;
// Envelope caps, on write and on read: above what their bounded contents encode to (base64 grows a recipe by 4/3),
// and together far below the few MiB a browser grants an origin's localStorage.
constexpr qsizetype kMaxPreferencesBytes = 64 * 1024;
constexpr qsizetype kMaxRecentLayoutsBytes = 2 * kMaxTotalLayoutBytes;
constexpr qsizetype kMaxGeometryBytes = 4 * 1024;
constexpr qsizetype kMaxIdentifierLength = 256;
// Upper bound on how long a preference change stays page-lifetime only; a reload sooner loses it.
[[maybe_unused]] constexpr std::chrono::milliseconds kPreferenceSyncInterval{1000};

enum class ValueKind {
  kBool,
  kInt,
  kShortIdentifier,
  kGeometry,
  kTheme,
  kSplashMode,
};

struct PreferenceRule {
  const char* key;
  ValueKind kind;
  int minimum = 0;
  int maximum = 0;
};

// Exact keys only. PluginConfig/, DialogGeometry/, pj.settings.v1 plugin keys and filesystem recents must never
// be added: they carry paths, credentials or plugin state.
constexpr std::array kPreferenceRules{
    PreferenceRule{"StyleSheet::theme", ValueKind::kTheme},
    PreferenceRule{"Preferences::precision", ValueKind::kInt, 1, 6},
    PreferenceRule{"Preferences::curve_color_global", ValueKind::kBool},
    PreferenceRule{"Preferences::check_updates_on_startup", ValueKind::kBool},
    PreferenceRule{"Preferences::splash_mode", ValueKind::kSplashMode},
    PreferenceRule{"Preferences::auto_zoom_plots", ValueKind::kBool},
    PreferenceRule{"Preferences::dialog_geometry", ValueKind::kGeometry},
    PreferenceRule{"ui/icon_size", ValueKind::kInt, 12, 48},
    PreferenceRule{"ui/icon_padding", ValueKind::kInt, 0, 32},
    PreferenceRule{"ui/layout_padding", ValueKind::kInt, 0, 16},
    PreferenceRule{"ui/layout_spacing", ValueKind::kInt, 0, 16},
    PreferenceRule{"MainWindow.buttonShowpoint", ValueKind::kBool},
    PreferenceRule{"MainWindow.buttonActivateGrid", ValueKind::kBool},
    PreferenceRule{"MainWindow.buttonDots", ValueKind::kBool},
    PreferenceRule{"MainWindow.buttonRatio", ValueKind::kBool},
    PreferenceRule{"MainWindow.buttonLink", ValueKind::kBool},
    PreferenceRule{"MainWindow.useTimeOffset", ValueKind::kBool},
    PreferenceRule{"MainWindow.dateTimeDisplay", ValueKind::kBool},
    PreferenceRule{"Preferences::clock_utc", ValueKind::kBool},
    PreferenceRule{"MainWindow.timeTrackerSetting", ValueKind::kInt, 0, 2},
    PreferenceRule{"MainWindow.legendStatus", ValueKind::kInt, 0, 4},
    PreferenceRule{"MainWindow.panelBottomExpandedHeight", ValueKind::kInt, 0, 16384},
    PreferenceRule{"MainWindow.streamingBufferValue", ValueKind::kInt, 1, 999},
    PreferenceRule{"MainWindow.streamingSource", ValueKind::kShortIdentifier},
    PreferenceRule{"MainWindow.cloudSource", ValueKind::kShortIdentifier},
    PreferenceRule{"CurveListPanel/show_topics", ValueKind::kBool},
    PreferenceRule{"CurveListPanel/show_values", ValueKind::kBool},
    // VisualizationPlaceholderWidget's idle GIF schedule: an ISO date and a file-name stem, counted across visits.
    PreferenceRule{"EasterEgg/next_date", ValueKind::kShortIdentifier},
    PreferenceRule{"EasterEgg/character", ValueKind::kShortIdentifier},
};

const PreferenceRule* preferenceRule(const QString& key) {
  for (const PreferenceRule& rule : kPreferenceRules) {
    if (key == QLatin1String(rule.key)) {
      return &rule;
    }
  }
  return nullptr;
}

[[maybe_unused]] std::optional<QVariant> sanitizeLegacyPreference(const QString& key, const QVariant& value) {
  if (const std::optional<QVariant> exact = BrowserPersistence::sanitizePreference(key, value); exact.has_value()) {
    return exact;
  }
  const PreferenceRule* rule = preferenceRule(key);
  if (rule == nullptr || value.metaType().id() != QMetaType::QString) {
    return std::nullopt;
  }
  const QString text = value.toString();
  if (rule->kind == ValueKind::kBool) {
    if (text == "true"_L1) {
      return true;
    }
    if (text == "false"_L1) {
      return false;
    }
  } else if (rule->kind == ValueKind::kInt) {
    bool ok = false;
    const int integer = text.toInt(&ok);
    if (ok && text == QString::number(integer)) {
      return BrowserPersistence::sanitizePreference(key, integer);
    }
  }
  return std::nullopt;
}

bool exactBool(const QVariant& value, bool& result) {
  if (value.metaType().id() != QMetaType::Bool) {
    return false;
  }
  result = value.toBool();
  return true;
}

bool exactInt(const QVariant& value, int minimum, int maximum, int& result) {
  const int type = value.metaType().id();
  if (type != QMetaType::Int && type != QMetaType::UInt && type != QMetaType::LongLong &&
      type != QMetaType::ULongLong) {
    return false;
  }
  bool ok = false;
  const qlonglong converted = value.toLongLong(&ok);
  if (!ok || converted < minimum || converted > maximum) {
    return false;
  }
  result = static_cast<int>(converted);
  return true;
}

// An upload identity or a MEMFS staging path: meaningless on the next load.
bool namesAnUpload(const QString& value) {
  return value.contains(QLatin1StringView(kBrowserUploadScheme), Qt::CaseInsensitive) ||
         value.contains(u"/pj_uploads"_s);
}

bool isControlCharacter(QChar character) {
  return character.unicode() < 0x20 || character == QChar(0x7f);
}

bool hasControlCharacter(const QString& value) {
  return std::any_of(value.cbegin(), value.cend(), isControlCharacter);
}

// A control character or a lone surrogate (which no UTF-8 encoding round-trips).
bool hasUnsafeCharacter(const QString& value) {
  return hasControlCharacter(value) || !QStringView(value).isValidUtf16();
}

// URL user info, whether the text is one URL (padded or not) or holds one anywhere, '\' read as '/'.
bool carriesUrlUserInfo(const QString& text) {
  static const QRegularExpression kAuthorityWithUserInfo(uR"(//[^/?#\s]*@)"_s);
  QString slashed = text;
  slashed.replace(u'\\', u'/');
  return !QUrl(text.trimmed()).userInfo().isEmpty() || slashed.contains(kAuthorityWithUserInfo);
}

// The three durable envelopes share one shape, a QByteArray holding {"version": <n>, "<field>": <payload>}, because
// the WebLocalStorageFormat codec loses scalar QVariant types.
struct Envelope {
  BrowserPersistence::EnvelopeStatus status = BrowserPersistence::EnvelopeStatus::kAbsent;
  QJsonValue payload;
};

Envelope decodeEnvelope(
    const QVariant& stored, int version, QLatin1StringView field, QJsonValue::Type payload_type, qsizetype max_bytes) {
  using Status = BrowserPersistence::EnvelopeStatus;
  if (!stored.isValid()) {
    return {};
  }
  if (stored.metaType().id() != QMetaType::QByteArray || stored.toByteArray().size() > max_bytes) {
    return {.status = Status::kRejected, .payload = {}};
  }
  const QJsonDocument document = QJsonDocument::fromJson(stored.toByteArray());
  const QJsonObject root = document.object();
  const QJsonValue payload = root.value(field);
  if (!document.isObject() || root.value(u"version"_s).toInt(-1) != version || payload.type() != payload_type) {
    return {.status = Status::kRejected, .payload = {}};
  }
  return {.status = Status::kValid, .payload = payload};
}

// Empty for an empty payload: writeOrRemove then removes the key.
QByteArray encodeEnvelope(int version, QLatin1StringView field, const QJsonValue& payload) {
  if ((payload.isObject() && payload.toObject().isEmpty()) || (payload.isArray() && payload.toArray().isEmpty())) {
    return {};
  }
  return QJsonDocument(QJsonObject{{u"version"_s, version}, {field, payload}}).toJson(QJsonDocument::Compact);
}

#ifdef PJ_TARGET_WASM
// Writes `envelope` under `key`, or removes the key when the envelope is empty. One above `max_bytes` is not written,
// since the next load would reject it, and since Qt 6.12's WebLocalStorageFormat (qsettings_wasm.cpp) calls
// localStorage.setItem with no try/catch, so a quota error there would not come back as a QSettings status.
void writeOrRemove(QSettings& durable, const char* key, const QByteArray& envelope, qsizetype max_bytes) {
  if (envelope.isEmpty()) {
    durable.remove(QLatin1String(key));
  } else if (envelope.size() > max_bytes) {
    qCWarning(lcBrowserPersistence) << "Not storing" << key << "above its size cap:" << envelope.size() << "bytes";
  } else {
    durable.setValue(QLatin1String(key), envelope);
  }
}
#endif

bool safeShortIdentifier(const QString& value) {
  return value.size() <= kMaxIdentifierLength && !value.contains(u'/') && !value.contains(u'\\') &&
         !namesAnUpload(value) && !hasControlCharacter(value);
}

// The candidates sanitizeDurablePluginSetting accepts, taken in key order while within the caps, so writing and
// reading apply one rule and a written envelope always reads back whole. The byte budget bounds each entry's encoded
// size from above: a UTF-16 unit encodes to at most 3 UTF-8 bytes ('"' and '\\' escape to 2; control characters are
// refused), plus quotes, colon and comma.
QHash<QString, QString> keepDurablePluginSettings(const QMap<QString, QVariant>& candidates) {
  constexpr qsizetype kEnvelopeOverhead = 64;
  QHash<QString, QString> kept;
  QHash<QString, int> per_plugin;
  qsizetype budget = BrowserPersistence::kMaxPluginSettingsBytes - kEnvelopeOverhead;
  for (auto it = candidates.cbegin(); it != candidates.cend(); ++it) {
    const auto setting = BrowserPersistence::sanitizeDurablePluginSetting(it.key(), it.value());
    if (!setting.has_value()) {
      continue;
    }
    const qsizetype cost = 3 * (it.key().size() + setting->value.size()) + 6;
    const auto count = per_plugin.constFind(setting->plugin_id);
    const bool new_plugin = count == per_plugin.cend();
    if (cost > budget || (new_plugin && per_plugin.size() >= BrowserPersistence::kMaxDurablePluginIds) ||
        (!new_plugin && *count >= BrowserPersistence::kMaxDurablePluginKeysPerPlugin)) {
      continue;
    }
    ++per_plugin[setting->plugin_id];
    budget -= cost;
    kept.insert(it.key(), setting->value);
  }
  return kept;
}

QString recipeDisplayName(const QString& browser_name) {
  QString name = browser_name.trimmed();
  name.replace(u'\\', u'/');
  name = name.section(u'/', -1);
  name.removeIf(isControlCharacter);
  if (name.isEmpty()) {
    name = u"Layout"_s;
  }
  return name.left(128);
}

QString recipeId(const QString& name) {
  return QString::fromLatin1(QCryptographicHash::hash(name.toUtf8(), QCryptographicHash::Sha256).toHex().left(24));
}

bool decodeRecipeObject(const QJsonValue& value, BrowserPersistence::LayoutRecipe& recipe) {
  if (!value.isObject()) {
    return false;
  }
  const QJsonObject object = value.toObject();
  const QString id = object.value(u"id"_s).toString();
  const QString name = object.value(u"name"_s).toString();
  const QString encoded_xml = object.value(u"xml"_s).toString();
  if (id.size() != 24 || name.isEmpty() || name.size() > 128 || recipeId(name) != id || encoded_xml.isEmpty()) {
    return false;
  }
  const QByteArray xml = QByteArray::fromBase64(encoded_xml.toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
  if (!BrowserPersistence::isSafeGenericLayoutRecipe(xml)) {
    return false;
  }
  recipe = {.id = id, .name = name, .xml = xml};
  return true;
}

[[maybe_unused]] QList<BrowserPersistence::LayoutRecipe> decodeRecipes(const QJsonArray& items) {
  QList<BrowserPersistence::LayoutRecipe> recipes;
  QSet<QString> seen_ids;
  qsizetype total_bytes = 0;
  for (const QJsonValue& value : items) {
    BrowserPersistence::LayoutRecipe recipe;
    if (!decodeRecipeObject(value, recipe) || seen_ids.contains(recipe.id) || recipes.size() >= kMaxRecentLayouts ||
        total_bytes + recipe.xml.size() > kMaxTotalLayoutBytes) {
      continue;
    }
    seen_ids.insert(recipe.id);
    total_bytes += recipe.xml.size();
    recipes.push_back(std::move(recipe));
  }
  return recipes;
}

[[maybe_unused]] QByteArray encodeRecipes(const QList<BrowserPersistence::LayoutRecipe>& recipes) {
  QJsonArray items;
  for (const BrowserPersistence::LayoutRecipe& recipe : recipes) {
    items.push_back(
        QJsonObject{
            {u"id"_s, recipe.id},
            {u"name"_s, recipe.name},
            {u"xml"_s, QString::fromLatin1(recipe.xml.toBase64())},
        });
  }
  return encodeEnvelope(kRecentLayoutsVersion, "items"_L1, items);
}

QHash<QString, QVariant> decodePreferenceValues(const QJsonObject& values) {
  QHash<QString, QVariant> preferences;
  for (const PreferenceRule& rule : kPreferenceRules) {
    const QString key = QLatin1String(rule.key);
    const QJsonValue encoded = values.value(key);
    if (encoded.isUndefined() || encoded.isNull()) {
      continue;
    }
    QVariant candidate;
    switch (rule.kind) {
      case ValueKind::kBool:
        if (encoded.isBool()) {
          candidate = encoded.toBool();
        }
        break;
      case ValueKind::kInt:
        if (encoded.isDouble()) {
          const double number = encoded.toDouble();
          if (number >= rule.minimum && number <= rule.maximum) {
            const int integer = static_cast<int>(number);
            if (number == integer) {
              candidate = integer;
            }
          }
        }
        break;
      case ValueKind::kShortIdentifier:
      case ValueKind::kTheme:
      case ValueKind::kSplashMode:
        if (encoded.isString()) {
          candidate = encoded.toString();
        }
        break;
      case ValueKind::kGeometry:
        if (encoded.isString()) {
          candidate = QByteArray::fromBase64(encoded.toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
        }
        break;
    }
    if (const std::optional<QVariant> sanitized = BrowserPersistence::sanitizePreference(key, candidate);
        sanitized.has_value()) {
      preferences.insert(key, *sanitized);
    }
  }
  return preferences;
}

[[maybe_unused]] QByteArray encodePreferences(const QHash<QString, QVariant>& preferences) {
  QJsonObject values;
  for (const PreferenceRule& rule : kPreferenceRules) {
    const QString key = QLatin1String(rule.key);
    const auto found = preferences.constFind(key);
    if (found == preferences.cend()) {
      continue;
    }
    if (rule.kind == ValueKind::kGeometry) {
      values.insert(key, QString::fromLatin1(found->toByteArray().toBase64()));
    } else {
      values.insert(key, QJsonValue::fromVariant(*found));
    }
  }
  return encodeEnvelope(kPreferencesVersion, "values"_L1, values);
}

BrowserPersistence* g_instance = nullptr;

}  // namespace

BrowserPersistence::Decoded<QHash<QString, QVariant>> BrowserPersistence::decodePreferences(const QVariant& stored) {
  const Envelope envelope =
      decodeEnvelope(stored, kPreferencesVersion, "values"_L1, QJsonValue::Object, kMaxPreferencesBytes);
  if (envelope.status != EnvelopeStatus::kValid) {
    return {.status = envelope.status, .values = {}};
  }
  return {.status = EnvelopeStatus::kValid, .values = decodePreferenceValues(envelope.payload.toObject())};
}

struct BrowserPersistence::Impl {
#ifdef PJ_TARGET_WASM
  std::unique_ptr<QSettings> durable;
  QHash<QString, QVariant> last_preferences;
  QHash<QString, QString> last_plugin_settings;
#endif
  QList<LayoutRecipe> recipes;
};

BrowserPersistence::BrowserPersistence(QObject* parent) : QObject(parent), impl_(std::make_unique<Impl>()) {
  if (g_instance != nullptr) {
    qFatal("Only one BrowserPersistence instance may exist");
  }
  g_instance = this;

#ifdef PJ_TARGET_WASM
  // Moves every default QSettings caller to page-lifetime MEMFS. Runs after QApplication and its org/app
  // identity, and before the splash, Theme, WidgetTuner, MainWindow or any plugin host: those read settings.
  QDir().mkpath(QString::fromLatin1(kSessionSettingsRoot));
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, QString::fromLatin1(kSessionSettingsRoot));
  QSettings::setPath(
      QSettings::IniFormat, QSettings::SystemScope, QString::fromLatin1(kSessionSettingsRoot) + u"/system"_s);

  impl_->durable = std::make_unique<QSettings>(
      QSettings::WebLocalStorageFormat, QSettings::UserScope, QCoreApplication::organizationName(),
      QCoreApplication::applicationName());
  impl_->durable->setFallbacksEnabled(false);

  // Any key besides the envelopes (per-key values of Qt's NativeFormat backend) is migrated when allowlisted, then
  // deleted, path-bearing and plugin-owned keys included. A rejected envelope (malformed, oversized or another
  // version) is left exactly as stored and imports nothing; the next change of its contents replaces it.
  const QVariant stored_preferences = impl_->durable->value(QLatin1String(kPreferencesKey));
  Decoded<QHash<QString, QVariant>> decoded_preferences = decodePreferences(stored_preferences);
  QHash<QString, QVariant> restored_preferences = std::move(decoded_preferences.values);
  if (decoded_preferences.status == EnvelopeStatus::kRejected) {
    qCWarning(lcBrowserPersistence) << "Browser preferences could not be decoded; leaving the stored envelope intact";
  }
  for (const QString& key : impl_->durable->allKeys()) {
    if (key == QLatin1String(kPreferencesKey) || key == QLatin1String(kRecentLayoutsKey) ||
        key == QLatin1String(kPluginSettingsKey)) {
      continue;
    }
    if (!restored_preferences.contains(key)) {
      if (const std::optional<QVariant> migrated = sanitizeLegacyPreference(key, impl_->durable->value(key));
          migrated.has_value()) {
        restored_preferences.insert(key, *migrated);
      }
    }
    impl_->durable->remove(key);
  }
  if (decoded_preferences.status != EnvelopeStatus::kRejected) {
    writeOrRemove(*impl_->durable, kPreferencesKey, encodePreferences(restored_preferences), kMaxPreferencesBytes);
  }

  const Envelope recipes = decodeEnvelope(
      impl_->durable->value(QLatin1String(kRecentLayoutsKey)), kRecentLayoutsVersion, "items"_L1, QJsonValue::Array,
      kMaxRecentLayoutsBytes);
  if (recipes.status == EnvelopeStatus::kRejected) {
    qCWarning(lcBrowserPersistence)
        << "Recent browser layouts could not be decoded; leaving the stored envelope intact";
  } else {
    impl_->recipes = decodeRecipes(recipes.payload.toArray());
    writeOrRemove(*impl_->durable, kRecentLayoutsKey, encodeRecipes(impl_->recipes), kMaxRecentLayoutsBytes);
  }

  Decoded<QHash<QString, QString>> decoded_plugin_settings =
      decodePluginSettings(impl_->durable->value(QLatin1String(kPluginSettingsKey)));
  QHash<QString, QString> restored_plugin_settings = std::move(decoded_plugin_settings.values);
  if (decoded_plugin_settings.status == EnvelopeStatus::kRejected) {
    qCWarning(lcBrowserPersistence)
        << "Browser plugin settings could not be decoded; leaving the stored envelope intact";
  } else {
    writeOrRemove(
        *impl_->durable, kPluginSettingsKey, encodePluginSettings(restored_plugin_settings), kMaxPluginSettingsBytes);
  }

  QSettings session;
  session.setFallbacksEnabled(false);
  for (auto it = restored_preferences.cbegin(); it != restored_preferences.cend(); ++it) {
    session.setValue(it.key(), it.value());
  }
  for (auto it = restored_plugin_settings.cbegin(); it != restored_plugin_settings.cend(); ++it) {
    session.setValue(it.key(), it.value());
  }
  impl_->last_preferences = std::move(restored_preferences);
  impl_->last_plugin_settings = std::move(restored_plugin_settings);
  session.sync();
  impl_->durable->sync();
  if (impl_->durable->status() != QSettings::NoError) {
    qCWarning(lcBrowserPersistence)
        << "Browser preference storage is unavailable; continuing with page-lifetime settings";
  }

  // A poll, because nothing else fires in time: Qt 6.12's WASM event dispatcher never emits aboutToBlock, and a
  // closed or reloaded tab never runs aboutToQuit (pj_app TECHNICAL_NOTES "Browser preferences flush on a timer").
  auto* sync_timer = new QTimer(this);
  connect(sync_timer, &QTimer::timeout, this, &BrowserPersistence::syncPreferences);
  sync_timer->start(kPreferenceSyncInterval);
  connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this, &BrowserPersistence::syncPreferences);
#ifdef PJ_WASM_ENABLE_INGRESS_PROBE
  pj_wasm_install_persistence_test_probes();
#endif
#endif
}

BrowserPersistence::~BrowserPersistence() {
  syncPreferences();
  if (g_instance == this) {
    g_instance = nullptr;
  }
}

BrowserPersistence* BrowserPersistence::instance() {
  return g_instance;
}

QStringList BrowserPersistence::durablePreferenceKeys() {
  QStringList keys;
  keys.reserve(static_cast<qsizetype>(kPreferenceRules.size()));
  for (const PreferenceRule& rule : kPreferenceRules) {
    keys.push_back(QLatin1String(rule.key));
  }
  return keys;
}

bool BrowserPersistence::isDurablePreferenceKey(const QString& key) {
  return preferenceRule(key) != nullptr;
}

std::optional<QVariant> BrowserPersistence::sanitizePreference(const QString& key, const QVariant& value) {
  const PreferenceRule* rule = preferenceRule(key);
  if (rule == nullptr || !value.isValid()) {
    return std::nullopt;
  }
  switch (rule->kind) {
    case ValueKind::kBool: {
      bool result = false;
      return exactBool(value, result) ? std::optional<QVariant>{result} : std::nullopt;
    }
    case ValueKind::kInt: {
      int result = 0;
      return exactInt(value, rule->minimum, rule->maximum, result) ? std::optional<QVariant>{result} : std::nullopt;
    }
    case ValueKind::kShortIdentifier: {
      if (value.metaType().id() != QMetaType::QString || !safeShortIdentifier(value.toString())) {
        return std::nullopt;
      }
      return value.toString();
    }
    case ValueKind::kGeometry: {
      if (value.metaType().id() != QMetaType::QByteArray || value.toByteArray().size() > kMaxGeometryBytes) {
        return std::nullopt;
      }
      return value.toByteArray();
    }
    case ValueKind::kTheme: {
      if (value.metaType().id() != QMetaType::QString ||
          (value.toString() != "light"_L1 && value.toString() != "dark"_L1)) {
        return std::nullopt;
      }
      return value.toString();
    }
    case ValueKind::kSplashMode: {
      if (value.metaType().id() != QMetaType::QString ||
          (value.toString() != "memes"_L1 && value.toString() != "serious"_L1)) {
        return std::nullopt;
      }
      return value.toString();
    }
  }
  return std::nullopt;
}

bool BrowserPersistence::isSafeGenericLayoutRecipe(const QByteArray& xml) {
  if (xml.isEmpty() || xml.size() > kMaxLayoutBytes || xml.contains(kBrowserUploadScheme) ||
      xml.contains("/pj_uploads") || xml.contains(kStreamIdentityScheme)) {
    return false;
  }
  QDomDocument document;
  if (!document.setContent(xml)) {
    return false;
  }
  const QDomElement root = document.documentElement();
  // <materialize> (a cloud-provider source record) and a stream record are
  // source-bound by definition; in a well-formed layout they only appear under
  // their <previouslyLoaded_*> wrappers, but reject them anywhere so a
  // hand-crafted document cannot smuggle one into durable browser storage.
  if (root.tagName() != "root"_L1 || root.attribute(u"binding"_s) != "generic"_L1 ||
      !root.firstChildElement(u"previouslyLoaded_Datafiles"_s).isNull() ||
      !document.elementsByTagName(u"plugin"_s).isEmpty() || !document.elementsByTagName(u"transform"_s).isEmpty() ||
      !document.elementsByTagName(u"materialize"_s).isEmpty() ||
      !document.elementsByTagName(u"previouslyLoaded_StreamingSources"_s).isEmpty() ||
      !document.elementsByTagName(u"streamInfo"_s).isEmpty()) {
    return false;
  }
  // Scan the decoded DOM as well as the original bytes so XML entities cannot
  // disguise an upload identity, a MEMFS backing path or a stream identity.
  const auto contains_source_identity = [](const QString& value) {
    return namesAnUpload(value) || value.contains(QLatin1StringView(kStreamIdentityScheme), Qt::CaseInsensitive);
  };
  for (QDomElement element = root; !element.isNull();) {
    const QDomNamedNodeMap attributes = element.attributes();
    for (int index = 0; index < attributes.size(); ++index) {
      if (contains_source_identity(attributes.item(index).nodeValue())) {
        return false;
      }
    }
    if (contains_source_identity(element.text())) {
      return false;
    }
    QDomElement next = element.firstChildElement();
    while (next.isNull() && !element.isNull() && element != root) {
      next = element.nextSiblingElement();
      element = element.parentNode().toElement();
    }
    if (element == root && next.isNull()) {
      break;
    }
    element = next;
  }
  return true;
}

std::optional<BrowserPersistence::DurablePluginSetting> BrowserPersistence::sanitizeDurablePluginSetting(
    const QString& key, const QVariant& value) {
  // "<kPluginSettingsRoot>/<plugin>/<kDurablePluginSettingsGroup>/<name>", no segment empty.
  const QStringList segments = key.split(u'/');
  if (key.size() > kMaxIdentifierLength || segments.size() < 4 || segments[0] != kPluginSettingsRoot ||
      segments[2] != kDurablePluginSettingsGroup || segments.contains(QString()) || namesAnUpload(key) ||
      hasUnsafeCharacter(key) || isSecretKey(segments.mid(3).join(u'/').toStdString())) {
    return std::nullopt;
  }
  if (value.metaType().id() != QMetaType::QString) {
    return std::nullopt;
  }
  const QString text = value.toString();
  if (text.size() > kMaxDurablePluginValueLength || namesAnUpload(text) || hasUnsafeCharacter(text) ||
      carriesUrlUserInfo(text)) {
    return std::nullopt;
  }
  return DurablePluginSetting{.plugin_id = segments[1], .value = text};
}

QHash<QString, QString> BrowserPersistence::durablePluginSettings(QSettings& settings) {
  QMap<QString, QVariant> candidates;
  settings.beginGroup(kPluginSettingsRoot);
  for (const QString& key : settings.allKeys()) {
    candidates.insert(QString(kPluginSettingsRoot) + u'/' + key, settings.value(key));
  }
  settings.endGroup();
  return keepDurablePluginSettings(candidates);
}

QByteArray BrowserPersistence::encodePluginSettings(const QHash<QString, QString>& settings) {
  QJsonObject values;
  for (auto it = settings.cbegin(); it != settings.cend(); ++it) {
    values.insert(it.key(), it.value());
  }
  return encodeEnvelope(kPluginSettingsVersion, "values"_L1, values);
}

BrowserPersistence::Decoded<QHash<QString, QString>> BrowserPersistence::decodePluginSettings(const QVariant& stored) {
  const Envelope envelope =
      decodeEnvelope(stored, kPluginSettingsVersion, "values"_L1, QJsonValue::Object, kMaxPluginSettingsBytes);
  if (envelope.status != EnvelopeStatus::kValid) {
    return {.status = envelope.status, .values = {}};
  }
  // toVariantMap() keeps each value's JSON type; the sanitizer refuses every non-string.
  const QVariantMap values = envelope.payload.toObject().toVariantMap();
  return {.status = EnvelopeStatus::kValid, .values = keepDurablePluginSettings(values)};
}

QList<BrowserPersistence::LayoutRecipe> BrowserPersistence::recentLayouts() const {
  return impl_->recipes;
}

std::optional<BrowserPersistence::LayoutRecipe> BrowserPersistence::recentLayout(const QString& id) const {
  for (const LayoutRecipe& recipe : impl_->recipes) {
    if (recipe.id == id) {
      return recipe;
    }
  }
  return std::nullopt;
}

bool BrowserPersistence::rememberGenericLayout(const QString& browser_name, const QByteArray& xml, QString* error) {
  if (!isSafeGenericLayoutRecipe(xml)) {
    if (error != nullptr) {
      *error = tr("The layout is not a bounded, source-free browser recipe.");
    }
    return false;
  }
  const QString name = recipeDisplayName(browser_name);
  const QString id = recipeId(name);
  impl_->recipes.removeIf([&id](const LayoutRecipe& recipe) { return recipe.id == id; });
  impl_->recipes.prepend({.id = id, .name = name, .xml = xml});
  while (impl_->recipes.size() > kMaxRecentLayouts) {
    impl_->recipes.removeLast();
  }
  qsizetype total_bytes = 0;
  for (qsizetype index = 0; index < impl_->recipes.size();) {
    total_bytes += impl_->recipes[index].xml.size();
    if (total_bytes > kMaxTotalLayoutBytes) {
      impl_->recipes.removeAt(index);
    } else {
      ++index;
    }
  }
#ifdef PJ_TARGET_WASM
  writeOrRemove(*impl_->durable, kRecentLayoutsKey, encodeRecipes(impl_->recipes), kMaxRecentLayoutsBytes);
  impl_->durable->sync();
  if (impl_->durable->status() != QSettings::NoError) {
    if (error != nullptr) {
      *error = tr("Browser storage rejected the recent layout recipe; it remains available for this page only.");
    }
    qCWarning(lcBrowserPersistence) << "Could not persist recent layout recipe; keeping the bounded page-lifetime copy";
  }
#endif
  return true;
}

void BrowserPersistence::clearRecentLayouts() {
  impl_->recipes.clear();
#ifdef PJ_TARGET_WASM
  impl_->durable->remove(QLatin1String(kRecentLayoutsKey));
  impl_->durable->sync();
#endif
}

void BrowserPersistence::syncPreferences() {
#ifdef PJ_TARGET_WASM
  if (impl_->durable == nullptr) {
    return;
  }
  QSettings session;
  session.setFallbacksEnabled(false);
  QHash<QString, QVariant> current;
  for (const PreferenceRule& rule : kPreferenceRules) {
    const QString key = QLatin1String(rule.key);
    if (!session.contains(key)) {
      continue;
    }
    if (const std::optional<QVariant> sanitized = sanitizePreference(key, session.value(key)); sanitized.has_value()) {
      current.insert(key, *sanitized);
    }
  }
  QHash<QString, QString> plugin_settings = durablePluginSettings(session);
  const bool preferences_changed = current != impl_->last_preferences;
  const bool plugin_settings_changed = plugin_settings != impl_->last_plugin_settings;
  if (!preferences_changed && !plugin_settings_changed) {
    return;
  }
  // Recorded before writing, so a write that fails is not retried every interval.
  impl_->last_preferences = current;
  impl_->last_plugin_settings = plugin_settings;
  if (preferences_changed) {
    writeOrRemove(*impl_->durable, kPreferencesKey, encodePreferences(current), kMaxPreferencesBytes);
  }
  if (plugin_settings_changed) {
    writeOrRemove(*impl_->durable, kPluginSettingsKey, encodePluginSettings(plugin_settings), kMaxPluginSettingsBytes);
  }
  impl_->durable->sync();
  if (impl_->durable->status() != QSettings::NoError) {
    qCWarning(lcBrowserPersistence)
        << "Could not synchronize browser preferences; page-lifetime settings remain available";
  }
#endif
}

#if defined(PJ_TARGET_WASM) && defined(PJ_WASM_ENABLE_INGRESS_PROBE)
void BrowserPersistence::seedLegacyUnsafeValuesForTest() {
  impl_->durable->setValue(u"FileLoader/lastDir"_s, u"/pj_uploads/legacy-token"_s);
  impl_->durable->setValue(u"PluginConfig/legacy"_s, u"secret-token"_s);
  impl_->durable->sync();
}

bool BrowserPersistence::durableContainsForTest(const QString& key) const {
  return impl_->durable->contains(key);
}

qsizetype BrowserPersistence::durableKeyCountForTest() const {
  return impl_->durable->allKeys().size();
}
#endif

#if defined(PJ_TARGET_WASM) && defined(PJ_WASM_ENABLE_INGRESS_PROBE)
namespace {

QString probePluginKey(const QString& name) {
  return pluginSettingsGroup("probe-plugin") + u'/' + name;
}

extern "C" EMSCRIPTEN_KEEPALIVE void pj_wasm_test_seed_browser_persistence() {
  BrowserPersistence* persistence = BrowserPersistence::instance();
  if (persistence == nullptr) {
    qWarning("PJ_WASM_PERSISTENCE_SEED_FAILED no bridge");
    return;
  }
  QSettings session;
  session.setValue(u"StyleSheet::theme"_s, u"dark"_s);
  session.setValue(u"Preferences::precision"_s, 6);
  session.setValue(u"FileLoader/lastDir"_s, u"/pj_uploads/dead-token"_s);
  session.setValue(u"PluginConfig/secret-source"_s, u"{\"token\":\"secret-token\"}"_s);
  session.setValue(u"File/recent"_s, QStringList{u"/home/user/private.csv"_s});
  {
    // Through the plugin's own store, so a browser run exercises its writes on MEMFS.
    auto store = QSettingsBackend::forPlugin("probe-plugin");
    store->setString("durable/url", "ws://robot.local:9871");
    store->setString("token", "plugin-secret-token");
  }
  persistence->syncPreferences();

  // Keys as the NativeFormat backend leaves them; they survive this page so the next construction must scrub them.
  persistence->seedLegacyUnsafeValuesForTest();

  for (int index = 0; index < 7; ++index) {
    const QByteArray xml =
        u"<root pj4_version=\"4\" binding=\"generic\"><profile index=\"%1\"/></root>"_s.arg(index).toUtf8();
    persistence->rememberGenericLayout(u"recipe-%1.pj4.xml"_s.arg(index), xml);
  }
  qInfo("PJ_WASM_PERSISTENCE_SEEDED recipes=%lld", static_cast<long long>(persistence->recentLayouts().size()));
}

extern "C" EMSCRIPTEN_KEEPALIVE void pj_wasm_test_report_browser_persistence() {
  BrowserPersistence* persistence = BrowserPersistence::instance();
  if (persistence == nullptr) {
    qWarning("PJ_WASM_PERSISTENCE_STATE_FAILED no bridge");
    return;
  }
  const QSettings session;
  qInfo(
      "PJ_WASM_PERSISTENCE_STATE theme=%s theme_present=%d precision=%d precision_present=%d unsafe_path=%d "
      "unsafe_plugin=%d unsafe_recent=%d recipes=%lld durable_keys=%lld plugin_url=%s plugin_token_present=%d",
      qUtf8Printable(session.value(u"StyleSheet::theme"_s, u"light"_s).toString()),
      session.contains(u"StyleSheet::theme"_s) ? 1 : 0, session.value(u"Preferences::precision"_s, 3).toInt(),
      session.contains(u"Preferences::precision"_s) ? 1 : 0,
      persistence->durableContainsForTest(u"FileLoader/lastDir"_s) ? 1 : 0,
      persistence->durableContainsForTest(u"PluginConfig/legacy"_s) ? 1 : 0,
      persistence->durableContainsForTest(u"File/recent"_s) ? 1 : 0,
      static_cast<long long>(persistence->recentLayouts().size()),
      static_cast<long long>(persistence->durableKeyCountForTest()),
      qUtf8Printable(session.value(probePluginKey(u"durable/url"_s), u"(absent)"_s).toString()),
      session.contains(probePluginKey(u"token"_s)) ? 1 : 0);
}

extern "C" EMSCRIPTEN_KEEPALIVE void pj_wasm_test_clear_browser_persistence() {
  BrowserPersistence* persistence = BrowserPersistence::instance();
  if (persistence == nullptr) {
    return;
  }
  QSettings session;
  session.remove(u"StyleSheet::theme"_s);
  session.remove(u"Preferences::precision"_s);
  session.remove(probePluginKey(u"durable/url"_s));
  persistence->clearRecentLayouts();
  persistence->syncPreferences();
  qInfo("PJ_WASM_PERSISTENCE_CLEARED");
}

}  // namespace
#endif

}  // namespace PJ
