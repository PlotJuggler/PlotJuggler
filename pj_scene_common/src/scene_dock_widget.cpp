// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene_common/scene_dock_widget.h"

#include <QBoxLayout>
#include <QLoggingCategory>
#include <QSizePolicy>
#include <QTimer>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/SessionManager.h"
using namespace Qt::StringLiterals;

namespace PJ {

namespace {

Q_LOGGING_CATEGORY(lcSceneDock, "pj.scene.dock")

[[nodiscard]] QString objectTypeName(sdk::BuiltinObjectType object_type) {
  const auto name = sdk::name(object_type);
  return QString::fromLatin1(name.data(), static_cast<qsizetype>(name.size()));
}

}  // namespace

SceneDockWidget::SceneDockWidget(QWidget* parent) : QWidget(parent) {
  setContentsMargins(0, 0, 0, 0);
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);

  QTimer::singleShot(0, this, [this]() { ensureSceneViewCreated(); });
}

SceneDockWidget::~SceneDockWidget() {
  // clearLayers() must not run here: it reconciles the concrete view through
  // the pure-virtual syncViewLayers(), which cannot be dispatched during
  // base-class destruction. Concrete docks whose view references layers call
  // clearLayers() in their own destructor; by the time we get here that has
  // either happened (this is a no-op) or no view reconciliation was needed.
  destroyLayersUnsynced();
}

QWidget* SceneDockWidget::widget() {
  return this;
}

void SceneDockWidget::setSessionManager(SessionManager* session) {
  session_ = session;
}

void SceneDockWidget::setObjectDatasetResolver(ObjectDatasetResolver resolver) {
  object_dataset_resolver_ = std::move(resolver);
}

bool SceneDockWidget::tryAcceptObjectTopic(
    ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& title) {
  // No acceptsObjectType() pre-gate: addLayer() consults handleSceneConfigTopic()
  // first (a family may consume a topic as scene-wide config, e.g. TF, without it
  // being a render layer) and rejects unsupported types via createAndAttachLayer().
  return addTopic(topic_id, object_type, title);
}

bool SceneDockWidget::addTopic(ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& title) {
  // Public accept/refuse contract: both "layer added" and "consumed as config"
  // count as accepted. Callers needing the distinction use addLayer().
  return addLayer(topic_id, object_type, title) != AddOutcome::kRejected;
}

SceneDockWidget::AddOutcome SceneDockWidget::addLayer(
    ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& title) {
  ensureSceneViewCreated();
  const int64_t key = topicKey(topic_id);
  if (layers_.find(key) != layers_.end()) {
    return AddOutcome::kRejected;
  }
  if (session_ != nullptr) {
    const DatasetId bound_dataset = representativeDatasetId();
    const DatasetId incoming_dataset = session_->objectStore().descriptor(topic_id).dataset_id;
    if (bound_dataset != 0 && incoming_dataset != 0 && incoming_dataset != bound_dataset) {
      // The dock currently has one display-time conversion. Arrival order must
      // not silently decide which dataset's clock is applied to every layer.
      return AddOutcome::kRejected;
    }
  }
  if (handleSceneConfigTopic(topic_id, object_type, title)) {
    ever_had_content_ = true;
    notifyWorkspaceChanged();
    return AddOutcome::kConsumedAsConfig;
  }

  if (!acceptsObjectType(object_type)) {
    return AddOutcome::kRejected;
  }
  std::unique_ptr<ISceneLayer> layer = factory_.create(topic_id, object_type, title);
  if (layer == nullptr) {
    return AddOutcome::kRejected;
  }
  return insertLayer(topic_id, std::move(layer)) ? AddOutcome::kLayerAdded : AddOutcome::kRejected;
}

bool SceneDockWidget::insertLayer(ObjectTopicId topic_id, std::unique_ptr<ISceneLayer> layer) {
  if (layer == nullptr) {
    return false;
  }
  ensureSceneViewCreated();
  const int64_t key = topicKey(topic_id);
  if (layers_.find(key) != layers_.end()) {
    qCWarning(lcSceneDock) << "insertLayer: topic id" << topic_id.id << "already has a layer";
    return false;
  }
  std::unique_ptr<SceneLayerContext> context = makeContext();
  SceneLayerContext fallback_context;
  fallback_context.session = session_;
  const SceneLayerContext& attach_context = context != nullptr ? *context : fallback_context;
  if (!layer->attach(attach_context)) {
    qCWarning(lcSceneDock) << "insertLayer: layer attach failed for topic id" << topic_id.id;
    return false;
  }
  ever_had_content_ = true;
  wireLayerSignals(layer.get(), topic_id);
  registerLayer(key, std::move(layer));

  // Mutate -> reconcile view -> notify: the view already includes the new layer
  // when observers react to layerAdded (a slot may trigger a paint).
  syncViewLayers();
  refreshView();
  emit layerAdded(topic_id);
  notifyWorkspaceChanged();
  return true;
}

