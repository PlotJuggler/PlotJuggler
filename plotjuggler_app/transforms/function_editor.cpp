#include "function_editor.h"
#include "custom_function.h"
#include "plotwidget.h"
#include <QDebug>
#include <QMessageBox>
#include <QFont>
#include <QDomDocument>
#include <QDomElement>
#include <QFontDatabase>
#include <QFile>
#include <QMenu>
#include <QAction>
#include <QDir>
#include <QToolTip>
#include <QFileDialog>
#include <QFileInfo>
#include <QSettings>
#include <QByteArray>
#include <QInputDialog>
#include <QDragEnterEvent>
#include <QMouseEvent>
#include <QEnterEvent>
#include <QMimeData>
#include <QTableWidgetItem>
#include <QTimer>
#include <QListWidget>
#include <QListWidgetItem>
#include <QSyntaxHighlighter>
#include <QHeaderView>
#include <QPlainTextEdit>

#include <QGraphicsDropShadowEffect>
#include <QFontDatabase>

#include "QLuaHighlighter"

#include "lua_custom_function.h"
#ifdef PJ_HAS_PYTHON
#include "python_custom_function.h"
#endif

#include "PlotJuggler/svg_util.h"
#include "ui_function_editor_help.h"
#include "stylesheet.h"

void FunctionEditorWidget::on_stylesheetChanged(QString theme)
{
  ui->pushButtonDeleteCurves->setIcon(LoadSvg(":/resources/svg/trash.svg", theme));
  ui->buttonLoadFunctions->setIcon(LoadSvg(":/resources/svg/import.svg", theme));
  ui->buttonSaveFunctions->setIcon(LoadSvg(":/resources/svg/export.svg", theme));
  ui->buttonSaveCurrent->setIcon(LoadSvg(":/resources/svg/save.svg", theme));
  ui->buttonLibraryBox->setIcon(LoadSvg(":/resources/svg/apps_box.svg", theme));

  auto style = GetLuaSyntaxStyle(theme);

  ui->globalVarsText->setSyntaxStyle(style);
  ui->globalVarsTextBatch->setSyntaxStyle(style);

  ui->functionText->setSyntaxStyle(style);
  ui->functionTextBatch->setSyntaxStyle(style);
}

FunctionEditorWidget::FunctionEditorWidget(PlotDataMapRef& plotMapData,
                                           const TransformsMap& mapped_custom_plots,
                                           QWidget* parent)
  : QWidget(parent)
  , _plot_map_data(plotMapData)
  , _transform_maps(mapped_custom_plots)
  , ui(new Ui::FunctionEditor)
  , _functions_library_ui(nullptr)
  , _functions_library_dialog(nullptr)
  , _functions_library_overlay(nullptr)
  , _v_count(1)
  , _preview_widget(new PlotWidget(_local_plot_data, this))
{
  ui->setupUi(this);

  setupFunctionAppsButton();

  ui->globalVarsText->setHighlighter(new QLuaHighlighter);
  ui->globalVarsTextBatch->setHighlighter(new QLuaHighlighter);

  ui->functionText->setHighlighter(new QLuaHighlighter);
  ui->functionTextBatch->setHighlighter(new QLuaHighlighter);

  lua_completer_ = new QLuaCompleter(this);
  lua_completer_batch_ = new QLuaCompleter(this);

  ui->globalVarsText->setCompleter(lua_completer_);
  ui->globalVarsTextBatch->setCompleter(lua_completer_);

  ui->functionText->setCompleter(lua_completer_batch_);
  ui->functionTextBatch->setCompleter(lua_completer_batch_);

  QSettings settings;

  this->setWindowTitle("Create a custom timeseries");

  QFont fixedFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
  fixedFont.setPointSize(10);

  ui->globalVarsText->setFont(fixedFont);
  ui->functionText->setFont(fixedFont);
  ui->globalVarsTextBatch->setFont(fixedFont);
  ui->functionTextBatch->setFont(fixedFont);

  auto theme = settings.value("StyleSheet::theme", "light").toString();
  on_stylesheetChanged(theme);

  QPalette palette = ui->listAdditionalSources->palette();
  palette.setBrush(QPalette::Highlight, palette.brush(QPalette::Base));
  palette.setBrush(QPalette::HighlightedText, palette.brush(QPalette::Text));
  ui->listAdditionalSources->setPalette(palette);

  QStringList numericPlotNames;
  for (const auto& p : _plot_map_data.numeric)
  {
    QString name = QString::fromStdString(p.first);
    numericPlotNames.push_back(name);
  }
  numericPlotNames.sort(Qt::CaseInsensitive);

  QByteArray saved_xml =
      settings.value("FunctionEditorWidget.recentSnippetsXML", QByteArray()).toByteArray();
  restoreGeometry(settings.value("FunctionEditorWidget.geometry").toByteArray());

  if (saved_xml.isEmpty())
  {
    QFile file("://resources/default.snippets.xml");
    if (!file.open(QIODevice::ReadOnly))
    {
      throw std::runtime_error("problem with default.snippets.xml");
    }
    saved_xml = file.readAll();
  }

  importSnippets(saved_xml);

  ui->globalVarsText->setPlainText(
      settings.value("FunctionEditorWidget.previousGlobals", "").toString());
  ui->globalVarsTextBatch->setPlainText(
      settings.value("FunctionEditorWidget.previousGlobalsBatch", "").toString());

  ui->functionText->setPlainText(
      settings.value("FunctionEditorWidget.previousFunction", "return value").toString());
  ui->functionTextBatch->setPlainText(
      settings.value("FunctionEditorWidget.previousFunctionBatch", "return value").toString());

  ui->listAdditionalSources->installEventFilter(this);
  ui->listAdditionalSources->viewport()->installEventFilter(this);
  ui->lineEditTab2Filter->installEventFilter(this);

  auto preview_layout = new QHBoxLayout(ui->framePlotPreview);
  preview_layout->setMargin(6);
  preview_layout->addWidget(_preview_widget);

  _preview_widget->setContextMenuEnabled(false);

  _update_preview_tab1.connectCallback([this]() { onUpdatePreview(); });
  onUpdatePreview();
  _update_preview_tab2.connectCallback([this]() { onUpdatePreviewBatch(); });
  onUpdatePreviewBatch();

  _tab2_filter.connectCallback([this]() { onLineEditTab2FilterChanged(); });

  int batch_filter_type = settings.value("FunctionEditorWidget.filterType", 2).toInt();
  switch (batch_filter_type)
  {
    case 1:
      ui->radioButtonContains->setChecked(true);
      break;
    case 2:
      ui->radioButtonWildcard->setChecked(true);
      break;
    case 3:
      ui->radioButtonRegExp->setChecked(true);
      break;
  }

  bool use_batch_prefix = settings.value("FunctionEditorWidget.batchPrefix", false).toBool();
  ui->radioButtonPrefix->setChecked(use_batch_prefix);
  ui->luaButton->setChecked(true);
  ui->luaBatchButton->setChecked(true);

#ifdef PJ_HAS_PYTHON
  if (!PythonCustomFunction::isAvailable())
  {
    const QString tip = tr("Python is unavailable in this build (embedded interpreter could "
                           "not initialize). Use Lua instead.");
    if (ui->pythonButton)
    {
      ui->pythonButton->setEnabled(false);
      ui->pythonButton->setToolTip(tip);
    }
    if (ui->pythonBatchButton)
    {
      ui->pythonBatchButton->setEnabled(false);
      ui->pythonBatchButton->setToolTip(tip);
    }
  }
#else
  if (ui->pythonButton)
  {
    ui->pythonButton->setEnabled(false);
    ui->pythonButton->setToolTip(tr("Python support not compiled in."));
  }
  if (ui->pythonBatchButton)
  {
    ui->pythonBatchButton->setEnabled(false);
    ui->pythonBatchButton->setToolTip(tr("Python support not compiled in."));
  }
#endif

  _source_group = new QButtonGroup(this);
  _source_group->setExclusive(true);

  connect(_source_group, QOverload<QAbstractButton*, bool>::of(&QButtonGroup::buttonToggled), this,
          [this](QAbstractButton* b, bool checked) {
            if (!checked)
            {
              return;
            }
            syncSourceFromRadio();
            on_listSourcesChanged();
            updatePreview();
          });

  if (ui->luaButton)
  {
    connect(ui->luaButton, &QAbstractButton::toggled, this, [this](bool checked) {
      if (checked)
      {
        onScriptLangChanged();
      }
    });
  }

  if (ui->pythonButton)
  {
    connect(ui->pythonButton, &QAbstractButton::toggled, this, [this](bool checked) {
      if (checked)
      {
        onScriptLangChanged();
      }
    });
  }

  if (ui->luaBatchButton)
  {
    connect(ui->luaBatchButton, &QAbstractButton::toggled, this, [this](bool checked) {
      if (checked)
      {
        onScriptLangChanged();
      }
    });
  }

  if (ui->pythonBatchButton)
  {
    connect(ui->pythonBatchButton, &QAbstractButton::toggled, this, [this](bool checked) {
      if (checked)
      {
        onScriptLangChanged();
      }
    });
  }
}

