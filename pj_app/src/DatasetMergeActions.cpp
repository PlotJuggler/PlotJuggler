// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "DatasetMergeActions.h"

#include <QCoreApplication>
#include <algorithm>
#include <set>
#include <string>
#include <unordered_map>

#include "pj_base/builtin/builtin_object.hpp"  // sdk::name(BuiltinObjectType) for the conflict dialog
#include "pj_datastore/engine.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_datastore/topic_storage.hpp"
#include "pj_runtime/AppSession.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/SessionManager.h"
#include "pj_widgets/MessageBox.h"
using namespace Qt::StringLiterals;

namespace PJ {
namespace {

QString tr(const char* text) {
  return QCoreApplication::translate("DatasetMergeActions", text);
}

}  // namespace

QString composeDatasetMergeWarning(const AppSession& session, const std::vector<DatasetId>& datasets) {
  struct Info {
    QString name;
    qint64 lo = 0;  // displayed start (raw_min - source display offset)
    qint64 hi = 0;  // displayed end
  };

  // Dataset names from the catalog (id → name).
  std::unordered_map<DatasetId, QString> names;
  for (const auto& [id, name] : session.catalogModel().datasets()) {
    names[id] = name;
  }

  SessionManager& sessions = session.sessionManager();
  std::unordered_map<DatasetId, Info> info;
  for (const DatasetId ds : datasets) {
    const auto bounds = session.datasetRawTimeRange(ds);
    if (!bounds) {
      continue;  // no time-bearing data → not a merge participant
    }
    const qint64 offset = sessions.sourceDisplayOffset(ds).value.count();
    info[ds] =
        Info{names.count(ds) != 0 ? names.at(ds) : QString::number(ds), bounds->min - offset, bounds->max - offset};
  }

  // Precompute each selected dataset's topic-name set once; the pairwise loop
  // below would otherwise recompute a dataset's set for every pair it appears in.
  DataEngine& engine = sessions.dataEngine();
  std::unordered_map<DatasetId, std::set<std::string>> topic_names_by_dataset;
  for (const DatasetId ds : datasets) {
    std::set<std::string>& topics = topic_names_by_dataset[ds];
    for (const TopicId tid : engine.listTopics(ds)) {
      if (const TopicStorage* st = engine.getTopicStorage(tid)) {
        topics.insert(st->descriptor().name);
      }
    }
  }

  // Pairwise: displayed-range overlap, and overlap that also shares a topic name.
  std::set<DatasetId> overlapping;
  std::set<DatasetId> colliding;
  for (std::size_t i = 0; i < datasets.size(); ++i) {
    for (std::size_t j = i + 1; j < datasets.size(); ++j) {
      const auto ai = info.find(datasets[i]);
      const auto bi = info.find(datasets[j]);
      if (ai == info.end() || bi == info.end()) {
        continue;
      }
      const Info& a = ai->second;
      const Info& b = bi->second;
      if (a.lo <= b.hi && b.lo <= a.hi) {
        overlapping.insert(datasets[i]);
        overlapping.insert(datasets[j]);
        const std::set<std::string>& names_a = topic_names_by_dataset.at(datasets[i]);
        const std::set<std::string>& names_b = topic_names_by_dataset.at(datasets[j]);
        if (std::any_of(
                names_a.begin(), names_a.end(), [&names_b](const std::string& n) { return names_b.count(n) != 0; })) {
          colliding.insert(datasets[i]);
          colliding.insert(datasets[j]);
        }
      }
    }
  }

  std::set<DatasetId> with_objects;
  for (const DatasetId ds : datasets) {
    if (!sessions.objectStore().listTopics(ds).empty()) {
      with_objects.insert(ds);
    }
  }

  // Each caveat is a lead sentence followed by the affected file names as an HTML
  // bullet list (the <ul> gives the space-before/space-after). MessageBox's body
  // QLabel is AutoText, so this renders as rich text. Names are HTML-escaped.
  const auto bullet_list = [&info](const std::set<DatasetId>& set) {
    QString html = u"<ul>"_s;
    for (const DatasetId ds : set) {
      const auto it = info.find(ds);
      const QString name = it != info.end() ? it->second.name : QString::number(ds);
      html += u"<li>"_s + name.toHtmlEscaped() + u"</li>"_s;
    }
    return html + u"</ul>"_s;
  };
  const auto section = [&bullet_list](const QString& lead, const std::set<DatasetId>& set) {
    return u"<p>"_s + lead + u"</p>"_s + bullet_list(set);
  };

  QString text = u"<p>"_s + tr("Merging datasets is a destructive operation. Do you wish to proceed?") + u"</p>"_s;
  if (!overlapping.empty()) {
    text += section(tr("The following datasets overlap in time:"), overlapping);
  }
  if (!colliding.empty()) {
    text += section(tr("The following datasets have colliding data:"), colliding);
  }
  if (!with_objects.empty()) {
    text +=
        section(tr("The following datasets contain object topics that will be merged into the result:"), with_objects);
  }
  return text;
}

std::optional<DatasetId> confirmAndMergeDatasets(
    QWidget* parent, AppSession& session, const std::vector<DatasetId>& ids) {
  // Keep only data-bearing datasets; need at least two to merge.
  std::vector<DatasetId> datasets;
  datasets.reserve(ids.size());
  for (const DatasetId id : ids) {
    if (session.datasetRawTimeRange(id)) {
      datasets.push_back(id);
    }
  }
  if (datasets.size() < 2) {
    return std::nullopt;
  }

  const DatasetId active_streaming_dataset = session.activeStreamingDataset();
  if (active_streaming_dataset != 0 &&
      std::find(datasets.begin(), datasets.end(), active_streaming_dataset) != datasets.end()) {
    MessageBox::warning(
        parent, tr("Cannot merge datasets"),
        u"<p>"_s + tr("Stop streaming the live source before merging it with other datasets.") + u"</p>"_s);
    return std::nullopt;
  }

  // Canonical-type conflict gate (shared by BOTH merge entry points — timeline and
  // curve tree). If two contributors share an object-topic NAME but disagree on the
  // builtin object type, fuse-by-name would silently merge incompatible payloads, so
  // hard-stop: name the offenders and abort before any state is touched.
  if (const auto conflicts = session.objectMergeConflicts(datasets); !conflicts.empty()) {
    constexpr qsizetype kMaxConflictItems = 10;
    QString list = u"<ul>"_s;
    const auto shown = std::min<qsizetype>(static_cast<qsizetype>(conflicts.size()), kMaxConflictItems);
    for (qsizetype i = 0; i < shown; ++i) {
      const ObjectMergeConflict& c = conflicts[static_cast<std::size_t>(i)];
      // Same type means the payload formats differ: name those instead.
      const auto describe = [&c](sdk::BuiltinObjectType type, const std::string& format) {
        if (c.anchor_type != c.source_type) {
          return QString::fromUtf8(sdk::name(type));
        }
        return format.empty() ? tr("canonical %1").arg(QString::fromUtf8(sdk::name(type)))
                              : QString::fromStdString(format).toHtmlEscaped();
      };
      list += u"<li>"_s +
              tr("\"%1\": %2 vs %3")
                  .arg(
                      QString::fromStdString(c.topic_name).toHtmlEscaped(), describe(c.anchor_type, c.anchor_format),
                      describe(c.source_type, c.source_format)) +
              u"</li>"_s;
    }
    if (const qsizetype remaining = static_cast<qsizetype>(conflicts.size()) - shown; remaining > 0) {
      list += u"<li>"_s + tr("...and %1 more").arg(remaining) + u"</li>"_s;
    }
    list += u"</ul>"_s;
    MessageBox::warning(
        parent, tr("Cannot merge datasets"),
        u"<p>"_s +
            tr("These object topics share a name but have incompatible types or formats, so the datasets cannot be "
               "merged:") +
            u"</p>"_s + list + u"<p>"_s + tr("Remove or rename the conflicting topic and try again.") + u"</p>"_s);
    return std::nullopt;
  }

  const int choice = MessageBox::question(
      parent, tr("Merge datasets"), composeDatasetMergeWarning(session, datasets),
      {{tr("Merge"), MessageBox::kDestructiveRole}, {tr("Cancel"), MessageBox::kCancelRole}});
  if (choice != 0) {
    return std::nullopt;  // Cancel / Esc
  }

  const DatasetId anchor = session.mergeDatasets(datasets);
  if (anchor == 0) {
    return std::nullopt;
  }
  return anchor;
}

}  // namespace PJ
