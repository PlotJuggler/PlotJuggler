/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "mainwindow.h"
#include <iostream>
#include <QApplication>
#include <QSplashScreen>
#include <QThread>
#include <QCommandLineParser>
#include <QDesktopWidget>
#include <QFontDatabase>
#include <QSettings>
#include <QPushButton>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QDir>
#include <QDialog>
#include <QDesktopServices>
#include <QHostInfo>
#include <QSslConfiguration>
#include <QSslSocket>
#include <QStyleFactory>
#include <QMessageBox>
#include <QTimer>
#include <QCheckBox>

#include "PlotJuggler/transform_function.h"
#include "transforms/binary_filter.h"
#include "transforms/first_derivative.h"
#include "transforms/samples_count.h"
#include "transforms/scale_transform.h"
#include "transforms/moving_average_filter.h"
#include "transforms/moving_variance.h"
#include "transforms/moving_rms.h"
#ifdef PJ_HAS_PYTHON
#include "transforms/python_custom_function.h"
#endif
#include "transforms/outlier_removal.h"
#include "transforms/integral_transform.h"
#include "transforms/absolute_transform.h"
#include "transforms/time_since_previous_point.h"

#ifdef COMPILED_WITH_CATKIN
#include <ros/ros.h>
#endif
#ifdef COMPILED_WITH_AMENT
#include <string_view>

// Strip ROS 2 CLI arguments ("--ros-args ... [--]") from argv.
// Equivalent to rclcpp::remove_ros_arguments, but without pulling the rclcpp /
// rosidl typesupport stack into PlotJuggler just for one call.
static std::vector<std::string> RemoveRos2Arguments(int argc, char* argv[])
{
  std::vector<std::string> out;
  out.reserve(argc);
  bool in_ros_block = false;
  for (int i = 0; i < argc; ++i)
  {
    const std::string_view tok(argv[i]);
    if (!in_ros_block)
    {
      if (tok == "--ros-args")
      {
        in_ros_block = true;
        continue;
      }
      out.emplace_back(argv[i]);
    }
    else if (tok == "--")
    {
      in_ros_block = false;
    }
  }
  return out;
}
#endif

static QString VERSION_STRING =
    QString("%1.%2.%3").arg(PJ_MAJOR_VERSION).arg(PJ_MINOR_VERSION).arg(PJ_PATCH_VERSION);

inline int GetVersionNumber(QString str)
{
  QStringList online_version = str.split('.');
  if (online_version.size() != 3)
  {
    return 0;
  }
  int major = online_version[0].toInt();
  int minor = online_version[1].toInt();
  int patch = online_version[2].toInt();
  return major * 10000 + minor * 100 + patch;
}

QPixmap getFunnySplashscreen()
{
  QSettings settings;
  srand(time(nullptr));

  auto getNum = []() {
    const int last_image_num = 106;
    return rand() % (last_image_num);
  };

  std::set<int> previous_set;
  std::list<int> previous_nums;

  QStringList previous_list = settings.value("previousFunnyMemesList").toStringList();
  for (auto str : previous_list)
  {
    int num = str.toInt();
    previous_set.insert(num);
    previous_nums.push_back(num);
  }

  int n = getNum();
  while (previous_set.count(n) != 0)
  {
    n = getNum();
  }

  while (previous_nums.size() >= 10)
  {
    previous_nums.pop_front();
  }
  previous_nums.push_back(n);

  QStringList new_list;
  for (int num : previous_nums)
  {
    new_list.push_back(QString::number(num));
  }

  settings.setValue("previousFunnyMemesList", new_list);
  auto filename = QString("://resources/memes/meme_%1.jpg").arg(n, 2, 10, QChar('0'));
  return QPixmap(filename);
}