FunctionEditorWidget::ScriptLang FunctionEditorWidget::currentLang() const
{
  if (ui->pythonButton && ui->pythonButton->isChecked())
  {
    return ScriptLang::Python;
  }
  return ScriptLang::Lua;
}

FunctionEditorWidget::ScriptLang FunctionEditorWidget::currentLangBatch() const
{
  if (ui->pythonBatchButton && ui->pythonBatchButton->isChecked())
  {
    return ScriptLang::Python;
  }
  return ScriptLang::Lua;
}

void FunctionEditorWidget::onScriptLangChanged()
{
  updateFunctionsLibraryPreview();
  onUpdatePreview();
  onUpdatePreviewBatch();
}

CustomPlotPtr FunctionEditorWidget::createCustomFunction(const SnippetData& snippet,
                                                         ScriptLang lang) const
{
  if (lang == ScriptLang::Python)
  {
#ifdef PJ_HAS_PYTHON
    return std::make_shared<PythonCustomFunction>(snippet);
#else
    throw std::runtime_error("Python support not available (compiled without Python3 dev).");
#endif
  }
  return std::make_shared<LuaCustomFunction>(snippet);
}

void FunctionEditorWidget::setupFunctionAppsButton()
{
  connect(ui->buttonLibraryBox, &QToolButton::clicked, this, [this]() {
    if (!_functions_library_dialog)
    {
      _functions_library_dialog = new QDialog(this);
      _functions_library_ui = new Ui::FunctionsLibrary();
      _functions_library_ui->setupUi(_functions_library_dialog);

      reloadFunctionsLibraryTable();
      updateFunctionsLibraryPreview();

      _functions_library_dialog->adjustSize();

      _functions_library_dialog->setWindowTitle(tr("Function Library"));
      _functions_library_dialog->setWindowFlags(Qt::Dialog);
      _functions_library_dialog->setMinimumSize(350, 300);

      _functions_library_dialog->installEventFilter(this);

      connect(_functions_library_ui->tableFunctions, &QTableWidget::cellDoubleClicked, this,
              [this](int, int) {
                if (_functions_library_ui && _functions_library_ui->useButton)
                {
                  _functions_library_ui->useButton->click();
                }
              });

      connect(_functions_library_dialog, &QObject::destroyed, this, [this]() {
        if (_functions_library_overlay)
        {
          _functions_library_overlay->hide();
          _functions_library_overlay->deleteLater();
          _functions_library_overlay = nullptr;
        }
        _functions_library_dialog = nullptr;
        delete _functions_library_ui;
        _functions_library_ui = nullptr;
      });

      connect(_functions_library_ui->tableFunctions, &QTableWidget::itemSelectionChanged, this,
              [this]() { updateFunctionsLibraryPreview(); });

      connect(_functions_library_ui->useButton, &QPushButton::clicked, this, [this]() {
        auto t = _functions_library_ui->tableFunctions;
        auto selected = t->selectionModel()->selectedRows();
        if (selected.isEmpty())
        {
          return;
        }

        QString combined_globals;
        QString combined_function;
        QString first_language;

        for (const auto& index : selected)
        {
          auto item = t->item(index.row(), 0);
          if (!item)
          {
            continue;
          }

          auto it = _snipped_saved.find(item->text());
          if (it == _snipped_saved.end())
          {
            continue;
          }

          const auto& sn = it->second;

          if (first_language.isEmpty())
          {
            first_language = sn.language;
          }

          if (!sn.global_vars.trimmed().isEmpty())
          {
            if (!combined_globals.isEmpty())
            {
              combined_globals += "\n\n";
            }
            combined_globals += sn.global_vars;
          }

          if (!sn.function.trimmed().isEmpty())
          {
            if (!combined_function.isEmpty())
            {
              combined_function += "\n\n";
            }
            combined_function += sn.function;
          }
        }

        bool wants_python = (first_language.toLower() == "python");
#ifdef PJ_HAS_PYTHON
        if (wants_python && !PythonCustomFunction::isAvailable())
        {
          QMessageBox::warning(this, tr("Python unavailable"),
                               tr("This snippet was saved as Python, but Python is "
                                  "disabled in this build. Falling back to Lua."));
          wants_python = false;
        }
#else
        if (wants_python)
        {
          QMessageBox::warning(this, tr("Python unavailable"),
                               tr("This snippet was saved as Python, but this build "
                                  "has no Python support. Falling back to Lua."));
          wants_python = false;
        }
#endif

        if (wants_python)
        {
          if (ui->pythonButton)
          {
            ui->pythonButton->setChecked(true);
          }
        }
        else
        {
          if (ui->luaButton)
          {
            ui->luaButton->setChecked(true);
          }
        }

        ui->globalVarsText->setPlainText(combined_globals);
        ui->functionText->setPlainText(combined_function);
        onUpdatePreview();

        _functions_library_dialog->hide();
      });

      connect(_functions_library_ui->searchLineEdit, &QLineEdit::textChanged, this,
              [this](const QString& text) {
                auto t = _functions_library_ui->tableFunctions;
                for (int row = 0; row < t->rowCount(); row++)
                {
                  auto item = t->item(row, 0);
                  bool match = !item || item->text().contains(text, Qt::CaseInsensitive);
                  t->setRowHidden(row, !match);
                }
              });
    }

    QWidget* top = window();

    if (!_functions_library_overlay)
    {
      _functions_library_overlay = new QWidget(top);
      _functions_library_overlay->setObjectName("functionsLibraryOverlay");
      _functions_library_overlay->setStyleSheet(
          "#functionsLibraryOverlay { background-color: rgba(0,0,0,90); }");
      _functions_library_overlay->setFocusPolicy(Qt::StrongFocus);
      _functions_library_overlay->show();
    }

    _functions_library_overlay->setGeometry(top->rect());
    _functions_library_overlay->raise();
    _functions_library_overlay->show();

    _functions_library_dialog->adjustSize();
    QRect topGeom = top->frameGeometry();
    int x = topGeom.x() + (topGeom.width() - _functions_library_dialog->width()) / 2;
    int y = topGeom.y() + (topGeom.height() - _functions_library_dialog->height()) / 2;
    _functions_library_dialog->move(x, y);

    if (_functions_library_ui && _functions_library_ui->searchLineEdit)
    {
      _functions_library_ui->searchLineEdit->setFocus();
      _functions_library_ui->searchLineEdit->selectAll();
    }

    _functions_library_dialog->exec();

    _functions_library_overlay->hide();
  });
}