void SceneDockWidget::wireLayerSignals(ISceneLayer* layer, ObjectTopicId topic_id) {
  connect(layer, &ISceneLayer::infoChanged, this, [this]() {
    syncViewLayers();
    refreshView();
  });
  connect(layer, &ISceneLayer::visibilityChanged, this, [this, topic_id](bool visible) {
    recordLayerVisibility(topic_id, visible);
    syncViewLayers();
    refreshView();
  });
  connect(layer, &ISceneLayer::repaintRequested, this, [this]() { refreshView(); });
  connect(layer, &ISceneLayer::configurationChanged, this, [this]() { notifyWorkspaceChanged(); });
  connect(layer, &ISceneLayer::warningChanged, this, [this, topic_id](bool warn, QString reason) {
    emit layerWarningChanged(topic_id, warn, std::move(reason));
  });
}

void SceneDockWidget::registerLayer(int64_t key, std::unique_ptr<ISceneLayer> layer) {
  ISceneLayer* layer_raw = layer.get();
  layer_visibility_cache_[key] = layer_raw->info().visible;
  layers_.emplace(key, std::move(layer));
  draw_order_.push_back(key);
  invalidateTrackerRenderKey();  // new layer → next tick must repaint

  if (layer_raw->info().visible) {
    seedLayerTrackerTime(layer_raw);
  }
}

void SceneDockWidget::seedLayerTrackerTime(ISceneLayer* layer) {
  const auto range = layer->timeRange();
  const PJ::Timepoint first = range.min;
  const PJ::Timepoint last = range.max;
  if (last_tracker_.has_value()) {
    const PJ::Timepoint seed = (last >= first) ? std::clamp(*last_tracker_, first, last) : *last_tracker_;
    layer->setTrackerTime(seed);
  } else if (last >= first) {
    // No tracker tick yet: deliberately show the layer's first frame instead
    // of fabricating a time (0 is a valid timestamp; absence is explicit).
    layer->setTrackerTime(first);
  }
}

bool SceneDockWidget::restoreLayerElement(const QDomElement& layer_el) {
  if (session_ == nullptr) {
    return false;
  }

  bool dataset_ok = false;
  const auto dataset_value = layer_el.attribute(u"dataset_id"_s).toULongLong(&dataset_ok);
  if (!dataset_ok || dataset_value > std::numeric_limits<uint32_t>::max()) {
    markWorkspaceRestoreFailed();
    return true;
  }
  const auto saved_id = static_cast<DatasetId>(dataset_value);
  const QString saved_source = layer_el.attribute(u"dataset_source"_s);
  const QString saved_path = layer_el.attribute(u"dataset_path"_s);
  const QString topic_name = layer_el.attribute(u"topic_name"_s);
  const QString object_type_str = layer_el.attribute(u"object_type"_s);
  const QString display_name = layer_el.attribute(u"display_name"_s);
  const bool visible = layer_el.attribute(u"visible"_s, u"true"_s) == "true"_L1;

  const auto object_type_opt = sdk::parseBuiltinObjectType(object_type_str.toStdString());
  if (!object_type_opt.has_value()) {
    markWorkspaceRestoreFailed();
    return true;
  }
  const auto dataset_id_opt = resolveObjectDataset(saved_id, saved_source, saved_path, topic_name);
  if (!dataset_id_opt.has_value()) {
    return false;
  }
  const auto topic_id_opt = session_->objectStore().findTopic(*dataset_id_opt, topic_name.toStdString());
  if (!topic_id_opt.has_value()) {
    return false;
  }
  const ObjectTopicDescriptor& descriptor = session_->objectStore().descriptor(*topic_id_opt);
  const sdk::BuiltinObjectType live_type = objectTypeFromMetadata(descriptor.metadata_json);
  if (live_type != sdk::BuiltinObjectType::kNone && live_type != *object_type_opt) {
    markWorkspaceRestoreFailed();
    return true;
  }
  if (addLayer(*topic_id_opt, *object_type_opt, display_name) != AddOutcome::kLayerAdded) {
    markWorkspaceRestoreFailed();
    return true;
  }
  bool order_ok = false;
  const int saved_order = layer_el.attribute(u"order"_s).toInt(&order_ok);
  if (order_ok && saved_order >= 0) {
    const int64_t key = topicKey(*topic_id_opt);
    const auto current = std::find(draw_order_.begin(), draw_order_.end(), key);
    if (current != draw_order_.end()) {
      draw_order_.erase(current);
      const auto insertion =
          draw_order_.begin() + std::min<std::size_t>(static_cast<std::size_t>(saved_order), draw_order_.size());
      draw_order_.insert(insertion, key);
    }
  }
  if (ISceneLayer* layer = layerFor(*topic_id_opt); layer != nullptr) {
    const QDomElement payload = layer_el.firstChildElement();
    if (!payload.isNull() && !layer->xmlLoadState(payload)) {
      removeTopic(*topic_id_opt);
      markWorkspaceRestoreFailed();
      return true;
    }
  }
  if (!visible) {
    setLayerVisible(*topic_id_opt, false);
  }
  return true;
}

