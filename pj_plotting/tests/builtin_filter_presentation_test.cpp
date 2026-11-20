// SPDX-License-Identifier: MPL-2.0
#include <gtest/gtest.h>

#include <QApplication>
#include <QComboBox>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QTranslator>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "pj_datastore/data_processor.hpp"
#include "pj_datastore/writer.hpp"
#include "pj_plotting/BuiltinFilterPresentation.h"
#include "pj_plotting/FilterEditorPanel.h"
#include "pj_plotting/ParameterForm.h"
#include "pj_plotting/PlotWidgetBase.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/SessionManager.h"
#include "pj_scripting/filter_catalogue.h"
#include "pj_scripting/script_engine.h"

using namespace Qt::StringLiterals;

namespace {

/// Marks every lookup in the real presentation context, independently of the
/// app catalog. Tests below exercise routing; app language tests own Chinese text.
class PresentationTranslator : public QTranslator {
 public:
  PresentationTranslator() {
    EXPECT_TRUE(qApp->installTranslator(this));
  }

  ~PresentationTranslator() override {
    qApp->removeTranslator(this);
  }

  bool isEmpty() const override {
    return false;
  }

  QString translate(const char* context, const char* source, const char*, int) const override {
    return context != nullptr && std::string_view(context) == "PJ::BuiltinFilters"
               ? u"translated:"_s + QString::fromUtf8(source)
               : QString{};
  }
};

/// Inspect the production resource, not a hand-maintained test schema.
std::vector<PJ::scripting::FilterClass> bundledFilters() {
  QFile resource(u":/filters/builtin_filters.luau"_s);
  EXPECT_TRUE(resource.open(QIODevice::ReadOnly));
  const auto engine = PJ::scripting::makeLuauEngine();
  auto classes = engine->inspectModule(resource.readAll().toStdString(), "bundled");
  EXPECT_TRUE(classes.has_value());
  return classes.has_value() ? std::move(*classes) : std::vector<PJ::scripting::FilterClass>{};
}

/// All human-facing metadata, including enum labels and optional tooltips.
std::set<std::string> displaySources(const PJ::scripting::FilterClass& filter) {
  std::set<std::string> sources{filter.name, filter.description};
  for (const auto& parameter : filter.parameters) {
    sources.insert(parameter.label);
    sources.insert(parameter.tooltip);
    for (const auto& choice : parameter.values) {
      sources.insert(choice.label);
    }
  }
  sources.erase("");
  return sources;
}

/// Localization must not alter any field consumed by filter configuration.
void expectSameParameterData(const PJ::scripting::ParamSpec& source, const PJ::scripting::ParamSpec& display) {
  EXPECT_EQ(display.name, source.name);
  EXPECT_EQ(display.type, source.type);
  EXPECT_EQ(display.default_value, source.default_value);
  EXPECT_EQ(display.min, source.min);
  EXPECT_EQ(display.max, source.max);
  EXPECT_EQ(display.step, source.step);
  EXPECT_EQ(display.decimals, source.decimals);
  EXPECT_EQ(display.unit, source.unit);
  EXPECT_EQ(display.visible_when_param, source.visible_when_param);
  EXPECT_EQ(display.visible_when_equals, source.visible_when_equals);
  ASSERT_EQ(display.values.size(), source.values.size());
  for (std::size_t i = 0; i < source.values.size(); ++i) {
    EXPECT_EQ(display.values[i].value, source.values[i].value);
  }
}

/// A real source for applying a built-in through FilterEditorPanel.
PJ::CurveDescriptor addSource(PJ::SessionManager& session, PJ::CatalogModel& catalog, PJ::DatasetId dataset_id) {
  PJ::DataWriter writer = session.dataEngine().createWriter();
  auto handle = writer.registerScalarSeries(dataset_id, "/imu/accel", PJ::NumericType::kFloat64);
  EXPECT_TRUE(handle.has_value());
  if (!handle.has_value()) {
    return {};
  }
  writer.appendScalar(*handle, 1'000'000, -3.0);
  writer.appendScalar(*handle, 2'000'000, 2.0);
  EXPECT_FALSE(session.commitChunks(writer.flushAll()).empty());
  for (const auto& curve : catalog.curves()) {
    const auto descriptor = catalog.curveDescriptor(curve.name);
    if (descriptor && descriptor->topic_id == handle->topic_id) {
      return *descriptor;
    }
  }
  ADD_FAILURE() << "source descriptor missing";
  return {};
}

/// Select by the stable stored id, never by the localized label.
QListWidgetItem* selectFilter(QListWidget& list, const QString& id) {
  for (int row = 0; row < list.count(); ++row) {
    if (list.item(row)->data(Qt::UserRole).toString() == id) {
      list.setCurrentRow(row);
      return list.item(row);
    }
  }
  ADD_FAILURE() << "filter id missing: " << id.toStdString();
  return nullptr;
}

}  // namespace

