#include "statepublisher_rostopic.h"
#include "PlotJuggler/any.hpp"
#include "qnodedialog.h"

#include "ros_type_introspection/ros_introspection.hpp"
#include <QDialog>
#include <QCheckBox>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <rosbag/bag.h>
#include <std_msgs/Header.h>
#include <unordered_map>
#include <rosgraph_msgs/Clock.h>
#include <QMessageBox>
#include "publisher_select_dialog.h"
#include "QtCore/qjsondocument.h"
#include "../../../3rdparty/json/json.hpp"
#include <QtCore/QTimer>
#include <cmath>


// for convenience
using json = nlohmann::json;

void TopicPublisherROS::ws_thread(){
    using websocketpp::lib::bind;
    try {
        // Initialize ASIO
        _ws.init_asio();
        _ws.set_access_channels(websocketpp::log::alevel::none);
        _ws.clear_access_channels(websocketpp::log::alevel::none);
        _ws.set_error_channels(websocketpp::log::elevel::none);

        // Register our callbacks
        _ws.set_open_handler(bind(&TopicPublisherROS::ws_connected, this, ::_1));
        _ws.set_close_handler(bind(&TopicPublisherROS::ws_disconnected, this, ::_1));

        websocketpp::lib::error_code ec;
        ws_client::connection_ptr con = _ws.get_connection(_ws_url, ec);
        if (ec) {
            std::cout << "could not create connection because: " << ec.message() << std::endl;
        }

        _ws.connect(con);

        _ws.run();
    } catch (websocketpp::exception const & e) {
        std::cout << e.what() << std::endl;
    }
}

TopicPublisherROS::TopicPublisherROS() : _enabled(false), _ros_selected(true), _ws_selected(false), _ws_show_video(true),
_node(nullptr), _publish_clock(true), _ws_trd(nullptr) {
    QSettings settings;
    _publish_clock = settings.value("TopicPublisherROS/publish_clock", true).toBool();
}

TopicPublisherROS::~TopicPublisherROS() {
    _enabled = false;
}

void TopicPublisherROS::setParentMenu(QMenu *menu, QAction *action) {
    StatePublisher::setParentMenu(menu, action);

    _enable_self_action = menu->actions().back();

    _select_topics_to_publish = new QAction(QString("Select topics to be published"), _menu);
    _menu->addAction(_select_topics_to_publish);
    connect(_select_topics_to_publish, &QAction::triggered, this, &TopicPublisherROS::filterDialog);
}

void TopicPublisherROS::ws_connected(connection_hdl hdl) {
    _ws_connection = hdl;
    _enabled = true;
    StatePublisher::setEnabled(true);
    if(_clock_publisher){
        //TODO: advertise clock with ws
    } else {
        //TODO: disadvertise clock with ws
    }
}

void TopicPublisherROS::ws_disconnected(connection_hdl hdl) {
    _ws_connection.reset(); //TODO: check if causing trouble
    _enabled = false;
    _ws_advertisers.clear();
    _ws_url.clear();
    StatePublisher::setEnabled(false);
}

void TopicPublisherROS::setEnabled(bool to_enable) {
    if(_ros_selected){
        if (!_node && to_enable) {
            _node = RosManager::getNode();
        }
        _enabled = (to_enable && _node);

        if (_enabled) {
            filterDialog(true);

            if (!_tf_broadcaster) {
                _tf_broadcaster = std::make_unique<tf2_ros::TransformBroadcaster>();
            }
            if (!_tf_static_broadcaster) {
                _tf_static_broadcaster = std::make_unique<tf2_ros::StaticTransformBroadcaster>();
            }

            _previous_play_index = std::numeric_limits<int>::max();

            if (_publish_clock) {
                _clock_publisher = _node->advertise<rosgraph_msgs::Clock>("/clock", 10, true);
            } else {
                _clock_publisher.shutdown();
            }
        } else {
            _node.reset();
            _publishers.clear();
            _clock_publisher.shutdown();

            _tf_broadcaster.reset();
            _tf_static_broadcaster.reset();
        }
        StatePublisher::setEnabled(_enabled);
    }
    else if(_ws_selected){
        if(to_enable){
            _ws_trd = new std::thread(&TopicPublisherROS::ws_thread, this);
        } else {
            _ws.stop();
        }
    }
}