void SceneDockWidget::rememberPendingRestore(const QDomElement& layer_el, QString topic_name) {
  QDomDocument doc;
  const QDomNode clone = doc.importNode(layer_el, /*deep=*/true);
  doc.appendChild(clone);
  if (topic_name.isEmpty()) {
    topic_name = layer_el.attribute(u"topic_name"_s);
  }
  const auto duplicate = std::find_if(
      pending_restore_elements_.cbegin(), pending_restore_elements_.cend(),
      [&layer_el](const PendingRestoreElement& p) {
        const QDomElement existing = p.document.documentElement();
        return existing.tagName() == layer_el.tagName() &&
               existing.attribute(u"topic_name"_s) == layer_el.attribute(u"topic_name"_s) &&
               existing.attribute(u"dataset_id"_s) == layer_el.attribute(u"dataset_id"_s) &&
               existing.attribute(u"dataset_source"_s) == layer_el.attribute(u"dataset_source"_s) &&
               existing.attribute(u"dataset_path"_s) == layer_el.attribute(u"dataset_path"_s);
      });
  if (duplicate == pending_restore_elements_.cend()) {
    pending_restore_elements_.push_back(PendingRestoreElement{.document = doc, .topic_name = std::move(topic_name)});
    emit pendingRestoresChanged();
  }
}

bool SceneDockWidget::deferTopicIntent(
    DatasetId dataset_id, const QString& topic_name, sdk::BuiltinObjectType object_type, const QString& display_name) {
  if (dataset_id == 0 || topic_name.isEmpty() || object_type == sdk::BuiltinObjectType::kNone ||
      !acceptsDeferredObjectType(object_type)) {
    return false;
  }
  const DatasetId bound_dataset = representativeDatasetId();
  if (bound_dataset != 0 && bound_dataset != dataset_id) {
    return false;
  }
  for (const PendingRestoreElement& pending : pending_restore_elements_) {
    bool pending_id_ok = false;
    const qulonglong pending_id =
        pending.document.documentElement().attribute(u"dataset_id"_s).toULongLong(&pending_id_ok);
    if (pending_id_ok && pending_id != 0 && pending_id != dataset_id) {
      return false;
    }
  }

  QDomDocument document;
  const DeferredElementKind kind = deferredElementKind(object_type);
  QDomElement element =
      document.createElement(kind == DeferredElementKind::kConfigTopic ? u"config_topic"_s : u"layer"_s);
  element.setAttribute(u"pending_intent"_s, u"true"_s);
  element.setAttribute(u"dataset_id"_s, QString::number(dataset_id));
  element.setAttribute(u"dataset_source"_s, datasetSourceName(session_, dataset_id));
  if (session_ != nullptr) {
    const QString path = session_->datasetSourcePath(dataset_id);
    if (!path.isEmpty()) {
      element.setAttribute(u"dataset_path"_s, path);
    }
  }
  element.setAttribute(u"topic_name"_s, topic_name);
  element.setAttribute(u"object_type"_s, objectTypeName(object_type));
  if (kind == DeferredElementKind::kRenderLayer) {
    element.setAttribute(u"display_name"_s, display_name.isEmpty() ? topic_name : display_name);
    element.setAttribute(u"visible"_s, u"true"_s);
    element.setAttribute(u"order"_s, static_cast<int>(draw_order_.size() + pending_restore_elements_.size()));
  }
  document.appendChild(element);
  const std::size_t before = pending_restore_elements_.size();
  rememberPendingRestore(element, topic_name);
  if (pending_restore_elements_.size() == before) {
    return false;
  }
  notifyWorkspaceChanged();
  return true;
}

bool SceneDockWidget::restoreOnePending(const QDomElement& element) {
  // Base default: every deferred element is a render <layer>. A family with other
  // deferred element kinds (e.g. 3D's <config_topic>) overrides this hook to dispatch.
  return restoreLayerElement(element);
}

void SceneDockWidget::removeTopic(ObjectTopicId topic_id) {
  const int64_t key = topicKey(topic_id);
  auto it = layers_.find(key);
  if (it == layers_.end()) {
    return;
  }
  invalidateTrackerRenderKey();  // layer set shrank → next tick must repaint
  // Mutate -> reconcile view -> detach -> notify. syncViewLayers() drives
  // SceneViewWidget::setLayers, which runs releaseGL() on the dropped layer under
  // makeCurrent. detach() MUST follow that, because for GL-owning layers detach
  // tears down GL-bearing containers (e.g. mesh/texture caches) with no context
  // current — running it first orphans those resources before the contexted
  // releaseGL can free them. Keep the removed layer alive until after the
  // notification so a paint from a layerRemoved slot never borrows a destroyed
  // source. Destroyed at scope exit.
  std::unique_ptr<ISceneLayer> removed = std::move(it->second);
  layers_.erase(it);
  layer_visibility_cache_.erase(key);
  draw_order_.erase(std::remove(draw_order_.begin(), draw_order_.end(), key), draw_order_.end());
  syncViewLayers();
  refreshView();
  if (removed != nullptr) {
    removed->detach();
  }
  emit layerRemoved(topic_id);
  notifyWorkspaceChanged();
}

