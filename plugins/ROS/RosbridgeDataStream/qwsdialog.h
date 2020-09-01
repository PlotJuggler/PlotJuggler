#ifndef QWSDIALOG_H
#define QWSDIALOG_H

#include <QDialog>
//#include "../3rdparty/rosbridgecpp/rosbridge_ws_client.hpp"
namespace Ui
{
class QWSDialog;
}

class QWSDialog : public QDialog
{
  Q_OBJECT

public:
  ~QWSDialog();
  explicit QWSDialog(QWidget* parent = 0);

  std::string get_url() { return m_url; }

private slots:
  void on_pushButtonConnect_pressed();

  void on_pushButtonCancel_pressed();

private:
  Ui::QWSDialog* ui;
  std::string m_url;
};

#endif  // QWSDIALOG_H
