#include "toolbox_quaternion.h"
#include "ui_quaternion_to_rpy.h"

#include <QDialogButtonBox>
#include <QEvent>
#include <QMimeData>
#include <QDragEnterEvent>
#include <array>
#include <math.h>
#include "quaternion_to_rpy.h"

ToolboxQuaternion::ToolboxQuaternion()
{
  _widget = new QWidget(nullptr);
  ui = new Ui::quaternion_to_RPY;

  ui->setupUi(_widget);

  ui->lineEditX->installEventFilter(this);
  ui->lineEditY->installEventFilter(this);
  ui->lineEditZ->installEventFilter(this);
  ui->lineEditW->installEventFilter(this);

  connect(ui->buttonBox, &QDialogButtonBox::rejected, this, &ToolboxQuaternion::onClosed);

  connect(ui->checkBoxUnwrap, &QCheckBox::toggled, this, &ToolboxQuaternion::onParametersChanged);

  connect(ui->radioButtonDegrees, &QRadioButton::toggled, this,
          &ToolboxQuaternion::onParametersChanged);

  connect(ui->pushButtonSave, &QPushButton::clicked, this,
          &ToolboxQuaternion::on_pushButtonSave_clicked);
}

ToolboxQuaternion::~ToolboxQuaternion()
{
}

void ToolboxQuaternion::init(PJ::PlotDataMapRef& src_data, PJ::TransformsMap& transform_map)
{
  _plot_data = &src_data;
  _transforms = &transform_map;

  _plot_widget = new PJ::PlotWidgetBase(ui->frame);

  auto preview_layout = new QHBoxLayout(ui->framePlotPreview);
  preview_layout->setMargin(6);
  preview_layout->addWidget(_plot_widget);
}

std::pair<QWidget*, PJ::ToolboxPlugin::WidgetType> ToolboxQuaternion::providedWidget() const
{
  return { _widget, PJ::ToolboxPlugin::FIXED };
}

bool ToolboxQuaternion::onShowWidget()
{
  return true;
}

bool ToolboxQuaternion::eventFilter(QObject* obj, QEvent* ev)
{
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

      QStringList curves;
      while (!stream.atEnd())
      {
        QString curve_name;
        stream >> curve_name;
        if (!curve_name.isEmpty())
        {
          curves.push_back(curve_name);
        }
      }
      if (curves.size() != 1)
      {
        return false;
      }

      _dragging_curve = curves.front();

      if (obj == ui->lineEditX || obj == ui->lineEditY || obj == ui->lineEditZ ||
          obj == ui->lineEditW)
      {
        event->acceptProposedAction();
        return true;
      }
    }
  }
  else if (ev->type() == QEvent::Drop)
  {
    auto lineEdit = qobject_cast<QLineEdit*>(obj);

    if (!lineEdit)
    {
      return false;
    }
    lineEdit->setText(_dragging_curve);

    if ((obj == ui->lineEditX && _dragging_curve.endsWith("x")) ||
        (obj == ui->lineEditY && _dragging_curve.endsWith("y")) ||
        (obj == ui->lineEditZ && _dragging_curve.endsWith("z")) ||
        (obj == ui->lineEditW && _dragging_curve.endsWith("w")))
    {
      autoFill(_dragging_curve.left(_dragging_curve.size() - 1));
    }

    // Drag & drop for PX4 quaternions (order is WXYZ with entries labeled q.0X)
    if ((obj == ui->lineEditX && _dragging_curve.endsWith(".01")) ||
        (obj == ui->lineEditY && _dragging_curve.endsWith(".02")) ||
        (obj == ui->lineEditZ && _dragging_curve.endsWith(".03")) ||
        (obj == ui->lineEditW && _dragging_curve.endsWith(".00")))
    {
      autoFill(_dragging_curve.left(_dragging_curve.size() - 3), { ".01", ".02", ".03", ".00" });
    }
  }

  return false;
}