void FunctionEditorWidget::syncSourceFromRadio()
{
  auto t = ui->listAdditionalSources;
  if (!t || !_source_group)
  {
    return;
  }

  for (auto* btn : _source_group->buttons())
  {
    auto* rb = qobject_cast<QRadioButton*>(btn);
    if (rb && rb->isChecked())
    {
      int row = rb->property("row").toInt();
      auto item = t->item(row, 2);
      _linked_source = item ? item->text() : "";

      // Restore previous row label
      if (_linked_source_row >= 0 && _linked_source_row < t->rowCount())
      {
        if (auto* prev_item = t->item(_linked_source_row, 1))
        {
          prev_item->setText(QString("v%1").arg(_linked_source_row + 1));
        }
      }
      // Set new row label
      if (auto* new_item = t->item(row, 1))
      {
        new_item->setText("value");
      }

      _linked_source_row = row;
      return;
    }
  }
  _linked_source.clear();
  _linked_source_row = -1;
}

void FunctionEditorWidget::reassignRadioRows()
{
  auto t = ui->listAdditionalSources;
  if (!t)
  {
    return;
  }

  for (int row = 0; row < t->rowCount(); row++)
  {
    if (auto* rb = qobject_cast<QRadioButton*>(t->cellWidget(row, 0)))
    {
      rb->setProperty("row", row);
    }
  }
}

