# Formats and the plugins they need

PlotJuggler has no built-in loader: every file format and live source is a plugin, and so is every message decoder.
A file whose payloads are encoded messages (ROS, Protobuf, JSON...) needs its loader **and** the parser for that
encoding. `about` (desktop) lists what is loaded, installed and installable; `get_status` lists the stream sources.

## Picking a loader

- File → Open Data File..., a drop onto the window, `open_file` and the command line all pick the loader by the
  file's **last extension**, lowercased, and take the first installed loader registering it. A file with no
  extension fails. "No DataSource plugin handles .xyz files" means no installed loader takes that extension: install
  one from the Marketplace.
- The Open dialog's filter lists only the extensions of the installed loaders.
- Generic extensions belong to one loader: `.log` and `.txt` go to the candump loader, `.dat` to MF4, `.yaml` to MCAP
  (a rosbag2 `metadata.yaml`), `.json` to LeRobot when it is installed. A plain text log or JSON file opened this way
  goes to that loader and fails or loads oddly.
- Layouts (`.pj4.xml`) are opened with File → Load Layout..., not as data; a layout dropped onto the window is sent
  to the data loaders and fails.

## Parsers match by encoding string

A parser registers encoding names; a loader or streamer binds each topic to the parser registering its encoding,
exactly. With no matching parser the topic is skipped ("N channel(s) skipped (no parser)" for MCAP). UDP, MQTT and
ZeroMQ dialogs let the user pick one encoding from those the installed parsers register.

| Parser (Marketplace name) | Encodings | Decodes |
|---|---|---|
| ROS CDR Parser (`ros-parser`) | `ros2msg`, `omgidl`, `ros2idl`, `ros1msg`, `ros1`, `ros2` | ROS 1 and ROS 2 messages; images, compressed images, PointCloud2, occupancy grids, grid maps, TF, markers as scene objects |
| Protobuf Parser (`protobuf-parser`) | `protobuf` | Protobuf via the file descriptor set; Foxglove schemas (images, point clouds, laser scans, grids, transforms, scene entities, logs, compressed video) as scene objects |
| JSON Parser (`json-parser`) | `json`, `cbor`, `msgpack`, `bson` | nested objects flattened with `/`, arrays as `[i]`, numbers as double |
| DataTamer Parser (`data-tamer-parser`) | `data_tamer` | DataTamer snapshots |
| InfluxDB Line Protocol Parser (`influx-parser`) | `influx` | line protocol; series `measurement/tag=value/.../field` |
| Arrow IPC Parser (`arrow-parser`) | `arrow-ipc` | one Arrow IPC stream per message; Mosaico data |
| NavMap Parser (`navmap-parser`) | `ros2msg` (NavMap types only) | EasyNavigation NavMap meshes for the 3D scene |

## Files

| Format | Extensions | Loader (Marketplace name) | Also needs | Browser |
|---|---|---|---|---|
| CSV | `.csv` | CSV Loader (`csv-loader`) | nothing | yes |
| MCAP, rosbag2 (MCAP storage) | `.mcap`, `.yaml` | MCAP Loader (`mcap-loader`) | the parser of each channel's encoding, below | yes |
| Parquet | `.parquet` | Parquet Loader (`parquet-loader`) | nothing | yes |
| PX4 ULog | `.ulg` | ULog Loader (`ulog-loader`) | nothing | yes |
| Vector BLF (CAN) | `.blf` | BLF Loader (`blf-loader`) | a DBC per channel, chosen in its dialog | yes |
| candump (CAN) | `.candump`, `.log`, `.txt` | candump Loader (`candump-loader`) | a DBC or dictionary per interface | yes |
| ASAM MF4 / MDF | `.mf4`, `.mdf`, `.dat` | MF4/MDF Loader (`mf4-loader`) | optional DBC | yes |
| Point cloud / mesh | `.ply`, `.pcd` | 3D Loader (PLY/PCD) (`pointcloud-3d-loader`) | nothing; one static object at time 0 | yes |
| MP4 video | `.mp4` | MP4 Loader (`mp4-loader`) | Protobuf Parser | no |
| LeRobot dataset | `.json` (pick `meta/info.json`) | LeRobot Loader (`lerobot-loader`) | Protobuf Parser for its videos | no |

**MCAP channels → parser:**

