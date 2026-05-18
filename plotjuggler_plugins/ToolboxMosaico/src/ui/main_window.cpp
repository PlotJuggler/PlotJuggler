/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

// SDK headers before Qt (arrow/flight/api.h vs Qt signals macro).
#include <flight/mosaico_client.hpp>
#include "main_window.h"

#include "../core/time_format.h"
#include "download_stats_dialog.h"
#include "elided_label.h"
#include "fetch_worker.h"
#include "format_utils.h"
#include "metadata/data_view_panel.h"
#include "metadata/query_bar.h"
#include <PlotJuggler/range_slider.h>
#include "security/cert_dialog.h"
#include "security/tls_utils.h"
#include "sequence/sequence_panel.h"
#include "server_history.h"
#include "theme_utils.h"
#include "topic/topic_panel.h"

#include <PlotJuggler/svg_util.h>

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QSplitter>
#include <QThread>
#include <QDebug>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <exception>
#include <set>
#include <utility>

static constexpr int kConnectionTimeoutSec = 30;
static constexpr int kConnectionPoolSize = 4;

namespace
{
// Cross-thread queued slots and user-event handlers must not let any
// exception escape back into Qt's event loop — that would terminate the
// host application. Any uncaught throw lands here, logs, and updates the
// plugin's status label so the failure is visible without crashing
// PlotJuggler.
template <typename Fn>
void guardedSlot(ElidedLabel* status, const char* where, Fn&& fn)
{
  try
  {
    std::forward<Fn>(fn)();
  }
  catch (const std::exception& e)
  {
    qWarning() << "[Mosaico]" << where << "exception:" << e.what();
    if (status)
    {
      status->setText(QStringLiteral("Mosaico error (%1): %2")
                          .arg(QString::fromUtf8(where), QString::fromUtf8(e.what())));
    }
  }
  catch (...)
  {
    qWarning() << "[Mosaico]" << where << "exception: unknown";
    if (status)
    {
      status->setText(QStringLiteral("Mosaico error (%1): unknown").arg(QString::fromUtf8(where)));
    }
  }
}

// Warning dialog whose message text is selectable and copyable. The default
// QMessageBox::warning() renders the body as a static label, which means
// long gRPC/server errors can't be copied for bug reports. This variant
// sets text-interaction flags so the user can drag-select + Ctrl+C, and
// also adds a dedicated "Copy to clipboard" button.
void showCopyableWarning(QWidget* parent, const QString& title, const QString& message)
{
  QMessageBox box(QMessageBox::Warning, title, message, QMessageBox::Ok, parent);
  box.setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
  auto* copy_btn = box.addButton("Copy to clipboard", QMessageBox::ActionRole);
  QObject::connect(copy_btn, &QPushButton::clicked,
                   [message]() { QApplication::clipboard()->setText(message); });
  box.setDefaultButton(QMessageBox::Ok);
  box.exec();
}

// The plugin now owns the URI scheme: the user only edits `host:port`. If
// the user pastes or types a legacy `grpc[+tls]://` / `tls+grpc://` prefix,
// we silently strip it so the rest of the pipeline has a canonical value.
QString stripUriScheme(const QString& s)
{
  const QString trimmed = s.trimmed();
  static const char* schemes[] = { "grpc+tls://", "tls+grpc://", "grpc://" };
  for (const char* p : schemes)
  {
    if (trimmed.startsWith(QLatin1String(p), Qt::CaseInsensitive))
    {
      return trimmed.mid(int(qstrlen(p)));
    }
  }
  return trimmed;
}
}  // namespace

MainWindow::MainWindow(QWidget* parent) : QWidget(parent)
{
  buildLayout();
  connectSignals();

  rebuildServerCombo();
  // Don't connect to server here — wait until the plugin is shown.
}

MainWindow::~MainWindow()
{
  if (worker_thread_)
  {
    worker_thread_->quit();
    worker_thread_->wait();
  }
}