void FunctionEditorWidget::reloadFunctionsLibraryTable()
{
  if (!_functions_library_ui || !_functions_library_ui->tableFunctions)
  {
    return;
  }

  auto t = _functions_library_ui->tableFunctions;

  _selected_library_name.clear();

  t->clear();
  t->setColumnCount(1);
  t->setHorizontalHeaderLabels({ "Name" });
  t->setRowCount((int)_snipped_saved.size());

  t->setEditTriggers(QAbstractItemView::NoEditTriggers);
  t->setSelectionBehavior(QAbstractItemView::SelectRows);
  t->setSelectionMode(QAbstractItemView::ExtendedSelection);

  t->horizontalHeader()->setStretchLastSection(true);
  t->verticalHeader()->setVisible(false);

  int row = 0;
  for (const auto& it : _snipped_saved)
  {
    t->setItem(row, 0, new QTableWidgetItem(it.first));
    row++;
  }

  if (t->rowCount() > 0)
  {
    t->selectRow(0);
    auto item0 = t->item(0, 0);
    if (item0)
    {
      _selected_library_name = item0->text();
    }
  }
}

void FunctionEditorWidget::updateFunctionsLibraryPreview()
{
  if (!_functions_library_ui)
  {
    return;
  }

  auto preview = _functions_library_ui->previewPlainText;
  if (!preview)
  {
    return;
  }

  auto t = _functions_library_ui->tableFunctions;
  auto selected = t->selectionModel()->selectedRows();
  if (selected.isEmpty())
  {
    preview->clear();
    return;
  }

  QString text;

  for (const auto& index : selected)
  {
    auto item = t->item(index.row(), 0);
    if (!item)
    {
      continue;
    }

    auto it = _snipped_saved.find(item->text());
    if (it == _snipped_saved.end())
    {
      continue;
    }

    const SnippetData& snippet = it->second;

    if (!text.isEmpty())
    {
      text += "\n\n";
    }

    if (!snippet.global_vars.isEmpty())
    {
      text += snippet.global_vars + "\n\n";
    }

    text += "function calc(time, value";
    for (int i = 1; i <= snippet.additional_sources.size(); i++)
    {
      text += QString(", v%1").arg(i);
    }
    QString lang_label = snippet.language;
    lang_label[0] = lang_label[0].toUpper();
    text += ")  [" + lang_label + "]\n";

    const auto lines = snippet.function.split("\n");
    for (const auto& line : lines)
    {
      text += "    " + line + "\n";
    }
    text += "end";
  }

  preview->setPlainText(text);
}

void FunctionEditorWidget::saveSettings()
{
  QSettings settings;
  settings.setValue("FunctionEditorWidget.recentSnippetsXML", exportSnippets());
  settings.setValue("FunctionEditorWidget.geometry", saveGeometry());

  settings.setValue("FunctionEditorWidget.previousGlobals", ui->globalVarsText->toPlainText());
  settings.setValue("FunctionEditorWidget.previousGlobalsBatch",
                    ui->globalVarsTextBatch->toPlainText());

  settings.setValue("FunctionEditorWidget.previousFunction", ui->functionText->toPlainText());
  settings.setValue("FunctionEditorWidget.previousFunctionBatch",
                    ui->functionTextBatch->toPlainText());
  int batch_filter_type = 0;
  if (ui->radioButtonContains->isChecked())
  {
    batch_filter_type = 1;
  }
  else if (ui->radioButtonWildcard->isChecked())
  {
    batch_filter_type = 2;
  }
  if (ui->radioButtonRegExp->isChecked())
  {
    batch_filter_type = 3;
  }
  settings.setValue("FunctionEditorWidget.filterType", batch_filter_type);

  settings.setValue("FunctionEditorWidget.batchPrefix", ui->radioButtonPrefix->isChecked());
}

FunctionEditorWidget::~FunctionEditorWidget()
{
  delete _preview_widget;

  saveSettings();

  delete ui;
}

void FunctionEditorWidget::setLinkedPlotName(const QString& linkedPlotName)
{
  _linked_source = linkedPlotName;
}

void FunctionEditorWidget::clear()
{
  _linked_source.clear();
  _linked_source_row = -1;
  ui->nameLineEdit->setText("");
  ui->listAdditionalSources->setRowCount(0);

  ui->suffixLineEdit->setText("");
  ui->listBatchSources->clear();
  ui->lineEditTab2Filter->setText("");
}

QString FunctionEditorWidget::getLinkedData() const
{
  return _linked_source;
}

void FunctionEditorWidget::createNewPlot()
{
  ui->nameLineEdit->setEnabled(true);
  _editor_mode = CREATE;
}

void FunctionEditorWidget::editExistingPlot(CustomPlotPtr data)
{
  bool wants_python = (data->language().toLower() == "python");
#ifdef PJ_HAS_PYTHON
  if (wants_python && !PythonCustomFunction::isAvailable())
  {
    QMessageBox::warning(this, tr("Python unavailable"),
                         tr("This custom plot was created in Python, but Python is "
                            "disabled in this build. The editor will open in Lua mode."));
    wants_python = false;
  }
#else
  if (wants_python)
  {
    QMessageBox::warning(this, tr("Python unavailable"),
                         tr("This custom plot was created in Python, but this build "
                            "has no Python support. The editor will open in Lua mode."));
    wants_python = false;
  }
#endif

  if (wants_python)
  {
    if (ui->pythonButton)
    {
      ui->pythonButton->setChecked(true);
    }
  }
  else
  {
    if (ui->luaButton)
    {
      ui->luaButton->setChecked(true);
    }
  }

  auto list_widget = ui->listAdditionalSources;
  list_widget->setRowCount(0);

  ui->globalVarsText->setPlainText(data->snippet().global_vars);
  ui->functionText->setPlainText(data->snippet().function);

  setLinkedPlotName(data->snippet().linked_source);
  ui->nameLineEdit->setText(data->aliasName());
  ui->nameLineEdit->setEnabled(false);

  _editor_mode = MODIFY;

  if (!data->snippet().linked_source.isEmpty())
  {
    int row = list_widget->rowCount();
    list_widget->setRowCount(row + 1);

    auto rb = new QRadioButton(list_widget);
    rb->setProperty("row", row);
    _source_group->addButton(rb);
    list_widget->setCellWidget(row, 0, rb);

    list_widget->setItem(row, 1, new QTableWidgetItem(QString("v%1").arg(row + 1)));
    list_widget->setItem(row, 2, new QTableWidgetItem(data->snippet().linked_source));

    rb->setChecked(true);
  }

  for (QString curve_name : data->snippet().additional_sources)
  {
    if (curve_name == data->snippet().linked_source)
    {
      continue;
    }

    int row = list_widget->rowCount();
    list_widget->setRowCount(row + 1);

    auto rb = new QRadioButton(list_widget);
    rb->setProperty("row", row);
    _source_group->addButton(rb);
    list_widget->setCellWidget(row, 0, rb);

    list_widget->setItem(row, 1, new QTableWidgetItem(QString("v%1").arg(row + 1)));
    list_widget->setItem(row, 2, new QTableWidgetItem(curve_name));
  }

  on_listSourcesChanged();
}