std::vector<std::string> MergeArguments(const std::vector<std::string>& args)
{
#ifdef PJ_DEFAULT_ARGS
  auto default_cmdline_args = QString(PJ_DEFAULT_ARGS).split(" ", PJ::SkipEmptyParts);

  std::vector<std::string> new_args;
  new_args.push_back(args.front());

  // Add the remain arguments, replacing escaped characters if necessary.
  // Escaping needed because some chars cannot be entered easily in the -DPJ_DEFAULT_ARGS
  // preprocessor directive
  //   _0x20_   -->   ' '   (space)
  //   _0x3b_   -->   ';'   (semicolon)
  for (auto cmdline_arg : default_cmdline_args)
  {
    // replace(const QString &before, const QString &after, Qt::CaseSensitivity cs =
    // Qt::CaseSensitive)
    cmdline_arg = cmdline_arg.replace("_0x20_", " ", Qt::CaseSensitive);
    cmdline_arg = cmdline_arg.replace("_0x3b_", ";", Qt::CaseSensitive);
    new_args.push_back(strdup(cmdline_arg.toLocal8Bit().data()));
  }

  // If an argument appears repeated, the second value overrides previous one.
  // Do this after adding default_cmdline_args so the command-line override default
  for (size_t i = 1; i < args.size(); ++i)
  {
    new_args.push_back(args[i]);
  }

  return new_args;

#else
  return args;
#endif
}