// ---------------------------------------------------------------------------
void MainWindow::buildLayout()
{
  auto* uri_row = new QHBoxLayout();

  // Uniform height across the whole row so combo, text button, and icon
  // buttons line up. 28px fits a standard QLineEdit comfortably on every
  // common theme; the icon is 8px smaller so there's 4px padding each side.
  constexpr int kRowH = 28;
  constexpr int kIconSz = 20;

  auto* uri_label = new QLabel("Server:", this);
  uri_label->setFixedWidth(50);

  server_uri_combo_ = new QComboBox(this);
  server_uri_combo_->setEditable(true);
  server_uri_combo_->setInsertPolicy(QComboBox::NoInsert);
  server_uri_combo_->lineEdit()->setPlaceholderText("host:port");
  server_uri_combo_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  server_uri_combo_->setFixedHeight(kRowH);

  connect_button_ = new QPushButton("Connect", this);
  // Minimum (not fixed) so the wider "Connecting…" state doesn't get clipped.
  connect_button_->setMinimumWidth(70);
  connect_button_->setFixedHeight(kRowH);

  auto theme = themeName();
  auto make_icon_btn = [&](const QString& svg_path, const QString& tooltip) {
    auto* btn = new QPushButton(this);
    btn->setIcon(LoadSvg(svg_path, theme));
    btn->setIconSize(QSize(kIconSz, kIconSz));
    btn->setFlat(true);
    btn->setFixedSize(kRowH, kRowH);
    btn->setToolTip(tooltip);
    return btn;
  };

  auto* back_button = make_icon_btn(":/resources/svg/left-arrow.svg", "Back");
  connect(back_button, &QPushButton::clicked, this, &MainWindow::backRequested);

  refresh_button_ = make_icon_btn(":/resources/svg/reload.svg", "Refresh sequences");
  refresh_button_->setEnabled(false);

  // Certificate/API-key entry — opens the cert dialog. Uses the plugin's
  // own SVG rather than the theme's.
  cert_button_ = new QPushButton(this);
  cert_button_->setFlat(true);
  cert_button_->setFixedSize(kRowH, kRowH);
  cert_button_->setIconSize(QSize(kIconSz, kIconSz));
  cert_button_->setToolTip(
      "Certificate & API key (cert optional — system trust store used by default)");
  QPixmap cert_pix(":/mosaico/certificate.svg");
  if (!cert_pix.isNull())
  {
    cert_button_->setIcon(QIcon(cert_pix));
  }
  else
  {
    cert_button_->setText("C");
  }

  uri_row->addWidget(back_button);
  uri_row->addWidget(uri_label);
  uri_row->addWidget(server_uri_combo_, /*stretch=*/1);
  uri_row->addWidget(connect_button_);
  uri_row->addWidget(cert_button_);
  uri_row->addWidget(refresh_button_);

  connect(cert_button_, &QPushButton::clicked, this, &MainWindow::onCertButtonClicked);

  sequence_panel_ = new SequencePanel(this);
  query_bar_ = new QueryBar(this);

  // Wrap SequencePanel + QueryBar in a container.
  auto* seq_container = new QWidget(this);
  auto* seq_layout = new QVBoxLayout(seq_container);
  seq_layout->addWidget(sequence_panel_, /*stretch=*/1);
  seq_layout->addWidget(query_bar_);

  topic_panel_ = new TopicPanel(this);
  data_view_panel_ = new DataViewPanel(this);

  top_splitter_ = new QSplitter(Qt::Horizontal, this);
  top_splitter_->addWidget(seq_container);
  top_splitter_->addWidget(topic_panel_);
  top_splitter_->addWidget(data_view_panel_);
  top_splitter_->setStretchFactor(0, 1);
  top_splitter_->setStretchFactor(1, 1);
  top_splitter_->setStretchFactor(2, 1);

  // --- Range slider row ---
  range_slider_ = new RangeSlider(Qt::Horizontal, RangeSlider::DoubleHandles, this);
  range_slider_->SetRange(0, kSliderSteps);
  range_slider_->SetLowerValue(0);
  range_slider_->SetUpperValue(kSliderSteps);
  range_slider_->setShowTicks(false);
  range_slider_->setShowTickLabels(false);
  range_slider_->setShowHandleValueTooltip(false);
  range_slider_->setFloatingLabelsVisible(true);
  range_slider_->setLabelFormatter([](double) -> QString { return QStringLiteral("-"); });
  range_slider_->setCenterLabelFormatter([](double, double) -> QString { return QString(); });
  range_slider_->setMinimumHeight(range_slider_->minimumSizeHint().height());
  range_slider_->setEnabled(false);

  fetch_button_ = new QPushButton("Download", this);
  fetch_button_->setToolTip("Download selected topics");
  fetch_button_->setEnabled(false);
  fetch_button_->setMinimumWidth(80);

  auto* slider_row = new QWidget(this);
  auto* slider_layout = new QHBoxLayout(slider_row);
  slider_layout->addWidget(range_slider_, /*stretch=*/1);
  slider_layout->addWidget(fetch_button_);

  // --- Root layout (VBoxLayout so slider_row never collapses) ---
  auto* vlayout = new QVBoxLayout(this);
  vlayout->addLayout(uri_row);
  vlayout->addWidget(top_splitter_, /*stretch=*/1);
  vlayout->addWidget(slider_row);

  status_label_ = new ElidedLabel(this);
  vlayout->addWidget(status_label_);

  setStatus("Click Connect to load sequences");
}

// ---------------------------------------------------------------------------
void MainWindow::connectSignals()
{
  connect(sequence_panel_, &SequencePanel::sequenceSelected, this, &MainWindow::onSequenceSelected);
  connect(topic_panel_, &TopicPanel::topicsSelected, this, &MainWindow::onTopicsSelected);
  connect(fetch_button_, &QPushButton::clicked, this, &MainWindow::onFetchClicked);
  connect(connect_button_, &QPushButton::clicked, this, &MainWindow::onConnectClicked);
  connect(query_bar_, &QueryBar::queryChanged, this, &MainWindow::onQueryChanged);
  connect(refresh_button_, &QPushButton::clicked, this, &MainWindow::onRefreshClicked);
  connect(server_uri_combo_, qOverload<int>(&QComboBox::activated), this,
          &MainWindow::onServerSelected);
}

// ---------------------------------------------------------------------------
void MainWindow::ensureWorkerThread()
{
  if (worker_thread_)
  {
    return;
  }

  worker_thread_ = new QThread(this);
  worker_ = new FetchWorker();
  worker_->moveToThread(worker_thread_);

  connect(worker_, &FetchWorker::sequenceListStarted, this, &MainWindow::onSequenceListStarted);
  connect(worker_, &FetchWorker::sequenceInfoReady, this, &MainWindow::onSequenceInfoReady);
  connect(worker_, &FetchWorker::sequencesReady, this, &MainWindow::onSequencesReady);
  connect(worker_, &FetchWorker::topicsReady, this, &MainWindow::onTopicsReady);
  connect(worker_, &FetchWorker::topicMetadataReady, this, &MainWindow::onTopicMetadataReady);
  connect(worker_, &FetchWorker::dataReady, this, &MainWindow::onDataReady);
  connect(worker_, &FetchWorker::topicStreamStarted, this, &MainWindow::onTopicStreamStarted,
          Qt::BlockingQueuedConnection);
  connect(worker_, &FetchWorker::topicBatchReady, this, &MainWindow::onTopicBatchReady,
          Qt::BlockingQueuedConnection);
  connect(worker_, &FetchWorker::topicStreamFinished, this, &MainWindow::onTopicStreamFinished);
  connect(worker_, &FetchWorker::fetchProgress, this, &MainWindow::onFetchProgress);
  connect(worker_, &FetchWorker::topicErrorOccurred, this, &MainWindow::onTopicFetchError);
  connect(worker_, &FetchWorker::errorOccurred, this, &MainWindow::onFetchError);

  connect(worker_thread_, &QThread::finished, worker_, &QObject::deleteLater);

  worker_thread_->start();
}

