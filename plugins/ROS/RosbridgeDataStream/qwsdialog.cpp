#include "qwsdialog.h"
#include "ui_qwsdialog.h"
#include <QSettings>

QWSDialog::QWSDialog(QWidget* parent) : QDialog(parent), ui(new Ui::QWSDialog)
{
  ui->setupUi(this);

  QSettings settings;

}

QWSDialog::~QWSDialog()
{
  QSettings settings;
  delete ui;
}

void QWSDialog::on_pushButtonConnect_pressed()
{
  if(ui->lineEditWebsocket->text().toStdString() != ""){
      m_url = ui->lineEditWebsocket->text().toStdString();
      this->close();
  }
}

void QWSDialog::on_pushButtonCancel_pressed()
{
  this->close();
}