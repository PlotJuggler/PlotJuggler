#ifndef STATE_PUBLISHER_ROSTOPIC_H
#define STATE_PUBLISHER_ROSTOPIC_H

#include <QObject>
#include <QtPlugin>
#include <map>
#include <ros/ros.h>
#include <ros_type_introspection/ros_introspection.hpp>
#include <tf2_ros/transform_broadcaster.h>
#include <tf2_ros/static_transform_broadcaster.h>
#include "PlotJuggler/statepublisher_base.h"
#include "shape_shifter_factory.hpp"
#include <rosbag/bag.h>
#include "TopicPublisherROS/ws/imageview_dialog.h"

#include <websocketpp/config/asio_no_tls_client.hpp>
#include <websocketpp/client.hpp>

typedef websocketpp::client<websocketpp::config::asio_client> ws_client;
using websocketpp::connection_hdl;
using websocketpp::lib::placeholders::_1;
using websocketpp::lib::placeholders::_2;
using websocketpp::lib::bind;

class TopicPublisherROS : public StatePublisher {
Q_OBJECT
    Q_PLUGIN_METADATA(IID "com.icarustechnology.PlotJuggler.StatePublisher"
                          "../statepublisher.json")
    Q_INTERFACES(StatePublisher)

public:
    TopicPublisherROS();

    virtual ~TopicPublisherROS() override;

    virtual void updateState(double current_time) override;

    virtual const char *name() const override {
        return "ROS Topic Re-Publisher";
    }

    virtual bool enabled() const override {
        return _enabled;
    }

    void setParentMenu(QMenu *menu, QAction *action) override;

    virtual void play(double interval) override;

public slots:

    virtual void setEnabled(bool enabled) override;

    void filterDialog(bool autoconfirm);

private:
    void broadcastTF(double current_time);

    std::map<std::string, ros::Publisher> _publishers;
    std::map<std::string, std::string> _ws_advertisers;
    bool _ros_selected;
    bool _ws_selected;
    bool _ws_show_video;
    bool _enabled;
    ros::NodeHandlePtr _node;
    bool _publish_clock;

    void ws_connected(connection_hdl hdl);
    void ws_disconnected(connection_hdl hdl);

    void ws_thread();

    ws_client _ws;
    connection_hdl _ws_connection;
    std::thread *_ws_trd;
    std::string _ws_url;

    std::shared_ptr<tf2_ros::TransformBroadcaster> _tf_broadcaster;
    std::shared_ptr<tf2_ros::StaticTransformBroadcaster> _tf_static_broadcaster;

    ros::Publisher _clock_publisher;

    QAction *_enable_self_action;
    QAction *_select_topics_to_publish;

    std::unordered_map<std::string, bool> _topics_to_publish;
//    ImageViewDialog image_dialog;
    std::map<std::string, ImageViewDialog*> _img_dialog_map;

    bool toPublish(const std::string &topic_name);

    double previous_time;

    int _previous_play_index;

    void publishAnyMsg(const rosbag::MessageInstance &msg_instance);
};

#endif  // DATALOAD_CSV_H