// ---------------------------------------------------------------------------
void MainWindow::setSequenceRange(int64_t min_ts_ns, int64_t max_ts_ns)
{
  seq_min_ns_ = min_ts_ns;
  seq_max_ns_ = max_ts_ns;
  long_format_ = needsLongFormat(max_ts_ns - min_ts_ns);

  // Restore proportional positions from previous sequence.
  int lower = static_cast<int>(lower_proportion_ * kSliderSteps);
  int upper = static_cast<int>(upper_proportion_ * kSliderSteps);
  range_slider_->SetLowerValue(lower);
  range_slider_->SetUpperValue(upper);
  range_slider_->setEnabled(true);

  // Handle labels: relative time offset from sequence start.
  range_slider_->setLabelFormatter([this](double raw_pos) -> QString {
    int64_t ns = sliderToNs(static_cast<int>(raw_pos));
    int64_t offset_ns = ns - seq_min_ns_;
    return QString::fromStdString(formatDuration(offset_ns));
  });

  // Center label: total selected duration.
  range_slider_->setCenterLabelFormatter([this](double lower_pos, double upper_pos) -> QString {
    int64_t lo = sliderToNs(static_cast<int>(lower_pos));
    int64_t hi = sliderToNs(static_cast<int>(upper_pos));
    return QString::fromStdString(formatDuration(hi - lo));
  });

  range_slider_->update();
}

int64_t MainWindow::sliderToNs(int pos) const
{
  double fraction = static_cast<double>(pos) / kSliderSteps;
  return seq_min_ns_ + static_cast<int64_t>(fraction * (seq_max_ns_ - seq_min_ns_));
}

// ---------------------------------------------------------------------------
void MainWindow::onSequenceListStarted(const std::vector<SequenceInfo>& sequences)
{
  guardedSlot(status_label_, "onSequenceListStarted", [&]() {
    if (error_context_ != ErrorContext::ExplicitConnect &&
        error_context_ != ErrorContext::AutoConnect)
    {
      return;
    }

    all_sequences_ = sequences;
    sequence_panel_->populateSequences(sequences);
    sequence_panel_->setMetadataLoadingProgress(0, static_cast<qint64>(sequences.size()));
    setStatus(QString("Connected - loading details for %1 sequence(s)").arg(sequences.size()));
  });
}

void MainWindow::onSequenceInfoReady(const SequenceInfo& sequence, qint64 completed, qint64 total)
{
  guardedSlot(status_label_, "onSequenceInfoReady", [&]() {
    if (error_context_ != ErrorContext::ExplicitConnect &&
        error_context_ != ErrorContext::AutoConnect)
    {
      return;
    }

    auto it = std::find_if(all_sequences_.begin(), all_sequences_.end(),
                           [&](const SequenceInfo& seq) { return seq.name == sequence.name; });
    if (it == all_sequences_.end())
    {
      all_sequences_.push_back(sequence);
    }
    else
    {
      *it = sequence;
    }

    sequence_panel_->updateSequence(sequence);
    sequence_panel_->setMetadataLoadingProgress(completed, total);
    setStatus(QString("Loading sequence details %1/%2").arg(completed).arg(total));
  });
}

// ---------------------------------------------------------------------------
void MainWindow::onSequencesReady(const std::vector<SequenceInfo>& sequences)
{
  guardedSlot(status_label_, "onSequencesReady", [&]() {
    // Persist connection details now that we know they work. Update
    // history first so saveState sees the promoted list and we don't
    // write the old list to disk only to overwrite it moments later.
    addServerToHistory(normalizeServerKey(currentServerText()));
    writeCachedCredentials(currentServerKey());
    QSettings settings;
    saveState(settings);

    all_sequences_ = sequences;
    sequence_panel_->setLoading(false);
    connect_button_->setEnabled(true);
    connect_button_->setText("Connect");
    refresh_button_->setEnabled(true);

    error_context_ = ErrorContext::None;
    connection_mode_ =
        attempted_plaintext_fallback_ ? ConnectionMode::Insecure : ConnectionMode::Secure;
    setStatus(QString("Connected \u2014 %1 sequence(s)").arg(sequences.size()));

    // Build metadata schema for the query bar.
    std::map<std::string, std::set<std::string>> key_values_set;
    for (const auto& seq : sequences)
    {
      for (const auto& [key, value] : seq.user_metadata)
      {
        key_values_set[key].insert(value);
      }
    }
    Schema schema;
    for (auto& [key, values_set] : key_values_set)
    {
      schema[key] = std::vector<std::string>(values_set.begin(), values_set.end());
    }
    query_bar_->setSchema(schema);
    emit schemaReady(schema);

    // Re-evaluate the restored query (if any) against the newly loaded sequences.
    // Without this, a query restored from QSettings would not be evaluated until
    // the user edits it, because restoreState() runs before sequences are available.
    auto current_query = query_bar_->query();
    if (!current_query.isEmpty())
    {
      onQueryChanged(current_query, Engine::validate(current_query.toStdString()).valid);
    }
  });
}

void MainWindow::onSequenceSelected(const QString& sequence_name)
{
  selected_sequence_ = sequence_name;
  selected_topics_.clear();

  topic_panel_->setEnabled(true);
  data_view_panel_->setEnabled(true);

  topic_panel_->clear();
  topic_panel_->setCurrentSequence(sequence_name);
  topic_panel_->setLoading(true);
  data_view_panel_->clear();
  fetch_button_->setEnabled(false);

  auto name_std = sequence_name.toStdString();
  for (const auto& seq : all_sequences_)
  {
    if (seq.name == name_std)
    {
      data_view_panel_->showSequenceInfo(seq);
      setSequenceRange(seq.min_ts_ns, seq.max_ts_ns);
      break;
    }
  }

  setStatus("Loading topics for " + sequence_name + QString::fromUtf8("\u2026"));
  QMetaObject::invokeMethod(worker_, "fetchTopics", Qt::QueuedConnection,
                            Q_ARG(QString, sequence_name));
}

