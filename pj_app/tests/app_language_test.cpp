// SPDX-License-Identifier: MPL-2.0
#include <QCoreApplication>
#include <QFile>
#include <QLocale>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QTranslator>

#include "AppLanguage.h"

using namespace Qt::StringLiterals;

// Runs with Qt Core alone against the same resource-bearing static library as
// pj_app_shell. In particular, no loose .qm file is on the runtime search path.
class AppLanguageTest : public QObject {
  Q_OBJECT

 private slots:
  void normalizesSupportedLanguages_data() {
    QTest::addColumn<QString>("requested");
    QTest::addColumn<QString>("expected");
    QTest::newRow("chinese") << u"zh_CN"_s << u"zh_CN"_s;
    QTest::newRow("bcp47-chinese") << u"zh-CN"_s << u"zh_CN"_s;
    QTest::newRow("english") << u"en_US"_s << u"en_US"_s;
    QTest::newRow("bcp47-english") << u"en-US"_s << u"en_US"_s;
    QTest::newRow("unsupported") << u"fr_FR"_s << u"en_US"_s;
    QTest::newRow("empty") << QString{} << u"en_US"_s;
  }

  void normalizesSupportedLanguages() {
    QFETCH(QString, requested);
    QFETCH(QString, expected);
    QCOMPARE(PJ::AppLanguage::normalizedLanguage(requested), expected);
  }

