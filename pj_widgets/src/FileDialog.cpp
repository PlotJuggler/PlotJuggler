// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_widgets/FileDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QLayout>
#include <QLineEdit>
#include <QList>
#include <QSize>
#include <QString>
#include <QToolButton>

#include "pj_widgets/FrameworkTokens.h"
#include "pj_widgets/SvgUtil.h"
using namespace Qt::StringLiterals;

namespace PJ {

namespace {

// objectNames of the QFileDialog toolbar buttons we re-skin and re-size.
constexpr const char* kToolbarButtons[] = {
    "backButton", "forwardButton", "toParentButton", "newFolderButton", "listModeButton", "detailModeButton",
};

// Shared height for the filename field and the look-in / file-type combos.
// QFileDialog lays them in a grid whose row height tracks the combo's
// minimumSizeHint — the style floors that a few px above a bare QLineEdit,
// so QSS min/max-height can't pull the combos down to match the field.
// Pin all three in C++ instead. Value mirrors the app's input-chrome box
// model in the stylesheets: input_min_height (18) + 2*2 padding + 2*1 border.
constexpr int kInputRowHeightPx = 24;

// Re-skin a QFileDialog toolbar button identified by objectName with one
// of the app's themed SVG icons. Silent no-op when the button is missing
// (defensive against Qt internal renames between versions).
void applySvgIcon(QFileDialog* dialog, const char* object_name, const QString& svg_resource) {
  if (auto* button = dialog->findChild<QToolButton*>(QString::fromLatin1(object_name))) {
    button->setIcon(QIcon(loadSvg(svg_resource, currentTheme())));
  }
}

}  // namespace

FileDialog::FileDialog(QWidget* parent) : Dialog(parent) {
  setDialogTitle(tr("Open File"));
  resize(820, 560);

  // Build the inner picker as a plain child widget — Qt::Widget strips
  // the default Qt::Dialog window flags so it doesn't try to be its own
  // top-level window. DontUseNativeDialog forces the Qt-drawn widget
  // tree, which is what our stylesheet targets.
  inner_ = new QFileDialog(contentWidget());
  inner_->setOption(QFileDialog::DontUseNativeDialog);
  inner_->setWindowFlags(Qt::Widget);
  inner_->setSizeGripEnabled(false);

  contentLayout()->addWidget(inner_);

  // Swap Qt's stock SP_FileDialog* icons (native theme glyphs that clash
  // with our flat chrome) for our keyboard_arrow_* chevrons. The _light
  // variants carry the #3D3D3D authoring ink that loadSvg's recolor pipeline
  // folds onto the active theme ink (the _dark variants' #E0E0E0 is
  // deliberately NOT folded and would stay pale on the light theme).
  applySvgIcon(inner_, "backButton", ":/resources/svg/keyboard_arrow_left_light.svg");
  applySvgIcon(inner_, "forwardButton", ":/resources/svg/keyboard_arrow_right_light.svg");
  applySvgIcon(inner_, "toParentButton", ":/resources/svg/keyboard_arrow_up_light.svg");
  applySvgIcon(inner_, "newFolderButton", ":/resources/svg/create_new_folder.svg");
  // QFileDialog's "list mode" is the columns-of-icons display, "detail
  // mode" is the table with name/size/type/date columns — match Material's
  // visual convention: Grid View for the icon-grid mode, View List for
  // the row-with-metadata mode.
  applySvgIcon(inner_, "listModeButton", ":/resources/svg/grid_view.svg");
  applySvgIcon(inner_, "detailModeButton", ":/resources/svg/view_list.svg");

  // Prime icon size from ChromeMetrics defaults. Callers (or the
  // templated static helpers) override with the live MainWindow values
  // and may wire chromeMetricsChanged to stay in step.
  onChromeMetricsChanged(chrome_metrics_);

  // Equalize the input row heights (see kInputRowHeightPx). Both combos
  // (lookInCombo, fileTypeCombo) and the filename field get the same
  // fixed height so they line up instead of the combos rendering taller.
  for (auto* combo : inner_->findChildren<QComboBox*>()) {
    combo->setFixedHeight(kInputRowHeightPx);
  }
  if (auto* name_edit = inner_->findChild<QLineEdit*>(u"fileNameEdit"_s)) {
    name_edit->setFixedHeight(kInputRowHeightPx);
  }

  // The inner QDialogButtonBox accepts/rejects route the outer Dialog,
  // so exec() on the outer returns the expected QDialog::DialogCode.
  connect(inner_, &QFileDialog::accepted, this, &QDialog::accept);
  connect(inner_, &QFileDialog::rejected, this, &QDialog::reject);
}

void FileDialog::onChromeMetricsChanged(const ChromeMetrics& metrics) {
  chrome_metrics_ = metrics;
  // Size the chrome title bar to the live metrics so the file picker's chrome
  // matches the main window (and every other dialog) rather than sitting at the
  // constructor's default height.
  setChromeMetrics(metrics);
  // Mirror CurveListPanel: the icon gets icon_size, the button's outer
  // extent gets icon_size + icon_padding. Setting only setIconSize leaves
  // the button at its default ~16px size and the larger icon clips.
  const QSize icon_sz(metrics.icon_size, metrics.icon_size);
  const int button_extent = metrics.icon_size + metrics.icon_padding;
  for (const char* name : kToolbarButtons) {
    if (auto* button = inner_->findChild<QToolButton*>(QString::fromLatin1(name))) {
      button->setIconSize(icon_sz);
      button->setMinimumSize(button_extent, button_extent);
      button->setMaximumSize(button_extent, button_extent);
    }
  }
}

FileDialog::~FileDialog() = default;

void FileDialog::setAcceptMode(QFileDialog::AcceptMode mode) {
  inner_->setAcceptMode(mode);
}

void FileDialog::setFileMode(QFileDialog::FileMode mode) {
  inner_->setFileMode(mode);
}

void FileDialog::setNameFilter(const QString& filter) {
  inner_->setNameFilter(filter);
}

void FileDialog::setDirectory(const QString& directory) {
  inner_->setDirectory(directory);
}

void FileDialog::setDefaultSuffix(const QString& suffix) {
  inner_->setDefaultSuffix(suffix);
}

void FileDialog::selectFile(const QString& filename) {
  inner_->selectFile(filename);
}

QStringList FileDialog::selectedFiles() const {
  return inner_->selectedFiles();
}

QString FileDialog::selectedFile() const {
  const QStringList files = inner_->selectedFiles();
  return files.isEmpty() ? QString{} : files.front();
}

QString FileDialog::getOpenFileName(
    QWidget* parent, const QString& caption, const QString& dir, const QString& filter, const QString& selected_name) {
  FileDialog dlg(parent);
  if (!caption.isEmpty()) {
    dlg.setDialogTitle(caption);
  }
  dlg.setAcceptMode(QFileDialog::AcceptOpen);
  dlg.setFileMode(QFileDialog::ExistingFile);
  if (!dir.isEmpty()) {
    dlg.setDirectory(dir);
  }
  if (!filter.isEmpty()) {
    dlg.setNameFilter(filter);
  }
  if (!selected_name.isEmpty()) {
    dlg.selectFile(selected_name);
  }
  if (dlg.exec() != QDialog::Accepted) {
    return {};
  }
  return dlg.selectedFile();
}

QStringList FileDialog::getOpenFileNames(
    QWidget* parent, const QString& caption, const QString& dir, const QString& filter) {
  FileDialog dlg(parent);
  if (!caption.isEmpty()) {
    dlg.setDialogTitle(caption);
  }
  dlg.setAcceptMode(QFileDialog::AcceptOpen);
  dlg.setFileMode(QFileDialog::ExistingFiles);
  if (!dir.isEmpty()) {
    dlg.setDirectory(dir);
  }
  if (!filter.isEmpty()) {
    dlg.setNameFilter(filter);
  }
  if (dlg.exec() != QDialog::Accepted) {
    return {};
  }
  return dlg.selectedFiles();
}

QString FileDialog::getExistingDirectory(QWidget* parent, const QString& caption, const QString& dir) {
  FileDialog dlg(parent);
  if (!caption.isEmpty()) {
    dlg.setDialogTitle(caption);
  }
  dlg.setAcceptMode(QFileDialog::AcceptOpen);
  dlg.setFileMode(QFileDialog::Directory);
  dlg.inner_->setOption(QFileDialog::ShowDirsOnly, true);
  if (!dir.isEmpty()) {
    dlg.setDirectory(dir);
  }
  if (dlg.exec() != QDialog::Accepted) {
    return {};
  }
  return dlg.selectedFile();
}

QString FileDialog::getSaveFileName(
    QWidget* parent, const QString& caption, const QString& dir, const QString& filter, const QString& default_suffix,
    const QString& selected_name) {
  FileDialog dlg(parent);
  if (!caption.isEmpty()) {
    dlg.setDialogTitle(caption);
  }
  dlg.setAcceptMode(QFileDialog::AcceptSave);
  dlg.setFileMode(QFileDialog::AnyFile);
  if (!dir.isEmpty()) {
    dlg.setDirectory(dir);
  }
  if (!filter.isEmpty()) {
    dlg.setNameFilter(filter);
  }
  if (!default_suffix.isEmpty()) {
    dlg.setDefaultSuffix(default_suffix);
  }
  if (!selected_name.isEmpty()) {
    dlg.selectFile(selected_name);
  }
  if (dlg.exec() != QDialog::Accepted) {
    return {};
  }
  return dlg.selectedFile();
}

std::vector<QCheckBox*> FileDialog::embedExtras(const std::vector<ExtraOption>& extras) {
  std::vector<QCheckBox*> boxes;
  if (extras.empty()) {
    return boxes;
  }
  boxes.reserve(extras.size());

  // The dialog uses QGridLayout; we append at the next row, spanning all
  // columns so the checkbox row sits cleanly above the OK/Cancel buttons.
  auto* extras_widget = new QWidget(inner_);
  auto* row = new QHBoxLayout(extras_widget);
  row->setContentsMargins(
      theme::space(theme::Space::None), theme::space(theme::Space::None), theme::space(theme::Space::None),
      theme::space(theme::Space::None));
  for (const ExtraOption& opt : extras) {
    auto* box = new QCheckBox(opt.label, extras_widget);
    box->setChecked(opt.default_checked);
    row->addWidget(box);
    boxes.push_back(box);
  }
  row->addStretch(1);
  if (auto* grid = qobject_cast<QGridLayout*>(inner_->layout())) {
    const int next_row = grid->rowCount();
    grid->addWidget(extras_widget, next_row, 0, 1, grid->columnCount());
  }
  return boxes;
}

FileDialog::SaveResult FileDialog::getSaveFileNameWithOptions(
    QWidget* parent, const QString& caption, const QString& dir, const QString& filter, const QString& default_suffix,
    const std::vector<ExtraOption>& extras) {
  FileDialog dlg(parent);
  if (!caption.isEmpty()) {
    dlg.setDialogTitle(caption);
  }
  dlg.setAcceptMode(QFileDialog::AcceptSave);
  dlg.setFileMode(QFileDialog::AnyFile);
  if (!dir.isEmpty()) {
    dlg.setDirectory(dir);
  }
  if (!filter.isEmpty()) {
    dlg.setNameFilter(filter);
  }
  if (!default_suffix.isEmpty()) {
    dlg.setDefaultSuffix(default_suffix);
  }
  auto boxes = dlg.embedExtras(extras);

  SaveResult result;
  result.option_states.assign(extras.size(), false);
  for (std::size_t i = 0; i < extras.size(); ++i) {
    result.option_states[i] = extras[i].default_checked;
  }

  if (dlg.exec() != QDialog::Accepted) {
    return result;  // empty path; option_states left at defaults
  }
  const QStringList selected = dlg.selectedFiles();
  if (!selected.isEmpty()) {
    result.path = selected.first();
  }
  for (std::size_t i = 0; i < boxes.size(); ++i) {
    result.option_states[i] = boxes[i]->isChecked();
  }
  return result;
}

}  // namespace PJ
