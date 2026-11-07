// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_runtime/DerivedObjects.h"

#include <QStringList>
#include <algorithm>
#include <limits>
#include <map>
#include <memory>
#include <tuple>
#include <unordered_map>
#include <utility>

#include "SeriesKey.h"
#include "pj_datastore/data_processor.hpp"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/OnDemandService.h"
#include "pj_runtime/SessionManager.h"
#include "pj_runtime/XmlCdata.h"

using namespace Qt::StringLiterals;

namespace PJ {

namespace {

/// Producers before dependents, document order otherwise: restore resolves
/// transform inputs by name, so a dependent replayed before its producer fails.
/// Used for both persisting and replaying, so a saved file is already ordered
/// and an older unordered one still loads. A cycle (never installable) keeps its
/// members in document order at the end.
void orderTransformsByDependency(std::vector<PJ::DataProcessorService::TransformRecipe>& recipes) {
  const auto produces = [](const PJ::DataProcessorService::TransformRecipe& recipe, const std::string& input) {
    const std::vector<std::string> topics = PJ::DataProcessorService::materializedTopicNames(recipe);
    return std::any_of(topics.begin(), topics.end(), [&input](const std::string& topic) {
      return PJ::seriesKeyBelongsToTopic(input, topic);
    });
  };
  // ponytail: O(n^3) passes over a handful of transforms; Kahn's algorithm if layouts grow large.
  std::vector<PJ::DataProcessorService::TransformRecipe> ordered;
  ordered.reserve(recipes.size());
  std::vector<bool> placed(recipes.size(), false);
  for (bool progress = true; progress;) {
    progress = false;
    for (std::size_t index = 0; index < recipes.size(); ++index) {
      if (placed[index]) {
        continue;
      }
      const bool ready =
          std::all_of(recipes[index].inputs.begin(), recipes[index].inputs.end(), [&](const std::string& input) {
            for (std::size_t other = 0; other < recipes.size(); ++other) {
              if (other != index && !placed[other] && produces(recipes[other], input)) {
                return false;
              }
            }
            return true;
          });
      if (ready) {
        placed[index] = true;
        ordered.push_back(std::move(recipes[index]));
        progress = true;
      }
    }
  }
  for (std::size_t index = 0; index < recipes.size(); ++index) {
    if (!placed[index]) {
      ordered.push_back(std::move(recipes[index]));
    }
  }
  recipes = std::move(ordered);
}

// The catalog's descriptor for `path`: the exact source dataset when the
// qualifiers still agree, a unique fallback otherwise, never a same-topic sibling
// by load order. Empty when no loaded dataset matches (a filter input, say, that
// only a later restore step creates).
std::optional<CurveDescriptor> resolveSavedSeries(const CatalogModel& catalog, const SavedSeriesPath& path) {
  const std::optional<QString> key = catalog.resolveCurveKey(path);
  return key.has_value() ? catalog.curveDescriptor(*key) : std::nullopt;
}

// The datasets in the catalog that carry every key of `inputs` as a series (every one of them,
// for no inputs; a dataset with no catalog item yet is not counted). Keys are read literally: a
// persisted generator input is a bare key, so a colon in it is part of the name, never a
// `dataset_source:` qualifier.
std::vector<DatasetId> datasetsCarryingInputs(const CatalogModel& catalog, const std::vector<std::string>& inputs) {
  std::vector<DatasetId> carriers;
  for (const DatasetId dataset : catalog.datasetIds()) {
    const bool carries_all = std::all_of(inputs.begin(), inputs.end(), [&](const std::string& key) {
      return catalog.findScalarForMarkerKey(dataset, key).has_value();
    });
    if (carries_all) {
      carriers.push_back(dataset);
    }
  }
  return carriers;
}

// Whether two recipes agree on every field `DerivedObjects::save` persists for a generator.
bool samePersistedGenerator(const BatchEngine::GeneratorRecipe& lhs, const BatchEngine::GeneratorRecipe& rhs) {
  const auto persisted = [](const BatchEngine::GeneratorRecipe& recipe) {
    return std::tie(
        recipe.id, recipe.history_exempt, recipe.language, recipe.all_datasets, recipe.dataset_id, recipe.window,
        recipe.inputs, recipe.declared_inputs, recipe.outputs, recipe.params_json, recipe.script);
  };
  return persisted(lhs) == persisted(rhs);
}

/// The `<key>/<output>` that `input` of `recipe` reads: the upstream recorded for it, which a qualified input does not
/// spell bare, else the input itself.
std::string upstreamOutputOf(const RecipeGraph::Recipe& recipe, const std::string& input) {
  const auto recorded = recipe.recorded_upstreams.find(input);
  return recorded != recipe.recorded_upstreams.end() ? recorded->second.upstream + "/" + recorded->second.name : input;
}

/// The `<output>` of an input naming `<upstream>/<output>` behind any qualifier; empty when it names no such thing. The
/// saved `upstream` decides, not the element's `dataset_source`: a generic layout drops that one, and after a merge it
/// is the anchor's while the input keeps the consumed dataset's qualifier.
std::string outputOfUpstream(const std::string& input, const std::string& upstream) {
  const std::string prefix = upstream + "/";
  if (input.starts_with(prefix)) {
    return input.substr(prefix.size());
  }
  const std::size_t qualified = input.find(":" + prefix);
  return qualified != std::string::npos ? input.substr(qualified + 1 + prefix.size()) : std::string{};
}

/// Producers before readers, document order otherwise: the graph binds a reader (an input naming `<key>/<output>`,
/// maybe qualified) only to an installed producer. The entries in a cycle, or reading one, are marked so; they and the
/// entries that failed to parse follow the rest, in document order.
void orderOnDemandByDependency(std::vector<WantedOnDemand>& entries) {
  std::unordered_map<std::string, std::size_t> producer_of;
  for (std::size_t index = 0; index < entries.size(); ++index) {
    if (!entries[index].parse_error.isEmpty()) {
      continue;
    }
    for (const scripting::OutputSpec& output : entries[index].recipe.outputs) {
      if (output.type == scripting::OutputType::kObject) {
        producer_of.emplace(entries[index].recipe.key() + "/" + output.name, index);
      }
    }
  }
  std::vector<std::vector<std::size_t>> readers(entries.size());
  std::vector<std::size_t> waiting(entries.size(), 0);
  std::vector<std::size_t> ready;
  for (std::size_t index = 0; index < entries.size(); ++index) {
    std::vector<std::size_t> upstreams;
    const RecipeGraph::Recipe& recipe = entries[index].recipe;
    for (const std::string& input : recipe.inputs) {
      const auto producer = producer_of.find(upstreamOutputOf(recipe, input));
      if (producer != producer_of.end() && std::ranges::find(upstreams, producer->second) == upstreams.end()) {
        upstreams.push_back(producer->second);
        readers[producer->second].push_back(index);
      }
    }
    waiting[index] = upstreams.size();
    if (upstreams.empty() && entries[index].parse_error.isEmpty()) {
      ready.push_back(index);
    }
  }
  for (std::size_t cursor = 0; cursor < ready.size(); ++cursor) {
    for (const std::size_t reader : readers[ready[cursor]]) {
      if (--waiting[reader] == 0 && entries[reader].parse_error.isEmpty()) {
        ready.push_back(reader);
      }
    }
  }
  std::vector<bool> placed(entries.size(), false);
  std::vector<WantedOnDemand> ordered;
  ordered.reserve(entries.size());
  for (const std::size_t index : ready) {
    placed[index] = true;
    ordered.push_back(std::move(entries[index]));
  }
  for (std::size_t index = 0; index < entries.size(); ++index) {
    if (!placed[index]) {
      if (entries[index].parse_error.isEmpty()) {
        entries[index].parse_error = DerivedObjects::tr("cyclic on-demand dependency (or depends on a cycle)");
      }
      ordered.push_back(std::move(entries[index]));
    }
  }
  entries = std::move(ordered);
}

}  // namespace

DerivedObjects::DerivedObjects(SessionManager& session, CatalogModel& catalog) : session_(session), catalog_(catalog) {}

QDomElement DerivedObjects::save(QDomDocument& doc, SnapshotScope scope) const {
  QDomElement element = doc.createElement(u"data_processors"_s);
  for (const auto& recipe : session_.dataProcessorService().recipes()) {
    // Resolve the input column to a stable (topic, field) path so it rebinds on
    // reload exactly like a plotted curve.
    const QString input_key = u"dataset:%1/topic:%2/column:%3"_s.arg(recipe.dataset_id)
                                  .arg(recipe.input_topic_id)
                                  .arg(recipe.input_column_index);
    const std::optional<CurveDescriptor> input_desc = catalog_.curveDescriptor(input_key);
    if (!input_desc.has_value()) {
      continue;  // input field is gone; nothing to persist
    }
    QDomElement processor = doc.createElement(u"processor"_s);
    processor.setAttribute(u"input_topic"_s, input_desc->topic_name);
    processor.setAttribute(u"input_field"_s, input_desc->field_path);
    // Dataset qualifiers so the input rebinds to its own source dataset on restore,
    // never a same-topic sibling (first-match was part of the reported undo bug).
    // input_dataset_path is stamped by stampDatasetSourcePaths at layout-file save.
    processor.setAttribute(u"input_dataset_id"_s, QString::number(recipe.dataset_id));
    if (const auto source = catalog_.datasetSourceName(recipe.dataset_id); source.has_value()) {
      processor.setAttribute(u"input_dataset_source"_s, *source);
    }
    processor.setAttribute(u"processor_id"_s, QString::fromStdString(recipe.processor_id));
    processor.setAttribute(u"output_name"_s, QString::fromStdString(recipe.output_name));
    if (recipe.processor) {
      appendJsonAsCdata(doc, processor, QString::fromStdString(recipe.processor->saveParams()));
    }
    // Embed the filter's Luau source so the layout reopens on a machine without
    // this filter installed (resolved by restore's source_fallback leg). A named
    // child element, NOT a second direct CDATA child — restore reads params via
    // directCdataText, which ignores child elements. Empty for native C++ builtins.
    if (!recipe.filter_source.empty()) {
      QDomElement source_el = doc.createElement(u"source_fallback"_s);
      appendJsonAsCdata(doc, source_el, QString::fromStdString(recipe.filter_source));
      processor.appendChild(source_el);
    }
    element.appendChild(processor);
  }
  // Plugin-created transforms (pj.data_processors.v1): named, owner-tagged nodes
  // that carry their script + bindings BY VALUE, so they replay on load WITHOUT the
  // originating plugin (needed only for re-editing). Persisted in the same snapshot
  // block as the per-curve filters above; restore re-installs them via
  // DataProcessorService::restoreTransform. Inputs/outputs are topic NAMES (the
  // engine resolves them on restore), so no (topic, field) rebinding is needed here.
  std::vector<DataProcessorService::TransformRecipe> transforms = session_.dataProcessorService().transformRecipes();
  orderTransformsByDependency(transforms);
  for (const auto& recipe : transforms) {
    // History snapshots omit entries the restore has no authority to replay.
    if (scope == SnapshotScope::kHistory && recipe.history_exempt) {
      continue;
    }
    QDomElement transform = doc.createElement(u"transform"_s);
    transform.setAttribute(u"owner_plugin"_s, QString::fromStdString(recipe.owner_plugin));
    transform.setAttribute(u"id"_s, QString::fromStdString(recipe.user_id));
    if (recipe.history_exempt) {
      transform.setAttribute(u"history_exempt"_s, u"1"_s);
    }
    transform.setAttribute(u"backend"_s, QString::fromStdString(recipe.backend));
    transform.setAttribute(u"api_version"_s, QString::fromStdString(recipe.api_version));
    if (!recipe.backend_version.empty()) {
      transform.setAttribute(u"backend_version"_s, QString::fromStdString(recipe.backend_version));
    }
    for (std::size_t index = 0; index < recipe.inputs.size(); ++index) {
      const std::string& input_name = recipe.inputs[index];
      QDomElement in = doc.createElement(u"input"_s);
      in.setAttribute(u"name"_s, QString::fromStdString(input_name));
      if (index < recipe.input_bindings.size()) {
        const auto& binding = recipe.input_bindings[index];
        in.setAttribute(u"dataset_id"_s, QString::number(binding.dataset_id));
        in.setAttribute(u"dataset_source"_s, QString::fromStdString(binding.dataset_source));
        in.setAttribute(u"topic"_s, QString::fromStdString(binding.topic_name));
        in.setAttribute(u"field"_s, QString::fromStdString(binding.field_path));
        in.setAttribute(u"column"_s, QString::number(static_cast<qulonglong>(binding.column_index)));
      }
      transform.appendChild(in);
    }
    for (const auto& output_name : recipe.outputs) {
      QDomElement out = doc.createElement(u"output"_s);
      out.setAttribute(u"name"_s, QString::fromStdString(output_name));
      transform.appendChild(out);
    }
    // params (create(params), JSON) and script (the full backend payload, Luau today)
    // each get their own CDATA child so restore reads them back independently.
    if (!recipe.params_json.empty()) {
      QDomElement params = doc.createElement(u"params"_s);
      appendJsonAsCdata(doc, params, QString::fromStdString(recipe.params_json));
      transform.appendChild(params);
    }
    QDomElement script = doc.createElement(u"script"_s);
    appendJsonAsCdata(doc, script, QString::fromStdString(recipe.script));
    transform.appendChild(script);
    element.appendChild(transform);
  }

  // Marker generators (kind=markers of pj.data_processors.v1): whole-series object
  // generators run by BatchEngine. Persisted like transforms — script + inputs
  // BY VALUE so they replay on load without the originating plugin. Dataset identity
  // is stamped for cross-session rebind (mirrors the <processor> qualifiers); an
  // all_datasets generator has no single dataset, so it stamps none.
  for (const auto& recipe : session_.batchEngine().recipes()) {
    // Marker generators use the same history scope as transforms.
    if (scope == SnapshotScope::kHistory && recipe.history_exempt) {
      continue;
    }
    QDomElement gen = doc.createElement(u"generator"_s);
    gen.setAttribute(u"id"_s, QString::fromStdString(recipe.id));
    if (recipe.history_exempt) {
      gen.setAttribute(u"history_exempt"_s, u"1"_s);
    }
    gen.setAttribute(u"language"_s, QString::fromStdString(recipe.language));
    gen.setAttribute(u"all_datasets"_s, recipe.all_datasets ? u"1"_s : u"0"_s);
    if (!recipe.all_datasets) {
      gen.setAttribute(u"dataset_id"_s, QString::number(recipe.dataset_id));
      if (const auto source = catalog_.datasetSourceName(recipe.dataset_id); source.has_value()) {
        gen.setAttribute(u"dataset_source"_s, *source);
      }
      gen.setAttribute(u"dataset_path"_s, session_.datasetSourcePath(recipe.dataset_id));
    }
    if (recipe.window.has_value()) {
      gen.setAttribute(u"window_start_ns"_s, QString::number(recipe.window->first));
      gen.setAttribute(u"window_end_ns"_s, QString::number(recipe.window->second));
    }
    for (std::size_t index = 0; index < recipe.inputs.size(); ++index) {
      QDomElement in = doc.createElement(u"input"_s);
      in.setAttribute(u"name"_s, QString::fromStdString(recipe.inputs[index]));
      if (index < recipe.declared_inputs.size()) {
        in.setAttribute(u"declared"_s, QString::fromStdString(recipe.declared_inputs[index]));
      }
      gen.appendChild(in);
    }
    for (const auto& output_name : recipe.outputs) {
      QDomElement out = doc.createElement(u"output"_s);
      out.setAttribute(u"name"_s, QString::fromStdString(output_name));
      gen.appendChild(out);
    }
    if (!recipe.params_json.empty()) {
      QDomElement params = doc.createElement(u"params"_s);
      appendJsonAsCdata(doc, params, QString::fromStdString(recipe.params_json));
      gen.appendChild(params);
    }
    QDomElement gen_script = doc.createElement(u"script"_s);
    appendJsonAsCdata(doc, gen_script, QString::fromStdString(recipe.script));
    gen.appendChild(gen_script);
    element.appendChild(gen);
  }

  // On-demand recipes (kind=on_demand): script, inputs and typed outputs by value, producers first, so they
  // replay without the plugin. Every input of a node resolved in its one dataset (D7), which each <input>
  // carries like a transform input (dataset_path is stamped at layout-file save).
  const RecipeGraph& graph = session_.onDemandService().graph();
  std::vector<WantedOnDemand> on_demand;
  for (const std::string& key : graph.keys()) {
    const RecipeGraph::Node& live = *graph.find(key);
    if (!live.recipe.ephemeral && !(scope == SnapshotScope::kHistory && live.recipe.history_exempt)) {
      WantedOnDemand entry{live.recipe, {}};
      for (std::size_t index = 0; index < live.bindings.size(); ++index) {
        if (!live.bindings[index].upstream.empty()) {
          entry.recipe.recorded_upstreams[live.recipe.inputs[index]] = live.bindings[index];
        }
      }
      on_demand.push_back(std::move(entry));
    }
  }
  orderOnDemandByDependency(on_demand);
  for (const WantedOnDemand& entry : on_demand) {
    const RecipeGraph::Recipe& recipe = entry.recipe;
    const RecipeGraph::Node& live = *graph.find(recipe.key());
    const DatasetId dataset = live.dataset;
    QDomElement node = doc.createElement(u"on_demand"_s);
    node.setAttribute(u"owner_plugin"_s, QString::fromStdString(recipe.owner_plugin));
    node.setAttribute(u"id"_s, QString::fromStdString(recipe.user_id));
    if (recipe.history_exempt) {
      node.setAttribute(u"history_exempt"_s, u"1"_s);
    }
    node.setAttribute(u"language"_s, QString::fromStdString(recipe.language));
    node.setAttribute(u"query_time"_s, recipe.query_time ? u"1"_s : u"0"_s);
    if (recipe.pinned_t.has_value()) {
      node.setAttribute(u"pinned_t_ns"_s, QString::number(*recipe.pinned_t));
    }
    const std::optional<QString> source = catalog_.datasetSourceName(dataset);
    for (std::size_t index = 0; index < recipe.inputs.size(); ++index) {
      QDomElement in = doc.createElement(u"input"_s);
      in.setAttribute(u"name"_s, QString::fromStdString(recipe.inputs[index]));
      in.setAttribute(u"dataset_id"_s, QString::number(dataset));
      if (source.has_value()) {
        in.setAttribute(u"dataset_source"_s, *source);
      }
      // Another node's output: recorded so a reader whose producer is gone restores missing its input.
      if (const RecipeGraph::Binding& binding = live.bindings[index]; !binding.upstream.empty()) {
        in.setAttribute(u"upstream"_s, QString::fromStdString(binding.upstream));
        in.setAttribute(u"type"_s, QString::fromUtf8(sdk::name(binding.object_type)));
      } else if (binding.name != recipe.inputs[index]) {
        in.setAttribute(u"topic"_s, QString::fromStdString(binding.name));
      }
      node.appendChild(in);
    }
    for (const scripting::OutputSpec& output : recipe.outputs) {
      QDomElement out = doc.createElement(u"output"_s);
      out.setAttribute(u"name"_s, QString::fromStdString(output.name));
      out.setAttribute(u"type"_s, QString::fromStdString(scripting::outputTypeName(output)));
      node.appendChild(out);
    }
    if (!recipe.params_json.empty()) {
      QDomElement params = doc.createElement(u"params"_s);
      appendJsonAsCdata(doc, params, QString::fromStdString(recipe.params_json));
      node.appendChild(params);
    }
    QDomElement script = doc.createElement(u"script"_s);
    appendJsonAsCdata(doc, script, QString::fromStdString(recipe.script));
    node.appendChild(script);
    element.appendChild(node);
  }
  return element;
}

std::vector<DataProcessorService::TransformRecipe> DerivedObjects::parseWantedTransforms(
    const QDomElement& element, bool skip_exempt, std::vector<DerivedDiagnostic>& diagnostics) const {
  std::vector<DataProcessorService::TransformRecipe> wanted_transforms;
  for (QDomElement transform = element.firstChildElement(u"transform"_s); !transform.isNull();
       transform = transform.nextSiblingElement(u"transform"_s)) {
    DataProcessorService::TransformRecipe recipe;
    QString transform_parse_error;
    recipe.owner_plugin = transform.attribute(u"owner_plugin"_s).toStdString();
    recipe.user_id = transform.attribute(u"id"_s).toStdString();
    recipe.key = DataProcessorService::makeTransformKey(recipe.owner_plugin, recipe.user_id);
    recipe.history_exempt = transform.attribute(u"history_exempt"_s) == u"1"_s;
    // Defensive: a kHistory document never carries an exempt entry by
    // construction (save omits them), but history still has no
    // authority to replay one if it somehow did (e.g. a hand-edited snapshot).
    if (skip_exempt && recipe.history_exempt) {
      continue;
    }
    recipe.backend = transform.attribute(u"backend"_s, u"luau"_s).toStdString();
    recipe.api_version = transform.attribute(u"api_version"_s, u"1"_s).toStdString();
    recipe.backend_version = transform.attribute(u"backend_version"_s).toStdString();
    std::vector<DataProcessorService::TransformInputBinding> parsed_bindings;
    bool has_persisted_binding = false;
    for (QDomElement in = transform.firstChildElement(u"input"_s); !in.isNull();
         in = in.nextSiblingElement(u"input"_s)) {
      recipe.inputs.push_back(in.attribute(u"name"_s).toStdString());
      DataProcessorService::TransformInputBinding binding;
      const bool has_binding = in.hasAttribute(u"dataset_id"_s) || in.hasAttribute(u"dataset_source"_s) ||
                               in.hasAttribute(u"dataset_path"_s) || in.hasAttribute(u"topic"_s) ||
                               in.hasAttribute(u"field"_s) || in.hasAttribute(u"column"_s);
      has_persisted_binding = has_persisted_binding || has_binding;
      if (in.hasAttribute(u"dataset_id"_s)) {
        bool ok = false;
        const qulonglong raw = in.attribute(u"dataset_id"_s).toULongLong(&ok);
        if (ok && raw <= std::numeric_limits<DatasetId>::max()) {
          binding.dataset_id = static_cast<DatasetId>(raw);
        } else {
          transform_parse_error = tr("invalid input dataset id");
        }
      }
      binding.dataset_source = in.attribute(u"dataset_source"_s).toStdString();
      binding.topic_name = in.attribute(u"topic"_s).toStdString();
      binding.field_path = in.attribute(u"field"_s).toStdString();
      const QString saved_dataset_path = in.attribute(u"dataset_path"_s);
      if (!saved_dataset_path.isEmpty()) {
        const DatasetIdentityResolution resolved = session_.resolveDatasetIdentity(
            binding.dataset_id, QString::fromStdString(binding.dataset_source), saved_dataset_path);
        if (resolved.id.has_value()) {
          binding.dataset_id = *resolved.id;
          if (const DatasetInfo* live = session_.dataEngine().getDataset(*resolved.id)) {
            binding.dataset_source = live->source_name;
          }
        } else {
          transform_parse_error =
              resolved.ambiguous ? tr("ambiguous input dataset path") : tr("input dataset path is not loaded");
        }
      }
      if (in.hasAttribute(u"column"_s)) {
        bool ok = false;
        const qulonglong raw = in.attribute(u"column"_s).toULongLong(&ok);
        if (ok && raw <= std::numeric_limits<std::size_t>::max()) {
          binding.column_index = static_cast<std::size_t>(raw);
        } else {
          transform_parse_error = tr("invalid input column");
        }
      }
      parsed_bindings.push_back(std::move(binding));
    }
    if (has_persisted_binding) {
      recipe.input_bindings = std::move(parsed_bindings);
    }
    for (QDomElement out = transform.firstChildElement(u"output"_s); !out.isNull();
         out = out.nextSiblingElement(u"output"_s)) {
      recipe.outputs.push_back(out.attribute(u"name"_s).toStdString());
    }
    recipe.params_json = directCdataText(transform.firstChildElement(u"params"_s)).toStdString();
    if (recipe.params_json.empty()) {
      recipe.params_json = "{}";
    }
    recipe.script = directCdataText(transform.firstChildElement(u"script"_s)).toStdString();
    if (!transform_parse_error.isEmpty()) {
      diagnostics.push_back(
          {"transform-restore-failed",
           tr("Could not restore transform '%1': %2").arg(QString::fromStdString(recipe.key), transform_parse_error)});
      continue;
    }
    wanted_transforms.push_back(std::move(recipe));
  }
  orderTransformsByDependency(wanted_transforms);  // older layouts were written unordered
  return wanted_transforms;
}

std::vector<WantedFilter> DerivedObjects::parseWantedFilters(const QDomElement& element) const {
  std::vector<WantedFilter> wanted;
  // A null <data_processors> yields a null firstChildElement, so this loop runs
  // zero times and the snapshot wants no filter at all.
  for (QDomElement processor = element.firstChildElement(u"processor"_s); !processor.isNull();
       processor = processor.nextSiblingElement(u"processor"_s)) {
    WantedFilter entry;
    entry.input = SavedSeriesPath{
        processor.attribute(u"input_topic"_s), processor.attribute(u"input_field"_s),
        static_cast<DatasetId>(processor.attribute(u"input_dataset_id"_s).toUInt()),
        processor.attribute(u"input_dataset_source"_s), processor.attribute(u"input_dataset_path"_s)};
    if (const std::optional<CurveDescriptor> input_desc = resolveSavedSeries(catalog_, entry.input);
        input_desc.has_value()) {
      entry.persisted.input_topic_id = input_desc->topic_id;
      entry.persisted.dataset_id = input_desc->dataset_id;
      entry.persisted.input_column_index = input_desc->column_index;
    }
    entry.persisted.processor_id = processor.attribute(u"processor_id"_s).toStdString();
    // Params are the <processor>'s OWN direct CDATA — directCdataText ignores the
    // <source_fallback> child (QDomElement::text() would recurse and merge them).
    const QString params = directCdataText(processor);
    entry.persisted.params_json = params.isEmpty() ? std::string("{}") : params.toStdString();
    entry.persisted.filter_source = processor.firstChildElement(u"source_fallback"_s).text().toStdString();
    entry.persisted.output_name = processor.attribute(u"output_name"_s).toStdString();
    wanted.push_back(std::move(entry));
  }
  return wanted;
}

bool DerivedObjects::repairDataProcessors(
    std::vector<WantedFilter>& wanted_filters,
    const std::vector<DataProcessorService::TransformRecipe>& wanted_transforms,
    const std::unordered_set<std::string>& live_exempt_keys, std::vector<DerivedDiagnostic>& diagnostics) {
  auto& service = session_.dataProcessorService();
  const auto is_live = [&service](const WantedFilter& wanted) {
    return wanted.persisted.input_topic_id != 0 &&
           service.isFilterLive(wanted.persisted.dataset_id, wanted.persisted.output_name);
  };
  // A failure is recorded per snapshot entry (its identity may not resolve yet)
  // and reported once at the end: an entry whose input only a later round
  // creates legitimately fails until then.
  std::map<std::size_t, QString> filter_failures;
  std::map<std::string, QString> transform_failures;
  for (bool progress = true; progress;) {
    progress = false;
    if (std::any_of(
            wanted_filters.begin(), wanted_filters.end(), [&](const auto& wanted) { return !is_live(wanted); })) {
      catalog_.rebuildFromDatastore();
    }
    for (std::size_t index = 0; index < wanted_filters.size(); ++index) {
      WantedFilter& wanted = wanted_filters[index];
      if (is_live(wanted)) {
        continue;
      }
      DataProcessorService::PersistedFilter& persisted = wanted.persisted;
      const std::optional<CurveDescriptor> input_desc = resolveSavedSeries(catalog_, wanted.input);
      if (!input_desc.has_value()) {
        persisted.input_topic_id = 0;
        filter_failures[index] =
            tr("Layout filter on '%1/%2' has no matching data; skipping.").arg(wanted.input.topic, wanted.input.field);
        continue;
      }
      persisted.input_topic_id = input_desc->topic_id;
      persisted.dataset_id = input_desc->dataset_id;
      persisted.input_column_index = input_desc->column_index;
      if (service.isFilterLive(persisted.dataset_id, persisted.output_name)) {
        filter_failures.erase(index);  // resolved onto a filter already live (kept, or a duplicate)
        continue;
      }
      // Resolve order: live catalogue → embedded source → transitional C++ builtin.
      std::unique_ptr<proc::DataProcessor> built =
          service.makeRestoredProcessor(persisted.processor_id, persisted.params_json, persisted.filter_source);
      if (!built) {
        filter_failures[index] = tr("Layout filter '%1' is unknown to this PlotJuggler; skipping.")
                                     .arg(QString::fromStdString(persisted.processor_id));
        continue;
      }
      const auto applied = service.applyFilter(
          persisted.input_topic_id, persisted.dataset_id, std::move(built), persisted.output_name,
          persisted.input_column_index);
      if (applied.has_value()) {
        filter_failures.erase(index);
        progress = true;
      } else {
        filter_failures[index] =
            tr("Could not restore filter '%1': %2")
                .arg(QString::fromStdString(persisted.output_name), QString::fromStdString(applied.error()));
      }
    }
    for (const auto& recipe : wanted_transforms) {
      if (live_exempt_keys.count(recipe.key) != 0 || service.isTransformLive(recipe.key)) {
        continue;
      }
      if (const auto restored = service.restoreTransform(recipe); restored.has_value()) {
        transform_failures.erase(recipe.key);
        progress = true;
      } else {
        transform_failures[recipe.key] =
            tr("Could not restore transform '%1': %2")
                .arg(QString::fromStdString(recipe.key), QString::fromStdString(restored.error()));
      }
    }
  }
  const auto report = [&diagnostics](const auto& failures, const char* diagnostic_id) {
    for (const auto& [key, reason] : failures) {
      diagnostics.push_back({diagnostic_id, reason});
    }
    return failures.empty();
  };
  const bool filters_restored = report(filter_failures, "processor-restore-failed");
  const bool transforms_restored = report(transform_failures, "transform-restore-failed");
  return filters_restored && transforms_restored;
}

std::vector<WantedGenerator> DerivedObjects::parseWantedGenerators(const QDomElement& element) const {
  std::vector<WantedGenerator> wanted;
  for (QDomElement gen = element.firstChildElement(u"generator"_s); !gen.isNull();
       gen = gen.nextSiblingElement(u"generator"_s)) {
    WantedGenerator entry;
    BatchEngine::GeneratorRecipe& recipe = entry.recipe;
    recipe.id = gen.attribute(u"id"_s).toStdString();
    recipe.history_exempt = gen.attribute(u"history_exempt"_s) == u"1"_s;
    recipe.kind = GeneratorKind::kMarkers;
    recipe.language = gen.attribute(u"language"_s, u"luau"_s).toStdString();
    recipe.all_datasets = gen.attribute(u"all_datasets"_s) == u"1"_s;
    // An empty qualifier carries no information: save writes one only when it has a value.
    entry.bind_by_inputs = !recipe.all_datasets && !gen.hasAttribute(u"dataset_id"_s) &&
                           gen.attribute(u"dataset_source"_s).isEmpty() && gen.attribute(u"dataset_path"_s).isEmpty();
    QString& gen_parse_error = entry.parse_error;
    if (!recipe.all_datasets) {
      bool ok = false;
      const qulonglong raw = gen.attribute(u"dataset_id"_s).toULongLong(&ok);
      if (ok && raw <= std::numeric_limits<DatasetId>::max()) {
        recipe.dataset_id = static_cast<DatasetId>(raw);
      }
      const QString saved_source = gen.attribute(u"dataset_source"_s);
      const QString saved_path = gen.attribute(u"dataset_path"_s);
      // Ids are minted by load order, so a saved id is trusted only while its
      // portable qualifiers (source label, path) still agree; a bare id stays as is.
      if (!saved_source.isEmpty() || !saved_path.isEmpty()) {
        const DatasetIdentityResolution resolved =
            session_.resolveDatasetIdentity(recipe.dataset_id, saved_source, saved_path);
        if (resolved.id.has_value()) {
          recipe.dataset_id = *resolved.id;
        } else {
          gen_parse_error = resolved.ambiguous ? tr("ambiguous dataset identity") : tr("dataset is not loaded");
        }
      }
    }
    // Raw ns, inclusive; no attributes means no window.
    if (gen.hasAttribute(u"window_start_ns"_s) && gen.hasAttribute(u"window_end_ns"_s)) {
      recipe.window = std::pair<Timestamp, Timestamp>{
          gen.attribute(u"window_start_ns"_s).toLongLong(), gen.attribute(u"window_end_ns"_s).toLongLong()};
    }
    bool any_declared = false;
    for (QDomElement in = gen.firstChildElement(u"input"_s); !in.isNull(); in = in.nextSiblingElement(u"input"_s)) {
      recipe.inputs.push_back(in.attribute(u"name"_s).toStdString());
      recipe.declared_inputs.push_back(in.attribute(u"declared"_s, in.attribute(u"name"_s)).toStdString());
      any_declared = any_declared || in.hasAttribute(u"declared"_s);
    }
    if (!any_declared) {
      recipe.declared_inputs.clear();
    }
    for (QDomElement out = gen.firstChildElement(u"output"_s); !out.isNull();
         out = out.nextSiblingElement(u"output"_s)) {
      recipe.outputs.push_back(out.attribute(u"name"_s).toStdString());
    }
    recipe.params_json = directCdataText(gen.firstChildElement(u"params"_s)).toStdString();
    recipe.script = directCdataText(gen.firstChildElement(u"script"_s)).toStdString();
    wanted.push_back(std::move(entry));
  }
  return wanted;
}

std::vector<WantedOnDemand> DerivedObjects::parseWantedOnDemand(const QDomElement& element, bool skip_exempt) const {
  std::vector<WantedOnDemand> wanted;
  for (QDomElement node = element.firstChildElement(u"on_demand"_s); !node.isNull();
       node = node.nextSiblingElement(u"on_demand"_s)) {
    WantedOnDemand entry;
    RecipeGraph::Recipe& recipe = entry.recipe;
    QString& error = entry.parse_error;
    recipe.owner_plugin = node.attribute(u"owner_plugin"_s).toStdString();
    recipe.user_id = node.attribute(u"id"_s).toStdString();
    recipe.history_exempt = node.attribute(u"history_exempt"_s) == u"1"_s;
    if (skip_exempt && recipe.history_exempt) {
      continue;
    }
    // `label` and `infer_outputs`, which the prototype wrote, are not part of a recipe: read past.
    recipe.language = node.attribute(u"language"_s, u"luau"_s).toStdString();
    const QString clock = node.attribute(u"query_time"_s, u"1"_s);
    if (clock == u"1"_s || clock == u"0"_s) {
      recipe.query_time = clock == u"1"_s;
    } else {
      error = tr("invalid query_time '%1'").arg(clock);
    }
    if (node.hasAttribute(u"pinned_t_ns"_s)) {
      bool ok = false;
      const qlonglong pinned = node.attribute(u"pinned_t_ns"_s).toLongLong(&ok);
      if (ok) {
        recipe.pinned_t = pinned;
      } else {
        error = tr("invalid pinned time '%1'").arg(node.attribute(u"pinned_t_ns"_s));
      }
    }
    // A saved input dataset (source, path) must name a loaded dataset, the same one for every input, which binds
    // the recipe there; with neither, the graph binds the one dataset carrying every input (D7).
    std::optional<DatasetId> dataset;
    for (QDomElement in = node.firstChildElement(u"input"_s); !in.isNull(); in = in.nextSiblingElement(u"input"_s)) {
      const std::string input = in.attribute(u"name"_s).toStdString();
      recipe.inputs.push_back(input);
      if (in.hasAttribute(u"upstream"_s)) {
        const std::string upstream = in.attribute(u"upstream"_s).toStdString();
        const auto type = sdk::parseBuiltinObjectType(in.attribute(u"type"_s).toStdString());
        std::string output = outputOfUpstream(input, upstream);
        if (type.has_value() && *type != sdk::BuiltinObjectType::kNone && !output.empty()) {
          recipe.recorded_upstreams[input] = {*type, upstream, std::move(output)};
        } else if (error.isEmpty()) {
          error = tr("invalid upstream of input '%1'").arg(QString::fromStdString(input));
        }
      }
      if (in.hasAttribute(u"topic"_s)) {
        recipe.recorded_topics[input] = in.attribute(u"topic"_s).toStdString();
      }
      if (!error.isEmpty()) {
        continue;
      }
      DatasetId saved_id = 0;
      if (in.hasAttribute(u"dataset_id"_s)) {
        bool ok = false;
        const qulonglong raw = in.attribute(u"dataset_id"_s).toULongLong(&ok);
        if (!ok || raw > std::numeric_limits<DatasetId>::max()) {
          error = tr("invalid input dataset id");
          continue;
        }
        saved_id = static_cast<DatasetId>(raw);
      }
      const QString saved_source = in.attribute(u"dataset_source"_s);
      const QString saved_path = in.attribute(u"dataset_path"_s);
      if (saved_source.isEmpty() && saved_path.isEmpty()) {
        continue;
      }
      const DatasetIdentityResolution resolved = session_.resolveDatasetIdentity(saved_id, saved_source, saved_path);
      if (!resolved.id.has_value()) {
        error = resolved.ambiguous ? tr("ambiguous input dataset") : tr("input dataset is not loaded");
      } else if (dataset.has_value() && *dataset != *resolved.id) {
        error = tr("inputs resolve to different datasets");
      } else {
        dataset = resolved.id;
      }
    }
    recipe.dataset = dataset;
    for (QDomElement out = node.firstChildElement(u"output"_s); !out.isNull();
         out = out.nextSiblingElement(u"output"_s)) {
      const QString name = out.attribute(u"name"_s);
      const Expected<scripting::OutputSpec> parsed =
          scripting::parseOutputSpec((name + u":"_s + out.attribute(u"type"_s)).toStdString());
      if (parsed.has_value()) {
        recipe.outputs.push_back(*parsed);
      } else if (error.isEmpty()) {
        error = tr("invalid output '%1': %2").arg(name, QString::fromStdString(parsed.error()));
      }
    }
    recipe.params_json = directCdataText(node.firstChildElement(u"params"_s)).toStdString();
    if (recipe.params_json.empty()) {
      recipe.params_json = "{}";
    }
    recipe.script = directCdataText(node.firstChildElement(u"script"_s)).toStdString();
    wanted.push_back(std::move(entry));
  }
  orderOnDemandByDependency(wanted);  // the prototype wrote them unordered
  return wanted;
}

DerivedSnapshot DerivedObjects::parse(const QDomElement& root, RestoreIntent intent) const {
  DerivedSnapshot snapshot;
  snapshot.intent = intent;
  const QDomElement element = root.firstChildElement(u"data_processors"_s);
  snapshot.filters = parseWantedFilters(element);
  snapshot.transforms =
      parseWantedTransforms(element, /*skip_exempt=*/intent == RestoreIntent::kHistory, snapshot.diagnostics);
  snapshot.generators = parseWantedGenerators(element);
  snapshot.on_demand = parseWantedOnDemand(element, /*skip_exempt=*/intent == RestoreIntent::kHistory);
  return snapshot;
}

DerivedObjects::KeepSets DerivedObjects::keepSets(const DerivedSnapshot& snapshot) const {
  auto& service = session_.dataProcessorService();
  std::vector<DataProcessorService::PersistedFilter> persisted_filters;
  persisted_filters.reserve(snapshot.filters.size());
  for (const WantedFilter& wanted : snapshot.filters) {
    persisted_filters.push_back(wanted.persisted);
  }
  return {
      service.unchangedFilterOutputs(persisted_filters),
      service.unchangedTransformKeys(snapshot.transforms, snapshot.intent)};
}

std::optional<QString> DerivedObjects::refusal(const DerivedSnapshot& snapshot) const {
  const RecipeGraph& graph = session_.onDemandService().graph();
  // No restore clears a preview, so one holding a key the snapshot wants would make the restore skip that recipe.
  for (const WantedOnDemand& entry : snapshot.on_demand) {
    if (const RecipeGraph::Node* live = graph.find(entry.recipe.key()); live != nullptr && live->recipe.ephemeral) {
      return tr("Cannot restore: the on-demand recipe '%1' is held by a preview of an open panel; close the panel "
                "first")
          .arg(QString::fromStdString(entry.recipe.key()));
    }
  }
  if (snapshot.intent != RestoreIntent::kHistory) {
    return std::nullopt;
  }
  auto& service = session_.dataProcessorService();
  BatchEngine& batch_engine = session_.batchEngine();
  // Reject before the first clear if what this restore removes — each filter and
  // each non-exempt transform the snapshot drops or changes, plus everything the
  // removal cascades into — would strand an exempt transform or marker generator.
  // Removal cascades between filters and transforms, so checking after the first
  // clear would be too late. Nothing below mutates the session.
  if (!service.hasHistoryExemptTransforms() && !batch_engine.hasHistoryExemptGenerators() &&
      !graph.hasHistoryExemptRecipes()) {
    return std::nullopt;
  }
  const KeepSets keep = keepSets(snapshot);
  const DataProcessorService::RemovalImpact impact =
      service.removalImpact(RestoreIntent::kHistory, keep.transform_keys, keep.filter_outputs);
  std::vector<std::string> blocked = impact.exempt_transform_names;
  const std::vector<std::string> blocked_markers = batch_engine.exemptDependentsOf(impact.removed_output_names);
  blocked.insert(blocked.end(), blocked_markers.begin(), blocked_markers.end());
  // An exempt on-demand node reads series and on-demand outputs: it strands on a removed processor output, and on
  // an object output the snapshot takes away (its node dropped, moved to another dataset, or no longer declaring
  // it). A changed node that keeps the output reinstalls it under the same topic, and its exempt readers run again
  // on it.
  std::vector<std::string> removed = impact.removed_output_names;
  std::unordered_map<std::string, const RecipeGraph::Recipe*> wanted_by_key;
  for (const WantedOnDemand& entry : snapshot.on_demand) {
    if (entry.parse_error.isEmpty()) {
      wanted_by_key.emplace(entry.recipe.key(), &entry.recipe);
    }
  }
  for (const std::string& key : graph.keys()) {
    const RecipeGraph::Node& node = *graph.find(key);
    const RecipeGraph::Recipe& live = node.recipe;
    if (live.ephemeral || live.history_exempt) {
      continue;
    }
    const auto wanted = wanted_by_key.find(key);
    const bool kept_here =
        wanted != wanted_by_key.end() && (!wanted->second->dataset || *wanted->second->dataset == node.dataset);
    for (const scripting::OutputSpec& output : live.outputs) {
      if (output.type == scripting::OutputType::kObject &&
          (!kept_here || std::ranges::find(wanted->second->outputs, output) == wanted->second->outputs.end())) {
        removed.push_back(key + "/" + output.name);
      }
    }
  }
  const std::vector<std::string> blocked_on_demand = graph.exemptDependentsOf(removed);
  blocked.insert(blocked.end(), blocked_on_demand.begin(), blocked_on_demand.end());
  std::sort(blocked.begin(), blocked.end());
  blocked.erase(std::unique(blocked.begin(), blocked.end()), blocked.end());
  if (blocked.empty()) {
    return std::nullopt;
  }
  QStringList names;
  names.reserve(static_cast<int>(blocked.size()));
  for (const std::string& name : blocked) {
    names.push_back(QString::fromStdString(name));
  }
  return tr("Cannot undo: protected outputs would lose an input this step removes: %1").arg(names.join(u", "_s));
}

DerivedRestoreReport DerivedObjects::restore(DerivedSnapshot snapshot) {
  auto& service = session_.dataProcessorService();
  BatchEngine& batch_engine = session_.batchEngine();
  const RestoreIntent intent = snapshot.intent;
  const bool history = intent == RestoreIntent::kHistory;
  DerivedRestoreReport report;
  const bool parsed_all = snapshot.diagnostics.empty();
  report.diagnostics = std::move(snapshot.diagnostics);

  // 1. What stays live, upstream to downstream. A kept filter or transform keeps its
  //    output TopicId, so curves bound to it (e.g. in a preserved history-exempt tab,
  //    which no history snapshot describes) survive an unrelated undo. A kept generator
  //    (its bindings equal the snapshot's and nothing it reads is replaced) keeps its
  //    topic, published set, run state and coverage, so a restore costs it no re-run.
  //    Replaced means a removed output (a kept processor reading a removed output falls
  //    with it) or one this restore creates, which every transform it installs does, a
  //    reinstalled key's new names included. An all-datasets rule also needs a published
  //    set in the current display frame and no run in flight.
  const KeepSets keep = keepSets(snapshot);
  std::unordered_set<std::string> live_exempt_keys;
  for (const auto& live : service.transformRecipes()) {
    if (history && live.history_exempt) {
      live_exempt_keys.insert(live.key);
    }
  }
  OnDemandService& on_demand = session_.onDemandService();
  const bool any_on_demand = !snapshot.on_demand.empty() || !on_demand.graph().empty();
  std::vector<std::string> replaced_outputs;
  if (batch_engine.hasGenerators() || any_on_demand) {
    replaced_outputs = service.removalImpact(intent, keep.transform_keys, keep.filter_outputs).removed_output_names;
    for (const WantedFilter& wanted : snapshot.filters) {
      if (!service.isFilterLive(wanted.persisted.dataset_id, wanted.persisted.output_name)) {
        replaced_outputs.push_back(wanted.persisted.output_name);
      }
    }
    for (const auto& recipe : snapshot.transforms) {
      if (keep.transform_keys.count(recipe.key) == 0 && live_exempt_keys.count(recipe.key) == 0) {
        const std::vector<std::string> topics = DataProcessorService::materializedTopicNames(recipe);
        replaced_outputs.insert(replaced_outputs.end(), topics.begin(), topics.end());
      }
    }
  }
  std::unordered_set<std::string> keep_generators;
  if (batch_engine.hasGenerators()) {
    std::vector<BatchEngine::GeneratorRecipe> wanted_generators;
    for (const WantedGenerator& wanted : snapshot.generators) {
      if (wanted.parse_error.isEmpty() && !wanted.bind_by_inputs) {
        wanted_generators.push_back(wanted.recipe);
      }
    }
    keep_generators = batch_engine.unchangedGeneratorIds(wanted_generators, intent, replaced_outputs);
    // A binding with no dataset yet has nothing to compare, so its rule is re-created.
    for (const WantedGenerator& wanted : snapshot.generators) {
      if (wanted.bind_by_inputs) {
        keep_generators.erase(wanted.recipe.id);
      }
    }
  }

  // An on-demand node reads object topics, other nodes' outputs and series, a processor output among them: it is
  // kept when its recipe, its upstreams and the series it reads all stay (LAY-21).
  std::unordered_set<std::string> keep_on_demand;
  if (any_on_demand) {
    std::vector<RecipeGraph::Recipe> wanted_on_demand;
    wanted_on_demand.reserve(snapshot.on_demand.size());
    for (const WantedOnDemand& wanted : snapshot.on_demand) {
      if (wanted.parse_error.isEmpty()) {
        wanted_on_demand.push_back(wanted.recipe);
      }
    }
    keep_on_demand = on_demand.graph().unchangedRecipeKeys(wanted_on_demand, intent, replaced_outputs);
  }

  // 2. Clear downstream to upstream what the snapshot drops or changes; a kept filter
  //    or transform reading a removed output falls with it and is re-created below. A
  //    snapshot with NO <data_processors> clears everything; the catalog rebuild below
  //    MUST run on every path so the retired outputs leave the catalog. Replacement may
  //    drop every recipe; history never touches the exempt ones, after the caller's
  //    refusal check.
  //    The on-demand clear cancels the cleared nodes' runs and keeps their topics for the reinstall.
  const bool on_demand_cleared = on_demand.clearExcept(intent, keep_on_demand);
  // An exception below must not leave the service restoring: the sweep is a no-op once the normal path has run it.
  const struct SweepOnExit {
    OnDemandService& service;
    ~SweepOnExit() {
      (void)service.sweepUnclaimed();
    }
  } sweep_on_exit{on_demand};
  const std::size_t generators_before = batch_engine.persistentBindingCount();
  batch_engine.clearGenerators(intent, keep_generators);
  service.clearAllFilters(keep.filter_outputs);
  service.clearTransforms(intent, keep.transform_keys);

  // 3. Install upstream to downstream: filters and transforms in rounds, then the
  //    generators, which resolve their inputs by name through the catalog (and, naming
  //    no dataset, pick their dataset through it), so it lists the outputs just created
  //    first (marker topics are not catalog items, so the generators leave it as it
  //    is). upsertGenerator re-creates AND runs each in one call. A kept rule is not
  //    touched at all. A layout load waits a bounded time for a changed or new one,
  //    whose markers arrive later (markersPublished); an undo restores what the user
  //    saw, so it runs a changed rule to completion.
  const bool processors_restored =
      repairDataProcessors(snapshot.filters, snapshot.transforms, live_exempt_keys, report.diagnostics);
  catalog_.rebuildIfChanged();
  const BatchEngine::CreateWait create_wait =
      history ? BatchEngine::CreateWait::kToCompletion : BatchEngine::CreateWait::kBounded;
  bool any_generator_restored = false;
  // BatchEngine holds one binding per (id, dataset): a snapshot whose bindings of one rule
  // resolve to the same dataset must not let a later one silently replace an earlier.
  std::map<std::pair<std::string, DatasetId>, const BatchEngine::GeneratorRecipe*> restored_bindings;
  for (WantedGenerator& wanted : snapshot.generators) {
    BatchEngine::GeneratorRecipe& recipe = wanted.recipe;
    // Mirrors the transform loop: history has no authority over an exempt
    // generator, whether the document flags it or a live exempt one holds its id.
    const BatchEngine::GeneratorRecipe* live = batch_engine.firstBinding(recipe.id);
    if (history && (recipe.history_exempt || (live != nullptr && live->history_exempt))) {
      continue;
    }
    // A generic layout names no dataset: the generator takes the one loaded dataset that
    // carries its inputs, and stays unbound rather than guess between several.
    if (wanted.bind_by_inputs && wanted.parse_error.isEmpty()) {
      const std::vector<DatasetId> carriers = datasetsCarryingInputs(catalog_, recipe.inputs);
      if (carriers.size() == 1) {
        recipe.dataset_id = carriers.front();
      } else if (carriers.empty()) {
        wanted.parse_error = tr("no loaded dataset carries its input series");
      } else {
        wanted.parse_error =
            tr("its input series are carried by several loaded recordings; load only the one it was made for, or "
               "re-apply the rule");
      }
    }
    // A generator that cannot be restored is reported and skipped, but does NOT fail
    // the restore: unlike a filter or transform, it publishes only overlay decoration
    // — no topic that a curve, a plot or a scene binds to. Failing the restore here
    // costs the user the whole workspace (applyWorkspace rolls back) to save a set of
    // annotations, and a rule whose input series is legitimately absent in the data
    // this layout was reopened against is an ordinary outcome, not a corrupt layout.
    // Reported before the keep check: the id's other binding may be the one kept.
    if (!wanted.parse_error.isEmpty()) {
      report.diagnostics.push_back(
          {"generator-restore-failed", tr("Could not restore marker generator '%1': %2")
                                           .arg(QString::fromStdString(recipe.id), wanted.parse_error)});
      continue;
    }
    if (keep_generators.count(recipe.id) != 0) {
      continue;
    }
    const auto binding_key = std::make_pair(recipe.id, recipe.dataset_id);
    if (const auto earlier = restored_bindings.find(binding_key); earlier != restored_bindings.end()) {
      if (!samePersistedGenerator(*earlier->second, recipe)) {
        report.diagnostics.push_back(
            {"generator-restore-failed",
             tr("Could not restore marker generator '%1': another binding of this rule, saved with different "
                "settings, resolves to the same dataset; only the first is kept")
                 .arg(QString::fromStdString(recipe.id))});
      }
      continue;
    }
    if (const auto restored = batch_engine.upsertGenerator(recipe, create_wait); !restored.has_value()) {
      report.diagnostics.push_back(
          {"generator-restore-failed",
           tr("Could not restore marker generator '%1': %2")
               .arg(QString::fromStdString(recipe.id), QString::fromStdString(restored.error()))});
    } else {
      any_generator_restored = true;
      restored_bindings.emplace(binding_key, &recipe);
    }
  }

  // 4. On-demand recipes, producers first, once the catalog lists the processor outputs a series input may read.
  //    A kept node is not touched (its topics, results and state stay); a changed or new one is installed, and
  //    the install requests it where the session wants it (a finding at its pin, a demanded node at the cursor)
  //    on the pool. History has no authority over a node it left live (an exempt one). Then the sweep retires
  //    what no install reclaimed.
  bool on_demand_restored = true;
  std::unordered_set<std::string> installed_keys;
  for (const WantedOnDemand& wanted : snapshot.on_demand) {
    const QString key = QString::fromStdString(wanted.recipe.key());
    if (!wanted.parse_error.isEmpty()) {
      report.diagnostics.push_back(
          {"on-demand-restore-failed", tr("Could not restore on-demand recipe '%1': %2").arg(key, wanted.parse_error)});
      on_demand_restored = false;
      continue;
    }
    if (on_demand.graph().find(wanted.recipe.key()) != nullptr) {
      continue;
    }
    if (const auto installed = on_demand.install(wanted.recipe); installed.has_value()) {
      installed_keys.insert(wanted.recipe.key());
    } else {
      report.diagnostics.push_back(
          {"on-demand-restore-failed",
           tr("Could not restore on-demand recipe '%1': %2").arg(key, QString::fromStdString(installed.error()))});
      on_demand_restored = false;
    }
  }
  (void)on_demand.sweepUnclaimed();
  // A node outside the restore's authority (a preview, an exempt one under history) reading a processor output the
  // restore re-created holds results of the old series: it runs again, with its readers.
  if (!replaced_outputs.empty()) {
    std::vector<std::string> stale = on_demand.graph().readersOfSeries(replaced_outputs);
    std::erase_if(stale, [&installed_keys](const std::string& key) { return installed_keys.contains(key); });
    on_demand.rerun(stale);
  }

  // 5. Finish. upsertGenerator publishes to the store without announcing it; the
  //    toolbox's onDataChanged sends the same notification, so this restore does too,
  //    once. Likewise the on-demand topics registered, reclaimed or retired.
  if (any_generator_restored || batch_engine.persistentBindingCount() != generators_before) {
    session_.notifyMarkersChanged();
  }
  if (on_demand_cleared || !installed_keys.empty()) {
    catalog_.rebuildIfChanged();
    session_.notifyObjectTopicsChanged();
  }
  report.restored_all = parsed_all && processors_restored && on_demand_restored;
  return report;
}

}  // namespace PJ