int main(int argc, char* argv[])
{
  std::vector<std::string> args;

#if !defined(COMPILED_WITH_CATKIN) && !defined(COMPILED_WITH_AMENT)
  for (int i = 0; i < argc; i++)
  {
    args.push_back(argv[i]);
  }
#elif defined(COMPILED_WITH_CATKIN)
  ros::removeROSArgs(argc, argv, args);
#elif defined(COMPILED_WITH_AMENT)
  args = RemoveRos2Arguments(argc, argv);
#endif

  args = MergeArguments(args);

  int new_argc = args.size();
  std::vector<char*> new_argv;
  for (int i = 0; i < new_argc; i++)
  {
    new_argv.push_back(args[i].data());
  }

  // Must be set before QApplication is constructed. Tells Qt to scale
  // widget metrics and QSS pixel values by the screen's scale factor,
  // so XWayland (which reports DPI=N*96 with dpr=1) renders identically
  // to native Wayland (DPI=96 dpr=N). Without this, Fusion metrics
  // auto-scale by DPI but QSS hardcoded `px` values don't, causing
  // inconsistent widget sizing in the AppImage.
  QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
  QCoreApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);

  QApplication app(new_argc, new_argv.data());

  // Pin the Qt base style so the app's QSS paints on top of a known
  // palette. Without this, the AppImage inherits the host's Qt platform
  // theme (e.g. GTK3 dark on Ubuntu), causing unstyled widgets like the
  // menu bar to render with system-dark colors while QSS-covered widgets
  // stay light — the mixed-theme look users report.
  QApplication::setStyle(QStyleFactory::create("Fusion"));

  //-------------------------

  QCoreApplication::setOrganizationName("PlotJuggler");
  QCoreApplication::setApplicationName("io.plotjuggler.PlotJuggler");
  QSettings::setDefaultFormat(QSettings::IniFormat);

  QSettings settings;

  if (!settings.isWritable())
  {
    qDebug() << "ERROR: the file [" << settings.fileName()
             << "] is not writable. This may happen when you run PlotJuggler with sudo. "
                "Change the permissions of the file (\"sudo chmod 666 <file_name>\"on "
                "linux)";
  }

  app.setApplicationVersion(VERSION_STRING);

  //---------------------------
  TransformFactory::registerTransform<FirstDerivative>();
  TransformFactory::registerTransform<ScaleTransform>();
  TransformFactory::registerTransform<MovingAverageFilter>();
  TransformFactory::registerTransform<MovingRMS>();
  TransformFactory::registerTransform<OutlierRemovalFilter>();
  TransformFactory::registerTransform<IntegralTransform>();
  TransformFactory::registerTransform<AbsoluteTransform>();
  TransformFactory::registerTransform<TimeSincePreviousPointTranform>();
  TransformFactory::registerTransform<MovingVarianceFilter>();
  TransformFactory::registerTransform<SamplesCountFilter>();
  TransformFactory::registerTransform<BinaryFilter>();
  //---------------------------

  QCommandLineParser parser;
  parser.setApplicationDescription("PlotJuggler: the time series visualization"
                                   " tool that you deserve ");
  parser.addVersionOption();
  parser.addHelpOption();

  QCommandLineOption nosplash_option(QStringList() << "n"
                                                   << "nosplash",
                                     "Don't display the splashscreen");
  parser.addOption(nosplash_option);

  QCommandLineOption test_option(QStringList() << "t"
                                               << "test",
                                 "Generate test curves at startup");
  parser.addOption(test_option);

  QCommandLineOption loadfile_option(QStringList() << "d"
                                                   << "datafile",
                                     "Load a file containing data", "file_path");
  parser.addOption(loadfile_option);

  QCommandLineOption layout_option(QStringList() << "l"
                                                 << "layout",
                                   "Load a file containing the layout configuration", "file_path");
  parser.addOption(layout_option);

  QCommandLineOption publish_option(QStringList() << "p"
                                                  << "publish",
                                    "Automatically start publisher when loading the "
                                    "layout file");
  parser.addOption(publish_option);

  QCommandLineOption folder_option(QStringList() << "plugin_folders",
                                   "Add semicolon-separated list of folders where you "
                                   "should look "
                                   "for additional plugins.",
                                   "directory_paths");
  parser.addOption(folder_option);

  QCommandLineOption buffersize_option(
      QStringList() << "buffer_size",
      QCoreApplication::translate(
          "main", "Change the maximum size of the streaming buffer (minimum: 10 default: 60)"),
      QCoreApplication::translate("main", "seconds"));
  parser.addOption(buffersize_option);

  QCommandLineOption nogl_option(
      QStringList() << "disable_opengl",
      "Disable OpenGL display before starting the application. You can enable it again in the 'Preferences' menu.");
  parser.addOption(nogl_option);

  QCommandLineOption enabled_plugins_option(
      QStringList() << "enabled_plugins",
      "Limit the loaded plugins to ones in the semicolon-separated list", "name_list");
  parser.addOption(enabled_plugins_option);

  QCommandLineOption disabled_plugins_option(
      QStringList() << "disabled_plugins",
      "Do not load any of the plugins in the semicolon separated list", "name_list");
  parser.addOption(disabled_plugins_option);

  QCommandLineOption skin_path_option(
      QStringList() << "skin_path",
      "New \"skin\". Refer to the sample in [plotjuggler_app/resources/skin] path to folder");
  parser.addOption(skin_path_option);

  QCommandLineOption start_streamer(
      QStringList() << "start_streamer",
      "Automatically start a Streaming Plugin with the given file_name (no extension)");
  parser.addOption(start_streamer);

  QCommandLineOption window_title(QStringList() << "window_title", "Set the window title",
                                  "window_title");
  parser.addOption(window_title);

  QCommandLineOption auto_prefix_option("auto-prefix",
                                        "Automatically prefix each data file with its filename");
  parser.addOption(auto_prefix_option);

  parser.process(*qApp);

  if (parser.isSet(publish_option) && !parser.isSet(layout_option))
  {
    std::cerr << "Option [ -p / --publish ] is invalid unless [ -l / --layout ] is used too."
              << std::endl;
    return -1;
  }

  if (parser.isSet(enabled_plugins_option) && parser.isSet(disabled_plugins_option))
  {
    std::cerr << "Option [ --enabled_plugins ] and [ --disabled_plugins ] can't be used together."
              << std::endl;
    return -1;
  }

  if (parser.isSet(nogl_option))
  {
    settings.setValue("Preferences::use_opengl", false);
  }

  if (parser.isSet(skin_path_option))
  {
    QDir path(parser.value(skin_path_option));
    if (!path.exists())
    {
      qDebug() << "Skin path [" << parser.value(skin_path_option) << "] not found";
      return -1;
    }
  }

  QIcon app_icon("://resources/plotjuggler.svg");
  QApplication::setWindowIcon(app_icon);

  MainWindow* window = nullptr;

  /*
   * You, fearless code reviewer, decided to start a journey into my source code.
   * For your bravery, you deserve to know the truth.
   * The splashscreen is useless; not only it is useless, it will make your start-up
   * time slower by few seconds for absolutely no reason.
   * But what are two seconds compared with the time that PlotJuggler will save you?
   * The splashscreen is the connection between me and my users, the glue that keeps
   * together our invisible relationship.
   * Now, it is up to you to decide: you can block the splashscreen forever or not,
   * reject a message that brings a little of happiness into your day, spent analyzing
   * data. Please don't do it.
   */
