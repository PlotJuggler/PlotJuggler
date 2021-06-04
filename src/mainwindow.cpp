#include <functional>
#include <stdio.h>
#include <numeric>

#include <QApplication>
#include <QActionGroup>
#include <QCheckBox>
#include <QCommandLineParser>
#include <QDebug>
#include <QDesktopServices>
#include <QDomDocument>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QInputDialog>
#include <QMenu>
#include <QGroupBox>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPluginLoader>
#include <QPushButton>
#include <QKeySequence>
#include <QScrollBar>
#include <QSettings>
#include <QStringListModel>
#include <QStringRef>
#include <QThread>
#include <QTextStream>
#include <QWindow>
#include <QHeaderView>
#include <QStandardPaths>

#include "mainwindow.h"
#include "curvelist_panel.h"
#include "tabbedplotwidget.h"
#include "PlotJuggler/plotdata.h"
#include "qwt_plot_canvas.h"
#include "transforms/function_editor.h"
#include "utils.h"
#include "PlotJuggler/svg_util.h"
#include "stylesheet.h"

#include "ui_aboutdialog.h"
#include "ui_support_dialog.h"
#include "preferences_dialog.h"
#include "nlohmann_parsers.h"
#include "cheatsheet/cheatsheet_dialog.h"

#ifdef COMPILED_WITH_CATKIN

#endif
#ifdef COMPILED_WITH_AMENT
#include <ament_index_cpp/get_package_prefix.hpp>
#include <ament_index_cpp/get_package_share_directory.hpp>
#endif

MainWindow::MainWindow(const QCommandLineParser& commandline_parser, QWidget* parent)
  : QMainWindow(parent)
  , ui(new Ui::MainWindow)
  , _undo_shortcut(QKeySequence(Qt::CTRL + Qt::Key_Z), this)
  , _redo_shortcut(QKeySequence(Qt::CTRL + Qt::SHIFT + Qt::Key_Z), this)
  , _fullscreen_shortcut(Qt::Key_F10, this)
  , _streaming_shortcut(QKeySequence(Qt::CTRL + Qt::Key_Space), this)
  , _playback_shotcut(Qt::Key_Space, this)
  , _minimized(false)
  , _active_streamer_plugin(nullptr)
  , _disable_undo_logging(false)
  , _tracker_time(0)
  , _tracker_param(CurveTracker::VALUE)
  , _labels_status(LabelStatus::RIGHT)
  , _recent_data_files(new QMenu())
  ,_recent_layout_files(new QMenu())
{
  QLocale::setDefault(QLocale::c());  // set as default

  _test_option = commandline_parser.isSet("test");
  _autostart_publishers = commandline_parser.isSet("publish");

  _curvelist_widget = new CurveListPanel(_custom_plots, this);

  ui->setupUi(this);

  QSettings settings;

  ui->playbackLoop->setText("");
  ui->pushButtonZoomOut->setText("");
  ui->pushButtonPlay->setText("");
  ui->pushButtonUseDateTime->setText("");
  ui->pushButtonActivateGrid->setText("");
  ui->pushButtonRatio->setText("");
  ui->pushButtonLink->setText("");
  ui->pushButtonTimeTracker->setText("");
  ui->pushButtonLoadDatafile->setText("");
  ui->pushButtonRemoveTimeOffset->setText("");
  ui->pushButtonLegend->setText("");

  if (commandline_parser.isSet("buffer_size"))
  {
    int buffer_size = std::max(10, commandline_parser.value("buffer_size").toInt());
    ui->streamingSpinBox->setMaximum(buffer_size);
  }

  _animated_streaming_movie = new QMovie(":/resources/animated_radio.gif");
  _animated_streaming_movie->setScaledSize(ui->labelStreamingAnimation->size());
  _animated_streaming_movie->jumpToFrame( 0 );

  _animated_streaming_timer = new QTimer();
  _animated_streaming_timer->setSingleShot(true);

  connect( _animated_streaming_timer, &QTimer::timeout,
           this, [this]()
  {
    _animated_streaming_movie->stop();
    _animated_streaming_movie->jumpToFrame( 0 );
  });

  ui->labelStreamingAnimation->setMovie(_animated_streaming_movie);
  ui->labelStreamingAnimation->setHidden(true);

  connect(this, &MainWindow::stylesheetChanged, this, &MainWindow::on_stylesheetChanged);

  connect(this, &MainWindow::stylesheetChanged, _curvelist_widget, &CurveListPanel::on_stylesheetChanged);

  connect(_curvelist_widget, &CurveListPanel::hiddenItemsChanged, this, &MainWindow::onUpdateLeftTableValues);

  connect(_curvelist_widget, &CurveListPanel::deleteCurves, this, &MainWindow::onDeleteMultipleCurves);

  connect(_curvelist_widget, &CurveListPanel::createMathPlot, this, &MainWindow::onAddCustomPlot);

  connect(_curvelist_widget, &CurveListPanel::editMathPlot, this, &MainWindow::onEditCustomPlot);

  connect(_curvelist_widget, &CurveListPanel::refreshMathPlot, this, &MainWindow::onRefreshCustomPlot);

  connect(ui->timeSlider, &RealSlider::realValueChanged, this, &MainWindow::onTimeSlider_valueChanged);

  connect(ui->playbackRate, &QDoubleSpinBox::editingFinished, this, [this]() { ui->playbackRate->clearFocus(); });

  connect(ui->playbackStep, &QDoubleSpinBox::editingFinished, this, [this]() { ui->playbackStep->clearFocus(); });

  _main_tabbed_widget = new TabbedPlotWidget("Main Window", this, _mapped_plot_data, this);

  connect(this, &MainWindow::stylesheetChanged, _main_tabbed_widget, &TabbedPlotWidget::on_stylesheetChanged);

  ui->plottingLayout->insertWidget(0, _main_tabbed_widget, 1);
  ui->leftLayout->addWidget(_curvelist_widget,1);

  ui->mainSplitter->setCollapsible(0, true);
  ui->mainSplitter->setStretchFactor(0, 2);
  ui->mainSplitter->setStretchFactor(1, 6);

  ui->layoutTimescale->removeWidget( ui->widgetButtons );
  _main_tabbed_widget->tabWidget()->setCornerWidget( ui->widgetButtons );

  connect(ui->mainSplitter, SIGNAL(splitterMoved(int, int)), SLOT(on_splitterMoved(int, int)));

  initializeActions();

  //------------ Load plugins -------------
  QStringList loaded;
  QStringList plugin_folders;

  plugin_folders += commandline_parser.value("plugin_folders").split(";", QString::SkipEmptyParts);
  plugin_folders += QCoreApplication::applicationDirPath();
  plugin_folders += settings.value("Preferences::plugin_folders", QStringList()).toStringList();

  try {
#ifdef COMPILED_WITH_CATKIN
    const char * env = std::getenv("CMAKE_PREFIX_PATH");
    if (env) {
      QString env_catkin_paths = QString::fromStdString( env );
      env_catkin_paths.replace(";",":"); // for windows
      auto catkin_paths = env_catkin_paths.split(":");

      for(const auto& path: catkin_paths)
      {
        plugin_folders += path + "/lib/plotjuggler_ros";
      }
    }
    plugin_folders += QCoreApplication::applicationDirPath() + "_ros";
#endif
#ifdef COMPILED_WITH_AMENT
    auto ros2_path = QString::fromStdString(ament_index_cpp::get_package_prefix("plotjuggler_ros"));
    ros2_path += "/lib/plotjuggler_ros";
    loaded += initializePlugins(ros2_path);
#endif
  } catch (...) {

    QMessageBox::warning(nullptr, "Missing package [plotjuggler-ros]",
                         "If you just upgraded from PlotJuggler 2.x to 3.x , try installing this package:\n\n"
                         "sudo apt install ros-${ROS_DISTRO}-plotjuggler-ros",
                         QMessageBox::Cancel, QMessageBox::Cancel);
  }

  plugin_folders += QStandardPaths::writableLocation( QStandardPaths::GenericDataLocation) + "/PlotJuggler";
  plugin_folders.removeDuplicates();

  for(const auto& folder: plugin_folders)
  {
    loaded += initializePlugins(folder);
  }

  //------------------------------------

  _undo_timer.start();

  // save initial state
  onUndoableChange();

  _replot_timer = new QTimer(this);
  _replot_timer->setInterval(40);
  connect(_replot_timer, &QTimer::timeout, this, [this]() { updateDataAndReplot(false); });

  _publish_timer = new QTimer(this);
  _publish_timer->setInterval(20);
  connect(_publish_timer, &QTimer::timeout, this, &MainWindow::onPlaybackLoop);

  ui->menuFile->setToolTipsVisible(true);

  this->setMenuBar(ui->menuBar);
  ui->menuBar->setNativeMenuBar(false);

  if (_test_option)
  {
    buildDummyData();
  }

  bool file_loaded = false;
  if (commandline_parser.isSet("datafile"))
  {
    QStringList datafiles = commandline_parser.values("datafile");
    file_loaded = loadDataFromFiles(datafiles);
  }
  if (commandline_parser.isSet("layout"))
  {
    loadLayoutFromFile(commandline_parser.value("layout"));
  }


  restoreGeometry(settings.value("MainWindow.geometry").toByteArray());
  restoreState(settings.value("MainWindow.state").toByteArray());

  //qDebug() << "restoreGeometry";

  bool activate_grid = settings.value("MainWindow.activateGrid", false).toBool();
  ui->pushButtonActivateGrid->setChecked(activate_grid);

  bool zoom_link_active = settings.value("MainWindow.buttonLink",true).toBool();
  ui->pushButtonLink->setChecked(zoom_link_active);

  bool ration_active = settings.value("MainWindow.buttonRatio",true).toBool();
  ui->pushButtonRatio->setChecked(ration_active);

  int streaming_buffer_value = settings.value("MainWindow.streamingBufferValue", 5).toInt();
  ui->streamingSpinBox->setValue(streaming_buffer_value);

  bool datetime_display = settings.value("MainWindow.dateTimeDisplay", false).toBool();
  ui->pushButtonUseDateTime->setChecked(datetime_display);

  bool remove_time_offset = settings.value("MainWindow.removeTimeOffset", true).toBool();
  ui->pushButtonRemoveTimeOffset->setChecked(remove_time_offset);

  //  ui->widgetOptions->setVisible(ui->pushButtonOptions->isChecked());

  if( settings.value("MainWindow.hiddenFileFrame", false).toBool() )
  {
    ui->buttonHideFileFrame->setText("+");
    ui->frameFile->setHidden(true);
  }
  if( settings.value("MainWindow.hiddenStreamingFrame", false).toBool() )
  {
    ui->buttonHideStreamingFrame->setText("+");
    ui->frameStreaming->setHidden(true);
  }
  if( settings.value("MainWindow.hiddenPublishersFrame", false).toBool() )
  {
    ui->buttonHidePublishersFrame->setText("+");
    ui->framePublishers->setHidden(true);
  }

  //----------------------------------------------------------
  QIcon trackerIconA, trackerIconB, trackerIconC;

  trackerIconA.addFile(QStringLiteral(":/style_light/line_tracker.png"), QSize(36, 36));
  trackerIconB.addFile(QStringLiteral(":/style_light/line_tracker_1.png"), QSize(36, 36));
  trackerIconC.addFile(QStringLiteral(":/style_light/line_tracker_a.png"), QSize(36, 36));

  _tracker_button_icons[CurveTracker::LINE_ONLY] = trackerIconA;
  _tracker_button_icons[CurveTracker::VALUE] = trackerIconB;
  _tracker_button_icons[CurveTracker::VALUE_NAME] = trackerIconC;

  int tracker_setting = settings.value("MainWindow.timeTrackerSetting", (int)CurveTracker::VALUE).toInt();
  _tracker_param = static_cast<CurveTracker::Parameter>(tracker_setting);

  ui->pushButtonTimeTracker->setIcon(_tracker_button_icons[_tracker_param]);

  forEachWidget([&](PlotWidget* plot) { plot->configureTracker(_tracker_param); });

  auto editor_layout = new QVBoxLayout();
  editor_layout->setMargin(0);
  ui->formulaPage->setLayout(editor_layout);
  _function_editor = new FunctionEditorWidget( _mapped_plot_data, _custom_plots, this);
  editor_layout->addWidget(_function_editor);

  connect(_function_editor, &FunctionEditorWidget::closed,
          this, [this]() {
    ui->widgetStack->setCurrentIndex(0);
  });

  connect(this, &MainWindow::stylesheetChanged,
          _function_editor, &FunctionEditorWidget::on_stylesheetChanged);

  connect(_function_editor, &FunctionEditorWidget::accept,
          this, &MainWindow::onCustomPlotCreated);

  QString theme = settings.value("Preferences::theme", "light").toString();
  if( theme != "dark") {
    theme = "light";
  }
  loadStyleSheet(tr(":/resources/stylesheet_%1.qss").arg(theme));

  // builtin messageParsers
  _message_parser_factory.insert( {"JSON", std::make_shared<JSON_ParserCreator>() });
  _message_parser_factory.insert( {"CBOR", std::make_shared<CBOR_ParserCreator>() });
  _message_parser_factory.insert( {"BSON", std::make_shared<BSON_ParserCreator>() });
  _message_parser_factory.insert( {"MessagePack", std::make_shared<MessagePack_ParserCreator>() });

}