bool SceneDockWidget::revalidateObjects() {
  // The keep-if-never-populated rule lives HERE (non-virtual) so no override can
  // forget it: an intentionally-empty dock (click-created or restored empty,
  // never populated) survives, while a dock whose content was all evicted resets
  // to the placeholder. Subclasses only customize the family-specific prune.
  return pruneEvictedObjects() || !pending_restore_elements_.empty() || !ever_had_content_;
}

bool SceneDockWidget::pruneEvictedObjects() {
  if (layers_.empty()) {
    return false;
  }
  if (session_ == nullptr) {
    return true;  // layers present but no store to validate against — keep them
  }
  // A live topic carries a name; an evicted one resolves to the empty
  // descriptor. Collect first — removeTopic() mutates layers_ and runs the
  // full removal path (detach, view re-point, layerRemoved notification), so
  // observers like the layer-list panels stay coherent for free.
  ObjectStore& store = session_->objectStore();
  std::vector<ObjectTopicId> dead;
  for (const auto& [key, layer] : layers_) {
    if (layer == nullptr) {
      continue;
    }
    const ObjectTopicId topic_id = layer->info().topic_id;
    if (store.descriptor(topic_id).topic_name.empty()) {
      dead.push_back(topic_id);
    }
  }
  for (const ObjectTopicId topic_id : dead) {
    removeTopic(topic_id);
  }
  return !layers_.empty();
}

void SceneDockWidget::setLayerVisible(ObjectTopicId topic_id, bool visible) {
  ISceneLayer* layer = layerFor(topic_id);
  if (layer == nullptr) {
    return;
  }
  const int64_t key = topicKey(topic_id);
  const auto old_it = layer_visibility_cache_.find(key);
  const bool old_visible = old_it != layer_visibility_cache_.end() ? old_it->second : layer->info().visible;
  if (old_visible == visible) {
    return;
  }
  layer->setVisible(visible);
  invalidateTrackerRenderKey();  // visible set changed → next tick must repaint
  // Hidden layers receive no tracker ticks (onTrackerTime skips them), so on
  // un-hide re-deliver the current playhead clamped to this layer's range (same
  // seeding as registerLayer): the layer catches up instead of repainting the
  // geometry it held at the moment of hiding.
  if (visible && !old_visible) {
    seedLayerTrackerTime(layer);
  }
  const auto new_it = layer_visibility_cache_.find(key);
  if (new_it == layer_visibility_cache_.end() || new_it->second == old_visible) {
    recordLayerVisibility(topic_id, visible);
  }
  syncViewLayers();
  refreshView();
  notifyWorkspaceChanged();
}

void SceneDockWidget::reorderLayers(const std::vector<ObjectTopicId>& ordered_topic_ids) {
  std::vector<int64_t> ordered;
  ordered.reserve(draw_order_.size());
  std::unordered_set<int64_t> seen;
  for (const ObjectTopicId topic_id : ordered_topic_ids) {
    const int64_t key = topicKey(topic_id);
    if (layers_.find(key) != layers_.end() && seen.insert(key).second) {
      ordered.push_back(key);
    }
  }
  for (const int64_t key : draw_order_) {
    if (seen.insert(key).second) {
      ordered.push_back(key);
    }
  }
  if (ordered == draw_order_) {
    return;
  }
  draw_order_ = std::move(ordered);
  syncViewLayers();
  refreshView();
  notifyWorkspaceChanged();
}

std::vector<SceneLayerInfo> SceneDockWidget::layers() const {
  std::vector<SceneLayerInfo> out;
  out.reserve(draw_order_.size());
  for (const int64_t key : draw_order_) {
    const auto it = layers_.find(key);
    if (it != layers_.end() && it->second != nullptr) {
      out.push_back(it->second->info());
    }
  }
  return out;
}

ISceneLayer* SceneDockWidget::layerFor(ObjectTopicId topic_id) const {
  const auto it = layers_.find(topicKey(topic_id));
  return it == layers_.end() ? nullptr : it->second.get();
}

