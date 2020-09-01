#pragma once
#include <QDialog>
#include <QLabel>
#include <QListWidget>
#include <QAbstractButton>
#include <unordered_set>

#include "ui_publisher_select_dialog.h"

namespace Ui
{
class Ui_PublisherSelect;
}

class PublisherSelectDialog : public QDialog
{
  Q_OBJECT
private:
  Ui::PublisherSelect* _ui;

private slots:
void on_radioButtonWS_toggled(bool checked){
    ui()->lineEditWebsocketUrl->setEnabled(checked);
    ui()->checkBoxShowVideo->setEnabled(checked);
};


public:
  explicit PublisherSelectDialog(QWidget* parent = nullptr)
    : QDialog(parent), _ui(new Ui::PublisherSelect)
  {
    _ui->setupUi(this);
  }

  Ui::PublisherSelect* ui() { return _ui; }

  ~PublisherSelectDialog()
  {
    delete _ui;
  }
};