void MainWindow::onTopicsReady(const QStringList& names, const std::vector<TopicInfo>& infos)
{
  guardedSlot(status_label_, "onTopicsReady", [&]() {
    cached_topic_infos_ = infos;
    topic_panel_->setLoading(false);
    topic_panel_->populateTopics(infos);
    if (names.isEmpty())
    {
      setStatus("No topics returned \u2014 enter a topic name manually if needed");
    }
    else
    {
      setStatus(QString("%1 topic(s) available").arg(names.size()));
    }
  });
}

void MainWindow::onTopicsSelected(const QString& /*sequence_name*/, const QStringList& topic_names)
{
  if (pending_fetches_ > 0)
  {
    return;
  }

  selected_topics_ = topic_names;

  data_view_panel_->clearTopics();
  fetch_button_->setEnabled(!topic_names.isEmpty());

  // Render each selected topic's info pane. listTopics gave us name +
  // timestamp range cheaply; the schema/ontology/user_metadata require a
  // per-topic GetFlightInfo, so we fire those lazily and merge the response
  // into the cache when it arrives.
  for (const auto& topic : topic_names)
  {
    const QString cache_key = selected_sequence_ + "/" + topic;
    auto cached = topic_metadata_cache_.constFind(cache_key);
    if (cached != topic_metadata_cache_.constEnd())
    {
      data_view_panel_->showTopicInfo(*cached);
      continue;
    }

    // Show whatever we already have (topic_name + ts range) so the pane
    // isn't blank while the metadata RPC is in flight.
    auto name_std = topic.toStdString();
    for (const auto& info : cached_topic_infos_)
    {
      if (info.topic_name == name_std)
      {
        data_view_panel_->showTopicInfo(info);
        break;
      }
    }

    if (!worker_)
    {
      continue;
    }
    if (topic_metadata_in_flight_.contains(cache_key))
    {
      continue;
    }
    topic_metadata_in_flight_.insert(cache_key);
    QMetaObject::invokeMethod(worker_, "fetchTopicMetadata", Qt::QueuedConnection,
                              Q_ARG(QString, selected_sequence_), Q_ARG(QString, topic));
  }

  if (topic_names.size() == 1)
  {
    setStatus("Selected " + topic_names.first() +
              QString::fromUtf8(" \u2014 adjust range and click Fetch"));
  }
  else
  {
    setStatus(
        QString("Selected %1 topics \u2014 adjust range and click Fetch").arg(topic_names.size()));
  }
}

void MainWindow::onTopicMetadataReady(const QString& sequence_name, const QString& topic_name,
                                      const TopicInfo& info)
{
  guardedSlot(status_label_, "onTopicMetadataReady", [&]() {
    const QString cache_key = sequence_name + "/" + topic_name;
    topic_metadata_cache_.insert(cache_key, info);
    topic_metadata_in_flight_.remove(cache_key);

    // If this topic isn't in the user's current selection (or they switched
    // sequences while the RPC was in flight) the cache still benefits the
    // next click — but no UI refresh is needed.
    if (sequence_name != selected_sequence_)
    {
      return;
    }
    if (!selected_topics_.contains(topic_name))
    {
      return;
    }

    // Re-render every selected topic from the (possibly partially-populated)
    // cache. This is the simplest correct merge: the panel currently shows
    // the appended-text format, and we want the freshly-arrived schema to
    // replace its corresponding placeholder block.
    data_view_panel_->clearTopics();
    for (const auto& selected : selected_topics_)
    {
      const QString key = sequence_name + "/" + selected;
      auto cached = topic_metadata_cache_.constFind(key);
      if (cached != topic_metadata_cache_.constEnd())
      {
        data_view_panel_->showTopicInfo(*cached);
        continue;
      }
      auto name_std = selected.toStdString();
      for (const auto& partial : cached_topic_infos_)
      {
        if (partial.topic_name == name_std)
        {
          data_view_panel_->showTopicInfo(partial);
          break;
        }
      }
    }
  });
}

void MainWindow::onDataReady(const QString& sequence_name, const QString& topic_name,
                             const PullResult& result)
{
  guardedSlot(status_label_, "onDataReady", [&]() {
    if (!cancelling_fetch_)
    {
      emit mosaicoTopicStarted(sequence_name, topic_name, result.schema);
      for (const auto& batch : result.batches)
      {
        emit mosaicoTopicBatchReady(sequence_name, topic_name, batch);
      }
      emit mosaicoTopicFinished(sequence_name, topic_name);
    }
    finishFetchTopic(topic_name, true);
  });
}

void MainWindow::onTopicStreamStarted(const QString& sequence_name, const QString& topic_name,
                                      const std::shared_ptr<arrow::Schema>& schema)
{
  guardedSlot(status_label_, "onTopicStreamStarted", [&]() {
    if (!cancelling_fetch_)
    {
      emit mosaicoTopicStarted(sequence_name, topic_name, schema);
    }
  });
}

void MainWindow::onTopicBatchReady(const QString& sequence_name, const QString& topic_name,
                                   const std::shared_ptr<arrow::RecordBatch>& batch)
{
  guardedSlot(status_label_, "onTopicBatchReady", [&]() {
    if (!cancelling_fetch_)
    {
      emit mosaicoTopicBatchReady(sequence_name, topic_name, batch);
    }
  });
}

void MainWindow::onTopicStreamFinished(const QString& sequence_name, const QString& topic_name)
{
  guardedSlot(status_label_, "onTopicStreamFinished", [&]() {
    emit mosaicoTopicFinished(sequence_name, topic_name);
    finishFetchTopic(topic_name, true);
  });
}

void MainWindow::requestFetchCancel()
{
  cancelling_fetch_ = true;
  if (worker_)
  {
    worker_->requestCancel();
  }
  if (download_stats_dialog_)
  {
    download_stats_dialog_->markCancelling();
  }
  fetch_button_->setEnabled(false);
  setStatus(QStringLiteral("Cancelling..."));
}