void SceneDockWidget::onTrackerTime(double time) {
  // `time` is display-axis seconds. Recover the ABSOLUTE instant the scene
  // stores objects by, by adding back the dataset's "Use time offset" shift
  // (display = raw - offset). A scene dock shows one dataset today, so a single
  // representative offset (representativeDatasetId() -> displayOffset) is exact; a
  // future mixed-dataset dock would convert per layer. NaN/inf carry no position.
  if (!std::isfinite(time)) {
    return;
  }
  PJ::DisplayOffset offset;
  if (session_ != nullptr) {
    if (const DatasetId repr_id = representativeDatasetId(); repr_id != 0) {
      offset = session_->displayOffset(repr_id);
    }
  }
  const PJ::Timepoint clamped = clampToLayerRange(PJ::toAbsolute(PJ::displaySeconds(time), offset));
  last_tracker_ = clamped;

  // Per-tick repaint coalescing: if every visible layer would render exactly what
  // it rendered at the last painted frame (same active sample, same transform),
  // nothing moved — skip the layer advance AND the repaint. This is what keeps a
  // 60 Hz playhead from repainting the scene/image docks 60×/s over <10 Hz data.
  // Settings/async changes bypass this gate via repaintRequested → refreshView.
  const uint64_t key = trackerRenderKey(clamped);
  if (have_render_key_ && key == last_render_key_) {
    return;
  }
  have_render_key_ = true;
  last_render_key_ = key;

  for (auto& [k, layer] : layers_) {
    if (layer != nullptr && layer->info().visible) {
      layer->setTrackerTime(clamped);
    }
  }
  refreshView();
}

namespace {
// splitmix64 finalizer — strong 64-bit avalanche so per-layer keys XOR-combine
// without the cancellation/clustering a plain XOR of raw ids would suffer.
[[nodiscard]] uint64_t mix64(uint64_t x) {
  x += 0x9e3779b97f4a7c15ULL;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
  return x ^ (x >> 31);
}
}  // namespace

uint64_t SceneDockWidget::trackerRenderKey(PJ::Timepoint time) const {
  // XOR of per-layer (topic ⊕ renderKey) mixes: order-independent (an
  // unordered_map rehash can't flip it) and collision-resistant via mix64.
  uint64_t key = 0;
  for (const auto& [k, layer] : layers_) {
    if (layer != nullptr && layer->info().visible) {
      key ^= mix64(static_cast<uint64_t>(k) * 0x9e3779b97f4a7c15ULL ^ layer->renderKey(time));
    }
  }
  // Fold in view-owned, non-layer content (e.g. the 3D TF overlay), so the gate
  // coalesces it without freezing it. Run it through mix64 like the per-layer terms
  // so a future viewRenderKey() override returning a sparse value (a small enum /
  // bitmask) still avalanches and can't cancel a layer's bits. mix64(0) is a
  // harmless constant offset for the default (no view content) case.
  key ^= mix64(viewRenderKey(time));
  return key;
}

DatasetId SceneDockWidget::representativeDatasetId() const {
  // First non-zero dataset_id among render layers. Guarded so a direct subclass
  // call with no session is safe (onTrackerTime already gates on session_).
  if (session_ == nullptr) {
    return 0;
  }
  for (const auto& [key, layer] : layers_) {
    if (layer == nullptr) {
      continue;
    }
    const DatasetId dataset_id = session_->objectStore().descriptor(layer->info().topic_id).dataset_id;
    if (dataset_id != 0) {
      return dataset_id;
    }
  }
  return 0;
}

QDomElement SceneDockWidget::xmlSaveState(QDomDocument& doc) const {
  QDomElement root = doc.createElement(xmlTag());
  root.setAttribute(u"version"_s, u"1"_s);

  if (session_ == nullptr) {
    appendPendingRestoreElements(doc, root);
    return root;
  }

  int saved_order = 0;
  for (const int64_t key : draw_order_) {
    const auto it = layers_.find(key);
    if (it == layers_.end() || it->second == nullptr) {
      continue;
    }
    const auto info = it->second->info();
    const auto& desc = session_->objectStore().descriptor(info.topic_id);

    QDomElement layer_el = doc.createElement(u"layer"_s);
    layer_el.setAttribute(u"dataset_id"_s, QString::number(desc.dataset_id));
    layer_el.setAttribute(u"dataset_source"_s, datasetSourceName(session_, desc.dataset_id));
    const QString path = session_->datasetSourcePath(desc.dataset_id);
    if (!path.isEmpty()) {
      layer_el.setAttribute(u"dataset_path"_s, path);
    }
    layer_el.setAttribute(u"topic_name"_s, QString::fromStdString(desc.topic_name));
    layer_el.setAttribute(u"object_type"_s, objectTypeName(info.object_type));
    layer_el.setAttribute(u"display_name"_s, info.display_name);
    layer_el.setAttribute(u"visible"_s, info.visible ? u"true"_s : u"false"_s);
    layer_el.setAttribute(u"order"_s, saved_order++);

    QDomElement payload = it->second->xmlSaveState(doc);
    if (!payload.isNull()) {
      layer_el.appendChild(payload);
    }
    root.appendChild(layer_el);
  }
  appendPendingRestoreElements(doc, root);
  return root;
}