void TopicPublisherROS::filterDialog(bool autoconfirm) {
    auto all_topics = RosIntrospectionFactory::get().getTopicList();

    if (all_topics.empty()) {
        return;
    }

    auto *dialog = new PublisherSelectDialog();

    if(!_ws_url.empty()){
        QString qurl(_ws_url.c_str());
        dialog->ui()->lineEditWebsocketUrl->setText(qurl);
        dialog->ui()->radioButtonWS->setChecked(_ws_selected);
        dialog->ui()->radioButtonROS->setChecked(_ros_selected);
        dialog->ui()->checkBoxShowVideo->setChecked(_ws_show_video);
    }

    std::map<std::string, QCheckBox *> checkbox;

    for (const auto &topic : all_topics) {
        auto cb = new QCheckBox(dialog);
        auto filter_it = _topics_to_publish.find(*topic);
        if (filter_it == _topics_to_publish.end()) {
            cb->setChecked(true);
        } else {
            cb->setChecked(filter_it->second);
        }
        cb->setFocusPolicy(Qt::NoFocus);
        dialog->ui()->formLayout->addRow(new QLabel(QString::fromStdString(*topic)), cb);
        checkbox.insert(std::make_pair(*topic, cb));
        connect(dialog->ui()->pushButtonSelect, &QPushButton::pressed, [cb]() { cb->setChecked(true); });
        connect(dialog->ui()->pushButtonDeselect, &QPushButton::pressed, [cb]() { cb->setChecked(false); });
    }

    if (!autoconfirm) {
        dialog->exec();
    }

    if (autoconfirm || dialog->result() == QDialog::Accepted) {
        _topics_to_publish.clear();
        for (const auto &it : checkbox) {
            _topics_to_publish.insert({it.first, it.second->isChecked()});
        }

        // remove already created publisher if not needed anymore
        if(_ros_selected){
            for (auto it = _publishers.begin(); it != _publishers.end(); /* no increment */) {
                const std::string &topic_name = it->first;
                if (!toPublish(topic_name)) {
                    it = _publishers.erase(it);
                } else {
                    it++;
                }
            }
        }
        else if(_ws_selected){
            for (auto it = _ws_advertisers.begin(); it != _ws_advertisers.end(); /* no increment */) {
                const std::string &topic_name = it->first;
                if (!toPublish(topic_name)) {
                    if(_enabled){
                        json advertise_msg;
                        advertise_msg["op"] = "unadvertise";
                        advertise_msg["topic"] = it->first;
                        _ws.send(_ws_connection, advertise_msg.dump(), websocketpp::frame::opcode::text);
                    }
                    it = _ws_advertisers.erase(it);
                } else {
                    it++;
                }
            }
        }

        _publish_clock = dialog->ui()->radioButtonClock->isChecked();
        _ros_selected = dialog->ui()->radioButtonROS->isChecked();
        _ws_selected = dialog->ui()->radioButtonWS->isChecked();
        _ws_show_video = dialog->ui()->checkBoxShowVideo->isChecked();

        if(_ws_selected){
            _ws_url = dialog->ui()->lineEditWebsocketUrl->text().toStdString();
        }

        if (_enabled && _publish_clock) {
            if(_ros_selected){
                _clock_publisher = _node->advertise<rosgraph_msgs::Clock>("/clock", 10, true);
            }
            else if(_ws_selected){
                //TODO: advertise clock with ws
            }
        } else {
            if(_ros_selected){
                _clock_publisher.shutdown();
            }
            else if(_ws_selected){
                //TODO: disadvertise clock with ws
            }
        }

        dialog->deleteLater();

        QSettings settings;
        settings.setValue("TopicPublisherROS/publish_clock", _publish_clock);
    }
}