void MainWindow::finishFetchTopic(const QString& topic_name, bool success)
{
  const bool batch_active = error_context_ == ErrorContext::Fetch;
  if (!topic_name.isEmpty())
  {
    if (completed_fetch_topics_.contains(topic_name))
    {
      return;
    }
    completed_fetch_topics_.insert(topic_name);
  }
  if (pending_fetches_ > 0)
  {
    --pending_fetches_;
  }
  if (download_stats_dialog_ && !topic_name.isEmpty())
  {
    download_stats_dialog_->markFinished(topic_name, cancelling_fetch_ ?
                                                         QStringLiteral("Cancelled") :
                                                     success ? QStringLiteral("Done") :
                                                               QStringLiteral("Failed"));
  }
  if (pending_fetches_ <= 0)
  {
    if (batch_active)
    {
      finishFetchBatch();
    }
  }
}

void MainWindow::finishFetchBatch()
{
  fetch_button_->setText("Download");
  fetch_button_->setToolTip("Download selected topics");
  fetch_button_->setEnabled(!selected_topics_.isEmpty());

  error_context_ = ErrorContext::None;
  if (client_)
  {
    refresh_button_->setEnabled(true);
  }
  if (download_stats_dialog_)
  {
    const QString unfinished_status = cancelling_fetch_ ? QStringLiteral("Cancelled") :
                                      pending_fetch_errors_.isEmpty() ? QStringLiteral("Done") :
                                                                        QStringLiteral("Failed");
    download_stats_dialog_->markComplete(unfinished_status);
  }

  if (cancelling_fetch_)
  {
    cancelling_fetch_ = false;
    if (worker_)
    {
      worker_->resetCancel();
    }
    setStatus(QStringLiteral("Download cancelled"));
    emit fetchCancelled();
  }
  else
  {
    setStatus(QStringLiteral("Loaded selected data"));
    emit allFetchesComplete();
  }

  if (!pending_fetch_errors_.isEmpty())
  {
    int total = 0;
    QStringList lines;
    lines.reserve(pending_fetch_errors_.size());
    for (auto it = pending_fetch_errors_.constBegin(); it != pending_fetch_errors_.constEnd(); ++it)
    {
      total += it.value();
      lines << (it.value() > 1 ? QStringLiteral("  [%1x] %2").arg(it.value()).arg(it.key()) :
                                 QStringLiteral("  %1").arg(it.key()));
    }
    const QString title = total == 1 ? QStringLiteral("Fetch Error") :
                                       QStringLiteral("Fetch Errors (%1 topics)").arg(total);
    const QString body =
        total == 1 ? lines.first().trimmed() :
                     QStringLiteral("%1 topic(s) failed:\n\n%2").arg(total).arg(lines.join("\n"));
    showCopyableWarning(this, title, body);
    pending_fetch_errors_.clear();
  }
}

void MainWindow::onFetchProgress(const QString& topic_name, qint64 bytes, qint64 total_bytes)
{
  if (error_context_ != ErrorContext::Fetch || selected_topics_.isEmpty())
  {
    return;
  }
  const int total = selected_topics_.size();
  const int idx = total - pending_fetches_ + 1;

  QString body;
  if (total_bytes > 0)
  {
    const int pct = static_cast<int>((100 * bytes) / total_bytes);
    body =
        QStringLiteral("%1 / %2 (%3%)").arg(formatBytes(bytes), formatBytes(total_bytes)).arg(pct);
  }
  else
  {
    body = QStringLiteral("%1 decoded").arg(formatBytes(bytes));
  }
  setStatus(
      QStringLiteral("Fetching %1 (%2/%3): %4").arg(topic_name).arg(idx).arg(total).arg(body));
}

void MainWindow::onTopicFetchError(const QString& topic_name, const QString& message)
{
  guardedSlot(status_label_, "onTopicFetchError", [&]() {
    if (error_context_ != ErrorContext::Fetch)
    {
      onFetchError(message);
      return;
    }

    const bool cancel_drain = cancelling_fetch_;
    if (!cancel_drain)
    {
      setStatus("Error: " + message);
      pending_fetch_errors_[message]++;
    }
    finishFetchTopic(topic_name, false);
  });
}

void MainWindow::onFetchClicked()
{
  // The button doubles as a Cancel when a fetch is in flight.
  if (error_context_ == ErrorContext::Fetch)
  {
    requestFetchCancel();
    return;
  }

  if (selected_sequence_.isEmpty() || selected_topics_.isEmpty())
  {
    return;
  }

  error_context_ = ErrorContext::Fetch;
  pending_fetches_ = selected_topics_.size();
  completed_fetch_topics_.clear();
  decoded_fetch_bytes_.clear();
  pending_fetch_errors_.clear();
  cancelling_fetch_ = false;
  if (worker_)
  {
    worker_->resetCancel();
  }

  lower_proportion_ = static_cast<double>(range_slider_->GetLowerValue()) / kSliderSteps;
  upper_proportion_ = static_cast<double>(range_slider_->GetUpperValue()) / kSliderSteps;

  qint64 start_ns = sliderToNs(range_slider_->GetLowerValue());
  qint64 end_ns = sliderToNs(range_slider_->GetUpperValue());

  fetch_button_->setText("Cancel");
  fetch_button_->setToolTip("Cancel the in-flight download");
  setStatus(QString::fromUtf8("Fetching %1 topic(s)…").arg(pending_fetches_));
  if (!download_stats_dialog_)
  {
    download_stats_dialog_ = new DownloadStatsDialog(this);
    connect(download_stats_dialog_, &DownloadStatsDialog::cancelRequested, this,
            &MainWindow::requestFetchCancel);
  }
  download_stats_dialog_->start(selected_topics_);
  download_stats_dialog_->show();
  download_stats_dialog_->raise();
  // One queued call hands the whole batch to the worker, which delegates to
  // the SDK's pullTopics — that runs up to pool_size pulls in parallel
  // (kConnectionPoolSize=4). Queueing N separate fetchData calls would
  // serialize on the worker's event loop and starve the pool.
  QMetaObject::invokeMethod(
      worker_, "fetchDataMulti", Qt::QueuedConnection, Q_ARG(QString, selected_sequence_),
      Q_ARG(QStringList, selected_topics_), Q_ARG(qint64, start_ns), Q_ARG(qint64, end_ns));
}