// CustomPlotPtr FunctionEditorWidget::getCustomPlotData() const
//{
//  return _plot;
//}

bool FunctionEditorWidget::eventFilter(QObject* obj, QEvent* ev)
{
  if (obj == _functions_library_dialog && ev->type() == QEvent::Hide)
  {
    if (_functions_library_overlay)
    {
      _functions_library_overlay->hide();
    }
    return false;
  }

  bool is_table = (obj == ui->listAdditionalSources);
  bool is_viewport = (obj == ui->listAdditionalSources->viewport());
  bool is_filter = (obj == ui->lineEditTab2Filter);

  if (ev->type() == QEvent::DragEnter)
  {
    auto event = static_cast<QDragEnterEvent*>(ev);
    const QMimeData* mimeData = event->mimeData();
    QStringList mimeFormats = mimeData->formats();

    for (const QString& format : mimeFormats)
    {
      QByteArray encoded = mimeData->data(format);
      QDataStream stream(&encoded, QIODevice::ReadOnly);

      if (format != "curveslist/add_curve")
      {
        return false;
      }

      _dragging_curves.clear();

      while (!stream.atEnd())
      {
        QString curve_name;
        stream >> curve_name;
        if (!curve_name.isEmpty())
        {
          _dragging_curves.push_back(curve_name);
        }
      }

      if ((is_filter && _dragging_curves.size() == 1) ||
          ((is_table || is_viewport) && _dragging_curves.size() > 0))
      {
        event->acceptProposedAction();
        return true;
      }
    }
  }
  else if (ev->type() == QEvent::DragMove)
  {
    if (is_table || is_viewport || is_filter)
    {
      static_cast<QDragMoveEvent*>(ev)->acceptProposedAction();
      return true;
    }
  }
  else if (ev->type() == QEvent::Drop)
  {
    if (is_filter)
    {
      ui->lineEditTab2Filter->setText(_dragging_curves.front());
      return true;
    }
    else if (is_table || is_viewport)
    {
      auto list_widget = ui->listAdditionalSources;
      for (QString curve_name : _dragging_curves)
      {
        if (list_widget->findItems(curve_name, Qt::MatchExactly).isEmpty() &&
            curve_name != _linked_source)
        {
          int row = list_widget->rowCount();
          list_widget->setRowCount(row + 1);

          auto rb = new QRadioButton(list_widget);
          rb->setFocusPolicy(Qt::NoFocus);
          rb->setProperty("row", row);
          _source_group->addButton(rb);
          list_widget->setCellWidget(row, 0, rb);

          list_widget->setItem(row, 1, new QTableWidgetItem(QString("v%1").arg(row + 1)));
          list_widget->setItem(row, 2, new QTableWidgetItem(curve_name));

          if (row == 0)
          {
            rb->setChecked(true);
          }
        }
      }
      on_listSourcesChanged();
      return true;
    }
  }

  return false;
}

void FunctionEditorWidget::importSnippets(const QByteArray& xml_text)
{
  _snipped_saved = GetSnippetsFromXML(xml_text);

  for (const auto& custom_it : _transform_maps)
  {
    auto math_plot = dynamic_cast<CustomFunction*>(custom_it.second.get());
    if (!math_plot)
    {
      continue;
    }

    SnippetData snippet;
    snippet.alias_name = math_plot->aliasName();
    snippet.language = math_plot->language().toLower();

    if (_snipped_saved.count(snippet.alias_name) > 0)
    {
      continue;
    }

    snippet.global_vars = math_plot->snippet().global_vars;
    snippet.function = math_plot->snippet().function;
    snippet.linked_source = math_plot->snippet().linked_source;
    snippet.additional_sources = math_plot->snippet().additional_sources;
    _snipped_saved.insert({ snippet.alias_name, snippet });
  }

  if (_functions_library_ui)
  {
    reloadFunctionsLibraryTable();
    updateFunctionsLibraryPreview();
  }
}

QByteArray FunctionEditorWidget::exportSnippets() const
{
  QDomDocument doc;
  auto root = ExportSnippets(_snipped_saved, doc);
  doc.appendChild(root);
  return doc.toByteArray(2);
}

void FunctionEditorWidget::on_nameLineEdit_textChanged(const QString& name)
{
  if (_plot_map_data.numeric.count(name.toStdString()) == 0)
  {
    ui->pushButtonCreate->setText("Create New Timeseries");
  }
  else
  {
    ui->pushButtonCreate->setText("Modify Timeseries");
  }
  updatePreview();
}

