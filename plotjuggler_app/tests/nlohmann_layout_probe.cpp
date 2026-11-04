// Configure with BUILD_TESTING=ON; build target test_nlohmann_layout.
// Run: QT_QPA_PLATFORM=offscreen build/bin/test_nlohmann_layout
// Assertions are test checks and must stay active in Release builds.
#undef NDEBUG
#include <QApplication>
#include <QTemporaryDir>
#include <array>
#include <cassert>
#include <iostream>
#include "../nlohmann_parsers.h"

int main(int argc, char** argv)
{
  QApplication app(argc, argv);
  QTemporaryDir settings_dir;
  assert(settings_dir.isValid());
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_dir.path());
  app.setOrganizationName("PlotJugglerLayoutProbe");
  app.setApplicationName("Timestamp");
  const std::array<ParserFactoryPtr, 4> factories{ std::make_shared<JSON_ParserFactory>(),
                                                   std::make_shared<CBOR_ParserFactory>(),
                                                   std::make_shared<BSON_ParserFactory>(),
                                                   std::make_shared<MessagePack_ParserFactory>() };
  for (const auto& factory : factories)
  {
    auto* options = static_cast<QCheckBoxClose*>(factory->optionsWidget());
    options->setChecked(true);
    options->lineedit->setText("sensor_time");
    QDomDocument doc;
    const auto state = factory->xmlSaveState(doc);
    options->setChecked(false);
    options->lineedit->setText("wrong_time");
    assert(factory->xmlLoadState(state));
    options = static_cast<QCheckBoxClose*>(factory->optionsWidget());
    assert(options->isChecked());
    assert(options->lineedit->text() == "sensor_time");

    const nlohmann::json message = { { "sensor_time", 123.5 }, { "value", 42.0 } };
    std::vector<uint8_t> payload;
    const std::string encoding = factory->encoding();
    if (encoding == "json")
    {
      const auto text = message.dump();
      payload.assign(text.begin(), text.end());
    }
    else if (encoding == "cbor")
    {
      payload = nlohmann::json::to_cbor(message);
    }
    else if (encoding == "bson")
    {
      payload = nlohmann::json::to_bson(message);
    }
    else
    {
      payload = nlohmann::json::to_msgpack(message);
    }
    PlotDataMapRef data;
    auto parser = factory->createParser("", "", "", data);
    double timestamp = 1000.0;
    assert(parser->parseMessage(MessageRef(payload), timestamp));
    assert(timestamp == 123.5);

    options->setChecked(false);
    const auto disabled_state = factory->xmlSaveState(doc);
    options->setChecked(true);
    assert(factory->xmlLoadState(disabled_state));
    assert(!options->isChecked());
    // Old layouts have no parser settings and must preserve user preferences.
    const auto legacy_state = doc.createElement("plugin");
    assert(!factory->xmlLoadState(legacy_state));
    assert(!options->isChecked());
    std::cout << encoding << ": layout restores enabled/disabled timestamp and payload time\n";
  }
}