void MainWindow::onFetchError(const QString& message)
{
  guardedSlot(status_label_, "onFetchError", [&]() {
    const bool in_connect = error_context_ == ErrorContext::ExplicitConnect ||
                            error_context_ == ErrorContext::AutoConnect;

    // Plaintext fallback path. TLS attempt just failed; retry once in
    // plaintext iff the user opted in AND there's no custom cert (a custom
    // cert means the user committed to a specific TLS setup; silently
    // falling back would defeat supplying it). Per the user's design we
    // don't classify the error — any TLS-phase failure qualifies once
    // allow_insecure_ is on, and the first attempt still gave us TLS when
    // available.
    if (in_connect && !attempted_plaintext_fallback_ && cert_path_.isEmpty() && allow_insecure_)
    {
      attempted_plaintext_fallback_ = true;
      const QString host_port = stripUriScheme(currentServerText());
      const std::string uri = (QStringLiteral("grpc://") + host_port).toStdString();
      auto new_client = std::make_unique<MosaicoClient>(
          uri, kConnectionTimeoutSec, /*pool_size=*/kConnectionPoolSize, cert_path_.toStdString(),
          api_key_.toStdString());
      QMetaObject::invokeMethod(worker_, "setClient", Qt::BlockingQueuedConnection,
                                Q_ARG(MosaicoClient*, new_client.get()));
      client_ = std::move(new_client);
      // Keep error_context_ as-is; the user still sees the connecting state.
      QMetaObject::invokeMethod(worker_, "fetchSequences", Qt::QueuedConnection);
      return;
    }

    // TLS failed and no fallback is configured. Swap the raw transport error
    // for the explicit "disabled" message in both popup and status, per the
    // user design.
    QString display_msg = message;
    const bool insecure_blocked =
        in_connect && !attempted_plaintext_fallback_ && cert_path_.isEmpty() && !allow_insecure_;
    if (insecure_blocked)
    {
      display_msg = "Connectivity to insecure channels is disabled, visit certificate settings";
    }

    const bool cancel_drain = cancelling_fetch_ && error_context_ == ErrorContext::Fetch;

    // Clear connection_mode_ on connect-phase failures so the suffix doesn't
    // lie about the current channel. Fetch errors while still connected keep
    // the suffix; a cancel-driven drain is not a real error at all.
    if (in_connect)
    {
      connection_mode_ = ConnectionMode::None;
    }

    if (!cancel_drain)
    {
      setStatus("Error: " + display_msg);
    }

    // Popup on explicit clicks and on the insecure-blocked case — but never
    // during a cancel drain; each drained topic would otherwise spam a popup.
    if (!cancel_drain && (error_context_ == ErrorContext::ExplicitConnect || insecure_blocked))
    {
      showCopyableWarning(this, "Connection Error", display_msg);
    }

    if (error_context_ == ErrorContext::Fetch)
    {
      if (!cancel_drain)
      {
        // Accumulate — flushed as a single popup once the batch settles.
        // Keyed by message so N identical failures collapse to one entry.
        pending_fetch_errors_[message]++;
      }
      finishFetchTopic(QString(), false);
    }

    if (in_connect)
    {
      connect_button_->setEnabled(true);
      connect_button_->setText("Connect");
      sequence_panel_->setLoading(false);
    }

    if (client_ && pending_fetches_ == 0)
    {
      refresh_button_->setEnabled(true);
    }

    error_context_ = ErrorContext::None;
  });
}

void MainWindow::onConnectClicked()
{
  connectToServer(/*explicit_connect=*/true);
}

void MainWindow::onRefreshClicked()
{
  if (!client_ || pending_fetches_ > 0 || error_context_ != ErrorContext::None)
  {
    return;
  }

  refresh_button_->setEnabled(false);
  sequence_panel_->setLoading(true);
  setStatus(QString::fromUtf8("Refreshing\u2026"));
  error_context_ = ErrorContext::ExplicitConnect;
  QMetaObject::invokeMethod(worker_, "fetchSequences", Qt::QueuedConnection);
}

namespace
{
// Header/URI values must be pure printable ASCII. Control bytes (CR, LF,
// NUL) are what gRPC asserts-and-aborts on, so we reject them here — at
// the earliest boundary — rather than letting them reach the SDK.
bool isPrintableAscii(const QString& s)
{
  for (QChar c : s)
  {
    ushort u = c.unicode();
    if (u < 0x20 || u > 0x7E)
    {
      return false;
    }
  }
  return true;
}

}  // namespace