bool SceneDockWidget::xmlLoadState(const QDomElement& element) {
  if (element.isNull() || element.tagName() != xmlTag()) {
    return false;
  }
  for (QDomElement child = element.firstChildElement(); !child.isNull(); child = child.nextSiblingElement()) {
    if (!acceptsStateChildTag(child.tagName())) {
      return false;
    }
    if (child.tagName() == "layer"_L1) {
      const QString visible = child.attribute(u"visible"_s, u"true"_s);
      if (visible != "true"_L1 && visible != "false"_L1) {
        return false;
      }
      if (child.hasAttribute(u"order"_s)) {
        bool order_ok = false;
        const int order = child.attribute(u"order"_s).toInt(&order_ok);
        if (!order_ok || order < 0) {
          return false;
        }
      }
    }
  }
  auto restoring_guard = beginWorkspaceRestore();
  resetWorkspaceRestoreStatus();
  clearPendingRestores();
  clearLayers();
  if (session_ == nullptr) {
    syncViewLayers();
    refreshView();
    return true;
  }

  int unresolved_layers = 0;
  int saved_order = 0;
  for (QDomElement layer_el = element.firstChildElement(u"layer"_s); !layer_el.isNull();
       layer_el = layer_el.nextSiblingElement(u"layer"_s)) {
    if (!layer_el.hasAttribute(u"order"_s)) {
      layer_el.setAttribute(u"order"_s, saved_order);
    }
    ++saved_order;
    if (!restoreLayerElement(layer_el)) {
      ++unresolved_layers;
      rememberPendingRestore(layer_el);
    }
  }
  if (unresolved_layers > 0) {
    qCWarning(lcSceneDock) << unresolved_layers << "saved layer(s) could not be restored (dataset not loaded)";
  }
  syncViewLayers();
  refreshView();
  return restore_failure_count_ == 0;
}

bool SceneDockWidget::acceptsStateChildTag(const QString& tag) const {
  return tag == "layer"_L1;
}

int SceneDockWidget::retryPendingRestores(const QSet<QString>& topic_names) {
  if (pending_restore_elements_.empty()) {
    return 0;
  }
  auto restoring_guard = beginWorkspaceRestore();

  int restored = 0;
  std::vector<PendingRestoreElement> still_pending;
  still_pending.reserve(pending_restore_elements_.size());
  for (PendingRestoreElement& pending : pending_restore_elements_) {
    if (!topic_names.isEmpty() && !topic_names.contains(pending.topic_name)) {
      still_pending.push_back(std::move(pending));
      continue;
    }
    const uint64_t failures_before = restore_failure_count_;
    if (restoreOnePending(pending.document.documentElement())) {
      if (restore_failure_count_ == failures_before) {
        ++restored;
      }
    } else {
      still_pending.push_back(std::move(pending));
    }
  }
  const std::size_t pending_before = pending_restore_elements_.size();
  pending_restore_elements_ = std::move(still_pending);
  if (pending_restore_elements_.size() != pending_before) {
    emit pendingRestoresChanged();
  }
  if (restored > 0) {
    syncViewLayers();
    refreshView();
  }
  return restored;
}

QStringList SceneDockWidget::unresolvedPendingRestores() const {
  QStringList unresolved;
  unresolved.reserve(static_cast<qsizetype>(pending_restore_elements_.size()));
  for (const PendingRestoreElement& pending : pending_restore_elements_) {
    if (!pending.topic_name.isEmpty()) {
      unresolved.push_back(pending.topic_name);
    }
  }
  return unresolved;
}

QStringList SceneDockWidget::unresolvedBlockingPendingRestores() const {
  QStringList unresolved;
  for (const PendingRestoreElement& pending : pending_restore_elements_) {
    if (pending.document.documentElement().attribute(u"pending_intent"_s) == "true"_L1) {
      continue;
    }
    if (!pending.topic_name.isEmpty()) {
      unresolved.push_back(pending.topic_name);
    }
  }
  return unresolved;
}

bool SceneDockWidget::hasPendingRestoreDemands() const {
  return std::any_of(
      pending_restore_elements_.cbegin(), pending_restore_elements_.cend(),
      [](const PendingRestoreElement& pending) { return !pending.topic_name.isEmpty(); });
}

std::vector<SceneDockWidget::PendingRestoreDemand> SceneDockWidget::pendingRestoreDemands() const {
  std::vector<PendingRestoreDemand> demands;
  demands.reserve(pending_restore_elements_.size());
  for (const PendingRestoreElement& pending : pending_restore_elements_) {
    const QDomElement element = pending.document.documentElement();
    if (pending.topic_name.isEmpty()) {
      continue;
    }
    bool id_ok = false;
    const qulonglong raw_id = element.attribute(u"dataset_id"_s).toULongLong(&id_ok);
    std::optional<DatasetId> preferred;
    if (id_ok && raw_id <= std::numeric_limits<DatasetId>::max()) {
      preferred = static_cast<DatasetId>(raw_id);
      // Same resolution ladder as restore itself (injected policy, else the
      // session default): the demand preference must never regress to trusting
      // a raw saved id that the restore path would refuse.
      const auto resolved = resolveObjectDataset(
          *preferred, element.attribute(u"dataset_source"_s), element.attribute(u"dataset_path"_s), pending.topic_name);
      if (resolved.has_value()) {
        preferred = resolved;
      }
    }
    demands.push_back(PendingRestoreDemand{.topic_name = pending.topic_name, .preferred_dataset = preferred});
  }
  return demands;
}