void TopicPublisherROS::broadcastTF(double current_time) {
    using StringPair = std::pair<std::string, std::string>;

    std::map<StringPair, geometry_msgs::TransformStamped> transforms;
    std::map<StringPair, geometry_msgs::TransformStamped> static_transforms;

    for (const auto &data_it : _datamap->user_defined) {
        const std::string &topic_name = data_it.first;
        const PlotDataAny &plot_any = data_it.second;

        if (!toPublish(topic_name)) {
            continue;  // Not selected
        }

        if (topic_name != "/tf_static" && topic_name != "/tf") {
            continue;
        }

        const PlotDataAny *tf_data = &plot_any;
        int last_index = tf_data->getIndexFromX(current_time);
        if (last_index < 0) {
            continue;
        }

        auto transforms_ptr = (topic_name == "/tf_static") ? &static_transforms : &transforms;

        std::vector<uint8_t> raw_buffer;
        // 2 seconds in the past (to be configurable in the future)
        int initial_index = tf_data->getIndexFromX(current_time - 2.0);

        if (_previous_play_index < last_index && _previous_play_index > initial_index) {
            initial_index = _previous_play_index;
        }

        for (size_t index = std::max(0, initial_index); index <= last_index; index++) {
            const nonstd::any &any_value = tf_data->at(index).y;

            const bool isRosbagMessage = any_value.type() == typeid(rosbag::MessageInstance);

            if (!isRosbagMessage) {
                continue;
            }

            const auto &msg_instance = nonstd::any_cast<rosbag::MessageInstance>(any_value);

            raw_buffer.resize(msg_instance.size());
            ros::serialization::OStream ostream(raw_buffer.data(), raw_buffer.size());
            msg_instance.write(ostream);

            tf2_msgs::TFMessage tf_msg;
            ros::serialization::IStream istream(raw_buffer.data(), raw_buffer.size());
            ros::serialization::deserialize(istream, tf_msg);

            for (const auto &stamped_transform : tf_msg.transforms) {
                const auto &parent_id = stamped_transform.header.frame_id;
                const auto &child_id = stamped_transform.child_frame_id;
                StringPair trans_id = std::make_pair(parent_id, child_id);
                auto it = transforms_ptr->find(trans_id);
                if (it == transforms_ptr->end()) {
                    transforms_ptr->insert({trans_id, stamped_transform});
                } else if (it->second.header.stamp <= stamped_transform.header.stamp) {
                    it->second = stamped_transform;
                }
            }
        }
        // ready to broadacast
        std::vector<geometry_msgs::TransformStamped> transforms_vector;
        transforms_vector.reserve(transforms_ptr->size());

        if(_ros_selected){
            auto now = ros::Time::now().toSec();
            for (auto &trans : *transforms_ptr) {
                if (!_publish_clock) {
                    trans.second.header.stamp.fromSec(now);
                }
                transforms_vector.emplace_back(std::move(trans.second));
            }
            if (transforms_vector.size() > 0) {
                if (transforms_ptr == &transforms) {
                    _tf_broadcaster->sendTransform(transforms_vector);
                } else {
                    _tf_static_broadcaster->sendTransform(transforms_vector);
                }
            }
        }
    }
}

bool TopicPublisherROS::toPublish(const std::string &topic_name) {
    auto it = _topics_to_publish.find(topic_name);
    if (it == _topics_to_publish.end()) {
        return false;
    } else {
        return it->second;
    }
}

//template<class T>
template<typename LeafValueType>
void addLeaf(json *obj, const std::string& path, LeafValueType value, bool is_blob= false){
    auto tail = path.substr(1, path.size() - 1); // remove first /
    if(tail.find('/') != std::string::npos){
        auto current = tail.substr(0, tail.find('/'));
        tail.erase(0, current.size());
        if(current.find('.') != std::string::npos){
            auto name = current.substr(0, current.find('.'));
            auto pos = std::stoi(current.substr(current.find('.') + 1, current.size() - 1),nullptr,0);
            addLeaf(&(*obj)[name.c_str()][pos], tail, value);
        }
        else {
            addLeaf(&(*obj)[current.c_str()], tail, value);
        }
    } else {
        if(is_blob){ // check if type is blob
            auto name = tail.substr(0, tail.find('.'));
            (*obj)[name] = value;
        }
        else if(tail.find('.') != std::string::npos){
            auto name = tail.substr(0, tail.find('.'));
            auto pos = std::stoi(tail.substr(tail.find('.') + 1, tail.size() - 1),nullptr,0);
            (*obj)[name.c_str()][pos] = value;
        }
        else {
            (*obj)[tail.c_str()] = value;
        }
    }
}

