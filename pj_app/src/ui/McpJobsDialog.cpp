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
  for (const Job& job : jobs) {
    listed.insert(job.id);
    Row& row = rows_[job.id];
    if (row.widget == nullptr) {
      // Rows come from live data, so they are built here.
      row.widget = new QWidget(ui_->jobRows);
      auto* layout = new QHBoxLayout(row.widget);
      layout->setContentsMargins(0, 0, 0, 0);
      row.text = new QLabel(row.widget);
      row.text->setTextFormat(Qt::PlainText);  // the arguments are the agent's own words
      row.text->setWordWrap(true);
      auto* stop = new QPushButton(tr("Stop"), row.widget);
      stop->setObjectName(u"stopJob%1"_s.arg(job.id));
      connect(stop, &QPushButton::clicked, this, [this, id = job.id] { emit stopRequested(id); });
      layout->addWidget(row.text, 1);
      layout->addWidget(stop, 0, Qt::AlignTop);
      ui_->jobRowsLayout->addWidget(row.widget);
    }
    row.text->setText(tr("%1 · %2 · %3 s\n%4").arg(job.client, job.state).arg(job.elapsed_s, 0, 'f', 0).arg(job.what));
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
}

}  // namespace PJ
