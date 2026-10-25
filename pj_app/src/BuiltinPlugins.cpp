// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "BuiltinPlugins.h"

#ifdef PJ_WASM_ENABLE_INGRESS_PROBE
extern const PJ_data_source_vtable_t* pj_static_get_data_source_vtable_WasmIngressProbeSource() noexcept;
extern const PJ_dialog_vtable_t* pj_static_get_dialog_vtable_WasmIngressProbeDialog() noexcept;
#endif

#ifdef PJ_WASM_ENABLE_MCAP_PROBE_PARSER
extern const PJ_message_parser_vtable_t* pj_static_get_message_parser_vtable_WasmMcapProbeParser() noexcept;
#endif

#ifdef PJ_WASM_WITH_CSV_PLUGIN
extern const PJ_data_source_vtable_t* pj_static_get_data_source_vtable_CsvSource() noexcept;
extern const PJ_dialog_vtable_t* pj_static_get_dialog_vtable_CsvDialog() noexcept;
#endif

#ifdef PJ_WASM_WITH_MCAP_PLUGIN
extern const PJ_data_source_vtable_t* pj_static_get_data_source_vtable_McapSource() noexcept;
extern const PJ_dialog_vtable_t* pj_static_get_dialog_vtable_McapDialog() noexcept;
#endif

#ifdef PJ_WASM_WITH_ROS_PLUGIN
extern const PJ_message_parser_vtable_t* pj_static_get_message_parser_vtable_RosParserPlugin() noexcept;
extern const PJ_dialog_vtable_t* pj_static_get_dialog_vtable_RosParserDialog() noexcept;
#endif

#ifdef PJ_WASM_WITH_PROTOBUF_PLUGIN
extern const PJ_message_parser_vtable_t* pj_static_get_message_parser_vtable_ProtobufParser() noexcept;
extern const PJ_dialog_vtable_t* pj_static_get_dialog_vtable_ProtobufParserDialog() noexcept;
#endif

#ifdef PJ_WASM_WITH_TRANSFORM_EDITOR_PLUGIN
extern const PJ_toolbox_vtable_t* pj_static_get_toolbox_vtable_TransformEditorToolbox() noexcept;
extern const PJ_dialog_vtable_t* pj_static_get_dialog_vtable_TransformEditorDialog() noexcept;
#endif

#ifdef PJ_WASM_WITH_DUMMY_STREAM_PLUGIN
extern const PJ_data_source_vtable_t* pj_static_get_data_source_vtable_DummyStreamer() noexcept;
#endif

#ifdef PJ_WASM_WITH_PJ_BRIDGE_PLUGIN
extern const PJ_data_source_vtable_t* pj_static_get_data_source_vtable_PjBridgeSource() noexcept;
extern const PJ_dialog_vtable_t* pj_static_get_dialog_vtable_PjBridgeDialog() noexcept;
#endif

#ifdef PJ_WASM_WITH_JSON_PLUGIN
extern const PJ_message_parser_vtable_t* pj_static_get_message_parser_vtable_JsonParser() noexcept;
extern const PJ_dialog_vtable_t* pj_static_get_dialog_vtable_JsonParserDialog() noexcept;
#endif

#ifdef PJ_WASM_WITH_DATA_TAMER_PLUGIN
extern const PJ_message_parser_vtable_t* pj_static_get_message_parser_vtable_DataTamerParserPlugin() noexcept;
#endif

#ifdef PJ_WASM_WITH_3D_LOADER_PLUGIN
extern const PJ_data_source_vtable_t* pj_static_get_data_source_vtable_Load3dSource() noexcept;
#endif

#ifdef PJ_WASM_WITH_QUATERNION_PLUGIN
extern const PJ_toolbox_vtable_t* pj_static_get_toolbox_vtable_QuaternionToolbox() noexcept;
extern const PJ_dialog_vtable_t* pj_static_get_dialog_vtable_QuaternionDialog() noexcept;
#endif

#ifdef PJ_WASM_WITH_FFT_PLUGIN
extern const PJ_toolbox_vtable_t* pj_static_get_toolbox_vtable_FFTToolbox() noexcept;
extern const PJ_dialog_vtable_t* pj_static_get_dialog_vtable_FFTDialog() noexcept;
#endif

