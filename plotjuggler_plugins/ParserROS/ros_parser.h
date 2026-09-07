#ifndef ROS_PARSER_H
#define ROS_PARSER_H

#include "fmt/core.h"
#include "PlotJuggler/messageparser_base.h"
#include "rosx_introspection/ros_parser.hpp"
#include "special_messages.h"

class ParserROS : public PJ::MessageParser
{
public:
  ParserROS(const std::string& topic_name, const std::string& type_name, const std::string& schema,
            RosMsgParser::Deserializer* deserializer, PJ::PlotDataMapRef& data,
            RosMsgParser::SchemaFormat schema_format = RosMsgParser::ROS_MSG);

  bool parseMessage(const PJ::MessageRef serialized_msg, double& timestamp) override;

  void setLargeArraysPolicy(bool clamp, unsigned max_size) override;

  void enableTruncationCheck(bool enable)
  {
    _strict_truncation_check = enable;
  }

  void setUseLegacyCdrAlignment(bool enable) override
  {
    // Only applies to NanoCDR deserializer (OMGIDL/ROS2)
    auto* cdr_deser = dynamic_cast<RosMsgParser::NanoCDR_Deserializer*>(_deserializer.get());
    if (cdr_deser)
    {
      cdr_deser->setAlignFromMessageStart(enable);
    }
  }

protected:
  RosMsgParser::Parser _parser;
  std::unique_ptr<RosMsgParser::Deserializer> _deserializer;
  RosMsgParser::FlatMessage _flat_msg;
  std::string _topic;

  PJ::Msg::Header readHeader(double& timestamp);
  void parseHeader(const std::string& prefix, double& timestamp);
  void parseEmpty(const std::string& prefix, double& timestamp);

  template <size_t N>
  void parseCovariance(const std::string& prefix, double& timestamp);

  void parseVector3(const std::string& prefix, double& timestamp);
  void parsePoint(const std::string& prefix, double& timestamp);
  void parseQuaternion(const std::string& prefix, double& timestamp);

  void parseTwist(const std::string& prefix, double& timestamp);
  void parseTwistWithCovariance(const std::string& prefix, double& timestamp);

  void parseTransform(const std::string& prefix, double& timestamp);
  void parseTransformStamped(const std::string& prefix, double& timestamp);

  void parsePose(const std::string& prefix, double& timestamp);
  void parsePoseStamped(const std::string& prefix, double& timestamp);
  void parsePoseWithCovariance(const std::string& prefix, double& timestamp);

  void parseImu(const std::string& prefix, double& timestamp);
  void parseOdometry(const std::string& prefix, double& timestamp);

  void parseDiagnosticMsg(const std::string& prefix, double& timestamp);
  void parseJointStateMsg(const std::string& prefix, double& timestamp);
  void parseTF2Msg(const std::string& prefix, double& timestamp);

  void parseDataTamerSchemas(const std::string& prefix, double& timestamp);
  void parseDataTamerSnapshot(const std::string& prefix, double& timestamp);

  void parsePalStatisticsNames(const std::string& prefix, double& timestamp);
  void parsePalStatisticsValues(const std::string& prefix, double& timestamp);

  void parseTSLDefinition(const std::string& prefix, double& timestamp);
  void parseTSLValues(const std::string& prefix, double& timestamp);
  void process_tsl_values(const std::string& prefix, const double& timestamp,
                          const std::vector<std::string>& definition,
                          const std::vector<double>& values);

  /// @brief Remove "/values" or "/names" suffix from a topic string
  std::string parsePalStatisticsPrefix(const std::string& in_prefix);

  std::function<void(const std::string& prefix, double&)> _customized_parser;

  bool _has_header = false;
  bool _strict_truncation_check = true;
};

template <size_t N>
inline void ParserROS::parseCovariance(const std::string& prefix, double& timestamp)
{
  std::array<double, N * N> cov;
  for (auto& val : cov)
  {
    val = _deserializer->deserialize(RosMsgParser::FLOAT64).convert<double>();
  }
  for (int i = 0; i < N; i++)
  {
    for (int j = i; j < N; j++)
    {
      const size_t index = i * N + j;
      getSeries(fmt::format("{}/[{};{}]", prefix, i, j)).pushBack({ timestamp, cov[index] });
    }
  }
}

#endif  // ROS_PARSER_H