MainWindow::~MainWindow()
{
  // important: avoid problems with plugins
  _mapped_plot_data.user_defined.clear();

  delete ui;
}

void MainWindow::onUndoableChange()
{
  if (_disable_undo_logging)
    return;

  int elapsed_ms = _undo_timer.restart();

  // overwrite the previous
  if (elapsed_ms < 100)
  {
    if (_undo_states.empty() == false)
      _undo_states.pop_back();
  }

  while (_undo_states.size() >= 100)
    _undo_states.pop_front();
  _undo_states.push_back(xmlSaveState());
  _redo_states.clear();
  //qDebug() << "undo " << _undo_states.size();
}

void MainWindow::onRedoInvoked()
{
  _disable_undo_logging = true;
  if (_redo_states.size() > 0)
  {
    QDomDocument state_document = _redo_states.back();
    while (_undo_states.size() >= 100)
      _undo_states.pop_front();
    _undo_states.push_back(state_document);
    _redo_states.pop_back();

    xmlLoadState(state_document);
  }
  //qDebug() << "undo " << _undo_states.size();
  _disable_undo_logging = false;
}

void MainWindow::onUndoInvoked()
{
  _disable_undo_logging = true;
  if (_undo_states.size() > 1)
  {
    QDomDocument state_document = _undo_states.back();
    while (_redo_states.size() >= 100)
      _redo_states.pop_front();
    _redo_states.push_back(state_document);
    _undo_states.pop_back();
    state_document = _undo_states.back();

    xmlLoadState(state_document);
  }
  //qDebug() << "undo " << _undo_states.size();
  _disable_undo_logging = false;
}

void MainWindow::onUpdateLeftTableValues()
{
  _curvelist_widget->update2ndColumnValues(_tracker_time, &_mapped_plot_data.numeric);
}

void MainWindow::onTrackerMovedFromWidget(QPointF relative_pos)
{
  _tracker_time = relative_pos.x() + _time_offset.get();

  auto prev = ui->timeSlider->blockSignals(true);
  ui->timeSlider->setRealValue(_tracker_time);
  ui->timeSlider->blockSignals(prev);

  onTrackerTimeUpdated(_tracker_time, true);
}

void MainWindow::onTimeSlider_valueChanged(double abs_time)
{
  _tracker_time = abs_time;
  onTrackerTimeUpdated(_tracker_time, true);
}

void MainWindow::onTrackerTimeUpdated(double absolute_time, bool do_replot)
{
  updatedDisplayTime();
  onUpdateLeftTableValues();

  for (auto& it : _state_publisher)
  {
    it.second->updateState(absolute_time);
  }

  forEachWidget([&](PlotWidget* plot) {
    plot->setTrackerPosition(_tracker_time);
    if (do_replot)
    {
      plot->replot();
    }
  });
}

void MainWindow::initializeActions()
{
  _undo_shortcut.setContext(Qt::ApplicationShortcut);
  _redo_shortcut.setContext(Qt::ApplicationShortcut);
  _fullscreen_shortcut.setContext(Qt::ApplicationShortcut);

  connect(&_undo_shortcut, &QShortcut::activated, this, &MainWindow::onUndoInvoked);
  connect(&_redo_shortcut, &QShortcut::activated, this, &MainWindow::onRedoInvoked);
  connect(&_streaming_shortcut, &QShortcut::activated, this, &MainWindow::on_streamingToggled);
  connect(&_playback_shotcut, &QShortcut::activated, ui->pushButtonPlay, &QPushButton::toggle);
  connect(&_fullscreen_shortcut, &QShortcut::activated, this, &MainWindow::onActionFullscreenTriggered);

  QShortcut* open_menu_shortcut = new QShortcut(QKeySequence(Qt::ALT + Qt::Key_F), this);
  connect(open_menu_shortcut, &QShortcut::activated,
          [this]() { ui->menuFile->exec(ui->menuBar->mapToGlobal(QPoint(0, 25))); });

  QShortcut* open_help_shortcut = new QShortcut(QKeySequence(Qt::ALT + Qt::Key_H), this);
  connect(open_help_shortcut, &QShortcut::activated,
          [this]() { ui->menuHelp->exec(ui->menuBar->mapToGlobal(QPoint(230, 25))); });

  //---------------------------------------------

  QSettings settings;
  updateRecentDataMenu(settings.value("MainWindow.recentlyLoadedDatafile").toStringList());
  updateRecentLayoutMenu(settings.value("MainWindow.recentlyLoadedLayout").toStringList());
}

QStringList MainWindow::initializePlugins(QString directory_name)
{
  static std::set<QString> loaded_plugins;
  QStringList loaded_out;

  qDebug() << "Loading compatible plugins from directory: " << directory_name;
  int loaded_count = 0;

  QDir pluginsDir(directory_name);

  for (const QString& filename : pluginsDir.entryList(QDir::Files))
  {
    QFileInfo fileinfo(filename);
    if (fileinfo.suffix() != "so" && fileinfo.suffix() != "dll" && fileinfo.suffix() != "dylib")
    {
      continue;
    }

    if (loaded_plugins.find(filename) != loaded_plugins.end())
    {
      continue;
    }

    QPluginLoader pluginLoader(pluginsDir.absoluteFilePath(filename), this);


    QObject* plugin = pluginLoader.instance();
    if (plugin)
    {
      auto class_name = pluginLoader.metaData().value("className").toString();
      loaded_out.push_back( class_name );

      DataLoader* loader = qobject_cast<DataLoader*>(plugin);
      StatePublisher* publisher = qobject_cast<StatePublisher*>(plugin);
      DataStreamer* streamer = qobject_cast<DataStreamer*>(plugin);
      MessageParserCreator* message_parser =  qobject_cast<MessageParserCreator*>(plugin);

      QString plugin_name;

      if (loader){
        plugin_name = loader->name();
      }
      else if (publisher){
        plugin_name = publisher->name();
      }
      else if (streamer){
        plugin_name = streamer->name();
      }
      else if (message_parser){
        plugin_name = message_parser->name();
      }

      if (loaded_plugins.find(plugin_name) == loaded_plugins.end())
      {
        loaded_plugins.insert(plugin_name);
        loaded_count++;
      }
      else
      {
        qDebug() << tr("Trying to load twice a plugin with name [%1]. Skipping...").arg(plugin_name);
        continue;
      }

      if (loader)
      {
        qDebug() << filename << ": is a DataLoader plugin";
        if (!_test_option && loader->isDebugPlugin())
        {
          qDebug() << filename << "...but will be ignored unless the argument -t is used.";
        }
        else
        {
          _data_loader.insert(std::make_pair(plugin_name, loader));
        }
      }
      else if (publisher)
      {
        publisher->setDataMap(&_mapped_plot_data);
        qDebug() << filename << ": is a StatePublisher plugin";
        if (!_test_option && publisher->isDebugPlugin())
        {
          qDebug() << filename << "...but will be ignored unless the argument -t is used.";
        }
        else
        {
          _state_publisher.insert(std::make_pair(plugin_name, publisher));

          ui->layoutPublishers->setColumnStretch(0, 1.0);

          int row = _state_publisher.size() -1;
          auto label = new QLabel(plugin_name, ui->framePublishers);
          ui->layoutPublishers->addWidget(label, row, 0);

          auto start_checkbox = new QCheckBox(ui->framePublishers);
          ui->layoutPublishers->addWidget(start_checkbox, row, 1);
          start_checkbox->setFocusPolicy( Qt::FocusPolicy::NoFocus );

          connect(start_checkbox, &QCheckBox::toggled, this,
                  [=](bool enable) { publisher->setEnabled(enable); });

          connect(publisher, &StatePublisher::closed, start_checkbox,
                  [=]() {start_checkbox->setChecked(false);} );

          if( publisher->availableActions().empty() )
          {
            QFrame* empty = new QFrame(ui->framePublishers);
            empty->setFixedSize({22,22});
            ui->layoutPublishers->addWidget(empty, row, 2);
          }
          else{
            auto options_button = new QPushButton(ui->framePublishers);
            options_button->setFlat(true);
            options_button->setFixedSize({24,24});
            ui->layoutPublishers->addWidget(options_button, row, 2);

            options_button->setIcon( LoadSvgIcon(":/resources/svg/settings_cog.svg", "light"));
            options_button->setIconSize( {16,16} );

            auto optionsMenu = [=]()
            {
              PopupMenu* menu = new PopupMenu(options_button, this);
              for(auto action: publisher->availableActions()) {
                menu->addAction(action);
              }
              menu->exec();
            };

            connect( options_button, &QPushButton::clicked,
                     options_button, optionsMenu);

            connect( this, &MainWindow::stylesheetChanged,
                     options_button, [=](QString style)
            {
              options_button->setIcon( LoadSvgIcon(":/resources/svg/settings_cog.svg", style));
            });
          }
        }
      }
      else if (message_parser)
      {
        _message_parser_factory.insert( std::make_pair(plugin_name, message_parser ) );
      }
      else if (streamer)
      {
        qDebug() << filename << ": is a DataStreamer plugin";
        if (!_test_option && streamer->isDebugPlugin())
        {
          qDebug() << filename << "...but will be ignored unless the argument -t is used.";
        }
        else
        {
          _data_streamer.insert(std::make_pair(plugin_name, streamer));

          streamer->setAvailableParsers( &_message_parser_factory );

          connect(streamer, &DataStreamer::closed, this,
                  [this]() { this->stopStreamingPlugin(); } );

          connect(streamer, &DataStreamer::clearBuffers,
                  this, &MainWindow::on_actionClearBuffer_triggered);

          connect(streamer, &DataStreamer::dataReceived,
                  _animated_streaming_movie, [this]()
          {
            _animated_streaming_movie->start();
            _animated_streaming_timer->start(500);
          });
        }
      }
    }
    else
    {
      if (pluginLoader.errorString().contains("is not an ELF object") == false)
      {
        qDebug() << filename << ": " << pluginLoader.errorString();
      }
    }
  }
  if( !_data_streamer.empty() )
  {
    QSignalBlocker block(ui->comboStreaming);
    ui->comboStreaming->setEnabled(true);
    ui->buttonStreamingStart->setEnabled(true);

    for(const auto& it: _data_streamer)
    {
      if( ui->comboStreaming->findText(it.first) == -1){
        ui->comboStreaming->addItem(it.first);
      }
    }

    // remember the previous one
    QSettings settings;
    QString streaming_name = settings.value("MainWindow.previousStreamingPlugin",
                                            ui->comboStreaming->itemText(0)).toString();

    auto streamer_it = _data_streamer.find(streaming_name);
    if( streamer_it == _data_streamer.end() )
    {
      streamer_it = _data_streamer.begin();
      streaming_name = streamer_it->first;
    }

    ui->comboStreaming->setCurrentText(streaming_name);

    bool contains_options = !streamer_it->second->availableActions().empty();
    ui->buttonStreamingOptions->setEnabled(contains_options);
  }
  qDebug() << "Number of plugins loaded: " << loaded_count << "\n";
  return loaded_out;
}