#ifdef PJ_HAS_PYTHON
  // Probe the embedded Python interpreter BEFORE constructing MainWindow, so
  // FunctionEditorWidget (built inside the MainWindow ctor) sees the correct
  // PythonCustomFunction::isAvailable() state when it decides whether to
  // enable / disable the Python radio buttons.
  const bool python_ok = PythonCustomFunction::probeAvailable();
  if (!python_ok)
  {
    qWarning() << "Embedded Python could not be initialized — Python custom "
                  "functions will be disabled for this session.";
  }
#endif

  if (!parser.isSet(nosplash_option) &&
      !(parser.isSet(loadfile_option) || parser.isSet(layout_option)) &&
      !(settings.value("Preferences::no_splash", false).toBool()))
  // if(false) // if you uncomment this line, a kitten will die somewhere in the world.
  {
    QPixmap main_pixmap;

    if (parser.isSet(skin_path_option))
    {
      QDir path(parser.value(skin_path_option));
      QFile splash = path.filePath("pj_splashscreen.png");
      if (splash.exists())
      {
        main_pixmap = QPixmap(splash.fileName());
      }
    }

    if (main_pixmap.isNull())
    {
      main_pixmap = getFunnySplashscreen();
    }

    QSplashScreen splash(main_pixmap, Qt::WindowStaysOnTopHint);
    QDesktopWidget* desktop = QApplication::desktop();
    const int scrn = desktop->screenNumber();
    const QPoint currentDesktopsCenter = desktop->availableGeometry(scrn).center();
    splash.move(currentDesktopsCenter - splash.rect().center());

    splash.show();
    app.processEvents();

    auto deadline = QDateTime::currentDateTime().addMSecs(500);
    while (QDateTime::currentDateTime() < deadline)
    {
      app.processEvents();
    }

    window = new MainWindow(parser);

    deadline = QDateTime::currentDateTime().addMSecs(3000);
    while (QDateTime::currentDateTime() < deadline && !splash.isHidden())
    {
      app.processEvents();
    }
  }

  if (!window)
  {
    window = new MainWindow(parser);
  }

  window->show();

#ifdef PJ_HAS_PYTHON
  if (!python_ok)
  {
    // Show a one-time, dismissible warning after the main window is up so the
    // user immediately knows Python custom functions won't work on this host.
    const QString suppress_key = "PythonUnavailable.suppressWarning";
    if (!QSettings().value(suppress_key, false).toBool())
    {
      QTimer::singleShot(0, window, [window, suppress_key]() {
        QMessageBox box(window);
        box.setIcon(QMessageBox::Warning);
        box.setWindowTitle(QObject::tr("Python disabled"));
        box.setText(QObject::tr("PlotJuggler could not initialize the embedded "
                                "Python interpreter."));
        box.setInformativeText(
            QObject::tr("Python custom functions are disabled for this session. Lua custom "
                        "functions remain available.\n\n"
                        "This usually means the Python standard library expected by this "
                        "build is not present on the host system (common when running an "
                        "AppImage on a distro with a different Python version)."));
        QCheckBox* dont_show = new QCheckBox(QObject::tr("Don't show this again"), &box);
        box.setCheckBox(dont_show);
        box.setStandardButtons(QMessageBox::Ok);
        box.exec();
        if (dont_show->isChecked())
        {
          QSettings().setValue(suppress_key, true);
        }
      });
    }
  }
