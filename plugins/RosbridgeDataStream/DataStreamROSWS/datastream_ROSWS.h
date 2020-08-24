#ifndef DATASTREAM_ROS_TOPIC_H
#define DATASTREAM_ROS_TOPIC_H

#include <QtPlugin>
#include <QAction>
#include <QTimer>
#include <QWebSocket>
#include <thread>
#include "PlotJuggler/datastreamer_base.h"
#include "dialog_select_ros_topics.h"
//#include "../../../3rdparty/rosbridgecpp/rosbridge_ws_client.hpp"

class DataStreamROSWS : public DataStreamer
{
  Q_OBJECT
  Q_PLUGIN_METADATA(IID "com.icarustechnology.PlotJuggler.DataStreamer"
                        "../datastreamer.json")
  Q_INTERFACES(DataStreamer)

public:
  DataStreamROSWS();

  virtual bool start(QStringList* selected_datasources) override;

  virtual void shutdown() override;

  virtual bool isRunning() const override;

  virtual ~DataStreamROSWS() override;

  virtual const char* name() const override
  {
    return "ROS Topic Subscriber WS";
  }

  virtual bool xmlSaveState(QDomDocument& doc, QDomElement& parent_element) const override;

  virtual bool xmlLoadState(const QDomElement& parent_element) override;

//  virtual void addActionsToParentMenu(QMenu* menu) override;


private slots:
    void onConnected();
    void onDisconnected();
    void handleWsMessage(const QString &message);


private:
//  void topicCallback(std::shared_ptr<WsClient::InMessage> in_message, const std::string& topic_name);

  void timerCallback();


//  void subscribe();

  void saveDefaultSettings();

  void loadDefaultSettings();

  bool _running;

  double _initial_time;

  std::string _prefix;

//  std::shared_ptr<RosbridgeWsClient> _ws;
  QWebSocket _ws;

//  std::map<std::string, RosbridgeWsSubscriber> _ws_subscribers;

  int _received_msg_count;

  QAction* _action_saveIntoRosbag;

  std::map<std::string, int> _msg_index;

  DialogSelectRosTopics::Configuration _config;

  QTimer* _periodic_timer;

  double _prev_clock_time;

private:
//  static void saveIntoRosbag(const PlotDataMapRef& data);
};

#endif  // DATALOAD_CSV_H