void SceneDockWidget::clearBlockingPendingRestores() {
  erasePendingRestoresIf([](const PendingRestoreElement& pending) {
    return pending.document.documentElement().attribute(u"pending_intent"_s) != "true"_L1;
  });
}

void SceneDockWidget::erasePendingRestoresIf(const std::function<bool(const PendingRestoreElement&)>& predicate) {
  const std::size_t before = pending_restore_elements_.size();
  std::erase_if(pending_restore_elements_, predicate);
  if (pending_restore_elements_.size() != before) {
    emit pendingRestoresChanged();
  }
}

// True when any element of the pending payload references `dataset_id` through
// a dataset-reference attribute. Convention (see the base header): payload
// attributes named `dataset_id` / `source_dataset_id` are dataset references;
// discard honors exactly these.
static bool documentReferencesDataset(const QDomDocument& document, DatasetId dataset_id) {
  const QDomNodeList elements = document.elementsByTagName(u"*"_s);
  for (int index = -1; index < elements.size(); ++index) {
    const QDomElement element = index < 0 ? document.documentElement() : elements.at(index).toElement();
    for (const QString& attribute : {u"dataset_id"_s, u"source_dataset_id"_s}) {
      bool ok = false;
      const qulonglong raw_id = element.attribute(attribute).toULongLong(&ok);
      if (ok && raw_id == dataset_id) {
        return true;
      }
    }
  }
  return false;
}

void SceneDockWidget::discardPendingRestoresForDataset(DatasetId dataset_id) {
  erasePendingRestoresIf([dataset_id](const PendingRestoreElement& pending) {
    return documentReferencesDataset(pending.document, dataset_id);
  });
}

void SceneDockWidget::discardPendingRestoresForTopic(DatasetId dataset_id, const QString& topic_name) {
  erasePendingRestoresIf([dataset_id, &topic_name](const PendingRestoreElement& pending) {
    return pending.topic_name == topic_name && documentReferencesDataset(pending.document, dataset_id);
  });
}

void SceneDockWidget::appendPendingRestoreElements(QDomDocument& doc, QDomElement& root) const {
  for (const PendingRestoreElement& pending : pending_restore_elements_) {
    const QDomElement element = pending.document.documentElement();
    if (!element.isNull()) {
      root.appendChild(doc.importNode(element, /*deep=*/true));
    }
  }
}

SceneDockWidget::PendingRestoreSnapshot SceneDockWidget::capturePendingRestoreState() const {
  PendingRestoreSnapshot snapshot;
  snapshot.elements = pending_restore_elements_;  // QDomDocument handles are implicitly shared — cheap copies
  snapshot.restore_failure_count = restore_failure_count_;
  snapshot.ever_had_content = ever_had_content_;
  return snapshot;
}

void SceneDockWidget::restorePendingRestoreState(const PendingRestoreSnapshot& snapshot) {
  pending_restore_elements_ = snapshot.elements;
  restore_failure_count_ = snapshot.restore_failure_count;
  ever_had_content_ = snapshot.ever_had_content;
}

void SceneDockWidget::markWorkspaceRestoreFailed() {
  ++restore_failure_count_;
}

void SceneDockWidget::resetWorkspaceRestoreStatus() {
  restore_failure_count_ = 0;
}

void SceneDockWidget::clearPendingRestores() {
  if (pending_restore_elements_.empty()) {
    return;
  }
  pending_restore_elements_.clear();
  emit pendingRestoresChanged();
}

LayerFactory& SceneDockWidget::layerFactory() {
  return factory_;
}

const LayerFactory& SceneDockWidget::layerFactory() const {
  return factory_;
}

SessionManager* SceneDockWidget::sessionManager() const {
  return session_;
}

QString SceneDockWidget::datasetSourceName(const SessionManager* session, DatasetId dataset_id) {
  if (session == nullptr) {
    return {};
  }
  const PJ::DatasetInfo* info = const_cast<SessionManager*>(session)->dataEngine().getDataset(dataset_id);
  return info != nullptr ? QString::fromStdString(info->source_name) : QString();
}

std::optional<DatasetId> SceneDockWidget::resolveObjectDataset(
    DatasetId saved_id, const QString& saved_source, const QString& saved_path, const QString& topic_name) const {
  if (object_dataset_resolver_) {
    return object_dataset_resolver_(saved_id, saved_source, saved_path, topic_name);
  }
  // Default policy: the session's ambiguity-safe object identity ladder. An
  // injected resolver overrides it (custom host policies, tests); without a
  // session the restore stays pending.
  if (session_ == nullptr) {
    return std::nullopt;
  }
  const DatasetIdentityResolution resolved =
      session_->resolveObjectDatasetIdentity(saved_id, saved_source, saved_path, topic_name);
  return resolved.id;
}