void MainWindow::buildDummyData()
{
  PlotDataMapRef datamap;

  static int count = 0;
  size_t SIZE = 1000;
  QElapsedTimer timer;
  timer.start();
  QStringList words_list;
  words_list << "world/siam"
             << "world/tre"
             << "walk/piccoli"
             << "walk/porcellin"
             << "fly/high/mai"
             << "fly/high/nessun"
             << "fly/low/ci"
             << "fly/low/dividera"
             << "data_1"
             << "data_2"
             << "data_3"
             << "data_10";

  for (int i = 0; i < 100; i++)
  {
    words_list.append(QString("data_vect/%1").arg(count++));
  }

  for (const QString& name : words_list)
  {
    double A = 6 * ((double)rand() / (double)RAND_MAX) - 3;
    double B = 3 * ((double)rand() / (double)RAND_MAX);
    double C = 3 * ((double)rand() / (double)RAND_MAX);
    double D = 20 * ((double)rand() / (double)RAND_MAX);

    auto it = datamap.addNumeric(name.toStdString());
    PlotData& plot = it->second;

    double t = 0;
    for (unsigned indx = 0; indx < SIZE; indx++)
    {
      t += 0.01;
      plot.pushBack(PlotData::Point(t + 35, A * sin(B * t + C) + D * t * 0.02));
    }
  }

  PlotData& sin_plot = datamap.addNumeric("_sin")->second;
  PlotData& cos_plot = datamap.addNumeric("_cos")->second;

  double t = 0;
  for (unsigned indx = 0; indx < SIZE; indx++)
  {
    t += 0.01;
    sin_plot.pushBack(PlotData::Point(t + 20, sin(t * 0.4)));
    cos_plot.pushBack(PlotData::Point(t + 20, cos(t * 0.4)));
  }

  importPlotDataMap(datamap, true);
}

void MainWindow::on_splitterMoved(int, int)
{
  QList<int> sizes = ui->mainSplitter->sizes();
  int max_left_size = _curvelist_widget->maximumWidth();
  int totalWidth = sizes[0] + sizes[1];

  // this is needed only once to restore the old size
  static bool first = true;
  if (sizes[0] != 0 && first)
  {
    first = false;
    QSettings settings;
    int splitter_width = settings.value("MainWindow.splitterWidth", 200).toInt();
    auto sizes = ui->mainSplitter->sizes();
    int tot_splitter_width = sizes[0] + sizes[1];
    sizes[0] = splitter_width;
    sizes[1] = tot_splitter_width - splitter_width;
    ui->mainSplitter->setSizes(sizes);
    return;
  }

  if (sizes[0] > max_left_size)
  {
    sizes[0] = max_left_size;
    sizes[1] = totalWidth - max_left_size;
    ui->mainSplitter->setSizes(sizes);
  }
}

void MainWindow::resizeEvent(QResizeEvent*)
{
  on_splitterMoved(0, 0);
}

void MainWindow::onPlotAdded(PlotWidget* plot)
{
  connect(plot, &PlotWidget::undoableChange, this, &MainWindow::onUndoableChange);

  connect(plot, &PlotWidget::trackerMoved, this, &MainWindow::onTrackerMovedFromWidget);

  // TODO connect(plot, &PlotWidget::swapWidgetsRequested, this, &MainWindow::onSwapPlots);

  connect(this, &MainWindow::dataSourceRemoved, plot, &PlotWidget::onSourceDataRemoved);

  connect(plot, &PlotWidget::curveListChanged, this, [this]() {
    updateTimeOffset();
    updateTimeSlider();
  });

  connect(&_time_offset, SIGNAL(valueChanged(double)), plot, SLOT(on_changeTimeOffset(double)));

  connect(ui->pushButtonUseDateTime, &QPushButton::toggled, plot, &PlotWidget::on_changeDateTimeScale);

  connect(plot, &PlotWidget::curvesDropped, _curvelist_widget, &CurveListPanel::clearSelections);

  connect(plot, &PlotWidget::legendSizeChanged, this, [=](int point_size)
  {
    auto visitor = [=](PlotWidget* p){
      if(plot != p) p->setLegendSize(point_size);
    };
    this->forEachWidget(visitor);
  });

  connect(plot, &PlotWidget::rectChanged, this, &MainWindow::onPlotZoomChanged);

  plot->on_changeTimeOffset(_time_offset.get());
  plot->on_changeDateTimeScale(ui->pushButtonUseDateTime->isChecked());
  plot->activateGrid(ui->pushButtonActivateGrid->isChecked());
  plot->enableTracker(!isStreamingActive());
  plot->configureTracker(_tracker_param);
}

void MainWindow::onPlotZoomChanged(PlotWidget* modified_plot, QRectF new_range)
{
  if (ui->pushButtonLink->isChecked())
  {
    auto visitor = [=](PlotWidget* plot){
      if(plot != modified_plot &&
         !plot->isEmpty() &&
         !plot->isXYPlot())
      {
        QRectF bound_act = plot->canvasBoundingRect();
        bound_act.setLeft(new_range.left());
        bound_act.setRight(new_range.right());
        plot->setZoomRectangle(bound_act, false);
        plot->on_zoomOutVertical_triggered(false);
        plot->replot();
      }
    };
    this->forEachWidget(visitor);
  }

  onUndoableChange();
}

void MainWindow::onPlotTabAdded(PlotDocker* docker)
{
  connect(docker, &PlotDocker::plotWidgetAdded, this, &MainWindow::onPlotAdded);

  connect( this, &MainWindow::stylesheetChanged, docker, &PlotDocker::on_stylesheetChanged);

  // TODO  connect(matrix, &PlotMatrix::undoableChange, this, &MainWindow::onUndoableChange);
}

QDomDocument MainWindow::xmlSaveState() const
{
  QDomDocument doc;
  QDomProcessingInstruction instr = doc.createProcessingInstruction("xml", "version='1.0' encoding='UTF-8'");

  doc.appendChild(instr);

  QDomElement root = doc.createElement("root");

  for (auto& it : TabbedPlotWidget::instances())
  {
    QDomElement tabbed_area = it.second->xmlSaveState(doc);
    root.appendChild(tabbed_area);
  }

  doc.appendChild(root);

  QDomElement relative_time = doc.createElement("use_relative_time_offset");
  relative_time.setAttribute("enabled", ui->pushButtonRemoveTimeOffset->isChecked());
  root.appendChild(relative_time);

  return doc;
}

void MainWindow::checkAllCurvesFromLayout(const QDomElement& root)
{
  std::set<std::string> curves;

  for (QDomElement tw = root.firstChildElement("tabbed_widget"); !tw.isNull(); tw = tw.nextSiblingElement("tabbed_"
                                                                                                          "widget"))
  {
    for (QDomElement pm = tw.firstChildElement("plotmatrix"); !pm.isNull(); pm = pm.nextSiblingElement("plotmatrix"))
    {
      for (QDomElement pl = pm.firstChildElement("plot"); !pl.isNull(); pl = pl.nextSiblingElement("plot"))
      {
        QDomElement tran_elem = pl.firstChildElement("transform");
        std::string trans = tran_elem.attribute("value").toStdString();
        bool is_XY_plot = (trans == "XYPlot");

        for (QDomElement cv = pl.firstChildElement("curve"); !cv.isNull(); cv = cv.nextSiblingElement("curve"))
        {
          if (is_XY_plot)
          {
            curves.insert(cv.attribute("curve_x").toStdString());
            curves.insert(cv.attribute("curve_y").toStdString());
          }
          else
          {
            curves.insert(cv.attribute("name").toStdString());
          }
        }
      }
    }
  }

  std::vector<std::string> missing_curves;

  for (auto& curve_name : curves)
  {
    if (_mapped_plot_data.numeric.count(curve_name) == 0)
    {
      missing_curves.push_back(curve_name);
    }
  }
  if (missing_curves.size() > 0)
  {
    QMessageBox msgBox(this);
    msgBox.setWindowTitle("Warning");
    msgBox.setText(tr("One or more timeseries in the layout haven't been loaded yet\n"
                      "What do you want to do?"));

    QPushButton* buttonRemove = msgBox.addButton(tr("Remove curves from plots"), QMessageBox::RejectRole);
    QPushButton* buttonPlaceholder = msgBox.addButton(tr("Create empty placeholders"), QMessageBox::YesRole);
    msgBox.setDefaultButton(buttonPlaceholder);
    msgBox.exec();
    if (msgBox.clickedButton() == buttonPlaceholder)
    {
      for (auto& name : missing_curves)
      {
        _curvelist_widget->addCurve(QString::fromStdString(name));
        _mapped_plot_data.addNumeric(name);
      }
      _curvelist_widget->refreshColumns();
    }
  }
}

bool MainWindow::xmlLoadState(QDomDocument state_document)
{
  QDomElement root = state_document.namedItem("root").toElement();
  if (root.isNull())
  {
    qWarning() << "No <root> element found at the top-level of the XML file!";
    return false;
  }

  size_t num_floating = 0;
  std::map<QString, QDomElement> tabbed_widgets_with_name;

  for (QDomElement tw = root.firstChildElement("tabbed_widget");
       tw.isNull() == false;
       tw = tw.nextSiblingElement("tabbed_widget"))
  {
    if (tw.attribute("parent") != ("main_window"))
    {
      num_floating++;
    }
    tabbed_widgets_with_name[tw.attribute("name")] = tw;
  }

  // add if missing
  for (const auto& it : tabbed_widgets_with_name)
  {
    if (TabbedPlotWidget::instance(it.first) == nullptr)
    {
      // TODO createTabbedDialog(it.first, nullptr);
    }
  }

  // remove those which don't share list of names
  for (const auto& it : TabbedPlotWidget::instances())
  {
    if (tabbed_widgets_with_name.count(it.first) == 0)
    {
      it.second->deleteLater();
    }
  }

  //-----------------------------------------------------
  checkAllCurvesFromLayout(root);
  //-----------------------------------------------------

  for (QDomElement tw = root.firstChildElement("tabbed_widget");
       tw.isNull() == false;
       tw = tw.nextSiblingElement("tabbed_widget"))
  {
    TabbedPlotWidget* tabwidget = TabbedPlotWidget::instance(tw.attribute("name"));
    tabwidget->xmlLoadState(tw);
  }

  QDomElement relative_time = root.firstChildElement("use_relative_time_offset");
  if (!relative_time.isNull())
  {
    bool remove_offset = (relative_time.attribute("enabled") == QString("1"));
    ui->pushButtonRemoveTimeOffset->setChecked(remove_offset);
  }
  return true;
}

void MainWindow::onDeleteMultipleCurves(const std::vector<std::string>& curve_names)
{
  for (const auto& curve_name : curve_names)
  {
    auto plot_curve = _mapped_plot_data.numeric.find(curve_name);
    if (plot_curve == _mapped_plot_data.numeric.end())
    {
      continue;
    }

    emit dataSourceRemoved(curve_name);
    _mapped_plot_data.numeric.erase(plot_curve);

    auto custom_it = _custom_plots.find(curve_name);
    if (custom_it != _custom_plots.end())
    {
      _custom_plots.erase(custom_it);
    }

    _curvelist_widget->removeCurve(curve_name);
  }

  forEachWidget([](PlotWidget* plot) { plot->replot(); });
}

void MainWindow::updateRecentDataMenu(QStringList new_filenames)
{
  QMenu* menu = _recent_data_files;

  QAction* separator = nullptr;
  QStringList prev_filenames;
  for (QAction* action : menu->actions())
  {
    if (action->isSeparator())
    {
      separator = action;
      break;
    }
    if (new_filenames.contains(action->text()) == false)
    {
      prev_filenames.push_back(action->text());
    }
    menu->removeAction(action);
  }

  new_filenames.append(prev_filenames);
  while (new_filenames.size() > 10)
  {
    new_filenames.removeLast();
  }

  for (const auto& filename : new_filenames)
  {
    QAction* action = new QAction(filename, nullptr);
    connect(action, &QAction::triggered, this, [this, filename] { loadDataFromFiles({ filename }); });
    menu->insertAction(separator, action);
  }

  QSettings settings;
  settings.setValue("MainWindow.recentlyLoadedDatafile", new_filenames);
  menu->setEnabled(new_filenames.size() > 0);
}