int nthOccurrence(const std::string& str, const std::string& findMe, int nth)
{
    size_t  pos = 0;
    int     cnt = 0;

    while( cnt != nth )
    {
        pos+=1;
        pos = str.find(findMe, pos);
        if ( pos == std::string::npos )
            return -1;
        cnt++;
    }
    return pos;
}

void castValue(json *msg, const std::string& path, const RosIntrospection::Variant& value){
    using namespace RosIntrospection;
    switch (value.getTypeID()) {
        case BOOL:
            if (value.convert<int>()) addLeaf(msg, path, true);
            else addLeaf(msg, path, false);
            break;
        case BYTE:
            addLeaf(msg, path, value.convert<uint8_t>());
            break;
        case INT8:
            addLeaf(msg, path, value.convert<int8_t>());
            break;
        case CHAR:
            addLeaf(msg, path, value.convert<uint8_t>());
            break;
        case UINT8:
            addLeaf(msg, path, value.convert<uint8_t>());
            break;
        case UINT16:
            addLeaf(msg, path, value.convert<uint16_t>());
            break;
        case INT16:
            addLeaf(msg, path, value.convert<int16_t>());
            break;
        case UINT32:
            addLeaf(msg, path, value.convert<uint32_t>());
            break;
        case INT32:
            addLeaf(msg, path, value.convert<int32_t>());
            break;
        case FLOAT32:
            if (!std::isnan(value.convert<float>())) addLeaf(msg, path, value.convert<float>());
            break;
        case UINT64:
            addLeaf(msg, path, value.convert<uint64_t>());
            break;
        case INT64:
            addLeaf(msg, path, value.convert<int64_t>());
            break;
        case FLOAT64:
        case TIME:
        case DURATION:
        default:
            if (!std::isnan(value.convert<double>())) addLeaf(msg, path, value.convert<double>());
            break;
    }
}

json messageToJson(const rosbag::MessageInstance &msg_instance, bool publish_clock){
    using namespace RosIntrospection;

    const auto &topic_name = msg_instance.getTopic();

    std::vector<uint8_t> raw_buffer;
    raw_buffer.resize(msg_instance.size());
    ros::serialization::OStream ostream(raw_buffer.data(), raw_buffer.size());
    msg_instance.write(ostream);

    Parser parser;
    const std::string&  datatype   =  msg_instance.getDataType();
    const std::string&  definition =  msg_instance.getMessageDefinition();

    // don't worry if you do this more than once
    parser.registerMessageDefinition( topic_name,
                                      RosIntrospection::ROSType(datatype),
                                      definition );

    FlatMessage   flat_container;
    RenamedValues renamed_value;
    parser.deserializeIntoFlatContainer( topic_name,
                                         Span<uint8_t>(raw_buffer),
                                         &flat_container,
                                         100);

    parser.applyNameTransform( topic_name, flat_container, &renamed_value );

    json msg;
    msg["op"] = "publish";
    msg["topic"] = topic_name;


    for (const auto& it: renamed_value)
    {
        auto key = it.first;
        const Variant& value   = it.second;

        key = key.substr(topic_name.length(), key.length() -1 );

        if(!publish_clock){
            if(key.find("header") != std::string::npos){
                auto header = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
                if(key.find("stamp") != std::string::npos) {
                    addLeaf(&msg["msg"], key, header);
                }
                else {
                    castValue(&msg["msg"], key, value);
                }
            }
            else {
                castValue(&msg["msg"], key, value);
            }
        } else {
            castValue(&msg["msg"], key, value);
        }
    }
    for (const auto& it: flat_container.name)
    {
        auto key    = it.first.toStdString();
        const std::string& value  = it.second;

        key = key.substr(topic_name.length(), key.length() -1 );

        addLeaf(&msg["msg"], key, value);
    }
    for (const auto& it: flat_container.blob)
    {
        auto key    = it.first.toStdString();
        auto value  = it.second;

        key = key.substr(topic_name.length(), key.length() -1 );

        addLeaf(&msg["msg"], key, value, true);
    }

    return msg;
}