void MainWindow::connectToServer(bool explicit_connect)
{
  // The user now edits only host:port — silently strip any legacy scheme
  // they may have pasted or had saved. The plugin owns scheme selection.
  const QString host_port = stripUriScheme(currentServerText());
  if (host_port.isEmpty())
  {
    return;
  }
  if (host_port != currentServerText().trimmed())
  {
    setCurrentServerText(host_port);
  }

  // Guard: don't replace the client while any RPC is in flight.
  if (pending_fetches_ > 0 || error_context_ != ErrorContext::None)
  {
    return;
  }

  const auto fail = [&](const QString& message) {
    qWarning() << "[Mosaico] connect rejected:" << message;
    // Stale connection_mode_ from a previous session could misleadingly
    // suffix this error with "(secure connection)". Clear it first.
    connection_mode_ = ConnectionMode::None;
    setStatus("Error: " + message);
    if (explicit_connect)
    {
      showCopyableWarning(this, "Invalid input", message);
    }
  };

  if (!isPrintableAscii(host_port) || !host_port.contains(QLatin1Char(':')))
  {
    fail("Server must be host:port in printable ASCII.");
    return;
  }
  if (!isPrintableAscii(cert_path_))
  {
    fail("TLS certificate path contains invalid characters.");
    return;
  }
  if (!api_key_.isEmpty() && !isPrintableAscii(api_key_.trimmed()))
  {
    fail("API key contains invalid characters (control or non-ASCII bytes).");
    return;
  }

  // TLS first, always. Plaintext fallback lives in onFetchError: it fires
  // only when TLS actually fails, there's no custom cert to honor, and the
  // user has explicitly opted in via the per-URL allow_insecure_ flag.
  attempted_plaintext_fallback_ = false;
  connection_mode_ = ConnectionMode::None;

  // The cache is keyed by "sequence/topic" but holds schemas the previous
  // server vouched for. Don't carry them across a connect — the new server
  // could be unrelated, or could have re-published topics with a different
  // schema while keeping the same name.
  topic_metadata_cache_.clear();
  topic_metadata_in_flight_.clear();

  const std::string uri = (QStringLiteral("grpc+tls://") + host_port).toStdString();
  auto new_client =
      std::make_unique<MosaicoClient>(uri, kConnectionTimeoutSec, /*pool_size=*/kConnectionPoolSize,
                                      cert_path_.toStdString(), api_key_.toStdString());

  ensureWorkerThread();
  // Blocking so the worker swaps its pointer before we destroy the old one.
  QMetaObject::invokeMethod(worker_, "setClient", Qt::BlockingQueuedConnection,
                            Q_ARG(MosaicoClient*, new_client.get()));
  client_ = std::move(new_client);

  error_context_ = explicit_connect ? ErrorContext::ExplicitConnect : ErrorContext::AutoConnect;
  connect_button_->setEnabled(false);
  connect_button_->setText(QString::fromUtf8("Connecting\u2026"));
  sequence_panel_->setLoading(true);
  setStatus(QString::fromUtf8("Connecting\u2026"));
  QMetaObject::invokeMethod(worker_, "fetchSequences", Qt::QueuedConnection);
}

// ---------------------------------------------------------------------------
void MainWindow::saveState(QSettings& settings) const
{
  settings.beginGroup("ToolboxMosaico");
  settings.setValue("selected_sequence", selected_sequence_);
  settings.setValue("selected_topics", selected_topics_);
  settings.setValue("lower_proportion", lower_proportion_);
  settings.setValue("upper_proportion", upper_proportion_);
  settings.setValue("server_uri", currentServerText());
  settings.setValue("server_history", server_history_);
  settings.setValue("metadata_query", query_bar_->query());
  settings.endGroup();
}

void MainWindow::restoreState(QSettings& settings)
{
  settings.beginGroup("ToolboxMosaico");
  selected_sequence_ = settings.value("selected_sequence").toString();
  selected_topics_ = settings.value("selected_topics").toStringList();
  lower_proportion_ = settings.value("lower_proportion", 0.0).toDouble();
  upper_proportion_ = settings.value("upper_proportion", 1.0).toDouble();
  // Strip any legacy scheme a previous version might have saved; the plugin
  // now owns the scheme and the user edits only host:port.
  QString uri = stripUriScheme(settings.value("server_uri", "localhost:6726").toString());
  setCurrentServerText(uri);
  server_history_ = settings.value("server_history").toStringList();
  rebuildServerCombo();
  QString query = settings.value("metadata_query").toString();
  if (!query.isEmpty())
  {
    query_bar_->setQuery(query);
  }
  // Default credentials come from the MOSAICO_API_KEY env var (mirroring
  // the Python SDK's from_env()). The per-server cache read below can
  // override this if the user previously saved credentials for the
  // current URI via the cert dialog.
  api_key_ = QString::fromLocal8Bit(qgetenv("MOSAICO_API_KEY"));

  // Legacy migration: top-level tls_cert_path / api_key used to be a
  // single pair for all servers. If they're still present, fold them
  // into server_cache for the current URI's key (if non-empty) and
  // delete the legacy keys so this migration runs at most once.
  const QString legacy_cert = settings.value("tls_cert_path").toString();
  const QString legacy_key = settings.value("api_key").toString();
  const bool has_legacy = settings.contains("tls_cert_path") || settings.contains("api_key");
  if (has_legacy)
  {
    const QString migrate_key = normalizeServerKey(currentServerText());
    if (!migrate_key.isEmpty())
    {
      settings.beginGroup("server_cache");
      settings.beginGroup(migrate_key);
      if (!settings.contains("cert_path"))
      {
        settings.setValue("cert_path", legacy_cert);
      }
      if (!settings.contains("api_key"))
      {
        settings.setValue("api_key", legacy_key);
      }
      settings.endGroup();
      settings.endGroup();

      // Also promote the URI into server_history (if not already there).
      server_history_ = promoteToHead(server_history_, migrate_key, kServerHistoryCap);
      settings.setValue("server_history", server_history_);
      rebuildServerCombo();

      // Restore the live cert_path_ / api_key_ too, so the running
      // session doesn't lose them. Only overwrite when the legacy value
      // is non-empty — an empty legacy api_key must not clobber the
      // MOSAICO_API_KEY env var that was just applied above.
      if (!legacy_cert.isEmpty())
      {
        cert_path_ = legacy_cert;
      }
      if (!legacy_key.isEmpty())
      {
        api_key_ = legacy_key;
      }
    }
    settings.remove("tls_cert_path");
    settings.remove("api_key");
  }

  // Per-server cached credentials override the env-var fallback when
  // set. Empty cache entries leave the fallback in place so users who
  // rely on MOSAICO_API_KEY for automation don't get their key wiped
  // by a server whose cache is empty.
  const QString current_key = normalizeServerKey(currentServerText());
  if (!current_key.isEmpty())
  {
    settings.beginGroup("server_cache");
    settings.beginGroup(current_key);
    const QString cached_cert = settings.value("cert_path").toString();
    const QString cached_key = settings.value("api_key").toString();
    if (!cached_cert.isEmpty())
    {
      cert_path_ = cached_cert;
    }
    if (!cached_key.isEmpty())
    {
      api_key_ = cached_key;
    }
    settings.endGroup();
    settings.endGroup();
  }

  if (!cert_path_.isEmpty() && !isCertReadable(cert_path_))
  {
    cert_path_.clear();
  }
  settings.endGroup();
}