#endif

  if (parser.isSet(start_streamer))
  {
    window->on_buttonStreamingStart_clicked();
  }

  // Check for new releases on GitHub
  QNetworkAccessManager* manager_new_release = new QNetworkAccessManager(&app);
  QObject::connect(
      manager_new_release, &QNetworkAccessManager::finished, [window](QNetworkReply* reply) {
        if (reply->error())
        {
          qDebug() << "GitHub release check error:" << reply->error() << reply->errorString();
          return;
        }

        QString answer = reply->readAll();
        QJsonDocument document = QJsonDocument::fromJson(answer.toUtf8());
        QJsonObject data = document.object();
        QString url = data["html_url"].toString();
        QString name = data["name"].toString();
        QString tag_name = data["tag_name"].toString();

        int online_number = GetVersionNumber(tag_name);
        int current_number = GetVersionNumber(VERSION_STRING);

        qDebug() << "Current version:" << VERSION_STRING << ". Latest release version:" << tag_name;

        if (online_number > current_number)
        {
          QString message = QString("New release available: <b>%1</b><br>"
                                    "<a href=\"%2\">View on GitHub</a>")
                                .arg(name, url);
          QPixmap icon(":/resources/success_kid.png");
          window->showToast(message, icon);
        }
      });

  QNetworkRequest request_new_release;
  request_new_release.setUrl(
      QUrl("https://api.github.com/repos/PlotJuggler/PlotJuggler/releases/latest"));

  // Disable SSL peer verification for GitHub API (workaround for Qt5/OpenSSL 3.0 incompatibility)
  QSslConfiguration sslConfig_release = request_new_release.sslConfiguration();
  sslConfig_release.setPeerVerifyMode(QSslSocket::VerifyNone);
  request_new_release.setSslConfiguration(sslConfig_release);

  manager_new_release->get(request_new_release);

  QNetworkAccessManager manager_message;
  QObject::connect(
      &manager_message, &QNetworkAccessManager::finished, [window](QNetworkReply* reply) {
        if (reply->error())
        {
          qDebug() << "Telemetry reply error:" << reply->error() << reply->errorString();
          return;
        }
        qDebug() << "Telemetry reply received";
        QString answer = reply->readAll();
        QJsonDocument document = QJsonDocument::fromJson(answer.toUtf8());
        QJsonObject data = document.object();
        QString message = data["message"].toString();
        window->setStatusBarMessage(message);
      });

  // These are 100% anonymous requests; no personal data is sent.
  // We collect your statistics to improve PlotJuggler.
  // Create JSON payload
  QJsonObject payload;
  payload["user_id"] = QString::fromLatin1(QSysInfo::machineUniqueId());
  payload["os"] = QSysInfo::productType();
  payload["version"] = VERSION_STRING;
  payload["installation"] = QString(PJ_INSTALLATION);

  QJsonDocument doc(payload);
  QByteArray jsonData = doc.toJson();

  // Test DNS resolution first
  QHostInfo hostInfo = QHostInfo::fromName("app.plotjuggler.io");
  if (hostInfo.error() != QHostInfo::NoError)
  {
    qDebug() << "DNS lookup failed:" << hostInfo.errorString()
             << " Addresses found:" << hostInfo.addresses();
  }

  // Create network request
  QNetworkRequest request_message;
  request_message.setUrl(QUrl("https://app.plotjuggler.io/telemetry"));
  request_message.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

  // Disable SSL peer verification for telemetry (workaround for Qt5/OpenSSL 3.0 incompatibility)
  // This is acceptable for anonymous telemetry data
  QSslConfiguration sslConfig = request_message.sslConfiguration();
  sslConfig.setPeerVerifyMode(QSslSocket::VerifyNone);
  request_message.setSslConfiguration(sslConfig);

  // Send POST request
  manager_message.post(request_message, jsonData);

  return app.exec();
}