void FunctionEditorWidget::on_buttonLoadFunctions_clicked()
{
  QSettings settings;
  QString directory_path =
      settings.value("AddCustomPlotDialog.loadDirectory", QDir::currentPath()).toString();

  QString fileName = QFileDialog::getOpenFileName(this, tr("Open Snippet Library"), directory_path,
                                                  tr("Snippets (*.snippets.xml)"));
  if (fileName.isEmpty())
  {
    return;
  }

  QFile file(fileName);

  if (!file.open(QIODevice::ReadOnly))
  {
    QMessageBox::critical(this, "Error", QString("Failed to open the file [%1]").arg(fileName));
    return;
  }

  directory_path = QFileInfo(fileName).absolutePath();
  settings.setValue("AddCustomPlotDialog.loadDirectory", directory_path);

  importSnippets(file.readAll());
}

void FunctionEditorWidget::on_buttonSaveFunctions_clicked()
{
  QSettings settings;
  QString directory_path =
      settings.value("AddCustomPlotDialog.loadDirectory", QDir::currentPath()).toString();

  QString fileName = QFileDialog::getSaveFileName(this, tr("Open Snippet Library"), directory_path,
                                                  tr("Snippets (*.snippets.xml)"));

  if (fileName.isEmpty())
  {
    return;
  }
  if (!fileName.endsWith(".snippets.xml"))
  {
    fileName.append(".snippets.xml");
  }

  QFile file(fileName);
  if (!file.open(QIODevice::WriteOnly))
  {
    QMessageBox::critical(this, "Error", QString("Failed to open the file [%1]").arg(fileName));
    return;
  }
  auto data = exportSnippets();

  file.write(data);
  file.close();

  directory_path = QFileInfo(fileName).absolutePath();
  settings.setValue("AddCustomPlotDialog.loadDirectory", directory_path);
}

void FunctionEditorWidget::on_buttonSaveCurrent_clicked()
{
  QString name = _selected_library_name;

  if (_functions_library_ui && _functions_library_ui->tableFunctions)
  {
    int r = _functions_library_ui->tableFunctions->currentRow();
    if (r >= 0)
    {
      auto item = _functions_library_ui->tableFunctions->item(r, 0);
      if (item)
      {
        name = item->text();
      }
    }
  }

  bool ok = false;
  name = QInputDialog::getText(this, tr("Name of the Function"), tr("Name:"), QLineEdit::Normal,
                               name, &ok);
  if (!ok || name.isEmpty())
  {
    return;
  }

  SnippetData snippet;
  snippet.alias_name = name;
  snippet.language = (currentLang() == ScriptLang::Python) ? "python" : "lua";
  snippet.global_vars = ui->globalVarsText->toPlainText();
  snippet.function = ui->functionText->toPlainText();

  snippet.linked_source = getLinkedData();
  for (int row = 0; row < ui->listAdditionalSources->rowCount(); row++)
  {
    snippet.additional_sources.push_back(ui->listAdditionalSources->item(row, 2)->text());
  }

  addToSaved(name, snippet);
}

bool FunctionEditorWidget::addToSaved(const QString& name, const SnippetData& snippet)
{
  if (_snipped_saved.count(name))
  {
    QMessageBox msgBox(this);
    msgBox.setWindowTitle("Warning");
    msgBox.setText(
        tr("A function with the same name exists already in the list of saved functions.\n"));
    msgBox.addButton(QMessageBox::Cancel);
    QPushButton* button = msgBox.addButton(tr("Overwrite"), QMessageBox::YesRole);
    msgBox.setDefaultButton(button);

    int res = msgBox.exec();
    if (res < 0 || res == QMessageBox::Cancel)
    {
      return false;
    }
  }

  _snipped_saved[name] = snippet;

  if (_functions_library_ui)
  {
    reloadFunctionsLibraryTable();
    _selected_library_name = name;
    updateFunctionsLibraryPreview();
  }

  return true;
}

void FunctionEditorWidget::on_pushButtonCreate_clicked()
{
  std::vector<CustomPlotPtr> created_plots;

  try
  {
    if (ui->tabWidget->currentIndex() == 0)
    {
      std::string new_plot_name = ui->nameLineEdit->text().toStdString();

      if (_editor_mode == CREATE && _transform_maps.count(new_plot_name) != 0)
      {
        QMessageBox msgBox(this);
        msgBox.setWindowTitle("Warning");
        msgBox.setText(tr("A custom time series with the same name exists already.\n"
                          " Do you want to overwrite it?\n"));
        msgBox.addButton(QMessageBox::Cancel);
        QPushButton* button = msgBox.addButton(tr("Overwrite"), QMessageBox::YesRole);
        msgBox.setDefaultButton(button);

        int res = msgBox.exec();

        if (res < 0 || res == QMessageBox::Cancel)
        {
          return;
        }
      }

      SnippetData snippet;
      snippet.function = ui->functionText->toPlainText();
      snippet.global_vars = ui->globalVarsText->toPlainText();
      snippet.alias_name = ui->nameLineEdit->text();
      snippet.language = (currentLang() == ScriptLang::Python) ? "python" : "lua";
      snippet.linked_source = getLinkedData();
      for (int row = 0; row < ui->listAdditionalSources->rowCount(); row++)
      {
        snippet.additional_sources.push_back(ui->listAdditionalSources->item(row, 2)->text());
      }
      created_plots.push_back(createCustomFunction(snippet, currentLang()));
    }
    else  // ----------- batch ------
    {
      for (int row = 0; row < ui->listBatchSources->count(); row++)
      {
        SnippetData snippet;
        snippet.language = (currentLangBatch() == ScriptLang::Python) ? "python" : "lua";
        snippet.function = ui->functionTextBatch->toPlainText();
        snippet.global_vars = ui->globalVarsTextBatch->toPlainText();
        snippet.linked_source = ui->listBatchSources->item(row)->text();
        if (ui->radioButtonPrefix->isChecked())
        {
          snippet.alias_name = ui->suffixLineEdit->text() + snippet.linked_source;
        }
        else
        {
          snippet.alias_name = snippet.linked_source + ui->suffixLineEdit->text();
        }
        created_plots.push_back(createCustomFunction(snippet, currentLangBatch()));
      }
    }

    accept(created_plots);
    saveSettings();
  }
  catch (const std::runtime_error& e)
  {
    QMessageBox::critical(this, "Error",
                          "Failed to create math plot : " + QString::fromStdString(e.what()));
  }
}