#ifdef PJ_WASM_WITH_ANOMALY_DETECTOR_PLUGIN
extern const PJ_toolbox_vtable_t* pj_static_get_toolbox_vtable_AnomalyDetectorToolbox() noexcept;
extern const PJ_dialog_vtable_t* pj_static_get_dialog_vtable_AnomalyDetectorDialog() noexcept;
#endif

namespace pj_app {

PJ::StaticPluginSet builtInPlugins() {
  PJ::StaticPluginSet plugins;
#ifdef PJ_WASM_ENABLE_INGRESS_PROBE
  plugins.data_sources.emplace_back(
      pj_static_get_data_source_vtable_WasmIngressProbeSource(), pj_static_get_dialog_vtable_WasmIngressProbeDialog());
#endif
#ifdef PJ_WASM_ENABLE_MCAP_PROBE_PARSER
  plugins.message_parsers.emplace_back(pj_static_get_message_parser_vtable_WasmMcapProbeParser());
#endif
#ifdef PJ_WASM_WITH_CSV_PLUGIN
  plugins.data_sources.emplace_back(
      pj_static_get_data_source_vtable_CsvSource(), pj_static_get_dialog_vtable_CsvDialog());
#endif
#ifdef PJ_WASM_WITH_MCAP_PLUGIN
  plugins.data_sources.emplace_back(
      pj_static_get_data_source_vtable_McapSource(), pj_static_get_dialog_vtable_McapDialog());
#endif
#ifdef PJ_WASM_WITH_ROS_PLUGIN
  plugins.message_parsers.emplace_back(
      pj_static_get_message_parser_vtable_RosParserPlugin(), pj_static_get_dialog_vtable_RosParserDialog());
#endif
#ifdef PJ_WASM_WITH_PROTOBUF_PLUGIN
  plugins.message_parsers.emplace_back(
      pj_static_get_message_parser_vtable_ProtobufParser(), pj_static_get_dialog_vtable_ProtobufParserDialog());
#endif
#ifdef PJ_WASM_WITH_TRANSFORM_EDITOR_PLUGIN
  plugins.toolboxes.emplace_back(
      pj_static_get_toolbox_vtable_TransformEditorToolbox(), pj_static_get_dialog_vtable_TransformEditorDialog());
#endif
#ifdef PJ_WASM_WITH_DUMMY_STREAM_PLUGIN
  plugins.data_sources.emplace_back(pj_static_get_data_source_vtable_DummyStreamer());
#endif
#ifdef PJ_WASM_WITH_PJ_BRIDGE_PLUGIN
  plugins.data_sources.emplace_back(
      pj_static_get_data_source_vtable_PjBridgeSource(), pj_static_get_dialog_vtable_PjBridgeDialog());
#endif
#ifdef PJ_WASM_WITH_JSON_PLUGIN
  plugins.message_parsers.emplace_back(
      pj_static_get_message_parser_vtable_JsonParser(), pj_static_get_dialog_vtable_JsonParserDialog());
#endif
#ifdef PJ_WASM_WITH_DATA_TAMER_PLUGIN
  plugins.message_parsers.emplace_back(pj_static_get_message_parser_vtable_DataTamerParserPlugin());
#endif
#ifdef PJ_WASM_WITH_3D_LOADER_PLUGIN
  plugins.data_sources.emplace_back(pj_static_get_data_source_vtable_Load3dSource());
#endif
#ifdef PJ_WASM_WITH_QUATERNION_PLUGIN
  plugins.toolboxes.emplace_back(
      pj_static_get_toolbox_vtable_QuaternionToolbox(), pj_static_get_dialog_vtable_QuaternionDialog());
#endif
#ifdef PJ_WASM_WITH_FFT_PLUGIN
  plugins.toolboxes.emplace_back(pj_static_get_toolbox_vtable_FFTToolbox(), pj_static_get_dialog_vtable_FFTDialog());
#endif
#ifdef PJ_WASM_WITH_ANOMALY_DETECTOR_PLUGIN
  plugins.toolboxes.emplace_back(
      pj_static_get_toolbox_vtable_AnomalyDetectorToolbox(), pj_static_get_dialog_vtable_AnomalyDetectorDialog());
#endif
  return plugins;
}

}  // namespace pj_app
