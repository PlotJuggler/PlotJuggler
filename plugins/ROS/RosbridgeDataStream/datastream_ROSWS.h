#ifndef DATASTREAM_ROS_TOPIC_H
#define DATASTREAM_ROS_TOPIC_H

#include <QtPlugin>
#include <QAction>
#include <QTimer>
#include <thread>
#include "PlotJuggler/datastreamer_base.h"
#include "dialog_select_ros_topics.h"
#include <websocketpp/config/asio_no_tls_client.hpp>
#include <websocketpp/client.hpp>

typedef websocketpp::client<websocketpp::config::asio_client> ws_client;
using websocketpp::connection_hdl;
using websocketpp::lib::placeholders::_1;
using websocketpp::lib::placeholders::_2;
using websocketpp::lib::bind;

class DataStreamROSWS : public DataStreamer {
Q_OBJECT
    Q_PLUGIN_METADATA(IID "com.icarustechnology.PlotJuggler.DataStreamer"
                          "../datastreamer.json")
    Q_INTERFACES(DataStreamer)

public:
    DataStreamROSWS();

    virtual bool start(QStringList *selected_datasources) override;

    virtual void shutdown() override;

    virtual bool isRunning() const override;

    virtual ~DataStreamROSWS() override;

    virtual const char *name() const override {
        return "ROS Topic Subscriber WS";
    }

    virtual bool xmlSaveState(QDomDocument &doc, QDomElement &parent_element) const override;

    virtual bool xmlLoadState(const QDomElement &parent_element) override;

//  virtual void addActionsToParentMenu(QMenu* menu) override;


private slots:

    void onConnected(connection_hdl hdl);

    void onDisconnected(connection_hdl hdl);

    void handleWsMessage(connection_hdl hdl, ws_client::message_ptr msg);


private:
    void subscribe();

    void saveDefaultSettings();

    void loadDefaultSettings();

    void ws_thread();

    bool _running;

    double _initial_time;

    std::string _prefix;

    bool _connected;

    ws_client _ws;
    connection_hdl _ws_connection;
    std::thread *_ws_trd;
    std::string _ws_url;


    std::vector<std::pair<QString, QString>> _all_topics;

    bool _fetched_topics;

//  QAction* _action_saveIntoRosbag;

    std::map<std::string, int> _msg_index;

    DialogSelectRosTopics::Configuration _config;

    double _prev_clock_time;

};

#endif  // DATASTREAM_ROS_TOPIC_H