void FunctionEditorWidget::on_pushButtonCancel_pressed()
{
  if (_editor_mode == MODIFY)
  {
    clear();
  }
  saveSettings();
  closed();
}

void FunctionEditorWidget::on_listSourcesChanged()
{
  syncSourceFromRadio();
  const QString source = getLinkedData();

  QString function_text("function( time, value");

  for (int row = 0; row < ui->listAdditionalSources->rowCount(); row++)
  {
    auto name_item = ui->listAdditionalSources->item(row, 2);

    if (!name_item || name_item->text() == source)
    {
      continue;
    }

    function_text += ", ";
    function_text += ui->listAdditionalSources->item(row, 1)->text();
  }
  function_text += " )";
  ui->labelFunction->setText(function_text);

  updatePreview();
}

void FunctionEditorWidget::on_listAdditionalSources_itemSelectionChanged()
{
  bool any_selected = !ui->listAdditionalSources->selectedItems().isEmpty();
  ui->pushButtonDeleteCurves->setEnabled(any_selected);
}

void FunctionEditorWidget::on_pushButtonDeleteCurves_clicked()
{
  auto list_sources = ui->listAdditionalSources;
  bool source_deleted = false;

  QSignalBlocker block_group(_source_group);

  QModelIndexList selected = list_sources->selectionModel()->selectedRows();
  while (!selected.isEmpty())
  {
    int row = selected.first().row();

    auto name_item = list_sources->item(row, 2);
    if (name_item && name_item->text() == _linked_source)
    {
      source_deleted = true;
    }

    if (auto* rb = qobject_cast<QRadioButton*>(list_sources->cellWidget(row, 0)))
    {
      _source_group->removeButton(rb);
    }

    list_sources->removeRow(row);
    selected = list_sources->selectionModel()->selectedRows();
  }

  for (int row = 0; row < list_sources->rowCount(); row++)
  {
    if (auto* v_item = list_sources->item(row, 1))
    {
      v_item->setText(QString("v%1").arg(row + 1));
    }

    if (auto* rb = qobject_cast<QRadioButton*>(list_sources->cellWidget(row, 0)))
    {
      rb->setProperty("row", row);
    }
  }

  if (source_deleted && list_sources->rowCount() > 0)
  {
    if (auto* rb0 = qobject_cast<QRadioButton*>(list_sources->cellWidget(0, 0)))
    {
      rb0->setChecked(true);
    }
  }

  block_group.unblock();

  on_listAdditionalSources_itemSelectionChanged();
  on_listSourcesChanged();
}

void FunctionEditorWidget::updatePreview()
{
  _update_preview_tab1.triggerSignal(250);
}

void FunctionEditorWidget::onUpdatePreview()
{
  QString errors;
  std::string new_plot_name = ui->nameLineEdit->text().toStdString();

  if (_transform_maps.count(new_plot_name) != 0)
  {
    QString new_name = ui->nameLineEdit->text();
    if (_linked_source.toStdString() == new_plot_name ||
        !ui->listAdditionalSources->findItems(new_name, Qt::MatchExactly).isEmpty())
    {
      errors += "- The name of the new timeseries is the same of one of its "
                "dependencies.\n";
    }
  }

  if (new_plot_name.empty())
  {
    errors += "- Missing name of the new time series.\n";
  }
  else
  {
    // check if name is unique (except if is custom_plot)
    if (_plot_map_data.numeric.count(new_plot_name) != 0 &&
        _transform_maps.count(new_plot_name) == 0)
    {
      errors += "- Plot name already exists and can't be modified.\n";
    }
  }

  if (_linked_source.isEmpty())
  {
    errors += "- Missing source time series.\n";
  }

  SnippetData snippet;
  snippet.function = ui->functionText->toPlainText();
  snippet.global_vars = ui->globalVarsText->toPlainText();
  snippet.alias_name = ui->nameLineEdit->text();
  snippet.language = (currentLang() == ScriptLang::Python) ? "python" : "lua";
  snippet.linked_source = getLinkedData();
  for (int row = 0; row < ui->listAdditionalSources->rowCount(); row++)
  {
    snippet.additional_sources.push_back(ui->listAdditionalSources->item(row, 2)->text());
  }

  CustomPlotPtr custom_function;
  try
  {
    custom_function = createCustomFunction(snippet, currentLang());
    ui->buttonSaveCurrent->setEnabled(true);
  }
  catch (std::runtime_error& err)
  {
    const QString lang = (currentLang() == ScriptLang::Python) ? "Python" : "Lua";
    errors += QString("- Error in %1 script: %2").arg(lang).arg(err.what());
    ui->buttonSaveCurrent->setEnabled(false);
  }

  if (custom_function)
  {
    try
    {
      std::string name = new_plot_name.empty() ? "no_name" : new_plot_name;
      PlotData& out_data = _local_plot_data.getOrCreateNumeric(name);
      out_data.clear();

      std::vector<PlotData*> out_vector = { &out_data };
      custom_function->setData(&_plot_map_data, {}, out_vector);
      custom_function->calculate();

      _preview_widget->removeAllCurves();
      _preview_widget->addCurve(name, Qt::blue);
      _preview_widget->zoomOut(false);
    }
    catch (std::runtime_error& err)
    {
      const QString lang = (currentLang() == ScriptLang::Python) ? "Python" : "Lua";
      errors += QString("- Error in %1 script: %2").arg(lang).arg(err.what());
    }
  }

  if (new_plot_name.empty())
  {
    ui->nameLineEdit->setStyleSheet("QLineEdit{ background-color: #ffcccc; }");
  }
  else
  {
    ui->nameLineEdit->setStyleSheet("");
  }

  if (errors.isEmpty())
  {
    ui->terminalPlainText->hide();
    ui->framePlotPreview->show();
    ui->pushButtonCreate->setEnabled(true);
    return;
  }

  ui->terminalPlainText->show();
  ui->framePlotPreview->hide();
  ui->pushButtonCreate->setEnabled(false);
  ui->terminalPlainText->setPlainText(errors.trimmed());

  if (QWidget* p = ui->terminalPlainText->parentWidget())
  {
    if (QLayout* l = p->layout())
    {
      l->activate();
    }
  }
  ui->terminalPlainText->repaint();
}

