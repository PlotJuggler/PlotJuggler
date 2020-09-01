#pragma once
#include <QDialog>
#include <QLabel>
#include <QListWidget>
#include <QAbstractButton>
#include <unordered_set>
#include <sensor_msgs/Image.h>
#include <rosbag/bag.h>
#include <QPainter>
#include <QtCore/QElapsedTimer>

#include "ui_imageview_dialog.h"

namespace Ui
{
class Ui_ImageView;
}

class ImageViewDialog : public QDialog
{
  Q_OBJECT
private:
  Ui::ImageView* _ui;
    sensor_msgs::Image::ConstPtr _current_frame;

public:
  explicit ImageViewDialog(QWidget* parent = nullptr)
    : QDialog(parent), _ui(new Ui::ImageView)
  {
    _ui->setupUi(this);
    setModal(false);
  }

  Ui::ImageView* ui() { return _ui; }

  void update_frame(const rosbag::MessageInstance &msg_instance){
      if(!isVisible()){
          setWindowTitle(msg_instance.getTopic().c_str());
          open();
      }
      sensor_msgs::Image::ConstPtr img = msg_instance.instantiate<sensor_msgs::Image>();
      if (img != NULL){
        _current_frame = img;
      }

      repaint();
  }

  void paintEvent(QPaintEvent *event){
      if (_current_frame != NULL){
          QImage image;
          if(_current_frame->encoding == "8UC1"){
              image = QImage((const unsigned char*)_current_frame->data.data(),_current_frame->width,
                                    _current_frame->height,_current_frame->step,QImage::Format_Grayscale8);
          }
          else if(_current_frame->encoding == "16UC1"){
              image = QImage((const unsigned char*)_current_frame->data.data(),_current_frame->width,
                                    _current_frame->height,_current_frame->step,QImage::Format_RGB16);
          }
          else if(_current_frame->encoding == "rgb8"){
              image = QImage((const unsigned char*)_current_frame->data.data(),_current_frame->width,
                                    _current_frame->height,_current_frame->step,QImage::Format_RGB888);
          } else {
              image = QImage((const unsigned char*)_current_frame->data.data(),_current_frame->width,
                             _current_frame->height,_current_frame->step,QImage::Format_ARGB32);
          }


          QRectF target(0.0,0.0,image.width(),image.height());
          QRectF source(0.0,0.0,image.width(),image.height());
          QPainter painter(this);
          painter.drawImage(target,image,source);
      }
  }

  ~ImageViewDialog()
  {
    delete _ui;
  }
};