void MainWindow::updateRecentLayoutMenu(QStringList new_filenames)
{
  QMenu* menu = _recent_layout_files;

  QAction* separator = nullptr;
  QStringList prev_filenames;
  for (QAction* action : menu->actions())
  {
    if (action->isSeparator())
    {
      separator = action;
      break;
    }
    if (new_filenames.contains(action->text()) == false)
    {
      prev_filenames.push_back(action->text());
    }
    menu->removeAction(action);
  }

  new_filenames.append(prev_filenames);
  while (new_filenames.size() > 10)
  {
    new_filenames.removeLast();
  }

  for (const auto& filename : new_filenames)
  {
    QAction* action = new QAction(filename, nullptr);
    connect(action, &QAction::triggered, this, [this, filename] {
      if (this->loadLayoutFromFile(filename))
      {
        updateRecentLayoutMenu({ filename });
      }
    });
    menu->insertAction(separator, action);
  }

  QSettings settings;
  settings.setValue("MainWindow.recentlyLoadedLayout", new_filenames);
  menu->setEnabled(new_filenames.size() > 0);
}

void MainWindow::deleteAllData()
{
  forEachWidget([](PlotWidget* plot) { plot->removeAllCurves(); });

  _mapped_plot_data.numeric.clear();
  _mapped_plot_data.user_defined.clear();
  _custom_plots.clear();
  _curvelist_widget->clear();
  _loaded_datafiles.clear();

  bool stopped = false;

  for(int idx = 0; idx < ui->layoutPublishers->count(); idx++)
  {
    QLayoutItem * item = ui->layoutPublishers->itemAt(idx);
    if(dynamic_cast<QWidgetItem *>(item))
    {
      if( auto checkbox = dynamic_cast<QCheckBox *>(item->widget())) {
        if( checkbox->isChecked() ) {
          checkbox->setChecked(false);
          stopped = true;
        }
      }
    }
  }

  if (stopped)
  {
    QMessageBox::warning(this, "State publishers stopped",
                         "All the state publishers have been stopped because old data has been deleted.");
  }
}

template <typename T>
void importPlotDataMapHelper(std::unordered_map<std::string, T>& source,
                             std::unordered_map<std::string, T>& destination, bool delete_older)
{
  for (auto& it : source)
  {
    const std::string& name = it.first;
    T& source_plot = it.second;
    auto plot_with_same_name = destination.find(name);

    // this is a new plot
    if (plot_with_same_name == destination.end())
    {
      plot_with_same_name =
          destination.emplace(std::piecewise_construct, std::forward_as_tuple(name), std::forward_as_tuple(name)).first;
    }
    T& destination_plot = plot_with_same_name->second;

    if (delete_older)
    {
      double max_range_x = destination_plot.maximumRangeX();
      destination_plot.swapData(source_plot);
      destination_plot.setMaximumRangeX(max_range_x);  // just in case
    }
    else
    {
      for (size_t i = 0; i < source_plot.size(); i++)
      {
        destination_plot.pushBack(source_plot.at(i));
      }
    }
    source_plot.clear();
  }
}