void TopicPublisherROS::publishAnyMsg(const rosbag::MessageInstance &msg_instance) {
    if(_ws_selected){
        if(_ws_show_video){
            if(msg_instance.getDataType() == "sensor_msgs/Image"){
                auto img_dialog = _img_dialog_map.find(msg_instance.getTopic());
                if(img_dialog == _img_dialog_map.end()){
                    _img_dialog_map.insert({msg_instance.getTopic().c_str(), new ImageViewDialog()});
                    img_dialog = _img_dialog_map.find(msg_instance.getTopic());
                }
                img_dialog->second->update_frame(msg_instance);
            }
            else {
                auto msg = messageToJson(msg_instance, _publish_clock);
                if(_enabled){
                    _ws.send(_ws_connection, msg.dump(), websocketpp::frame::opcode::text);
                }
            }
        } else {
            auto msg = messageToJson(msg_instance, _publish_clock);
            if(_enabled){
                _ws.send(_ws_connection, msg.dump(), websocketpp::frame::opcode::text);
            }
        }
    }
    else if(_ros_selected){
        using namespace RosIntrospection;

        const auto &topic_name = msg_instance.getTopic();

        RosIntrospection::ShapeShifter *shapeshifted = RosIntrospectionFactory::get().getShapeShifter(topic_name);

        if (!shapeshifted) {
            return;  // Not registered, just skip
        }

        std::vector<uint8_t> raw_buffer;
        raw_buffer.resize(msg_instance.size());
        ros::serialization::OStream ostream(raw_buffer.data(), raw_buffer.size());
        msg_instance.write(ostream);

        if (!_publish_clock) {
            const ROSMessageInfo *msg_info = RosIntrospectionFactory::parser().getMessageInfo(topic_name);
            if (msg_info && msg_info->message_tree.croot()->children().size() >= 1) {
                const auto &first_field = msg_info->message_tree.croot()->child(0)->value();
                if (first_field->type().baseName() == "std_msgs/Header") {
                    std_msgs::Header msg;
                    ros::serialization::IStream is(raw_buffer.data(), raw_buffer.size());
                    ros::serialization::deserialize(is, msg);
                    msg.stamp = ros::Time::now();
                    ros::serialization::OStream os(raw_buffer.data(), raw_buffer.size());
                    ros::serialization::serialize(os, msg);
                }
            }
        }

        ros::serialization::IStream istream(raw_buffer.data(), raw_buffer.size());
        shapeshifted->read(istream);

        auto publisher_it = _publishers.find(topic_name);
        if (publisher_it == _publishers.end()) {
            auto res = _publishers.insert({topic_name, shapeshifted->advertise(*_node, topic_name, 10, true)});
            publisher_it = res.first;
        }

        const ros::Publisher &publisher = publisher_it->second;
        publisher.publish(*shapeshifted);
    }
}