void FunctionEditorWidget::onUpdatePreviewBatch()
{
  QString errors;

  if (ui->suffixLineEdit->text().isEmpty())
  {
    errors += "- Missing prefix/suffix.\n";
  }

  if (ui->listBatchSources->count() == 0)
  {
    errors += "- No input series.\n";
  }

  SnippetData snippet;
  snippet.function = ui->functionTextBatch->toPlainText();
  snippet.global_vars = ui->globalVarsTextBatch->toPlainText();
  snippet.language = (currentLangBatch() == ScriptLang::Python) ? "python" : "lua";

  if (ui->listBatchSources->count() > 0)
  {
    snippet.linked_source = ui->listBatchSources->item(0)->text();
  }

  CustomPlotPtr custom_function;
  try
  {
    custom_function = createCustomFunction(snippet, currentLangBatch());
  }
  catch (std::runtime_error& err)
  {
    const QString lang = (currentLangBatch() == ScriptLang::Python) ? "Python" : "Lua";
    errors += QString("- Error in %1 script: %2").arg(lang).arg(err.what());
  }

  if (custom_function && ui->listBatchSources->count() > 0)
  {
    try
    {
      std::string name = "batch_preview";
      PlotData& out_data = _local_plot_data.getOrCreateNumeric(name);
      out_data.clear();

      std::vector<PlotData*> out_vector = { &out_data };
      custom_function->setData(&_plot_map_data, {}, out_vector);
      custom_function->calculate();
    }
    catch (std::runtime_error& err)
    {
      const QString lang = (currentLangBatch() == ScriptLang::Python) ? "Python" : "Lua";
      errors += QString("- Error in %1 script: %2").arg(lang).arg(err.what());
    }
  }

  if (errors.isEmpty())
  {
    ui->terminalBatchPlainText->hide();
    ui->pushButtonCreate->setEnabled(true);
    return;
  }

  ui->terminalBatchPlainText->show();
  ui->pushButtonCreate->setEnabled(false);
  ui->terminalBatchPlainText->setPlainText(errors.trimmed());

  if (QWidget* p = ui->terminalBatchPlainText->parentWidget())
  {
    if (QLayout* l = p->layout())
    {
      l->activate();
    }
  }
  ui->terminalBatchPlainText->repaint();
}

void FunctionEditorWidget::on_pushButtonHelp_clicked()
{
  QDialog* dialog = new QDialog(this);
  auto ui = new Ui_FunctionEditorHelp();
  ui->setupUi(dialog);

  dialog->setAttribute(Qt::WA_DeleteOnClose);
  dialog->exec();
}

void FunctionEditorWidget::onLineEditTab2FilterChanged()
{
  QString filter_text = ui->lineEditTab2Filter->text();
  ui->listBatchSources->clear();

  if (ui->radioButtonRegExp->isChecked() || ui->radioButtonWildcard->isChecked())
  {
    QRegExp rx(filter_text);
    if (ui->radioButtonWildcard->isChecked())
    {
      rx.setPatternSyntax(QRegExp::Wildcard);
    }

    for (const auto& [name, plotdata] : _plot_map_data.numeric)
    {
      auto qname = QString::fromStdString(name);
      if (rx.exactMatch(qname))
      {
        ui->listBatchSources->addItem(qname);
      }
    }
  }
  else
  {
    QStringList spaced_items = filter_text.split(' ', PJ::SkipEmptyParts);
    for (const auto& [name, plotdata] : _plot_map_data.numeric)
    {
      bool show = true;
      auto qname = QString::fromStdString(name);
      for (const auto& part : spaced_items)
      {
        if (qname.contains(part) == false)
        {
          show = false;
          break;
        }
      }
      if (show)
      {
        ui->listBatchSources->addItem(qname);
      }
    }
  }
  ui->listBatchSources->sortItems();
  onUpdatePreviewBatch();
}

void FunctionEditorWidget::on_pushButtonHelpTab2_clicked()
{
  on_pushButtonHelp_clicked();
}

void FunctionEditorWidget::on_lineEditTab2Filter_textChanged(const QString& arg1)
{
  _tab2_filter.triggerSignal(250);
}

void FunctionEditorWidget::on_functionTextBatch_textChanged()
{
  _update_preview_tab2.triggerSignal(250);
}

void FunctionEditorWidget::on_suffixLineEdit_textChanged(const QString& arg1)
{
  _update_preview_tab2.triggerSignal(250);
}

void FunctionEditorWidget::on_tabWidget_currentChanged(int index)
{
  bool is_batch = (index == 1);

  ui->label_name->setVisible(!is_batch);
  ui->nameLineEdit->setVisible(!is_batch);

  if (index == 0)
  {
    onUpdatePreview();
  }
  else
  {
    onUpdatePreviewBatch();
  }
}

void FunctionEditorWidget::on_globalVarsTextBatch_textChanged()
{
  _update_preview_tab2.triggerSignal(250);
}

void FunctionEditorWidget::on_globalVarsText_textChanged()
{
  _update_preview_tab1.triggerSignal(250);
}

void FunctionEditorWidget::on_functionText_textChanged()
{
  _update_preview_tab1.triggerSignal(250);
}