TEST(BuiltinFilterPresentationTest, RegistryExactlyCoversActualBundledMetadata) {
  const auto filters = bundledFilters();
  ASSERT_EQ(filters.size(), 12U);
  std::set<std::string> expected;
  for (const auto& filter : filters) {
    const auto sources = displaySources(filter);
    expected.insert(sources.begin(), sources.end());
  }
  const auto registered = PJ::builtinFilterTranslationSources();
  const std::set<std::string> actual(registered.begin(), registered.end());
  EXPECT_EQ(actual.size(), registered.size()) << "duplicate display key";
  EXPECT_EQ(actual, expected) << "Luau display metadata and extraction registry diverged";
}

TEST(BuiltinFilterPresentationTest, TranslatesEveryBuiltinWithoutChangingParameterData) {
  PresentationTranslator translator;
  const auto filters = bundledFilters();
  ASSERT_FALSE(filters.empty());
  for (const auto& filter : filters) {
    SCOPED_TRACE(filter.id);
    const auto display = PJ::builtinFilterPresentation(filter);
    EXPECT_EQ(display.name, u"translated:"_s + QString::fromStdString(filter.name));
    EXPECT_EQ(display.description, u"translated:"_s + QString::fromStdString(filter.description));
    ASSERT_EQ(display.parameters.size(), filter.parameters.size());
    for (std::size_t i = 0; i < filter.parameters.size(); ++i) {
      const auto& source = filter.parameters[i];
      const auto& localized = display.parameters[i];
      expectSameParameterData(source, localized);
      EXPECT_EQ(localized.label, source.label.empty() ? "" : "translated:" + source.label);
      EXPECT_EQ(localized.tooltip, source.tooltip.empty() ? "" : "translated:" + source.tooltip);
      for (std::size_t j = 0; j < source.values.size(); ++j) {
        EXPECT_EQ(localized.values[j].label, "translated:" + source.values[j].label);
      }
    }
  }
}

TEST(BuiltinFilterPresentationTest, CustomMetadataAndGenericParameterFormStayUntouched) {
  PresentationTranslator translator;
  auto filters = bundledFilters();
  ASSERT_GT(filters.size(), 2U);
  auto custom = filters[2];  // Same id and English labels as the bundled Scale filter.
  custom.origin = "embedded";
  auto display = PJ::builtinFilterPresentation(custom);
  EXPECT_EQ(display.name, QString::fromStdString(custom.name));
  EXPECT_EQ(display.description, QString::fromStdString(custom.description));
  ASSERT_EQ(display.parameters.size(), custom.parameters.size());
  for (std::size_t i = 0; i < custom.parameters.size(); ++i) {
    EXPECT_EQ(display.parameters[i].label, custom.parameters[i].label);
    expectSameParameterData(custom.parameters[i], display.parameters[i]);
  }
  custom.origin = "bundled";
  custom.id = "user_scale";
  EXPECT_EQ(PJ::builtinFilterPresentation(custom).name, QString::fromStdString(custom.name));

  PJ::ParameterForm form;
  form.setSchema(custom.parameters);
  const auto labels = form.findChildren<QLabel*>();
  ASSERT_FALSE(labels.empty());
  EXPECT_EQ(labels.front()->text(), u"Scale"_s);
}

TEST(BuiltinFilterPresentationTest, TranslationDoesNotLeakIntoNextEnglishSession) {
  const auto filters = bundledFilters();
  ASSERT_FALSE(filters.empty());
  for (int launch = 0; launch < 2; ++launch) {
    {
      PresentationTranslator translator;
      EXPECT_EQ(PJ::builtinFilterPresentation(filters.front()).name, u"translated:None"_s);
    }
    EXPECT_EQ(PJ::builtinFilterPresentation(filters.front()).name, u"None"_s);
  }
}

TEST(BuiltinFilterPresentationTest, ParameterWidgetsTranslateLabelsAndKeepTypedValues) {
  PresentationTranslator translator;
  const auto filters = bundledFilters();
  ASSERT_FALSE(filters.empty());
  for (const auto& filter : filters) {
    SCOPED_TRACE(filter.id);
    PJ::ParameterForm canonical;
    canonical.setSchema(filter.parameters);
    PJ::ParameterForm localized;
    const auto display = PJ::builtinFilterPresentation(filter);
    localized.setSchema(display.parameters);
    EXPECT_EQ(localized.values(), canonical.values());
    for (const auto* label : localized.findChildren<QLabel*>()) {
      EXPECT_TRUE(label->text().startsWith(u"translated:"_s));
    }
    for (const auto& parameter : filter.parameters) {
      EXPECT_NE(localized.findChild<QWidget*>(QString::fromStdString(parameter.name)), nullptr);
      if (parameter.type != PJ::scripting::ParamType::kEnum) {
        continue;
      }
      auto* choices = localized.findChild<QComboBox*>(QString::fromStdString(parameter.name));
      ASSERT_NE(choices, nullptr);
      for (int i = 0; i < choices->count(); ++i) {
        EXPECT_EQ(choices->itemText(i), QString::fromStdString("translated:" + parameter.values[i].label));
        choices->setCurrentIndex(i);
        EXPECT_EQ(localized.values().at(parameter.name), parameter.values[i].value);
      }
    }
    localized.setValues(canonical.values());
    EXPECT_EQ(localized.values(), canonical.values());
  }
}

