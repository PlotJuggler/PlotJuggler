/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef NLOHMANN_PARSERS_H
#define NLOHMANN_PARSERS_H

#include "nlohmann/json.hpp"
#include "PlotJuggler/messageparser_base.h"
#include <QDebug>
#include <QSettings>

using namespace PJ;

class NlohmannParser : public MessageParser
{
public:
  NlohmannParser(const std::string& topic_name, PlotDataMapRef& data, bool use_msg_stamp,
                 const std::string& stamp_fieldname)
    : MessageParser(topic_name, data)
    , _use_message_stamp(use_msg_stamp)
    , _stamp_fieldname(stamp_fieldname)
  {
  }

protected:
  bool parseMessageImpl(double& timestamp);

  nlohmann::json _json;
  bool _use_message_stamp;
  std::string _stamp_fieldname;
};

class JSON_Parser : public NlohmannParser
{
public:
  JSON_Parser(const std::string& topic_name, PlotDataMapRef& data, bool use_msg_stamp,
              const std::string& stamp_fieldname)
    : NlohmannParser(topic_name, data, use_msg_stamp, stamp_fieldname)
  {
  }

  bool parseMessage(const MessageRef msg, double& timestamp) override;
};

class CBOR_Parser : public NlohmannParser
{
public:
  CBOR_Parser(const std::string& topic_name, PlotDataMapRef& data, bool use_msg_stamp,
              const std::string& stamp_fieldname)
    : NlohmannParser(topic_name, data, use_msg_stamp, stamp_fieldname)
  {
  }

  bool parseMessage(const MessageRef msg, double& timestamp) override;
};

class BSON_Parser : public NlohmannParser
{
public:
  BSON_Parser(const std::string& topic_name, PlotDataMapRef& data, bool use_msg_stamp,
              const std::string& stamp_fieldname)
    : NlohmannParser(topic_name, data, use_msg_stamp, stamp_fieldname)
  {
  }

  bool parseMessage(const MessageRef msg, double& timestamp) override;
};

class MessagePack_Parser : public NlohmannParser
{
public:
  MessagePack_Parser(const std::string& topic_name, PlotDataMapRef& data, bool use_msg_stamp,
                     const std::string& stamp_fieldname)
    : NlohmannParser(topic_name, data, use_msg_stamp, stamp_fieldname)
  {
  }

  bool parseMessage(const MessageRef msg, double& timestamp) override;
};

//------------------------------------------

#include <QCheckBox>
#include <QDialog>
#include <QFrame>
#include <QLabel>
#include <QVBoxLayout>

#include "PlotJuggler/line_edit.h"

class QCheckBoxClose : public QWidget
{
public:
  QCheckBox* checkbox;
  QFrame* frame;
  LineEdit* lineedit;

  QCheckBoxClose(QString text) : QWidget()
  {
    checkbox = new QCheckBox(text, this);
    checkbox->setChecked(false);

    frame = new QFrame(this);
    frame->setFrameStyle(QFrame::StyledPanel | QFrame::Plain);
    frame->setVisible(false);

    auto* label = new QLabel("Timestamp field name:", frame);
    lineedit = new LineEdit(frame);
    lineedit->setPlaceholderText("e.g. timestamp, time, ts");

    auto* frameLayout = new QVBoxLayout(frame);
    frameLayout->setContentsMargins(8, 8, 8, 8);
    frameLayout->setSpacing(4);
    frameLayout->addWidget(label);
    frameLayout->addWidget(lineedit);

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(4);
    mainLayout->addWidget(checkbox);
    mainLayout->addWidget(frame);

    connect(checkbox, &QCheckBox::toggled, this, [this](bool checked) {
      frame->setVisible(checked);
      // Trigger parent dialog resize
      if (auto* dlg = qobject_cast<QDialog*>(window()))
      {
        dlg->adjustSize();
      }
    });
  }

  bool isChecked() const
  {
    return checkbox->isChecked();
  }

  void setChecked(bool checked)
  {
    checkbox->setChecked(checked);
  }

  ~QCheckBoxClose() override
  {
    qDebug() << "Destroying QCheckBoxClose";
  }
};