void ToolboxQuaternion::autoFill(QString prefix, QStringList suffix)
{
  std::array<QLineEdit*, 4> lineEdits = { ui->lineEditX, ui->lineEditY, ui->lineEditZ,
                                          ui->lineEditW };
  QStringList names;
  for (int i = 0; i < 4; i++)
  {
    QString name = prefix + suffix[i];
    auto it = _plot_data->numeric.find(name.toStdString());
    if (it != _plot_data->numeric.end())
    {
      names.push_back(name);
    }
  }

  if (names.size() == 4)
  {
    for (int i = 0; i < 4; i++)
    {
      lineEdits[i]->setText(names[i]);
    }
    ui->lineEditOut->setText(prefix);
    ui->pushButtonSave->setEnabled(true);

    generateRPY(PREVIEW);
  }
}

bool ToolboxQuaternion::generateRPY(GenerateType type)
{
  using namespace PJ;

  bool wrap = ui->checkBoxUnwrap->isChecked();
  double unit_scale = ui->radioButtonDegrees->isChecked() ? (180.0 / M_PI) : 1.0;
  auto transform = std::make_shared<QuaternionToRollPitchYaw>();

  std::vector<const PlotData*> src_data;
  {
    for (QLineEdit* line : { ui->lineEditX, ui->lineEditY, ui->lineEditZ, ui->lineEditW })
    {
      auto it = _plot_data->numeric.find(line->text().toStdString());
      if (it == _plot_data->numeric.end())
      {
        return false;
      }
      src_data.push_back(&it->second);
    }
  }

  std::string prefix = ui->lineEditOut->text().toStdString();

  // remove previous cruves bvefore creating new one
  _plot_widget->removeAllCurves();

  _preview_data_roll.reset(new PlotData(prefix + "roll", {}));
  _preview_data_pitch.reset(new PlotData(prefix + "pitch", {}));
  _preview_data_yaw.reset(new PlotData(prefix + "yaw", {}));

  std::vector<PlotData*> dst_vector = { _preview_data_roll.get(), _preview_data_pitch.get(),
                                        _preview_data_yaw.get() };
  if (type == SAVE)
  {
    dst_vector[0] = &_plot_data->getOrCreateNumeric(prefix + "roll", {});
    dst_vector[1] = &_plot_data->getOrCreateNumeric(prefix + "pitch", {});
    dst_vector[2] = &_plot_data->getOrCreateNumeric(prefix + "yaw", {});
  }

  transform->setData(_plot_data, src_data, dst_vector);
  transform->setWarp(wrap);
  transform->setScale(unit_scale);

  transform->calculate();

  if (type == PREVIEW)
  {
    _plot_widget->removeAllCurves();
    for (auto dst_data : dst_vector)
    {
      _plot_widget->addCurve(dst_data->plotName(), *dst_data);
    }
    _plot_widget->resetZoom();
  }

  if (type == SAVE)
  {
    _transforms->insert({ prefix + "RPY", transform });

    emit plotCreated(prefix + "roll");
    emit plotCreated(prefix + "pitch");
    emit plotCreated(prefix + "yaw");
  }
  return true;
}

void ToolboxQuaternion::on_pushButtonSave_clicked()
{
  generateRPY(SAVE);

  ui->lineEditX->setText({});
  ui->lineEditY->setText({});
  ui->lineEditZ->setText({});
  ui->lineEditW->setText({});
  ui->lineEditOut->setText({});
  _plot_widget->removeAllCurves();

  emit this->closed();
}

void ToolboxQuaternion::onParametersChanged()
{
  if (ui->lineEditX->text().isEmpty() || ui->lineEditY->text().isEmpty() ||
      ui->lineEditZ->text().isEmpty() || ui->lineEditW->text().isEmpty() ||
      ui->lineEditOut->text().isEmpty())
  {
    ui->pushButtonSave->setEnabled(false);
    return;
  }
  bool valid = generateRPY(PREVIEW);
  ui->pushButtonSave->setEnabled(valid);
}

void ToolboxQuaternion::onClosed()
{
  emit this->closed();
}