QString SceneDockWidget::xmlTag() const {
  return u"scene"_s;
}

bool SceneDockWidget::handleSceneConfigTopic(
    ObjectTopicId /*topic_id*/, sdk::BuiltinObjectType /*object_type*/, const QString& /*title*/) {
  return false;
}

void SceneDockWidget::refreshView() {}

void SceneDockWidget::ensureSceneViewCreated() {
  if (scene_view_ != nullptr) {
    return;
  }
  auto* layout = qobject_cast<QVBoxLayout*>(this->layout());
  if (layout == nullptr) {
    layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
  }
  scene_view_ = createSceneView();
  if (scene_view_ == nullptr) {
    scene_view_ = new QWidget(this);
  }
  scene_view_->setParent(this);
  scene_view_->setContentsMargins(0, 0, 0, 0);
  scene_view_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  layout->addWidget(scene_view_);
}

PJ::Timepoint SceneDockWidget::clampToLayerRange(PJ::Timepoint time) const {
  // Latched-layer rule (semantics ported from pj_scene3d_core's
  // clampTrackerTimeToRanges, which the 3D dock used before migrating onto this
  // base): a *spanning* layer (first < last) bounds both ends; a *latched /
  // one-shot* layer (first == last, e.g. a map pinned to the recording start)
  // is valid from its stamp ONWARD — it lowers `lo` but must NOT cap `hi`, or
  // its lone early stamp would drag the live playhead backwards and hide
  // everything keyed to "now" (the old "TF doesn't render unless another topic
  // is present" bug). With no spanning layer there is no upper bound; with no
  // usable range the time passes through unchanged. `lo`/`hi` are guarded by
  // have_lo/have_hi — never a default Timepoint (which is epoch, not a bound).
  bool have_lo = false;
  bool have_hi = false;
  PJ::Timepoint lo{};
  PJ::Timepoint hi{};
  for (const auto& [key, layer] : layers_) {
    if (layer == nullptr) {
      continue;
    }
    const auto range = layer->timeRange();
    const PJ::Timepoint first = range.min;
    const PJ::Timepoint last = range.max;
    if (last < first) {
      continue;  // inverted: no data
    }
    if (!have_lo || first < lo) {
      lo = first;
      have_lo = true;
    }
    if (last > first && (!have_hi || last > hi)) {
      hi = last;
      have_hi = true;
    }
  }
  if (!have_lo) {
    return time;
  }
  if (time < lo) {
    return lo;
  }
  if (have_hi && time > hi) {
    return hi;
  }
  return time;
}

std::vector<ISceneLayer*> SceneDockWidget::orderedLayerPtrs() const {
  std::vector<ISceneLayer*> ordered;
  ordered.reserve(draw_order_.size());
  for (const int64_t key : draw_order_) {
    const auto it = layers_.find(key);
    if (it != layers_.end() && it->second != nullptr) {
      ordered.push_back(it->second.get());
    }
  }
  return ordered;
}

void SceneDockWidget::syncViewLayers() {
  syncViewLayers(orderedLayerPtrs());
}

void SceneDockWidget::clearLayers() {
  if (layers_.empty()) {
    return;
  }
  invalidateTrackerRenderKey();
  // Contract (same ordering removeTopic now uses): re-point the view off every
  // layer (a sync with an empty draw order) while the layers are still alive, so
  // the view runs each layer's releaseGL() under its current context BEFORE any
  // detach. Only then detach (which, for GL-owning layers, tears down GL-bearing
  // caches with no context current). The retired layers are destroyed at scope exit.
  auto retired = std::move(layers_);
  layers_.clear();
  draw_order_.clear();
  layer_visibility_cache_.clear();
  syncViewLayers();
  refreshView();
  for (auto& [key, layer] : retired) {
    if (layer != nullptr) {
      const ObjectTopicId topic_id = layer->info().topic_id;
      layer->detach();
      emit layerRemoved(topic_id);
    }
  }
}

void SceneDockWidget::notifyWorkspaceChanged() {
  if (!restoring_state_) {
    emit workspaceChanged();
  }
}

void SceneDockWidget::destroyLayersUnsynced() {
  for (auto& [key, layer] : layers_) {
    if (layer != nullptr) {
      layer->detach();
    }
  }
  layers_.clear();
  draw_order_.clear();
  layer_visibility_cache_.clear();
}

void SceneDockWidget::recordLayerVisibility(ObjectTopicId topic_id, bool visible) {
  const int64_t key = topicKey(topic_id);
  auto it = layer_visibility_cache_.find(key);
  if (it != layer_visibility_cache_.end() && it->second == visible) {
    return;
  }
  layer_visibility_cache_[key] = visible;
  emit layerVisibilityChanged(topic_id, visible);
}

}  // namespace PJ