void MainWindow::onQueryChanged(const QString& query, bool valid)
{
  emit queryChanged(query, valid);
}

void MainWindow::setVisibleSequences(const std::set<std::string>& names)
{
  sequence_panel_->setVisibleSequences(names);
}

void MainWindow::clearVisibleSequences()
{
  sequence_panel_->clearVisibleSequences();
}

void MainWindow::updateTheme(bool dark)
{
  query_bar_->updateTheme(dark);
}

void MainWindow::recordDecodedBytes(const QString& topic_name, qint64 decoded_bytes)
{
  if (decoded_bytes <= 0)
  {
    return;
  }
  qint64& cumulative = decoded_fetch_bytes_[topic_name];
  cumulative += decoded_bytes;
  if (download_stats_dialog_)
  {
    download_stats_dialog_->updateProgress(topic_name, cumulative, /*total_bytes=*/0);
  }
}

// ---------------------------------------------------------------------------
void MainWindow::onCertButtonClicked()
{
  CertDialog dialog(this);
  dialog.setCertPath(cert_path_);
  dialog.setApiKey(api_key_);
  dialog.setAllowInsecure(allow_insecure_);

  if (dialog.exec() == QDialog::Accepted)
  {
    cert_path_ = dialog.certPath();
    api_key_ = dialog.apiKey();
    allow_insecure_ = dialog.allowInsecure();
    writeCachedCredentials(currentServerKey());
    QSettings settings;
    saveState(settings);
  }
}

void MainWindow::onServerSelected(int index)
{
  guardedSlot(status_label_, "onServerSelected", [&]() {
    if (!server_uri_combo_)
    {
      return;
    }
    if (index < 0)
    {
      return;
    }

    const QString selected = server_uri_combo_->itemText(index);
    const QString key = normalizeServerKey(selected);
    if (key.isEmpty())
    {
      return;
    }

    // Pull cached credentials (may be empty). Do NOT connect — the user
    // still presses Connect. The scheme is added at connect time.
    loadCachedCredentials(key);

    // Persist the switch so api_key_ / cert_path_ / allow_insecure_
    // survive an immediate restart even if the user doesn't then press
    // Connect.
    QSettings settings;
    saveState(settings);
  });
}

void MainWindow::setStatus(const QString& message)
{
  if (!status_label_)
  {
    return;
  }
  switch (connection_mode_)
  {
    case ConnectionMode::Secure:
      status_label_->setText(message + QStringLiteral(" (secure connection)"));
      return;
    case ConnectionMode::Insecure:
      status_label_->setText(message + QStringLiteral(" (insecure connection)"));
      return;
    case ConnectionMode::None:
      status_label_->setText(message);
      return;
  }
}

// --- Server URI combobox accessors ---
// Thin wrappers so callers don't depend on the underlying Qt widget type.

QString MainWindow::currentServerText() const
{
  return server_uri_combo_ ? server_uri_combo_->currentText() : QString();
}

void MainWindow::setCurrentServerText(const QString& text)
{
  if (!server_uri_combo_)
  {
    return;
  }
  server_uri_combo_->setEditText(text);
}

void MainWindow::rebuildServerCombo()
{
  if (!server_uri_combo_)
  {
    return;
  }

  // Preserve the current edit-field text so that swapping items doesn't
  // wipe what the user is typing.
  const QString current = server_uri_combo_->currentText();

  // Signals from clear/add must not fire handlers while we mutate.
  QSignalBlocker blocker(server_uri_combo_);

  server_uri_combo_->clear();
  for (const auto& entry : server_history_)
  {
    server_uri_combo_->addItem(entry);
  }

  // Pin the demo seed at the bottom. If history already happens to
  // contain it (shouldn't, but defensive), skip the duplicate.
  const QString demo = QString::fromLatin1(kDemoServerKey);
  if (!server_history_.contains(demo))
  {
    server_uri_combo_->addItem(demo);
  }

  server_uri_combo_->setEditText(current);
}

void MainWindow::addServerToHistory(const QString& key)
{
  if (key.isEmpty())
  {
    return;
  }
  if (key == QString::fromLatin1(kDemoServerKey))
  {
    // Demo is always pinned; no need to store it in server_history_.
    return;
  }
  server_history_ = promoteToHead(server_history_, key, kServerHistoryCap);

  // Persist immediately so a crash between now and the next
  // saveState doesn't lose the entry.
  QSettings settings;
  settings.beginGroup("ToolboxMosaico");
  settings.setValue("server_history", server_history_);
  settings.endGroup();

  rebuildServerCombo();
}

QString MainWindow::currentServerKey() const
{
  return normalizeServerKey(currentServerText());
}

void MainWindow::loadCachedCredentials(const QString& key)
{
  if (key.isEmpty())
  {
    return;
  }
  QSettings settings;
  settings.beginGroup("ToolboxMosaico");
  settings.beginGroup("server_cache");
  settings.beginGroup(key);
  cert_path_ = settings.value("cert_path").toString();
  api_key_ = settings.value("api_key").toString();
  allow_insecure_ = settings.value("allow_insecure", false).toBool();
  settings.endGroup();
  settings.endGroup();
  settings.endGroup();
}

void MainWindow::writeCachedCredentials(const QString& key)
{
  if (key.isEmpty())
  {
    return;
  }
  QSettings settings;
  settings.beginGroup("ToolboxMosaico");
  settings.beginGroup("server_cache");
  settings.beginGroup(key);
  settings.setValue("cert_path", cert_path_);
  settings.setValue("api_key", api_key_);
  settings.setValue("allow_insecure", allow_insecure_);
  settings.endGroup();
  settings.endGroup();
  settings.endGroup();
}
