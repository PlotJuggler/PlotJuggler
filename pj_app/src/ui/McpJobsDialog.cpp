// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "ui/McpJobsDialog.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSet>

#include "ui_McpJobsDialog.h"
using namespace Qt::StringLiterals;

namespace PJ {

namespace {
/// The width a row's text wraps at. A wrapped label's height follows its width, and the layouts
/// above it do not carry that up to the window: each row is given the height this width takes.
constexpr int kRowTextWidth = 360;
}  // namespace

McpJobsDialog::McpJobsDialog(QWidget* parent) : Dialog(parent), ui_(new Ui::McpJobsDialog) {
  setDialogTitle(tr("AI agent activity"));
  auto* body = new QWidget;
  ui_->setupUi(body);
  contentLayout()->addWidget(body);
}

McpJobsDialog::~McpJobsDialog() {
  delete ui_;
}

void McpJobsDialog::setJobs(const std::vector<Job>& jobs) {
  QSet<quint64> listed;
  bool row_added = false;
  for (const Job& job : jobs) {
    listed.insert(job.id);
    Row& row = rows_[job.id];
    if (row.widget == nullptr) {
      row_added = true;
      // Rows come from live data, so they are built here.
      row.widget = new QWidget(ui_->jobRows);
      auto* layout = new QHBoxLayout(row.widget);
      layout->setContentsMargins(0, 0, 0, 0);
      row.text = new QLabel(row.widget);
      row.text->setTextFormat(Qt::PlainText);  // the arguments are the agent's own words
      row.text->setWordWrap(true);
      row.text->setMinimumWidth(kRowTextWidth);
      auto* stop = new QPushButton(tr("Stop"), row.widget);
      stop->setObjectName(u"stopJob%1"_s.arg(job.id));
      connect(stop, &QPushButton::clicked, this, [this, id = job.id] { emit stopRequested(id); });
      layout->addWidget(row.text, 1);
      layout->addWidget(stop, 0, Qt::AlignTop);
      ui_->jobRowsLayout->addWidget(row.widget);
    }
    row.text->setText(tr("%1 · %2 · %3 s\n%4").arg(job.client, job.state).arg(job.elapsed_s, 0, 'f', 0).arg(job.what));
    row.text->setMinimumHeight(row.text->heightForWidth(kRowTextWidth));
  }
  for (auto row = rows_.begin(); row != rows_.end();) {
    if (listed.contains(row.key())) {
      ++row;
      continue;
    }
    row->widget->hide();
    row->widget->deleteLater();  // its Stop button may be the one being clicked
    row = rows_.erase(row);
  }
  ui_->emptyLabel->setVisible(jobs.empty());
  if (row_added) {
    // The rows arrive after the dialog is shown: without this it keeps the size it had and clips
    // them. It only grows: shrinking would move the Stop buttons under the user's pointer.
    resize(size().expandedTo(sizeHint()));
  }
}

}  // namespace PJ