void TopicPublisherROS::updateState(double current_time) {
    if(!_enabled){
        if(_ros_selected) {
            if(!_node) return;
            if (!ros::master::check()) {
                QMessageBox::warning(nullptr, tr("Disconnected!"),
                                     "The roscore master cannot be detected.\n"
                                     "The publisher will be disabled.");
                _enable_self_action->setChecked(false);
                return;
            }
        }
        else if(_ws_selected){
            if(!_enabled){
                return;
            }
        }
    }


    //-----------------------------------------------
    broadcastTF(current_time);
    //-----------------------------------------------

    auto data_it = _datamap->user_defined.find("__consecutive_message_instances__");
    if (data_it != _datamap->user_defined.end()) {
        const PlotDataAny &continuous_msgs = data_it->second;
        _previous_play_index = continuous_msgs.getIndexFromX(current_time);
        // qDebug() << QString("u: %1").arg( current_index ).arg(current_time, 0, 'f', 4 );
    }

    for (const auto &data_it : _datamap->user_defined) {
        const std::string &topic_name = data_it.first;
        const PlotDataAny &plot_any = data_it.second;
        if (!toPublish(topic_name)) {
            continue;  // Not selected
        }

        const RosIntrospection::ShapeShifter *shapeshifter = RosIntrospectionFactory::get().getShapeShifter(topic_name);

        if (shapeshifter->getDataType() == "tf/tfMessage" || shapeshifter->getDataType() == "tf2_msgs/TFMessage") {
            continue;
        }

        int last_index = plot_any.getIndexFromX(current_time);
        if (last_index < 0) {
            continue;
        }

        const auto &any_value = plot_any.at(last_index).y;

        if (any_value.type() == typeid(rosbag::MessageInstance)) {
            const auto &msg_instance = nonstd::any_cast<rosbag::MessageInstance>(any_value);
            if(_ws_selected){
                auto it = _ws_advertisers.find(msg_instance.getTopic().c_str());
                if(it == _ws_advertisers.end()){
                    if(_ws_show_video) { // advertise only whats not a video topic
                        if (!(msg_instance.getDataType() == "sensor_msgs/Image")) {
                            _ws_advertisers.insert({msg_instance.getTopic(), msg_instance.getDataType()});
                            json advertise_msg;
                            advertise_msg["op"] = "advertise";
                            advertise_msg["topic"] = msg_instance.getTopic();
                            advertise_msg["type"] = msg_instance.getDataType();
                            _ws.send(_ws_connection, advertise_msg.dump(), websocketpp::frame::opcode::text);
                        }
                    }
                     else { // advertise all topics to ws
                        _ws_advertisers.insert({msg_instance.getTopic(), msg_instance.getDataType()});
                        json advertise_msg;
                        advertise_msg["op"] = "advertise";
                        advertise_msg["topic"] = msg_instance.getTopic();
                        advertise_msg["type"] = msg_instance.getDataType();
                        _ws.send(_ws_connection, advertise_msg.dump(), websocketpp::frame::opcode::text);

                    }
                }
            }
            publishAnyMsg(msg_instance);
        }
    }

    if (_publish_clock) {
        rosgraph_msgs::Clock clock;
        try {
            clock.clock.fromSec(current_time);
            if(_ros_selected){
                _clock_publisher.publish(clock);
            }
            else if(_ws_selected){
                //TODO: publish clock with ws
            }
        }
        catch (...) {
            qDebug() << "error: " << current_time;
        }
    }
}

void TopicPublisherROS::play(double current_time) {
    if(!_enabled){
        if(_ros_selected) {
            if(!_node) return;
            if (!ros::master::check()) {
                QMessageBox::warning(nullptr, tr("Disconnected!"),
                                     "The roscore master cannot be detected.\n"
                                     "The publisher will be disabled.");
                _enable_self_action->setChecked(false);
                return;
            }
        }
        else if(_ws_selected){
            if(!_enabled){
                return;
            }
        }
    }

    auto data_it = _datamap->user_defined.find("__consecutive_message_instances__");
    if (data_it == _datamap->user_defined.end()) {
        return;
    }
    const PlotDataAny &continuous_msgs = data_it->second;
    int current_index = continuous_msgs.getIndexFromX(current_time);

    if (_previous_play_index > current_index) {
        _previous_play_index = current_index;
        updateState(current_time);
        return;
    } else {
        const PlotDataAny &consecutive_msg = data_it->second;
        for (int index = _previous_play_index + 1; index <= current_index; index++) {
            const auto &any_value = consecutive_msg.at(index).y;
            if (any_value.type() == typeid(rosbag::MessageInstance)) {
                const auto &msg_instance = nonstd::any_cast<rosbag::MessageInstance>(any_value);

                if (!toPublish(msg_instance.getTopic())) {
                    continue;  // Not selected
                }

                // qDebug() << QString("p: %1").arg( index );
                if(_ws_selected){
                    auto it = _ws_advertisers.find(msg_instance.getTopic().c_str());
                    if(it == _ws_advertisers.end()){
                        _ws_advertisers.insert({msg_instance.getTopic(), msg_instance.getDataType()});
                        json advertise_msg;
                        advertise_msg["op"] = "advertise";
                        advertise_msg["topic"] = msg_instance.getTopic();
                        advertise_msg["type"] = msg_instance.getDataType();
                        _ws.send(_ws_connection, advertise_msg.dump(), websocketpp::frame::opcode::text);

                    }
                }
                publishAnyMsg(msg_instance);

                if (_publish_clock) {
                    rosgraph_msgs::Clock clock;
                    clock.clock = msg_instance.getTime();
                    if(_ros_selected){
                        _clock_publisher.publish(clock);
                    }
                    else if(_ws_selected){
                        //TODO: publish clock with ws
                    }
                }
            }
        }
        _previous_play_index = current_index;
    }
}
