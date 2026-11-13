#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QHash>
#include <QString>
#include <vector>

#include "pj_widgets/Dialog.h"

class QLabel;
class QWidget;

namespace Ui {
class McpJobsDialog;
}

namespace PJ {

/// What AI agents are running right now (the MCP server's unfinished jobs), each with a Stop
/// button: the user's view of agent activity, opened from the status-bar indicator. Not modal, so
/// the agents carry on while it is open. A view only: the owner feeds it (setJobs, again whenever
/// the jobs change) and acts on stopRequested.
class McpJobsDialog : public Dialog {
  Q_OBJECT
 public:
  /// One row. `what` is the tool and its arguments, as the agent called it.
  struct Job {
    quint64 id = 0;
    QString client;
    QString what;
    QString state;
    double elapsed_s = 0.0;
  };

  explicit McpJobsDialog(QWidget* parent = nullptr);
  ~McpJobsDialog() override;

  /// Brings the rows in line with `jobs`: a job that is still there keeps its row (and its Stop
  /// button, which the user may be pressing), with fresh text; none shows the empty note.
  void setJobs(const std::vector<Job>& jobs);

 signals:
  /// The user pressed Stop on job `id`.
  void stopRequested(quint64 id);

 private:
  /// A job's row and the text in it.
  struct Row {
    QWidget* widget = nullptr;
    QLabel* text = nullptr;
  };
  Ui::McpJobsDialog* ui_;
  QHash<quint64, Row> rows_;
};

}  // namespace PJ