TEST(BuiltinFilterPresentationTest, EditorUsesLocalizedMetadataButPersistsCanonicalNamesAndSource) {
  PresentationTranslator translator;
  PJ::PlotWidgetBase::setGpuCanvasDisabledOverride(true);
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  const auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "filter-test"});
  ASSERT_TRUE(dataset.has_value());
  const auto source = addSource(session, catalog, *dataset);
  PJ::FilterEditorPanel panel(&session, &catalog, {source}, {});
  auto* list = panel.findChild<QListWidget*>(u"transform_list"_s);
  auto* alias = panel.findChild<QLineEdit*>(u"alias_edit"_s);
  auto* form = panel.findChild<PJ::ParameterForm*>();
  auto* apply = panel.findChild<QPushButton*>(u"save_btn"_s);
  ASSERT_NE(list, nullptr);
  ASSERT_NE(alias, nullptr);
  ASSERT_NE(form, nullptr);
  ASSERT_NE(apply, nullptr);
  const auto* row = selectFilter(*list, u"scale"_s);
  ASSERT_NE(row, nullptr);
  EXPECT_EQ(row->text(), u"translated:Scale"_s);
  EXPECT_EQ(row->toolTip(), u"translated:Affine scale + offset, with a time shift."_s);
  EXPECT_EQ(row->data(Qt::UserRole).toString(), u"scale"_s);
  EXPECT_EQ(alias->text(), u"/imu/accel[Scale]"_s);
  for (const auto* label : form->findChildren<QLabel*>()) {
    EXPECT_TRUE(label->text().startsWith(u"translated:"_s));
  }
  const nlohmann::json params{{"value_scale", 2.5}, {"value_offset", 3.0}, {"time_offset_sec", 0.25}};
  form->setValues(params);
  QList<QPair<QString, QString>> replacements;
  QObject::connect(&panel, &PJ::FilterEditorPanel::applied, &panel, [&](const auto& value) { replacements = value; });
  apply->click();
  ASSERT_EQ(replacements.size(), 1);
  const auto output = catalog.curveDescriptor(replacements.front().second);
  ASSERT_TRUE(output.has_value());
  EXPECT_EQ(output->topic_name, u"/imu/accel[Scale]"_s);
  const auto* recipe = session.dataProcessorService().filterConfig(output->topic_id);
  ASSERT_NE(recipe, nullptr);
  EXPECT_EQ(recipe->processor_id, "scale");
  EXPECT_EQ(nlohmann::json::parse(recipe->processor->saveParams()), params);
  QFile resource(u":/filters/builtin_filters.luau"_s);
  ASSERT_TRUE(resource.open(QIODevice::ReadOnly));
  EXPECT_EQ(recipe->filter_source, resource.readAll().toStdString());
}

TEST(BuiltinFilterPresentationTest, EditorPreservesUnknownCatalogDisplayNamesAndAliases) {
  PresentationTranslator translator;
  PJ::PlotWidgetBase::setGpuCanvasDisabledOverride(true);
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto custom_catalogue = std::make_shared<PJ::scripting::FilterCatalogue>(PJ::scripting::makeLuauEngine());
  ASSERT_TRUE(custom_catalogue->addBundledSource("return {{id = 'user_scale', name = 'Scale'}}", "custom").has_value());
  session.dataProcessorService().setFilterCatalogue(custom_catalogue);
  const auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "custom-filter-test"});
  ASSERT_TRUE(dataset.has_value());
  const auto source = addSource(session, catalog, *dataset);
  PJ::FilterEditorPanel panel(&session, &catalog, {source}, {});
  auto* list = panel.findChild<QListWidget*>(u"transform_list"_s);
  auto* alias = panel.findChild<QLineEdit*>(u"alias_edit"_s);
  ASSERT_NE(list, nullptr);
  ASSERT_NE(alias, nullptr);
  const auto* row = selectFilter(*list, u"user_scale"_s);
  ASSERT_NE(row, nullptr);
  EXPECT_EQ(row->text(), u"Scale"_s);
  EXPECT_EQ(row->data(Qt::UserRole).toString(), u"user_scale"_s);
  EXPECT_EQ(alias->text(), u"/imu/accel[Scale]"_s);
}