| Channel content | Parser |
|---|---|
| ROS 2 (message encoding `cdr`, schema `ros2msg` / `ros2idl` / `omgidl`) | ROS CDR Parser |
| ROS 1 (`ros1`) | ROS CDR Parser |
| Protobuf, Foxglove schemas included | Protobuf Parser |
| JSON (schemaless included) | JSON Parser |
| DataTamer MCAP sink (`data_tamer`) | DataTamer Parser |
| NavMap (`navmap_ros_interfaces/msg/NavMap`, `NavMapLayer`) | NavMap Parser, for the 3D scene |

A rosbag2 folder opens through its `metadata.yaml`, every split as one dataset; only MCAP storage is accepted (a
`.db3` bag must be converted with `ros2 bag convert` first). Transform channels (`tf2_msgs/msg/TFMessage`,
`foxglove.FrameTransform`) are always loaded.

## Live sources

| Source (Marketplace name) | Connects to | Needs | Browser |
|---|---|---|---|
| Dummy Streamer (`dummy-streamer`) | generated sine, sawtooth and noise topics | nothing | yes |
| PlotJuggler Bridge (`plotjuggler-bridge`) | a PlotJuggler Bridge server over WebSocket (`ws://`, `wss://`); topics subscribed on demand | the parser of each topic's encoding (JSON Parser, ROS CDR Parser) | yes |
| Foxglove Bridge (`foxglove-bridge`) | a Foxglove WebSocket server | ROS CDR Parser for ROS channels, Protobuf Parser, JSON Parser; flatbuffer and ROS 1 channels are unsupported | yes |
| ROS 2 Topic Subscriber (`ros2-topic-subscriber`) | ROS 2 topics through the user's sourced ROS install; Linux only | ROS CDR Parser | no |
| MQTT Subscriber (`mqtt-subscriber`) | an MQTT broker over `tcp://` or `ssl://` | the parser of the encoding chosen in its dialog | no |
| ZMQ Subscriber (`zmq-subscriber`) | a ZeroMQ PUB socket | as MQTT | no |
| UDP Server (`udp-server`) | UDP unicast or multicast | as MQTT; `influx` for Telegraf | no |
| Lab Streaming Layer (`lsl-streaming`) | LSL inlets | nothing | no |
| WebRTC Video Client (`webrtc-client`) | a WHEP server (H.264 video) | Protobuf Parser | no |

Cloud: the Mosaico Cloud Server toolbox (`toolbox-mosaico`, Sources panel → Cloud) needs the Arrow IPC Parser for
both its scalar and its object topics; desktop only. See [ui_tasks/mosaico.md](./ui_tasks/mosaico.md).

## Desktop installs

The installers seed a curated set of plugins into the user's extensions folder; the rest come from File →
Extensions Marketplace…. Not bundled, Marketplace only: LeRobot Loader, MQTT Subscriber, ZMQ Subscriber, UDP Server,
Lab Streaming Layer, Basic Widgets. The ROS 2 Topic Subscriber and the Assistant Agent ship only in the Linux
AppImage. A plugin installed or enabled takes effect after a restart.

## Browser (app.plotjuggler.io)

- A fixed set of plugins is built in, at versions pinned when the browser app was built, which can lag the
  Marketplace. There is no Marketplace and no third-party plugin.
- Loaders: CSV, MCAP, Parquet, ULog, candump, BLF, MF4, PLY/PCD. Parsers: all seven above. Streams: Dummy, PlotJuggler
  Bridge, Foxglove Bridge. Toolboxes: Transform Editor, Quaternion to RPY, Fast Fourier Transform, CSV/Parquet
  Exporter, Marker Editor. 2D and 3D scenes, video decoding and Python object scripts are in.
- Not in the browser: MP4, LeRobot, ROS 2, MQTT, ZeroMQ, UDP, LSL, WebRTC, Mosaico, the Assistant, Custom Widget
  plugins, Record, the source cache, Python filters.
- A page on `https://` can open only `wss://` bridges: a robot's `ws://` bridge needs TLS in front of it
  ([`docs/WASM_HOSTING.md`](../WASM_HOSTING.md)). The browser reports a failed connection without its cause.
- A file picked with Open Data File is read lazily from disk and must stay unchanged while open; a dropped file is
  copied into the page's memory. Opened data lives in the tab: closing or reloading it loses the data.
- An agent reaches the browser app through the `plotjuggler-agent` relay, which hands files to the page; `open_file`
  there is capped by `kMaxPresetDataBytes` (`pj_runtime/include/pj_runtime/PresetCatalog.h`).

Details: [`docs/WASM_DEPLOYMENT.md`](../WASM_DEPLOYMENT.md).
