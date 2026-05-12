#pragma once

#include <QtPlugin>
#include <thread>
#include "PlotJuggler/toolbox_base.h"
#include "PlotJuggler/plotwidget_base.h"
#include "quaternion_to_rpy.h"

namespace Ui
{
class quaternion_to_RPY;
}

class ToolboxQuaternion : public PJ::ToolboxPlugin
{
  Q_OBJECT
  Q_PLUGIN_METADATA(IID "facontidavide.PlotJuggler3.Toolbox")
  Q_INTERFACES(PJ::ToolboxPlugin)

public:
  ToolboxQuaternion();

  ~ToolboxQuaternion() override;

  const char* name() const override
  {
    return "Quaternion to RPY";
  }

  void init(PJ::PlotDataMapRef& src_data, PJ::TransformsMap& transform_map) override;

  std::pair<QWidget*, WidgetType> providedWidget() const override;

public slots:

  bool onShowWidget() override;

private slots:

  void on_pushButtonSave_clicked();

  void onParametersChanged();

  void onClosed();

private:
  QWidget* _widget;
  Ui::quaternion_to_RPY* ui;

  bool eventFilter(QObject* obj, QEvent* event) override;

  QString _dragging_curve;

  void autoFill(QString prefix, QStringList suffix = { "x", "y", "z", "w" });

  PJ::PlotWidgetBase* _plot_widget = nullptr;

  PJ::PlotDataMapRef* _plot_data = nullptr;

  PJ::TransformsMap* _transforms = nullptr;

  std::unique_ptr<PlotData> _preview_data_roll;
  std::unique_ptr<PlotData> _preview_data_pitch;
  std::unique_ptr<PlotData> _preview_data_yaw;

  enum GenerateType
  {
    PREVIEW,
    SAVE
  };

  bool generateRPY(GenerateType type);
};