  void freshProfileDefaultsToEnglishWithoutWritingSettings() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QSettings settings(directory.filePath(u"preferences.ini"_s), QSettings::IniFormat);
    QCOMPARE(PJ::AppLanguage::preferredLanguage(settings), u"en_US"_s);
    QVERIFY(!settings.contains(PJ::AppLanguage::kPreferenceKey));
  }

  void savedLanguageSurvivesANewSettingsInstance() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(u"preferences.ini"_s);
    {
      QSettings settings(path, QSettings::IniFormat);
      settings.setValue(PJ::AppLanguage::kPreferenceKey, u"zh_CN"_s);
      settings.sync();
      QCOMPARE(settings.status(), QSettings::NoError);
    }
    const QSettings settings(path, QSettings::IniFormat);
    QCOMPARE(PJ::AppLanguage::preferredLanguage(settings), u"zh_CN"_s);
  }

  void invalidSavedLanguageFallsBackWithoutOverwritingIt() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings settings(directory.filePath(u"preferences.ini"_s), QSettings::IniFormat);
    settings.setValue(PJ::AppLanguage::kPreferenceKey, u"unsupported"_s);
    QCOMPARE(PJ::AppLanguage::preferredLanguage(settings), u"en_US"_s);
    QCOMPARE(settings.value(PJ::AppLanguage::kPreferenceKey).toString(), u"unsupported"_s);
  }

  void shippedCatalogTranslatesWidgetsAndQtButtons() {
    const PJ::AppLanguage language(*QCoreApplication::instance(), u"zh_CN"_s);
    QVERIFY(language.catalogLoaded());
    QVERIFY(QFile::exists(u":/i18n/plotjuggler4_zh_CN.qm"_s));
    QCOMPARE(language.language(), u"zh_CN"_s);
    const QString label = QCoreApplication::translate("PreferencesContent", "Language:");
    QVERIFY(!label.isEmpty());
    QVERIFY(label != "Language:"_L1);
    const QString title = QCoreApplication::translate("PJ::PreferencesDialog", "Preferences");
    QVERIFY(!title.isEmpty());
    QVERIFY(title != "Preferences"_L1);
    QCOMPARE(QCoreApplication::translate("QPlatformTheme", "Cancel"), u"取消"_s);
  }

  void embeddedCatalogTranslatesCurrentQtFileDialogKeys_data() {
    QTest::addColumn<QByteArray>("context");
    QTest::addColumn<QByteArray>("source");
    QTest::addColumn<QByteArray>("comment");
    QTest::addColumn<QString>("expected");
    QTest::newRow("QFileDialog-0") << QByteArray("QFileDialog") << QByteArray("&Look in:") << QByteArray("")
                                   << u"查找范围(&L)："_s;
    QTest::newRow("QFileDialog-1") << QByteArray("QFileDialog") << QByteArray("Files of &type:") << QByteArray("")
                                   << u"文件类型(&T)："_s;
    QTest::newRow("QAbstractFileIconProvider-2")
        << QByteArray("QAbstractFileIconProvider") << QByteArray("Drive") << QByteArray("") << u"驱动器"_s;
    QTest::newRow("QAbstractFileIconProvider-3")
        << QByteArray("QAbstractFileIconProvider") << QByteArray("File") << QByteArray("") << u"文件"_s;
    QTest::newRow("QAbstractFileIconProvider-4")
        << QByteArray("QAbstractFileIconProvider") << QByteArray("Unknown") << QByteArray("") << u"未知"_s;
    QTest::newRow("QAbstractFileIconProvider-5") << QByteArray("QAbstractFileIconProvider") << QByteArray("File Folder")
                                                 << QByteArray("Match Windows Explorer") << u"文件夹"_s;
    QTest::newRow("QAbstractFileIconProvider-6") << QByteArray("QAbstractFileIconProvider") << QByteArray("Folder")
                                                 << QByteArray("All other platforms") << u"文件夹"_s;
    QTest::newRow("QAbstractFileIconProvider-7")
        << QByteArray("QAbstractFileIconProvider") << QByteArray("Alias") << QByteArray("macOS Finder") << u"替身"_s;
    QTest::newRow("QAbstractFileIconProvider-8") << QByteArray("QAbstractFileIconProvider") << QByteArray("Shortcut")
                                                 << QByteArray("All other platforms") << u"快捷方式"_s;
  }

  void embeddedCatalogTranslatesCurrentQtFileDialogKeys() {
    QFETCH(QByteArray, context);
    QFETCH(QByteArray, source);
    QFETCH(QByteArray, comment);
    QFETCH(QString, expected);
    const char* disambiguation = comment.isEmpty() ? nullptr : comment.constData();
    {
      const PJ::AppLanguage chinese(*QCoreApplication::instance(), u"zh_CN"_s);
      QVERIFY(chinese.catalogLoaded());
      QCOMPARE(QCoreApplication::translate(context.constData(), source.constData(), disambiguation), expected);
    }
    const PJ::AppLanguage english(*QCoreApplication::instance(), u"en_US"_s);
    QCOMPARE(
        QCoreApplication::translate(context.constData(), source.constData(), disambiguation),
        QString::fromUtf8(source));
  }

  void embeddedCatalogTranslatesCurrentTimeControls_data() {
    QTest::addColumn<QByteArray>("context");
    QTest::addColumn<QByteArray>("source");
    QTest::addColumn<QString>("expected");
    QTest::newRow("local") << QByteArray("PJ::PreferencesDialog") << QByteArray("local") << u"本地"_s;
    QTest::newRow("utc") << QByteArray("PJ::PreferencesDialog") << QByteArray("UTC") << u"UTC"_s;
    QTest::newRow("clock-zone-label") << QByteArray("PreferencesContent") << QByteArray("Clock time zone:")
                                      << u"时钟时区："_s;
    QTest::newRow("clock-zone-help")
        << QByteArray("PreferencesContent")
        << QByteArray("Time zone of the clock times shown while the date-time toggle (bottom left) is on.")
        << u"启用左下角的日期时间开关时，时钟时间所使用的时区。"_s;
    QTest::newRow("time-offset-help") << QByteArray("TimelineWidget")
                                      << QByteArray(
                                             "Use time offset: show time relative to each dataset's start (axis begins "
                                             "at 0) instead of absolute time")
                                      << u"使用时间偏移：显示相对于各数据集起点的时间（横轴从 0 开始），而非绝对时间"_s;
    QTest::newRow("clock-time-help") << QByteArray("TimelineWidget")
                                     << QByteArray("Show time as clock time (HH:mm:ss) instead of seconds")
                                     << u"将时间显示为时钟时间（HH:mm:ss），而非秒数"_s;
  }

  void embeddedCatalogTranslatesCurrentTimeControls() {
    QFETCH(QByteArray, context);
    QFETCH(QByteArray, source);
    QFETCH(QString, expected);
    {
      const PJ::AppLanguage chinese(*QCoreApplication::instance(), u"zh_CN"_s);
      QVERIFY(chinese.catalogLoaded());
      QCOMPARE(QCoreApplication::translate(context.constData(), source.constData()), expected);
    }
    const PJ::AppLanguage english(*QCoreApplication::instance(), u"en_US"_s);
    QCOMPARE(QCoreApplication::translate(context.constData(), source.constData()), QString::fromUtf8(source));
  }

  void embeddedCatalogTranslatesMcpAndBrowserStatus_data() {
    QTest::addColumn<QByteArray>("context");
    QTest::addColumn<QByteArray>("source");
    QTest::addColumn<QString>("expected");
    QTest::newRow("mcp-browser-0")
        << QByteArray("PJ::FileLoader")
        << QByteArray(
               "The browser can no longer read '%1': it changed or was removed on disk since it was opened. Data "
               "loaded from it may be incomplete; open it again to reload it.")
        << u"浏览器无法再读取“%1”：该文件自打开后已在磁盘上更改或移除。从中加载的数据可能不完整，请重新打开以重新加载。"_s;
    QTest::newRow("mcp-browser-1")
        << QByteArray("PJ::MainWindow")
        << QByteArray(
               "'%1' has no content fingerprint yet: it is still being computed, or the file changed since it was "
               "picked. Save the layout again in a moment, or open the file again.")
        << u"“%1”尚无内容指纹：仍在计算中，或文件自选取后发生了更改。请稍后再次保存布局，或重新打开文件。"_s;
    QTest::newRow("mcp-browser-2")
        << QByteArray("PJ::MainWindow")
        << QByteArray("The MCP server could not listen on 127.0.0.1, so AI agents cannot reach PlotJuggler.")
        << u"MCP 服务器无法监听 127.0.0.1，因此 AI 智能体无法连接 PlotJuggler。"_s;
    QTest::newRow("mcp-browser-3") << QByteArray("PJ::MainWindow") << QByteArray("AI agent: %1") << u"AI 智能体：%1"_s;
    QTest::newRow("mcp-browser-4")
        << QByteArray("PJ::MainWindow")
        << QByteArray("%1 is controlling PlotJuggler through MCP. Turn this off in Preferences → Appearance.")
        << u"%1 正通过 MCP 控制 PlotJuggler。可在“首选项 → 外观”中关闭。"_s;
    QTest::newRow("mcp-browser-5") << QByteArray("PJ::McpServer") << QByteArray("an MCP client")
                                   << u"某个 MCP 客户端"_s;
    QTest::newRow("mcp-browser-6") << QByteArray("PJ::McpServer") << QByteArray("AI agent (%1)")
                                   << u"AI 智能体（%1）"_s;
    QTest::newRow("mcp-browser-7") << QByteArray("PJ::McpServer") << QByteArray("An AI agent asked you to choose: ")
                                   << u"AI 智能体请您选择："_s;
    QTest::newRow("mcp-browser-8")
        << QByteArray("PreferencesContent")
        << QByteArray(
               "Serve the Model Context Protocol on this computer only (127.0.0.1, behind a secret token), so AI "
               "agents such as Claude Code or Codex can open files, read series, and draw in their own tabs. The "
               "status bar names an agent while it is active. Shows whether outside agents may connect now (also when "
               "allowed with --mcp); on by default; applies on OK. An agent a toolbox plugin starts itself gets its "
               "own private access and does not need this.")
        << u"仅在本机提供模型上下文协议服务（127.0.0.1，使用秘密令牌保护），让 Claude Code 或 Codex 等 AI 智能体能够打开文件、读取序列，并在各自的标签页中绘图。智能体活动时，状态栏会显示其名称。此开关显示当前是否允许外部智能体连接（包括通过 --mcp 允许的连接）；默认开启，点击“确定”后生效。工具箱插件自行启动的智能体拥有独立的私有访问权限，无需开启此项。"_s;
    QTest::newRow("mcp-browser-9") << QByteArray("PreferencesContent")
                                   << QByteArray("Let AI agents control PlotJuggler (MCP server):")
                                   << u"允许 AI 智能体控制 PlotJuggler（MCP 服务器）："_s;
  }

  void embeddedCatalogTranslatesMcpAndBrowserStatus() {
    QFETCH(QByteArray, context);
    QFETCH(QByteArray, source);
    QFETCH(QString, expected);
    {
      const PJ::AppLanguage chinese(*QCoreApplication::instance(), u"zh_CN"_s);
      QVERIFY(chinese.catalogLoaded());
      QCOMPARE(QCoreApplication::translate(context.constData(), source.constData()), expected);
    }
    const PJ::AppLanguage english(*QCoreApplication::instance(), u"en_US"_s);
    QCOMPARE(QCoreApplication::translate(context.constData(), source.constData()), QString::fromUtf8(source));
  }

  void embeddedCatalogTranslatesReviewedMessages_data() {
    QTest::addColumn<QByteArray>("context");
    QTest::addColumn<QByteArray>("source");
    QTest::addColumn<int>("numerus");
    QTest::addColumn<QString>("expected");
    QTest::newRow("review-0") << QByteArray("CurveListPanel") << QByteArray("Custom Topics") << -1 << u"自定义话题"_s;
    QTest::newRow("review-1") << QByteArray("PJ::Scene3DConfigPanel") << QByteArray("Topics") << -1 << u"话题"_s;
    QTest::newRow("review-2") << QByteArray("QObject")
                              << QByteArray(
                                     "Camera calibration topic, used automatically by 2D and 3D views. It cannot be "
                                     "displayed on its own, but a plugin panel can read it.")
                              << -1 << u"相机标定话题，由二维和三维视图自动使用。无法单独显示，但插件面板可以读取。"_s;
    QTest::newRow("review-3") << QByteArray("pj::scene3d::RobotModelLayer") << QByteArray("Topic") << -1 << u"话题"_s;
    QTest::newRow("review-4") << QByteArray("PJ::PlotWidget") << QByteArray("Split &Horizontally") << -1
                              << u"水平拆分(&H)"_s;
    QTest::newRow("review-5") << QByteArray("PJ::PlotWidget") << QByteArray("Split &Vertically") << -1
                              << u"垂直拆分(&V)"_s;
    QTest::newRow("review-6") << QByteArray("PJ::VisualizationPlaceholderWidget") << QByteArray("Split &Horizontally")
                              << -1 << u"水平拆分(&H)"_s;
    QTest::newRow("review-7") << QByteArray("PJ::VisualizationPlaceholderWidget") << QByteArray("Split &Vertically")
                              << -1 << u"垂直拆分(&V)"_s;
    QTest::newRow("review-8") << QByteArray("PJ::PlotWidget") << QByteArray("Zoom Out Horizontally") << -1
                              << u"水平缩小至全范围"_s;
    QTest::newRow("review-9") << QByteArray("PJ::PlotWidget") << QByteArray("Zoom Out Vertically") << -1
                              << u"垂直缩小至全范围"_s;
    QTest::newRow("review-10") << QByteArray("PJ::TabbedPlotWidget") << QByteArray("Duplicate tab") << -1
                               << u"复制标签页"_s;
    QTest::newRow("review-11") << QByteArray("PJ::TabbedPlotWidget") << QByteArray("%1 (copy)") << -1
                               << u"%1（副本）"_s;
    QTest::newRow("review-12") << QByteArray("PJ::CurveListPanel") << QByteArray("Refresh derived") << -1
                               << u"刷新派生标记"_s;
    QTest::newRow("review-13")
        << QByteArray("PreferencesContent")
        << QByteArray(
               "Automatic re-runs a changed marker rule on its own, at the reload rate below. Manual only flags it as "
               "needing a refresh, reachable from the dataset's \"Refresh derived\" action.")
        << -1
        << u"自动模式按下方的重新加载频率自动重新运行已更改的标记规则。手动模式仅将其标记为需要刷新，可通过数据集的“刷新派生标记”操作进行刷新。"_s;
    QTest::newRow("review-14") << QByteArray("PJ::MainWindow") << QByteArray("Show point in plot") << -1
                               << u"显示光标处数据点的数值"_s;
    QTest::newRow("review-15") << QByteArray("DatasetMergeActions") << QByteArray("canonical %1") << -1
                               << u"%1（规范格式）"_s;
    QTest::newRow("review-16") << QByteArray("PJ::ExtensionManager")
                               << QByteArray("Not available for this platform (%1)") << -1 << u"不适用于此平台（%1）"_s;
    QTest::newRow("review-17") << QByteArray("PJ::SourcePromotionHost") << QByteArray("promoted") << -1
                               << u"已转为文件数据源"_s;
    QTest::newRow("review-18") << QByteArray("PJ::MainWindow")
                               << QByteArray("Failed to publish the source-promotion service for '%1': %2") << -1
                               << u"无法为“%1”发布数据源转换服务：%2"_s;
    QTest::newRow("review-19") << QByteArray("PJ::Scene3DConfigPanel") << QByteArray("Meshes opacity") << -1
                               << u"模型网格不透明度"_s;
    QTest::newRow("review-20") << QByteArray("PJ::Scene3DConfigPanel") << QByteArray("Show/hide visual meshes") << -1
                               << u"显示或隐藏外观模型网格"_s;
    QTest::newRow("review-21")
        << QByteArray("PJ::Scene3DDockWidget")
        << QByteArray("Only depth-encoded images can be displayed in a 3D view. Use a 2D view for colour images.") << -1
        << u"三维视图只能显示深度编码图像。彩色图像请使用二维视图。"_s;
    QTest::newRow("review-22") << QByteArray("QObject") << QByteArray("SceneEntities decode received no snapshot") << -1
                               << u"SceneEntities 解码未收到快照"_s;
    QTest::newRow("review-23") << QByteArray("QObject") << QByteArray("SceneEntities conversion failed: %1") << -1
                               << u"SceneEntities 转换失败：%1"_s;
    QTest::newRow("review-24") << QByteArray("LayoutXml")
                               << QByteArray(
                                      "Data source '%1' is a file on a local machine, which a web layout cannot open.")
                               << -1 << u"数据源“%1”是本机文件，网络托管的布局无法打开。"_s;
    QTest::newRow("review-25")
        << QByteArray("PJ::MainWindow")
        << QByteArray("Layout '%1' records %n streaming source(s); the desktop loader does not start them.") << 7
        << u"布局“%1”包含 7 个流式数据源；桌面加载器不会启动它们。"_s;
    QTest::newRow("review-26") << QByteArray("QObject") << QByteArray("computed at %1 s") << -1 << u"在 %1 秒处计算"_s;
    QTest::newRow("review-27") << QByteArray("PJ::PlotWidget") << QByteArray("%1 vs %2") << -1 << u"%1 vs %2"_s;
    QTest::newRow("review-28") << QByteArray("PJ::FileLoader") << QByteArray("Could not load '%1': %2") << -1
                               << u"无法加载“%1”：%2"_s;
    QTest::newRow("review-29")
        << QByteArray("PJ::MainWindow")
        << QByteArray(
               "Nothing to record: %n active source(s) write decoded samples directly, which cannot be recorded.")
        << 7 << u"没有可录制的内容：7 个活动数据源直接写入解码后的样本，无法录制。"_s;
    QTest::newRow("review-30") << QByteArray("PJ::MainWindow")
                               << QByteArray("%n source(s) write decoded samples directly and are not recorded: %1")
                               << 7 << u"7 个数据源直接写入解码后的样本，未参与录制：%1"_s;
  }

  void embeddedCatalogTranslatesReviewedMessages() {
    QFETCH(QByteArray, context);
    QFETCH(QByteArray, source);
    QFETCH(int, numerus);
    QFETCH(QString, expected);
    {
      const PJ::AppLanguage chinese(*QCoreApplication::instance(), u"zh_CN"_s);
      QVERIFY(chinese.catalogLoaded());
      QCOMPARE(QCoreApplication::translate(context.constData(), source.constData(), nullptr, numerus), expected);
    }
    const PJ::AppLanguage english(*QCoreApplication::instance(), u"en_US"_s);
    const QString fallback =
        numerus < 0 ? QString::fromUtf8(source) : QString::fromUtf8(source).replace(u"%n"_s, QString::number(numerus));
    QCOMPARE(QCoreApplication::translate(context.constData(), source.constData(), nullptr, numerus), fallback);
  }

  void embeddedCatalogOmitsObjectNamesAndStyles_data() {
    QTest::addColumn<QByteArray>("context");
    QTest::addColumn<QByteArray>("source");
    QTest::newRow("title-object-name") << QByteArray("CustomWidgetConfigPanel") << QByteArray("labelWidgetName");
    QTest::newRow("series-filter-name") << QByteArray("CustomWidgetConfigPanel")
                                        << QByteArray("customWidgetSeriesFilter");
    QTest::newRow("widget-filter-name") << QByteArray("CustomWidgetPickerPanel") << QByteArray("customWidgetFilter");
    QTest::newRow("detail-title-style") << QByteArray("MarketplaceWindow")
                                        << QByteArray("font-weight:700; font-size:14px;");
  }

  void embeddedCatalogOmitsObjectNamesAndStyles() {
    QFETCH(QByteArray, context);
    QFETCH(QByteArray, source);
    const PJ::AppLanguage chinese(*QCoreApplication::instance(), u"zh_CN"_s);
    QVERIFY(chinese.catalogLoaded());
    QTranslator catalog;
    QVERIFY(catalog.load(u":/i18n/plotjuggler4_zh_CN.qm"_s));
    QVERIFY(catalog.translate(context.constData(), source.constData()).isEmpty());
  }

  void englishUsesSourceTextAndDoesNotOverwriteThePreference() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings settings(directory.filePath(u"preferences.ini"_s), QSettings::IniFormat);
    settings.setValue(PJ::AppLanguage::kPreferenceKey, u"zh_CN"_s);
    const PJ::AppLanguage language(*QCoreApplication::instance(), u"en_US"_s);
    QVERIFY(!language.catalogLoaded());
    QCOMPARE(language.language(), u"en_US"_s);
    QCOMPARE(QCoreApplication::translate("PreferencesContent", "Language:"), u"Language:"_s);
    QCOMPARE(PJ::AppLanguage::preferredLanguage(settings), u"zh_CN"_s);
  }

  void translatorLifetimeDoesNotLeakIntoEnglishStartup() {
    {
      const PJ::AppLanguage chinese(*QCoreApplication::instance(), u"zh_CN"_s);
      QVERIFY(chinese.catalogLoaded());
      QVERIFY(QCoreApplication::translate("PreferencesContent", "Language:") != "Language:"_L1);
    }
    const PJ::AppLanguage english(*QCoreApplication::instance(), u"en_US"_s);
    QCOMPARE(QCoreApplication::translate("PreferencesContent", "Language:"), u"Language:"_s);
    QCOMPARE(QCoreApplication::translate("QPlatformTheme", "Cancel"), u"Cancel"_s);
  }

  void repeatedChineseStartupDoesNotAccumulateTranslators() {
    for (int launch = 0; launch < 3; ++launch) {
      {
        const PJ::AppLanguage chinese(*QCoreApplication::instance(), u"zh_CN"_s);
        QVERIFY(chinese.catalogLoaded());
        QVERIFY(QCoreApplication::translate("PreferencesContent", "Language:") != "Language:"_L1);
      }
      QCOMPARE(QCoreApplication::translate("PreferencesContent", "Language:"), u"Language:"_s);
    }
  }

  void changingThePreferenceNeedsANewStartup() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings settings(directory.filePath(u"preferences.ini"_s), QSettings::IniFormat);
    settings.setValue(PJ::AppLanguage::kPreferenceKey, u"zh_CN"_s);
    {
      const PJ::AppLanguage language(*QCoreApplication::instance(), PJ::AppLanguage::preferredLanguage(settings));
      QVERIFY(language.catalogLoaded());
      settings.setValue(PJ::AppLanguage::kPreferenceKey, u"en_US"_s);
      QCOMPARE(language.language(), u"zh_CN"_s);
      QVERIFY(QCoreApplication::translate("PreferencesContent", "Language:") != "Language:"_L1);
    }
    const PJ::AppLanguage restarted(*QCoreApplication::instance(), PJ::AppLanguage::preferredLanguage(settings));
    QCOMPARE(restarted.language(), u"en_US"_s);
    QCOMPARE(QCoreApplication::translate("PreferencesContent", "Language:"), u"Language:"_s);
  }

  void interfaceLanguagePreservesTheNumericLocale() {
    const QLocale original;
    const PJ::AppLanguage language(*QCoreApplication::instance(), u"zh_CN"_s);
    QCOMPARE(QLocale(), original);
    QCOMPARE(QLocale().toString(1234.5), original.toString(1234.5));
  }
};

QTEST_GUILESS_MAIN(AppLanguageTest)
#include "app_language_test.moc"