class NlohmannParserCreator : public ParserFactoryPlugin
{
public:
  NlohmannParserCreator(const char* encoding)
  {
    _encoding = encoding;
    _checkbox_use_timestamp = new QCheckBoxClose("use field as timestamp if available");
    loadSettings();
  }

  template <typename ParserT>
  MessageParserPtr createParserImpl(const std::string& topic_name, PlotDataMapRef& data)
  {
    saveSettings();

    std::string timestamp_name = _checkbox_use_timestamp->lineedit->text().toStdString();
    return std::make_shared<ParserT>(topic_name, data, _checkbox_use_timestamp->isChecked(),
                                     timestamp_name);
  }

  QWidget* optionsWidget() override
  {
    loadSettings();

    return _checkbox_use_timestamp;
  }

  const char* encoding() const override
  {
    return _encoding;
  }

  bool xmlSaveState(QDomDocument& /*doc*/, QDomElement& parent_element) const override
  {
    parent_element.setAttribute("timestampEnabled", _checkbox_use_timestamp->isChecked());
    parent_element.setAttribute("timestampFieldName", _checkbox_use_timestamp->lineedit->text());
    return true;
  }

  bool xmlLoadState(const QDomElement& parent_element) override
  {
    if (!parent_element.hasAttribute("timestampEnabled"))
    {
      return false;
    }
    _checkbox_use_timestamp->setChecked(parent_element.attribute("timestampEnabled").toInt() != 0);
    _checkbox_use_timestamp->lineedit->setText(parent_element.attribute("timestampFieldName"));
    saveSettings();
    return true;
  }

  virtual void loadSettings()
  {
    QSettings settings;
    QString prefix = QString("NlohmannParser.") + QString(encoding());
    bool checked = settings.value(prefix + ".timestampEnabled", false).toBool();
    QString field = settings.value(prefix + ".timestampFieldName", "").toString();

    _checkbox_use_timestamp->setChecked(checked);
    _checkbox_use_timestamp->lineedit->setText(field);
  }

  virtual void saveSettings()
  {
    QSettings settings;
    QString prefix = QString("NlohmannParser.") + QString(encoding());
    settings.setValue(prefix + ".timestampEnabled", _checkbox_use_timestamp->isChecked());
    settings.setValue(prefix + ".timestampFieldName", _checkbox_use_timestamp->lineedit->text());
  }

protected:
  QCheckBoxClose* _checkbox_use_timestamp;
  const char* _encoding;
};

class JSON_ParserFactory : public NlohmannParserCreator
{
public:
  JSON_ParserFactory() : NlohmannParserCreator("json")
  {
  }

  MessageParserPtr createParser(const std::string& topic_name, const std::string& /*type_name*/,
                                const std::string& /*schema*/, PlotDataMapRef& data) override
  {
    return createParserImpl<JSON_Parser>(topic_name, data);
  }
  const char* name() const override
  {
    return "JSON_ParserFactory";
  }
};

class CBOR_ParserFactory : public NlohmannParserCreator
{
public:
  CBOR_ParserFactory() : NlohmannParserCreator("cbor")
  {
  }

  MessageParserPtr createParser(const std::string& topic_name, const std::string&,
                                const std::string&, PlotDataMapRef& data) override
  {
    return createParserImpl<CBOR_Parser>(topic_name, data);
  }
  const char* name() const override
  {
    return "CBOR_ParserFactory";
  }
};

class BSON_ParserFactory : public NlohmannParserCreator
{
public:
  BSON_ParserFactory() : NlohmannParserCreator("bson")
  {
  }

  MessageParserPtr createParser(const std::string& topic_name, const std::string&,
                                const std::string&, PlotDataMapRef& data) override
  {
    return createParserImpl<BSON_Parser>(topic_name, data);
  }
  const char* name() const override
  {
    return "BSON_ParserFactory";
  }
};

class MessagePack_ParserFactory : public NlohmannParserCreator
{
public:
  MessagePack_ParserFactory() : NlohmannParserCreator("msgpack")
  {
  }

  MessageParserPtr createParser(const std::string& topic_name, const std::string&,
                                const std::string&, PlotDataMapRef& data) override
  {
    return createParserImpl<MessagePack_Parser>(topic_name, data);
  }
  const char* name() const override
  {
    return "MessagePack_ParserFactory";
  }
};

#endif  // NLOHMANN_PARSERS_H