void MainWindow::importPlotDataMap(PlotDataMapRef& new_data, bool remove_old)
{
  if (new_data.user_defined.empty() && new_data.numeric.empty())
  {
    return;
  }

  if (remove_old)
  {
    std::vector<std::string> old_plots_to_delete;

    for (auto& it : _mapped_plot_data.numeric)
    {
      // timeseries in old but not in new
      if (new_data.numeric.count(it.first) == 0)
      {
        old_plots_to_delete.push_back(it.first);
      }
    }

    if (!old_plots_to_delete.empty())
    {
      QMessageBox::StandardButton reply;
      reply = QMessageBox::question(this, tr("Warning"), tr("Do you want to remove the previously loaded data?\n"),
                                    QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
      if (reply == QMessageBox::Yes)
      {
        onDeleteMultipleCurves(old_plots_to_delete);
      }
    }
  }

  bool curvelist_modified = false;
  for (auto& it : new_data.numeric)
  {
    const std::string& name = it.first;
    if (it.second.size() > 0 && _mapped_plot_data.numeric.count(name) == 0)
    {
      _curvelist_widget->addCurve(QString::fromStdString(name));
      curvelist_modified = true;
    }
  }

  importPlotDataMapHelper(new_data.numeric, _mapped_plot_data.numeric, remove_old);
  importPlotDataMapHelper(new_data.user_defined, _mapped_plot_data.user_defined, remove_old);

  if (curvelist_modified)
  {
    _curvelist_widget->refreshColumns();
  }

}

bool MainWindow::isStreamingActive() const
{
  return !ui->buttonStreamingPause->isChecked() &&
      _active_streamer_plugin;
}

bool MainWindow::loadDataFromFiles(QStringList filenames)
{
  if (filenames.size() > 1)
  {
    QMessageBox msgbox;
    msgbox.setWindowTitle("Loading multiple files");
    msgbox.setText("You are loading multiple files at once. A prefix will be automatically added to the name of the "
                   "timeseries.\n\n"
                   "This is an experimental feature. Publishers will not work as you may expect.");
    msgbox.addButton(QMessageBox::Ok);
    //    QCheckBox *cb = new QCheckBox("Don't show this again");
    //    msgbox.setCheckBox(cb);
    //    connect(cb, &QCheckBox::stateChanged, this, [this, cb , &show_me]() {  show_me = !cb->isChecked(); } );
    msgbox.exec();
  }

  char prefix_ch = 'A';
  QStringList loaded_filenames;

  for (int i = 0; i < filenames.size(); i++)
  {
    FileLoadInfo info;
    info.filename = filenames[i];
    if (true)//filenames.size() > 1)
    {
      QFileInfo fi;
      fi = filenames[i];
      info.prefix = fi.fileName();
    }

    if (loadDataFromFile(info))
    {
      loaded_filenames.push_back(filenames[i]);
      prefix_ch++;
    }
  }
  if (loaded_filenames.size() > 0)
  {
    updateRecentDataMenu(loaded_filenames);
    return true;
  }
  return false;
}

bool MainWindow::loadDataFromFile(const FileLoadInfo& info)
{
  ui->pushButtonPlay->setChecked(false);

  const QString extension = QFileInfo(info.filename).suffix().toLower();

  typedef std::map<QString, DataLoaderPtr>::iterator MapIterator;

  std::vector<MapIterator> compatible_loaders;

  for (auto it = _data_loader.begin(); it != _data_loader.end(); ++it)
  {
    DataLoaderPtr data_loader = it->second;
    std::vector<const char*> extensions = data_loader->compatibleFileExtensions();

    for (auto& ext : extensions)
    {
      if (extension == QString(ext).toLower())
      {
        compatible_loaders.push_back(it);
        break;
      }
    }
  }

  DataLoaderPtr dataloader;

  if (compatible_loaders.size() == 1)
  {
    dataloader = compatible_loaders.front()->second;
  }
  else
  {
    static QString last_plugin_name_used;

    QStringList names;
    for (auto& cl : compatible_loaders)
    {
      const auto& name = cl->first;

      if (name == last_plugin_name_used)
      {
        names.push_front(name);
      }
      else
      {
        names.push_back(name);
      }
    }

    bool ok;
    QString plugin_name = QInputDialog::getItem(this, tr("QInputDialog::getItem()"), tr("Select the loader to use:"),
                                                names, 0, false, &ok);
    if (ok && !plugin_name.isEmpty())
    {
      dataloader = _data_loader[plugin_name];
      last_plugin_name_used = plugin_name;
    }
  }

  if (dataloader)
  {
    QFile file(info.filename);

    if (!file.open(QFile::ReadOnly | QFile::Text))
    {
      QMessageBox::warning(this, tr("Datafile"),
                           tr("Cannot read file %1:\n%2.").arg(info.filename).arg(file.errorString()));
      return false;
    }
    file.close();

    try
    {
      PlotDataMapRef mapped_data;
      FileLoadInfo new_info = info;

      if (dataloader->readDataFromFile(&new_info, mapped_data))
      {
        AddPrefixToPlotData(info.prefix.toStdString(), mapped_data.numeric);

        importPlotDataMap(mapped_data, true);

        QDomElement plugin_elem = dataloader->xmlSaveState(new_info.plugin_config);
        new_info.plugin_config.appendChild(plugin_elem);

        bool duplicate = false;

        // substitute an old item of _loaded_datafiles or push_back another item.
        for (auto& prev_loaded : _loaded_datafiles)
        {
          if (prev_loaded.filename == new_info.filename && prev_loaded.prefix == new_info.prefix)
          {
            prev_loaded = new_info;
            duplicate = true;
            break;
          }
        }

        if (!duplicate)
        {
          _loaded_datafiles.push_back(new_info);
        }
      }
    }
    catch (std::exception& ex)
    {
      QMessageBox::warning(
            this, tr("Exception from the plugin"),
            tr("The plugin [%1] thrown the following exception: \n\n %3\n").arg(dataloader->name()).arg(ex.what()));
      return false;
    }
  }
  else
  {
    QMessageBox::warning(this, tr("Error"),
                         tr("Cannot read files with extension %1.\n No plugin can handle that!\n").arg(info.filename));
  }
  _curvelist_widget->updateFilter();

  // clean the custom plot. Function updateDataAndReplot will update them
  for (auto& custom_it : _custom_plots)
  {
    auto dst_data_it = _mapped_plot_data.numeric.find( custom_it.first );
    if (dst_data_it != _mapped_plot_data.numeric.end())
    {
      dst_data_it->second.clear();
    }
    custom_it.second->clear();
  }

  updateDataAndReplot(true);
  ui->timeSlider->setRealValue(ui->timeSlider->getMinimum());

  return true;
}


void MainWindow::on_buttonStreamingPause_toggled(bool paused)
{
  if (!_active_streamer_plugin)
  {
    paused = true;
  }

  ui->pushButtonRemoveTimeOffset->setEnabled(paused);
  ui->widgetPlay->setEnabled(paused);

  if (!paused && ui->pushButtonPlay->isChecked())
  {
    ui->pushButtonPlay->setChecked(false);
  }

  forEachWidget([&](PlotWidget* plot) {
    plot->enableTracker(paused);
    plot->setZoomEnabled(paused);
  });

  if (!paused)
  {
    _replot_timer->start();
    updateTimeOffset();
  }
  else
  {
    _replot_timer->stop();
    updateDataAndReplot(true);
    onUndoableChange();
  }
}

void MainWindow::on_streamingToggled()
{
  if (_active_streamer_plugin)
  {
    bool prev_state = ui->buttonStreamingPause->isChecked();
    ui->buttonStreamingPause->setChecked(!prev_state);
  }
}


void MainWindow::stopStreamingPlugin()
{
  ui->comboStreaming->setEnabled(true);
  ui->buttonStreamingStart->setText("Start");
  ui->buttonStreamingPause->setEnabled(false);
  ui->labelStreamingAnimation->setHidden(true);

  // force the cleanups typically done in on_buttonStreamingPause_toggled
  if(  ui->buttonStreamingPause->isChecked() )
  {
    // Will call on_buttonStreamingPause_toggled
    ui->buttonStreamingPause->setChecked(false);
  }
  else{
    // call it manually
    on_buttonStreamingPause_toggled(true);
  }

  if( _active_streamer_plugin ) {
    _active_streamer_plugin->shutdown();
    _active_streamer_plugin = nullptr;
  }

  if (!_mapped_plot_data.numeric.empty())
  {
    ui->actionDeleteAllData->setToolTip("");
  }

  // reset max range.
  for (auto& it : _mapped_plot_data.numeric)
  {
    it.second.setMaximumRangeX(std::numeric_limits<double>::max());
  }
  for (auto& it : _mapped_plot_data.user_defined)
  {
    it.second.setMaximumRangeX(std::numeric_limits<double>::max());
  }
}

void MainWindow::startStreamingPlugin(QString streamer_name)
{
  if (_active_streamer_plugin)
  {
    _active_streamer_plugin->shutdown();
    _active_streamer_plugin = nullptr;
  }

  if (_data_streamer.empty())
  {
    qDebug() << "Error, no streamer loaded";
    return;
  }

  auto it = _data_streamer.find(streamer_name);
  if (it != _data_streamer.end())
  {
    _active_streamer_plugin = it->second;
  }
  else
  {
    qDebug() << "Error. The streamer " << streamer_name << " can't be loaded";
    _active_streamer_plugin = nullptr;
    return;
  }

  bool started = false;
  try
  {
    // TODO data sources (argument to _active_streamer_plugin->start()
    started = _active_streamer_plugin && _active_streamer_plugin->start(nullptr);
  }
  catch (std::runtime_error& err)
  {
    QMessageBox::warning(this, tr("Exception from the plugin"),
                         tr("The plugin thrown the following exception: \n\n %1\n").arg(err.what()));
    _active_streamer_plugin = nullptr;
    return;
  }
  if (started)
  {
    {
      std::lock_guard<std::mutex> lock(_active_streamer_plugin->mutex());
      importPlotDataMap(_active_streamer_plugin->dataMap(), true);
    }

    ui->actionClearBuffer->setEnabled(true);
    ui->actionDeleteAllData->setToolTip("Stop streaming to be able to delete the data");

    ui->buttonStreamingStart->setText("Stop");
    ui->buttonStreamingPause->setEnabled(true);
    ui->buttonStreamingPause->setChecked(false);
    ui->comboStreaming->setEnabled(false);
    ui->labelStreamingAnimation->setHidden(false);

    // force start
    on_buttonStreamingPause_toggled(false);
    // this will force the update the max buffer size values
    on_streamingSpinBox_valueChanged(ui->streamingSpinBox->value());
  }
  else
  {
    QSignalBlocker block( ui->buttonStreamingStart );
    ui->buttonStreamingStart->setChecked(false);
    qDebug() << "Failed to launch the streamer";
    _active_streamer_plugin = nullptr;
  }
}

void MainWindow::loadStyleSheet(QString file_path)
{
  QFile styleFile(file_path);
  styleFile.open(QFile::ReadOnly);
  try{
    QString theme = SetApplicationStyleSheet( styleFile.readAll() );

    forEachWidget([&](PlotWidget* plot) {
      plot->replot();
    });

    emit stylesheetChanged(theme);
  }
  catch( std::runtime_error& err )
  {
    QMessageBox::warning(this, tr("Error loading StyleSheet"), tr(err.what()));
  }
}


void MainWindow::on_stylesheetChanged(QString theme)
{
  ui->pushButtonLoadDatafile->setIcon(LoadSvgIcon(":/resources/svg/import.svg", theme));
  ui->buttonStreamingPause->setIcon(LoadSvgIcon(":/resources/svg/pause.svg", theme));

  ui->buttonRecentData->setIcon(LoadSvgIcon(":/resources/svg/right-arrow.svg", theme));
  ui->buttonRecentLayout->setIcon(LoadSvgIcon(":/resources/svg/right-arrow.svg", theme));

  ui->pushButtonZoomOut->setIcon(LoadSvgIcon(":/resources/svg/zoom_max.svg", theme));
  ui->playbackLoop->setIcon(LoadSvgIcon(":/resources/svg/loop.svg", theme));
  ui->pushButtonPlay->setIcon(LoadSvgIcon(":/resources/svg/play_arrow.svg", theme));
  ui->pushButtonUseDateTime->setIcon(LoadSvgIcon(":/resources/svg/datetime.svg", theme));
  ui->pushButtonActivateGrid->setIcon(LoadSvgIcon(":/resources/svg/grid.svg", theme));
  ui->pushButtonRatio->setIcon(LoadSvgIcon(":/resources/svg/ratio.svg", theme));

  ui->pushButtonLoadLayout->setIcon(LoadSvgIcon(":/resources/svg/import.svg", theme));
  ui->pushButtonSaveLayout->setIcon(LoadSvgIcon(":/resources/svg/export.svg", theme));

  ui->pushButtonLink->setIcon(LoadSvgIcon(":/resources/svg/link.svg", theme));
  ui->pushButtonRemoveTimeOffset->setIcon(LoadSvgIcon(":/resources/svg/t0.svg", theme));
  ui->pushButtonLegend->setIcon(LoadSvgIcon(":/resources/svg/legend.svg", theme));

  ui->buttonStreamingOptions->setIcon(LoadSvgIcon(":/resources/svg/settings_cog.svg", theme));
}

void MainWindow::loadPluginState(const QDomElement& root)
{
  QDomElement plugins = root.firstChildElement("Plugins");

  for (QDomElement plugin_elem = plugins.firstChildElement(); plugin_elem.isNull() == false;
       plugin_elem = plugin_elem.nextSiblingElement())
  {
    const QString plugin_name = plugin_elem.attribute("ID");

    if (plugin_elem.nodeName() != "plugin" || plugin_name.isEmpty())
    {
      QMessageBox::warning(this, tr("Error loading Plugin State from Layout"),
                           tr("The method xmlSaveState() must return a node like this <plugin ID=\"PluginName\" "));
    }

    if (_data_loader.find(plugin_name) != _data_loader.end())
    {
      _data_loader[plugin_name]->xmlLoadState(plugin_elem);
    }
    if (_data_streamer.find(plugin_name) != _data_streamer.end())
    {
      _data_streamer[plugin_name]->xmlLoadState(plugin_elem);
    }
    if (_state_publisher.find(plugin_name) != _state_publisher.end())
    {
      StatePublisherPtr publisher = _state_publisher[plugin_name];
      publisher->xmlLoadState(plugin_elem);

      if (_autostart_publishers && plugin_elem.attribute("status") == "active")
      {
        publisher->setEnabled(true);
      }
    }
  }
}

QDomElement MainWindow::savePluginState(QDomDocument& doc)
{
  QDomElement list_plugins = doc.createElement("Plugins");

  auto CheckValidFormat = [this](const QString& expected_name, const QDomElement& elem) {
    if (elem.nodeName() != "plugin" || elem.attribute("ID") != expected_name)
    {
      QMessageBox::warning(this, tr("Error saving Plugin State to Layout"),
                           tr("The method xmlSaveState() returned\n<plugin ID=\"%1\">\ninstead of\n<plugin ID=\"%2\">")
                           .arg(elem.attribute("ID"))
                           .arg(expected_name));
    }
  };

  for (auto& it : _data_loader)
  {
    const DataLoaderPtr dataloader = it.second;
    QDomElement plugin_elem = dataloader->xmlSaveState(doc);
    if (!plugin_elem.isNull())
    {
      list_plugins.appendChild(plugin_elem);
      CheckValidFormat(it.first, plugin_elem);
    }
  }

  for (auto& it : _data_streamer)
  {
    const DataStreamerPtr datastreamer = it.second;
    QDomElement plugin_elem = datastreamer->xmlSaveState(doc);
    if (!plugin_elem.isNull())
    {
      list_plugins.appendChild(plugin_elem);
      CheckValidFormat(it.first, plugin_elem);
    }
  }

  for (auto& it : _state_publisher)
  {
    const StatePublisherPtr state_publisher = it.second;
    QDomElement plugin_elem = state_publisher->xmlSaveState(doc);
    if (!plugin_elem.isNull())
    {
      list_plugins.appendChild(plugin_elem);
      CheckValidFormat(it.first, plugin_elem);
    }

    plugin_elem.setAttribute("status", state_publisher->enabled() ? "active" : "idle");
  }
  return list_plugins;
}

std::tuple<double, double, int> MainWindow::calculateVisibleRangeX()
{
  // find min max time
  double min_time = std::numeric_limits<double>::max();
  double max_time = -std::numeric_limits<double>::max();
  int max_steps = 0;

  forEachWidget([&](PlotWidget* widget) {
    for (auto& it : widget->curveList())
    {
      const auto& curve_name = it.src_name;

      auto plot_it = _mapped_plot_data.numeric.find(curve_name);
      if (plot_it == _mapped_plot_data.numeric.end())
      {
        continue;  // FIXME?
      }
      const auto& data = plot_it->second;
      if (data.size() >= 1)
      {
        const double t0 = data.front().x;
        const double t1 = data.back().x;
        min_time = std::min(min_time, t0);
        max_time = std::max(max_time, t1);
        max_steps = std::max(max_steps, (int)data.size());
      }
    }
  });

  // needed if all the plots are empty
  if (max_steps == 0 || max_time < min_time)
  {
    for (const auto& it : _mapped_plot_data.numeric)
    {
      const PlotData& data = it.second;
      if (data.size() >= 1)
      {
        const double t0 = data.front().x;
        const double t1 = data.back().x;
        min_time = std::min(min_time, t0);
        max_time = std::max(max_time, t1);
        max_steps = std::max(max_steps, (int)data.size());
      }
    }
  }

  // last opportunity. Everything else failed
  if (max_steps == 0 || max_time < min_time)
  {
    min_time = 0.0;
    max_time = 1.0;
    max_steps = 1;
  }
  return std::tuple<double, double, int>(min_time, max_time, max_steps);
}

static const QString LAYOUT_VERSION = "2.3.8";

bool MainWindow::loadLayoutFromFile(QString filename)
{
  QSettings settings;

  QFile file(filename);
  if (!file.open(QFile::ReadOnly | QFile::Text))
  {
    QMessageBox::warning(this, tr("Layout"), tr("Cannot read file %1:\n%2.").arg(filename).arg(file.errorString()));
    return false;
  }

  QString errorStr;
  int errorLine, errorColumn;

  QDomDocument domDocument;

  if (!domDocument.setContent(&file, true, &errorStr, &errorLine, &errorColumn))
  {
    QMessageBox::information(window(), tr("XML Layout"),
                             tr("Parse error at line %1:\n%2").arg(errorLine).arg(errorStr));
    return false;
  }

  //-------------------------------------------------
  // refresh plugins
  QDomElement root = domDocument.namedItem("root").toElement();

  if (!root.hasAttribute("version") || root.attribute("version") != LAYOUT_VERSION)
  {
    QMessageBox msgBox(this);
    msgBox.setWindowTitle("Obsolate Layout version");
    msgBox.setText(tr("This Layout ID is not supported [%1].\nThis version of PlotJuggler use Layout ID [%2]")
                   .arg(root.attribute("version"))
                   .arg(LAYOUT_VERSION));

    msgBox.setStandardButtons(QMessageBox::Abort);
    QPushButton* continueButton = msgBox.addButton(tr("Continue anyway"), QMessageBox::ActionRole);
    msgBox.setDefaultButton(continueButton);

    int ret = msgBox.exec();
    if (ret == QMessageBox::Abort)
    {
      return false;
    }
  }

  loadPluginState(root);
  //-------------------------------------------------
  QDomElement previously_loaded_datafile = root.firstChildElement("previouslyLoaded_Datafiles");

  QDomElement datafile_elem = previously_loaded_datafile.firstChildElement("fileInfo");
  while (!datafile_elem.isNull())
  {
    FileLoadInfo info;
    info.filename = datafile_elem.attribute("filename");
    info.prefix = datafile_elem.attribute("prefix");

    QDomElement datasources_elem = datafile_elem.firstChildElement("selected_datasources");
    QString topics_list = datasources_elem.attribute("value");
    info.selected_datasources = topics_list.split(";", QString::SkipEmptyParts);

    auto plugin_elem = datafile_elem.firstChildElement("plugin");
    info.plugin_config.appendChild(info.plugin_config.importNode(plugin_elem, true));

    loadDataFromFile(info);
    datafile_elem = datafile_elem.nextSiblingElement("fileInfo");
  }

  QDomElement previousl_streamer = root.firstChildElement("previouslyLoaded_Streamer");
  if (!previousl_streamer.isNull())
  {
    QString streamer_name = previousl_streamer.attribute("name");

    QMessageBox msgBox(this);
    msgBox.setWindowTitle("Start Streaming?");
    msgBox.setText(tr("Start the previously used streaming plugin?\n\n %1 \n\n").arg(streamer_name));
    QPushButton* yes = msgBox.addButton(tr("Yes"), QMessageBox::YesRole);
    QPushButton* no = msgBox.addButton(tr("No"), QMessageBox::RejectRole);
    msgBox.setDefaultButton(yes);
    msgBox.exec();

    if (msgBox.clickedButton() == yes)
    {
      if (_data_streamer.count(streamer_name) != 0)
      {
        startStreamingPlugin(streamer_name);
      }
      else
      {
        QMessageBox::warning(this, tr("Error Loading Streamer"),
                             tr("The streamer named %1 can not be loaded.").arg(streamer_name));
      }
    }
  }
  //-------------------------------------------------
  // autostart_publishers
  QDomElement plugins = root.firstChildElement("Plugins");

  if (!plugins.isNull() && _autostart_publishers)
  {
    for (QDomElement plugin_elem = plugins.firstChildElement(); plugin_elem.isNull() == false;
         plugin_elem = plugin_elem.nextSiblingElement())
    {
      const QString plugin_name = plugin_elem.nodeName();
      if (_state_publisher.find(plugin_name) != _state_publisher.end())
      {
        StatePublisherPtr publisher = _state_publisher[plugin_name];

        if (plugin_elem.attribute("status") == "active")
        {
          publisher->setEnabled(true);
        }
      }
    }
  }
  //-------------------------------------------------
  auto custom_equations = root.firstChildElement("customMathEquations");

  try
  {
    if (!custom_equations.isNull())
    {
      for (QDomElement custom_eq = custom_equations.firstChildElement("snippet"); custom_eq.isNull() == false;
           custom_eq = custom_eq.nextSiblingElement("snippet"))
      {
        CustomPlotPtr new_custom_plot = CustomFunction::createFromXML(custom_eq);
        const auto& name = new_custom_plot->name();
        _custom_plots[name] = new_custom_plot;
        new_custom_plot->calculateAndAdd(_mapped_plot_data);
        _curvelist_widget->addCustom(QString::fromStdString(name));
      }
      _curvelist_widget->refreshColumns();
    }
  }
  catch (std::runtime_error& err)
  {
    QMessageBox::warning(this, tr("Exception"), tr("Failed to refresh a customMathEquation \n\n %1\n").arg(err.what()));
  }

  QByteArray snippets_saved_xml = settings.value("AddCustomPlotDialog.savedXML", QByteArray()).toByteArray();

  auto snippets_element = root.firstChildElement("snippets");
  if (!snippets_element.isNull())
  {
    auto snippets_previous = GetSnippetsFromXML(snippets_saved_xml);
    auto snippets_layout = GetSnippetsFromXML(snippets_element);

    bool snippets_are_different = false;
    for (const auto& snippet_it : snippets_layout)
    {
      auto prev_it = snippets_previous.find(snippet_it.first);

      if (prev_it == snippets_previous.end() ||
          prev_it->second.function != snippet_it.second.function ||
          prev_it->second.globalVars != snippet_it.second.globalVars)
      {
        snippets_are_different = true;
        break;
      }
    }

    if (snippets_are_different)
    {
      QMessageBox msgBox(this);
      msgBox.setWindowTitle("Overwrite custom transforms?");
      msgBox.setText("Your layout file contains a set of custom transforms different from "
                     "the last one you used.\nWant to load these transformations?");
      msgBox.addButton(QMessageBox::No);
      msgBox.addButton(QMessageBox::Yes);
      msgBox.setDefaultButton(QMessageBox::Yes);

      if (msgBox.exec() == QMessageBox::Yes)
      {
        for (const auto& snippet_it : snippets_layout)
        {
          snippets_previous[snippet_it.first] = snippet_it.second;
        }
        QDomDocument doc;
        auto snippets_root_element = ExportSnippets(snippets_previous, doc);
        doc.appendChild(snippets_root_element);
        settings.setValue("AddCustomPlotDialog.savedXML", doc.toByteArray(2));
      }
    }
  }

  ///--------------------------------------------------

  xmlLoadState(domDocument);

  forEachWidget([&](PlotWidget* plot) { plot->zoomOut(false); });

  _undo_states.clear();
  _undo_states.push_back(domDocument);
  return true;
}

void MainWindow::on_tabbedAreaDestroyed(QObject* object)
{
  this->setFocus();
}


void MainWindow::forEachWidget(std::function<void(PlotWidget*, PlotDocker*, int)> operation)
{
  auto func = [&](QTabWidget* tabs) {
    for (int t = 0; t < tabs->count(); t++)
    {
      PlotDocker* matrix = dynamic_cast<PlotDocker*>(tabs->widget(t));
      if(!matrix){
        continue;
      }

      for (int index = 0; index < matrix->plotCount(); index++)
      {
        PlotWidget* plot = matrix->plotAt(index);
        operation(plot, matrix, index);
      }
    }
  };

  for (const auto& it : TabbedPlotWidget::instances())
  {
    func(it.second->tabWidget());
  }
}

void MainWindow::forEachWidget(std::function<void(PlotWidget*)> op)
{
  forEachWidget([&](PlotWidget* plot, PlotDocker*, int) { op(plot); });
}

void MainWindow::updateTimeSlider()
{
  auto range = calculateVisibleRangeX();

  ui->timeSlider->setLimits(std::get<0>(range), std::get<1>(range), std::get<2>(range));

  _tracker_time = std::max(_tracker_time, ui->timeSlider->getMinimum());
  _tracker_time = std::min(_tracker_time, ui->timeSlider->getMaximum());
}

void MainWindow::updateTimeOffset()
{
  auto range = calculateVisibleRangeX();
  double min_time = std::get<0>(range);

  const bool remove_offset = ui->pushButtonRemoveTimeOffset->isChecked();
  if (remove_offset && min_time != std::numeric_limits<double>::max())
  {
    _time_offset.set(min_time);
  }
  else
  {
    _time_offset.set(0.0);
  }
}

void MainWindow::updateDataAndReplot(bool replot_hidden_tabs)
{
  bool data_updated_by_streamer = false;

  if (_active_streamer_plugin)
  {
    std::vector<QString> curvelist_added;

    {
      std::lock_guard<std::mutex> lock(_active_streamer_plugin->mutex());
      auto res = MoveData(_active_streamer_plugin->dataMap(), _mapped_plot_data);
      curvelist_added = res.first;
      data_updated_by_streamer = res.second;
    }

    for (const auto& str : curvelist_added)
    {
      _curvelist_widget->addCurve(str);
    }

    if (curvelist_added.size() > 0)
    {
      _curvelist_widget->refreshColumns();
    }

    for (auto& it : _mapped_plot_data.numeric)
    {
      it.second.setMaximumRangeX( ui->streamingSpinBox->value() );
    }
  }

  const bool is_streaming_active = isStreamingActive();

  for (auto& custom_it : _custom_plots)
  {
    auto* dst_plot = &_mapped_plot_data.numeric.at(custom_it.first);
    custom_it.second->calculate(_mapped_plot_data, dst_plot);
  }

  forEachWidget([](PlotWidget* plot) {
    plot->updateCurves();
  });

  if( data_updated_by_streamer )
  {
    _animated_streaming_movie->start();
    _animated_streaming_timer->start(300);
  }

  //--------------------------------
  // trigger again the execution of this callback if steaming == true
  if (is_streaming_active)
  {
    auto range = calculateVisibleRangeX();
    double max_time = std::get<1>(range);
    _tracker_time = max_time;
    onTrackerTimeUpdated(_tracker_time, false);
  }
  else
  {
    updateTimeOffset();
    updateTimeSlider();
  }
  //--------------------------------
  if( data_updated_by_streamer )
  {
    for (const auto& it : TabbedPlotWidget::instances())
    {
      if (replot_hidden_tabs)
      {
        QTabWidget* tabs = it.second->tabWidget();
        for (int index = 0; index < tabs->count(); index++)
        {
          PlotDocker* matrix = static_cast<PlotDocker*>(tabs->widget(index));
          matrix->zoomOut();
        }
      }
      else
      {
        PlotDocker* matrix = it.second->currentTab();
        matrix->zoomOut();  // includes replot
      }
    }
  }
  else{
    forEachWidget([](PlotWidget* plot) {
      plot->replot();
    });
  }
}

void MainWindow::on_streamingSpinBox_valueChanged(int value)
{
  double real_value = value;

  if (isStreamingActive() == false)
  {
    return;
  }

  for (auto& it : _mapped_plot_data.numeric)
  {
    it.second.setMaximumRangeX(real_value);
  }

  for (auto& it : _mapped_plot_data.user_defined)
  {
    it.second.setMaximumRangeX(real_value);
  }

  if (_active_streamer_plugin)
  {
    _active_streamer_plugin->setMaximumRange(real_value);
  }
}



void MainWindow::on_actionExit_triggered()
{
  this->close();
}

void MainWindow::on_pushButtonRemoveTimeOffset_toggled(bool)
{
  updateTimeOffset();
  updatedDisplayTime();

  forEachWidget([](PlotWidget* plot) { plot->replot(); });

  if (this->signalsBlocked() == false){
    onUndoableChange();
  }
}

void MainWindow::updatedDisplayTime()
{
  QLineEdit* timeLine = ui->displayTime;
  const double relative_time = _tracker_time - _time_offset.get();
  if (ui->pushButtonUseDateTime->isChecked())
  {
    if (ui->pushButtonRemoveTimeOffset->isChecked())
    {
      QTime time = QTime::fromMSecsSinceStartOfDay(std::round(relative_time * 1000.0));
      timeLine->setText(time.toString("HH:mm::ss.zzz"));
    }
    else
    {
      QDateTime datetime = QDateTime::fromMSecsSinceEpoch(std::round(_tracker_time * 1000.0));
      timeLine->setText(datetime.toString("[yyyy MMM dd] HH:mm::ss.zzz"));
    }
  }
  else
  {
    timeLine->setText(QString::number(relative_time, 'f', 3));
  }

  QFontMetrics fm(timeLine->font());
  int width = fm.width(timeLine->text()) + 10;
  timeLine->setFixedWidth(std::max(100, width));
}

void MainWindow::on_pushButtonActivateGrid_toggled(bool checked)
{
  forEachWidget([checked](PlotWidget* plot) {
    plot->activateGrid(checked);
    plot->replot();
  });
}

void MainWindow::on_pushButtonRatio_toggled(bool checked)
{
  forEachWidget([checked](PlotWidget* plot) {
    plot->setConstantRatioXY(checked);
    plot->replot();
  });
}

void MainWindow::on_pushButtonPlay_toggled(bool checked)
{
  if (checked)
  {
    _publish_timer->start();
    _prev_publish_time = QDateTime::currentDateTime();
  }
  else
  {
    _publish_timer->stop();
  }
}

void MainWindow::on_actionClearBuffer_triggered()
{
  for (auto& it : _mapped_plot_data.numeric)
  {
    it.second.clear();
  }

  for (auto& it : _mapped_plot_data.user_defined)
  {
    it.second.clear();
  }

  for (auto& it : _custom_plots)
  {
    it.second->clear();
  }

  forEachWidget([](PlotWidget* plot) {
    plot->reloadPlotData();
    plot->replot();
  });
}

void MainWindow::on_pushButtonUseDateTime_toggled(bool checked)
{
  updatedDisplayTime();
}

void MainWindow::on_pushButtonTimeTracker_pressed()
{
  if (_tracker_param == CurveTracker::LINE_ONLY)
  {
    _tracker_param = CurveTracker::VALUE;
  }
  else if (_tracker_param == CurveTracker::VALUE)
  {
    _tracker_param = CurveTracker::VALUE_NAME;
  }
  else if (_tracker_param == CurveTracker::VALUE_NAME)
  {
    _tracker_param = CurveTracker::LINE_ONLY;
  }
  ui->pushButtonTimeTracker->setIcon(_tracker_button_icons[_tracker_param]);

  forEachWidget([&](PlotWidget* plot) {
    plot->configureTracker(_tracker_param);
    plot->replot();
  });
}

void MainWindow::closeEvent(QCloseEvent* event)
{
  _replot_timer->stop();
  _publish_timer->stop();

  if (_active_streamer_plugin)
  {
    _active_streamer_plugin->shutdown();
    _active_streamer_plugin = nullptr;
  }
  QSettings settings;
  settings.setValue("MainWindow.geometry", saveGeometry());
  settings.setValue("MainWindow.state", saveState());

  settings.setValue("MainWindow.activateGrid", ui->pushButtonActivateGrid->isChecked());
  settings.setValue("MainWindow.removeTimeOffset", ui->pushButtonRemoveTimeOffset->isChecked());
  settings.setValue("MainWindow.dateTimeDisplay", ui->pushButtonUseDateTime->isChecked());
  settings.setValue("MainWindow.buttonLink", ui->pushButtonLink->isChecked());
  settings.setValue("MainWindow.buttonRatio", ui->pushButtonRatio->isChecked());

  settings.setValue("MainWindow.streamingBufferValue", ui->streamingSpinBox->value());
  settings.setValue("MainWindow.timeTrackerSetting", (int)_tracker_param);
  settings.setValue("MainWindow.splitterWidth", ui->mainSplitter->sizes()[0]);
}

void MainWindow::onAddCustomPlot(const std::string& plot_name)
{
  ui->widgetStack->setCurrentIndex(1);
  _function_editor->setLinkedPlotName(QString::fromStdString(plot_name));
  _function_editor->createNewPlot();
}

void MainWindow::onEditCustomPlot(const std::string& plot_name)
{
  ui->widgetStack->setCurrentIndex(1);
  auto custom_it = _custom_plots.find(plot_name);
  if (custom_it == _custom_plots.end())
  {
    qWarning("failed to find custom equation");
    return;
  }
  _function_editor->editExistingPlot(custom_it->second);
}

void MainWindow::onRefreshCustomPlot(const std::string& plot_name)
{
  try
  {
    auto custom_it = _custom_plots.find(plot_name);
    if (custom_it == _custom_plots.end())
    {
      qWarning("failed to find custom equation");
      return;
    }
    CustomPlotPtr ce = custom_it->second;

    ce->calculateAndAdd(_mapped_plot_data);

    onUpdateLeftTableValues();
    updateDataAndReplot(true);
  }
  catch (const std::runtime_error& e)
  {
    QMessageBox::critical(this, "error", "Failed to refresh data : " + QString::fromStdString(e.what()));
  }
}


void MainWindow::onPlaybackLoop()
{
  qint64 delta_ms = (QDateTime::currentMSecsSinceEpoch() - _prev_publish_time.toMSecsSinceEpoch());
  _prev_publish_time = QDateTime::currentDateTime();
  delta_ms = std::max((qint64)_publish_timer->interval(), delta_ms);

  _tracker_time += delta_ms * 0.001 * ui->playbackRate->value();
  if (_tracker_time >= ui->timeSlider->getMaximum())
  {
    if (!ui->playbackLoop->isChecked())
    {
      ui->pushButtonPlay->setChecked(false);
    }
    _tracker_time = ui->timeSlider->getMinimum();
  }
  //////////////////
  auto prev = ui->timeSlider->blockSignals(true);
  ui->timeSlider->setRealValue(_tracker_time);
  ui->timeSlider->blockSignals(prev);

  //////////////////
  updatedDisplayTime();
  onUpdateLeftTableValues();

  for (auto& it : _state_publisher)
  {
    it.second->play(_tracker_time);
  }

  forEachWidget([&](PlotWidget* plot) {
    plot->setTrackerPosition(_tracker_time);
    plot->replot();
  });
}

void MainWindow::onCustomPlotCreated(CustomPlotPtr custom_plot)
{
  const std::string& name = custom_plot->name();

  // clear already existing data first
  auto data_it = _mapped_plot_data.numeric.find(name);
  if (data_it != _mapped_plot_data.numeric.end())
  {
    data_it->second.clear();
  }

  try
  {
    custom_plot->calculateAndAdd(_mapped_plot_data);
  }
  catch (std::exception& ex)
  {
    QMessageBox::warning(this, tr("Warning"),
                         tr("Failed to create the custom timeseries. Error:\n\n%1").arg(ex.what()));

    return;
  }
  ui->widgetStack->setCurrentIndex(0);
  _function_editor->clear();

  // keep data for reference
  auto custom_it = _custom_plots.find(name);
  if (custom_it == _custom_plots.end())
  {
    _custom_plots.insert({ name, custom_plot });
    _curvelist_widget->addCustom( QString::fromStdString(name) );
    onUpdateLeftTableValues();
  }
  else{
    custom_it->second = custom_plot;
  }

  _function_editor->clear();

  // update plots
  forEachWidget([&](PlotWidget* plot) {

    const auto& curves = plot->curveList();
    auto it = std::find_if(curves.begin(), curves.end(),
                           [&name](const PlotWidget::CurveInfo& info)
    {
      return info.src_name == name;
    });

    if (it != curves.end())
    {
      plot->updateCurves();
      plot->replot();
    }
  });
}

void MainWindow::on_actionReportBug_triggered()
{
  QDesktopServices::openUrl(QUrl("https://github.com/facontidavide/PlotJuggler/issues"));
}

void MainWindow::on_actionShare_the_love_triggered()
{
  QDesktopServices::openUrl(QUrl("https://twitter.com/intent/tweet?hashtags=PlotJuggler"));
}

void MainWindow::on_actionAbout_triggered()
{
  QDialog* dialog = new QDialog(this);
  auto ui = new Ui::AboutDialog();
  ui->setupUi(dialog);

  ui->label_version->setText(QApplication::applicationVersion());
  dialog->setAttribute(Qt::WA_DeleteOnClose);

  dialog->exec();
}

void MainWindow::on_actionCheatsheet_triggered()
{
  QSettings settings;

  CheatsheetDialog *dialog = new CheatsheetDialog(this);
  dialog->restoreGeometry(settings.value("Cheatsheet.geometry").toByteArray());
  dialog->exec();
  settings.setValue("Cheatsheet.geometry", dialog->saveGeometry());
  dialog->deleteLater();
}

void MainWindow::on_actionSupportPlotJuggler_triggered()
{
  QDialog* dialog = new QDialog(this);
  auto ui = new Ui::SupportDialog();
  ui->setupUi(dialog);

  dialog->setAttribute(Qt::WA_DeleteOnClose);

  dialog->exec();
}

/*
void MainWindow::on_actionSaveAllPlotTabs_triggered()
{
  QSettings settings;
  QString directory_path = settings.value("MainWindow.saveAllPlotTabs", QDir::currentPath()).toString();
  // Get destination folder
  QFileDialog saveDialog(this);
  saveDialog.setDirectory(directory_path);
  saveDialog.setFileMode(QFileDialog::FileMode::Directory);
  saveDialog.setAcceptMode(QFileDialog::AcceptSave);
  saveDialog.exec();

  uint image_number = 1;
  if (saveDialog.result() == QDialog::Accepted && !saveDialog.selectedFiles().empty())
  {
    // Save Plots
    QString directory = saveDialog.selectedFiles().first();
    settings.setValue("MainWindow.saveAllPlotTabs", directory);

    QStringList file_names;
    QStringList existing_files;
    QDateTime current_date_time(QDateTime::currentDateTime());
    QString current_date_time_name(current_date_time.toString("yyyy-MM-dd_HH-mm-ss"));
    for (const auto& it : TabbedPlotWidget::instances())
    {
      auto tab_widget = it.second->tabWidget();
      for (int i = 0; i < tab_widget->count(); i++)
      {
        PlotDocker* matrix = static_cast<PlotDocker*>(tab_widget->widget(i));
        QString name = QString("%1/%2_%3_%4.png")
                           .arg(directory)
                           .arg(current_date_time_name)
                           .arg(image_number, 2, 10, QLatin1Char('0'))
                           .arg(matrix->name());
        file_names.push_back(name);
        image_number++;

        QFileInfo check_file(file_names.back());
        if (check_file.exists() && check_file.isFile())
        {
          existing_files.push_back(name);
        }
      }
    }
    if (existing_files.isEmpty() == false)
    {
      QMessageBox msgBox;
      msgBox.setText("One or more files will be overwritten. ant to continue?");
      QString all_files;
      for (const auto& str : existing_files)
      {
        all_files.push_back("\n");
        all_files.append(str);
      }
      msgBox.setInformativeText(all_files);
      msgBox.setStandardButtons(QMessageBox::Cancel | QMessageBox::Ok);
      msgBox.setDefaultButton(QMessageBox::Ok);

      if (msgBox.exec() != QMessageBox::Ok)
      {
        return;
      }
    }

    image_number = 0;
    for (const auto& it : TabbedPlotWidget::instances())
    {
      auto tab_widget = it.second->tabWidget();
      for (int i = 0; i < tab_widget->count(); i++)
      {
        PlotDocker* matrix = static_cast<PlotDocker*>(tab_widget->widget(i));
        TabbedPlotWidget::saveTabImage(file_names[image_number], matrix);
        image_number++;
      }
    }
  }
}*/

void MainWindow::on_pushButtonLoadDatafile_clicked()
{
  if (_data_loader.empty())
  {
    QMessageBox::warning(this, tr("Warning"), tr("No plugin was loaded to process a data file\n"));
    return;
  }

  QSettings settings;

  QString file_extension_filter;

  std::set<QString> extensions;

  for (auto& it : _data_loader)
  {
    DataLoaderPtr loader = it.second;
    for (QString extension : loader->compatibleFileExtensions())
    {
      extensions.insert(extension.toLower());
    }
  }

  for (const auto& it : extensions)
  {
    file_extension_filter.append(QString(" *.") + it);
  }

  QString directory_path = settings.value("MainWindow.lastDatafileDirectory", QDir::currentPath()).toString();

  QFileDialog loadDialog(this);
  loadDialog.setFileMode(QFileDialog::ExistingFiles);
  loadDialog.setViewMode(QFileDialog::Detail);
  loadDialog.setNameFilter(file_extension_filter);
  loadDialog.setDirectory(directory_path);

  QStringList fileNames;
  if (loadDialog.exec())
  {
    fileNames = loadDialog.selectedFiles();
  }

  if (fileNames.isEmpty())
  {
    return;
  }

  directory_path = QFileInfo(fileNames[0]).absolutePath();
  settings.setValue("MainWindow.lastDatafileDirectory", directory_path);

  if (loadDataFromFiles(fileNames))
  {
    updateRecentDataMenu(fileNames);
  }
}

void MainWindow::on_pushButtonLoadLayout_clicked()
{
  QSettings settings;

  QString directory_path = settings.value("MainWindow.lastLayoutDirectory", QDir::currentPath()).toString();
  QString filename = QFileDialog::getOpenFileName(this, "Open Layout", directory_path, "*.xml");
  if (filename.isEmpty())
  {
    return;
  }

  if (loadLayoutFromFile(filename))
  {
    updateRecentLayoutMenu({ filename });
  }

  directory_path = QFileInfo(filename).absolutePath();
  settings.setValue("MainWindow.lastLayoutDirectory", directory_path);
}

void MainWindow::on_pushButtonSaveLayout_clicked()
{
  QDomDocument doc = xmlSaveState();

  QSettings settings;

  QString directory_path = settings.value("MainWindow.lastLayoutDirectory", QDir::currentPath()).toString();

  QFileDialog saveDialog(this);
  saveDialog.setOption(QFileDialog::DontUseNativeDialog, true);

  QGridLayout* save_layout = static_cast<QGridLayout*>(saveDialog.layout());

  QFrame* frame = new QFrame;
  frame->setFrameStyle(QFrame::Box | QFrame::Plain);
  frame->setLineWidth(1);

  QVBoxLayout* vbox = new QVBoxLayout;
  QLabel* title = new QLabel("Save Layout options");
  QFrame* separator = new QFrame;
  separator->setFrameStyle(QFrame::HLine | QFrame::Plain);

  auto checkbox_datasource = new QCheckBox("Save data source");
  checkbox_datasource->setToolTip("ant the layout to remember the source of your data,\n"
                                  "i.e. the Datafile used or the Streaming Plugin loaded ?");
  checkbox_datasource->setFocusPolicy(Qt::NoFocus);
  checkbox_datasource->setChecked(settings.value("MainWindow.saveLayoutDataSource", true).toBool());

  auto checkbox_snippets = new QCheckBox("Save custom transformations");
  checkbox_snippets->setToolTip("Do you want the layout to save the custom transformations?");
  checkbox_snippets->setFocusPolicy(Qt::NoFocus);
  checkbox_snippets->setChecked(settings.value("MainWindow.saveLayoutSnippets", true).toBool());

  vbox->addWidget(title);
  vbox->addWidget(separator);
  vbox->addWidget(checkbox_datasource);
  vbox->addWidget(checkbox_snippets);
  frame->setLayout(vbox);

  int rows = save_layout->rowCount();
  int col = save_layout->columnCount();
  save_layout->addWidget(frame, 0, col, rows, 1, Qt::AlignTop);

  saveDialog.setAcceptMode(QFileDialog::AcceptSave);
  saveDialog.setDefaultSuffix("xml");
  saveDialog.setNameFilter("XML (*.xml)");
  saveDialog.setDirectory(directory_path);
  saveDialog.exec();

  if (saveDialog.result() != QDialog::Accepted || saveDialog.selectedFiles().empty())
  {
    return;
  }

  QString fileName = saveDialog.selectedFiles().first();

  if (fileName.isEmpty())
  {
    return;
  }

  directory_path = QFileInfo(fileName).absolutePath();
  settings.setValue("MainWindow.lastLayoutDirectory", directory_path);
  settings.setValue("MainWindow.saveLayoutDataSource", checkbox_datasource->isChecked());
  settings.setValue("MainWindow.saveLayoutSnippets", checkbox_snippets->isChecked());

  QDomElement root = doc.namedItem("root").toElement();
  root.setAttribute("version", LAYOUT_VERSION);

  root.appendChild(doc.createComment(" - - - - - - - - - - - - - - "));

  root.appendChild(doc.createComment(" - - - - - - - - - - - - - - "));

  root.appendChild(savePluginState(doc));

  root.appendChild(doc.createComment(" - - - - - - - - - - - - - - "));

  if (checkbox_datasource->isChecked())
  {
    QDomElement loaded_list = doc.createElement("previouslyLoaded_Datafiles");

    for (const auto& loaded : _loaded_datafiles)
    {
      QDomElement file_elem = doc.createElement("fileInfo");
      file_elem.setAttribute("filename", loaded.filename);
      file_elem.setAttribute("prefix", loaded.prefix);

      QDomElement datasources_elem = doc.createElement("selected_datasources");
      QString topics_list = loaded.selected_datasources.join(";");
      datasources_elem.setAttribute("value", topics_list);
      file_elem.appendChild(datasources_elem);

      file_elem.appendChild(loaded.plugin_config.firstChild());
      loaded_list.appendChild(file_elem);
    }
    root.appendChild(loaded_list);

    if (_active_streamer_plugin)
    {
      QDomElement loaded_streamer = doc.createElement("previouslyLoaded_Streamer");
      QString streamer_name = _active_streamer_plugin->name();
      loaded_streamer.setAttribute("name", streamer_name);
      root.appendChild(loaded_streamer);
    }
  }
  //-----------------------------------
  root.appendChild(doc.createComment(" - - - - - - - - - - - - - - "));
  if (checkbox_snippets->isChecked())
  {
    QDomElement custom_equations = doc.createElement("customMathEquations");
    for (const auto& custom_it : _custom_plots)
    {
      const auto& custom_plot = custom_it.second;
      custom_equations.appendChild(custom_plot->xmlSaveState(doc));
    }
    root.appendChild(custom_equations);

    QByteArray snippets_xml_text = settings.value("AddCustomPlotDialog.savedXML", QByteArray()).toByteArray();
    auto snipped_saved = GetSnippetsFromXML(snippets_xml_text);
    auto snippets_root = ExportSnippets(snipped_saved, doc);
    root.appendChild(snippets_root);
  }
  root.appendChild(doc.createComment(" - - - - - - - - - - - - - - "));
  //------------------------------------
  QFile file(fileName);
  if (file.open(QIODevice::WriteOnly))
  {
    QTextStream stream(&file);
    stream << doc.toString() << endl;
  }
}

void MainWindow::onActionFullscreenTriggered()
{
  static bool first_call = true;
  if (first_call && !_minimized)
  {
    first_call = false;
    QMessageBox::information(this, "Remember!", "Press F10 to switch back to the normal view");
  }

  _minimized = !_minimized;

  ui->leftMainWindowFrame->setVisible(!_minimized);
  //  ui->widgetOptions->setVisible(!_minimized && ui->pushButtonOptions->isChecked());
  ui->widgetTimescale->setVisible(!_minimized);
  ui->menuBar->setVisible(!_minimized);

  for (auto& it : TabbedPlotWidget::instances())
  {
    it.second->setControlsVisible(!_minimized);
  }
}

void MainWindow::on_actionClearRecentData_triggered()
{
  QMenu* menu = _recent_data_files;
  for (QAction* action : menu->actions())
  {
    if (action->isSeparator())
    {
      break;
    }
    menu->removeAction(action);
  }
  menu->setEnabled(false);
  QSettings settings;
  settings.setValue("MainWindow.recentlyLoadedDatafile", {});
}

void MainWindow::on_actionClearRecentLayout_triggered()
{
  QMenu* menu = _recent_layout_files;
  for (QAction* action : menu->actions())
  {
    if (action->isSeparator())
    {
      break;
    }
    menu->removeAction(action);
  }
  menu->setEnabled(false);
  QSettings settings;
  settings.setValue("MainWindow.recentlyLoadedLayout", {});
}

void MainWindow::on_actionDeleteAllData_triggered()
{
  QMessageBox msgBox(this);
  msgBox.setWindowTitle("Warning. Can't be undone.");
  msgBox.setText(tr("Do you want to remove the previously loaded data?\n"));
  msgBox.addButton(QMessageBox::No);
  msgBox.addButton(QMessageBox::Yes);
  msgBox.setDefaultButton(QMessageBox::Yes);
  //  QPushButton* buttonPlaceholder = msgBox.addButton(tr("Keep empty placeholders"), QMessageBox::NoRole);
  auto reply = msgBox.exec();

  if (reply == QMessageBox::No)
  {
    return;
  }

  //    if( msgBox.clickedButton() == buttonPlaceholder )
  //    {
  //        for( auto& it: _mapped_plot_data.numeric )
  //        {
  //            it.second.clear();
  //        }
  //        for( auto& it: _mapped_plot_data.user_defined )
  //        {
  //            it.second.clear();
  //        }

  //        for(const auto& it: TabbedPlotWidget::instances())
  //        {
  //            PlotDocker* matrix =  it.second->currentTab() ;
  //            matrix->maximumZoomOut(); // includes replot
  //        }
  //    }
  //    else
  {
    deleteAllData();
  }
}

void MainWindow::on_actionPreferences_triggered()
{
  QSettings settings;
  QString prev_style = settings.value("Preferences::theme", "light").toString();

  PreferencesDialog dialog;
  dialog.exec();

  QString theme = settings.value("Preferences::theme").toString();

  if (theme != prev_style)
  {
    loadStyleSheet(tr(":/resources/stylesheet_%1.qss").arg(theme));
  }
}

void MainWindow::on_playbackStep_valueChanged(double step)
{
  ui->timeSlider->setFocus();
  ui->timeSlider->setRealStepValue(step);
}

void MainWindow::on_actionLoadStyleSheet_triggered()
{
  QSettings settings;
  QString directory_path = settings.value("MainWindow.loadStyleSheetDirectory", QDir::currentPath()).toString();

  QString fileName = QFileDialog::getOpenFileName(this, tr("Load StyleSheet"), directory_path, tr("(*.qss)"));
  if (fileName.isEmpty())
  {
    return;
  }

  loadStyleSheet(fileName);

  directory_path = QFileInfo(fileName).absolutePath();
  settings.setValue("MainWindow.loadStyleSheetDirectory", directory_path);
}

void MainWindow::on_pushButtonLegend_clicked()
{
  switch (_labels_status)
  {
  case LabelStatus::LEFT:
    _labels_status = LabelStatus::HIDDEN;
    break;
  case LabelStatus::RIGHT:
    _labels_status = LabelStatus::LEFT;
    break;
  case LabelStatus::HIDDEN:
    _labels_status = LabelStatus::RIGHT;
    break;
  }

  auto visitor = [=](PlotWidget* plot)
  {
    plot->activateLegend(_labels_status != LabelStatus::HIDDEN);

    if (_labels_status == LabelStatus::LEFT)
    {
      plot->setLegendAlignment(Qt::AlignLeft);
    }
    else if (_labels_status == LabelStatus::RIGHT)
    {
      plot->setLegendAlignment(Qt::AlignRight);
    }
    plot->replot();
  };

  this->forEachWidget(visitor);
}

void MainWindow::on_pushButtonZoomOut_clicked()
{
  auto visitor = [=](PlotWidget* plot)
  {
    plot->zoomOut(false);
  };
  this->forEachWidget(visitor);
  onUndoableChange();
}

void MainWindow::on_comboStreaming_currentIndexChanged(const QString &current_text)
{
  QSettings settings;
  settings.setValue("MainWindow.previousStreamingPlugin", current_text );
  auto streamer = _data_streamer.at( current_text );
  ui->buttonStreamingOptions->setEnabled( !streamer->availableActions().empty() );
}

void MainWindow::on_buttonStreamingStart_clicked()
{
  ui->buttonStreamingStart->setEnabled(false);
  if( ui->buttonStreamingStart->text() == "Start")
  {
    startStreamingPlugin( ui->comboStreaming->currentText() );
  }
  else {
    stopStreamingPlugin();
  }
  ui->buttonStreamingStart->setEnabled(true);
}


PopupMenu::PopupMenu(QWidget *relative_widget, QWidget *parent):
  QMenu(parent), _w(relative_widget)
{
}

void PopupMenu::showEvent(QShowEvent *)
{
  QPoint p = _w->mapToGlobal( {} );
  QRect geo = _w->geometry();
  this->move( p.x() + geo.width(), p.y() );
}

void PopupMenu::leaveEvent(QEvent *)
{
  close();
}

void PopupMenu::closeEvent(QCloseEvent *)
{
  _w->setAttribute(Qt::WA_UnderMouse, false);
}

void MainWindow::on_buttonRecentData_clicked()
{
  PopupMenu* menu = new PopupMenu(ui->buttonRecentData, this);

  for(auto action: _recent_data_files->actions()) {
    menu->addAction(action);
  }
  menu->exec();
}

void MainWindow::on_buttonStreamingOptions_clicked()
{
  auto streamer = _data_streamer.at( ui->comboStreaming->currentText() );

  PopupMenu* menu = new PopupMenu(ui->buttonStreamingOptions, this);
  for( auto action: streamer->availableActions() ) {
    menu->addAction(action);
  }
  menu->show();
}


void MainWindow::on_buttonHideFileFrame_clicked()
{
  bool hidden = !ui->frameFile->isHidden();
  ui->buttonHideFileFrame->setText( hidden ? "+" : " -");
  ui->frameFile->setHidden( hidden );

  QSettings settings;
  settings.setValue("MainWindow.hiddenFileFrame", hidden);
}

void MainWindow::on_buttonHideStreamingFrame_clicked()
{
  bool hidden = !ui->frameStreaming->isHidden();
  ui->buttonHideStreamingFrame->setText( hidden ? "+" : " -");
  ui->frameStreaming->setHidden( hidden );

  QSettings settings;
  settings.setValue("MainWindow.hiddenStreamingFrame", hidden);
}

void MainWindow::on_buttonHidePublishersFrame_clicked()
{
  bool hidden = !ui->framePublishers->isHidden();
  ui->buttonHidePublishersFrame->setText( hidden ? "+" : " -");
  ui->framePublishers->setHidden( hidden );

  QSettings settings;
  settings.setValue("MainWindow.hiddenPublishersFrame", hidden);
}

void MainWindow::on_buttonRecentLayout_clicked()
{
  PopupMenu* menu = new PopupMenu(ui->buttonRecentLayout, this);

  for(auto action: _recent_layout_files->actions()) {
    menu->addAction(action);
  }
  menu->exec();
}

