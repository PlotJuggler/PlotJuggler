# Mosaico canonical object download and rendering path

This note explains how the Mosaico cloud download path can present rich objects
without using `parser_ingest`, and how those downloaded objects are decoded,
decompressed, and visualised in PlotJuggler.

The important idea is this:

```text
Mosaico server typed object data
  -> Mosaico toolbox maps it to PJ canonical SDK structs
  -> toolbox serializes those structs with pj_base canonical codecs
  -> toolbox pushes canonical bytes into ObjectStore
  -> viewer deserializes canonical bytes directly, with no MessageParser
  -> viewer renders the canonical object
```

`parser_ingest` is only needed when the producer gives the host raw source
messages, for example ROS CDR or protobuf payloads, and asks PlotJuggler to run a
`MessageParser` over them. The Mosaico path described here does not do that. It
downloads typed Mosaico records, converts them to PlotJuggler canonical objects
in the toolbox, and then writes those canonical object bytes straight into the
host `ObjectStore`.

## 1. Two different ingest models

There are two object ingest models in this codebase.

### Parser-backed ingest

Parser-backed ingest is used by file or stream sources that still carry a
source-specific wire format.

Example:

```text
ROS / CDR / protobuf bytes
  -> parser_ingest / MessageParser
  -> sdk::PointCloud, sdk::Image, sdk::FrameTransforms, ...
  -> ObjectStore entry
```

In this path, the `ObjectStore` entry often stores the original source message
bytes, and a render-time parser binding exists for the object topic. Consumers
ask `SessionManager::parserBindingForObjectTopic(...)` for that parser binding,
then call `parseLocked(...)`.

Relevant code:

- `pj_scene3D/widgets/include/pj_scene3d_widgets/parse_locked.h`
- `pj_runtime/src/DataSourceRuntimeHost.cpp`, `cbEnsureParserBinding(...)`
- `pj_runtime/src/DataSourceRuntimeHost.cpp`, `cbPushMessage(...)`

### Parser-less canonical object ingest

The Mosaico download path is the other model.

Example:

```text
Mosaico point_cloud2 / pose / motion_state ontology
  -> toolbox builds an SDK canonical object
  -> serializePointCloud / serializePosesInFrame / serializeFrameTransforms / ...
  -> ObjectStore entry stores that serialized canonical blob
```

There is no `MessageParser` registered for these topics. That is deliberate.
The bytes in the store are already a PlotJuggler canonical object wire blob, not
a ROS/CDR/protobuf source message.

Relevant code:

- `plotjuggler_sdk/pj_base/include/pj_base/sdk/plugin_data_api.hpp`,
  `ToolboxHostView::registerObjectTopic(...)`
- `plotjuggler_sdk/pj_base/include/pj_base/sdk/plugin_data_api.hpp`,
  `ToolboxHostView::pushOwnedObject(...)`
- `pj_datastore/src/plugin_data_host.cpp`, `toolboxRegisterObjectTopic(...)`
- `pj_datastore/src/plugin_data_host.cpp`, `toolboxPushOwnedObject(...)`

## 2. What is stored in Mosaico and what PJ receives

The Mosaico server implementation is not in this worktree, so the exact database
tables or cloud API DTOs are outside this repository. What this worktree does
show is the client-side contract: the Mosaico toolbox treats the server output as
typed object ontologies such as:

- `point_cloud2`
- `pose`
- `motion_state`
- image/depth payloads, including `serialization_format=image`

At the boundary with PlotJuggler, those server ontologies are transformed into
canonical `pj_base` builtins:

| Mosaico-level data | PJ canonical object |
| --- | --- |
| point cloud records | `PJ::sdk::PointCloud` |
| compressed point cloud records | `PJ::sdk::CompressedPointCloud` |
| pose arrays / object poses | `PJ::sdk::PosesInFrame` |
| frame relationships / motion-state transforms | `PJ::sdk::FrameTransforms` |
| raster images and depth images | `PJ::sdk::Image` |
| markers / scene primitives, when applicable | `PJ::sdk::SceneEntities` |

The PJ canonical objects are defined by the SDK in `plotjuggler_sdk/pj_base`.
Their wire format is protobuf-based and documented in:

- `plotjuggler_sdk/pj_base/proto/pj/README.md`
- `plotjuggler_sdk/pj_base/proto/pj/Image.proto`
- `plotjuggler_sdk/pj_base/proto/pj/PointCloud.proto`
- `plotjuggler_sdk/pj_base/proto/pj/CompressedPointCloud.proto`
- `plotjuggler_sdk/pj_base/proto/pj/FrameTransforms.proto`
- `plotjuggler_sdk/pj_base/proto/pj/PosesInFrame.proto`

For example, a canonical `PointCloud` contains:

- `timestamp`
- `width`
- `height`
- `point_step`
- `row_step`
- `is_bigendian`
- `is_dense`
- `fields`: channel descriptors such as `x`, `y`, `z`, `intensity`, `rgba`
- `data`: packed point bytes
- `frame_id`

A canonical `PosesInFrame` contains:

- `timestamp`
- `frame_id`
- repeated `Pose` values

A canonical `FrameTransforms` contains repeated transform edges:

- transform timestamp
- parent frame id
- child frame id
- translation
- rotation

The critical point is that by the time the payload is pushed into PlotJuggler,
the source-specific Mosaico ontology has already been normalised into one of
these PJ canonical shapes.

## 3. ObjectStore layout

`ObjectStore` stores timestamped opaque object payloads. It does not parse them.
It only knows:

- dataset id
- object topic id
- topic name
- `metadata_json`
- timestamp
- payload bytes
- optional lazy fetch closure / ownership anchor

See:

- `pj_datastore/include/pj_datastore/object_store.hpp`
- `pj_datastore/src/object_store.cpp`

An object topic descriptor looks like this:

```cpp
struct ObjectTopicDescriptor {
  DatasetId dataset_id;
  std::string topic_name;
  std::string metadata_json;
};
```

For parser-less canonical topics, `metadata_json` must advertise the canonical
object type. Typical examples:

```json
{"builtin_object_type":"kPointCloud"}
```

```json
{"builtin_object_type":"kCompressedPointCloud"}
```

```json
{"builtin_object_type":"kPosesInFrame"}
```

```json
{"builtin_object_type":"kFrameTransforms"}
```

For canonical image topics consumed by the 2D image path, the existing convention
also includes the canonical image codec marker:

```json
{"builtin_object_type":"kImage","image_codec":"pj_image_v1"}
```

The catalog and scene docks inspect this metadata to know which viewer family can
handle a topic. The payload bytes are still opaque to the store.

## 4. Why no `parser_ingest` is required

`parser_ingest` solves this problem:

```text
I have source-format bytes and need a parser plugin to turn them into a PJ object.
```

Mosaico canonical download solves a different problem:

```text
I already know the source object structure and can build the PJ object myself.
```

So the toolbox performs the normalisation itself:

1. Download typed records from Mosaico.
2. Convert each record to a PJ SDK struct.
3. Serialize the SDK struct with the matching canonical codec.
4. Register an `ObjectStore` topic with `builtin_object_type` metadata.
5. Push the canonical bytes with `pushOwnedObject(...)`.
6. Notify the host that data changed.

No parser binding is created. Later, when a scene layer asks
`SessionManager::parserBindingForObjectTopic(topic_id)`, it receives an empty
binding. That empty binding is now meaningful: it tells the viewer that this is
a parser-less canonical object topic.

The new 3D decode seam is:

- `pj_scene3D/widgets/include/pj_scene3d_widgets/resolve_object.h`
- `pj_scene3D/widgets/src/resolve_object.cpp`

`resolveObject(...)` implements the rule:

```text
if parser binding exists:
    parse source bytes with MessageParser under parseLocked(...)
else:
    treat payload as serialized PJ canonical bytes
    choose the canonical deserializer from builtin_object_type
```

Supported parser-less 3D canonical codecs include:

- `kPointCloud` -> `deserializePointCloud(...)`
- `kCompressedPointCloud` -> `deserializeCompressedPointCloud(...)`
- `kFrameTransforms` -> `deserializeFrameTransforms(...)`
- `kPosesInFrame` -> `deserializePosesInFrame(...)`
- `kOccupancyGrid` -> `deserializeOccupancyGrid(...)`
- `kOccupancyGridUpdate` -> `deserializeOccupancyGridUpdate(...)`
- `kVoxelGrid` -> `deserializeVoxelGrid(...)`
- `kSceneEntities` -> `deserializeSceneEntities(...)`

That is the main change that lets a Mosaico canonical object render without
`parser_ingest`.

## 5. The decode pipeline in detail

### 5.1 Topic selection

The catalog extracts `builtin_object_type` from `metadata_json`:

- `pj_runtime/src/CatalogModel.cpp`, `objectTypeFromMetadata(...)`

The scene routing code then decides which dock can handle the type:

- `pj_app/src/scene_object_classification.h`
- `pj_scene3D/widgets/src/Scene3DDockWidget.cpp`,
  `Scene3DDockWidget::handlesObjectType(...)`

The 3D dock registers factories for:

- point clouds
- compressed point clouds
- depth images represented as `kImage`
- robot descriptions
- occupancy grids
- scene entities
- poses in frame
- voxel grids

`kFrameTransforms` is handled as a scene configuration topic rather than a
visible render layer.

### 5.2 Canonical object resolution

Every 3D consumer now goes through `resolveObject(...)` or the equivalent
canonical path.

For a Mosaico parser-less topic:

```text
ObjectStore latestAt(...)
  -> payload bytes
  -> no parser binding
  -> builtin_object_type metadata
  -> deserialize* canonical codec
  -> sdk::BuiltinObject
```

That returned `sdk::BuiltinObject` is a `std::any` holding the concrete SDK type.
The layer then uses `std::any_cast` to get the type it expects.

Example from `PointCloudLayer`:

```text
resolveObject(...)
  -> any_cast<CompressedPointCloud>
  -> async compressed-cloud decode
  -> PointCloud
  -> render

or

resolveObject(...)
  -> any_cast<PointCloud>
  -> render
```

Example from `PosesInFrameLayer`:

```text
resolveObject(...)
  -> any_cast<PosesInFrame>
  -> build pose triad instances
  -> render
```

Example from `TransformService`:

```text
resolveObject(...)
  -> any_cast<FrameTransforms>
  -> insert edges into TransformBuffer
```

## 6. Decompression and transcoding

There are two different meanings of "decode" in this feature.

### Canonical deserialization

This is the step that turns a `pj_base` canonical wire blob back into an SDK
struct.

Examples:

```text
serialized PJ.PointCloud bytes
  -> deserializePointCloud(...)
  -> sdk::PointCloud
```

```text
serialized PJ.PosesInFrame bytes
  -> deserializePosesInFrame(...)
  -> sdk::PosesInFrame
```

This is not `parser_ingest`. It is not CDR parsing. It is simply the canonical
object's own serialization format.

### Media / asset decompression

Some canonical objects contain compressed media inside them.

That is a second-stage decode:

```text
sdk::CompressedPointCloud
  -> Cloudini or Draco decoder
  -> sdk::PointCloud
```

```text
sdk::Image with PNG/JPEG data
  -> PNG/JPEG decoder
  -> pixels or flat raw samples
```

This is allowed in the viewer because the canonical object is already known. The
viewer is not parsing source transport messages; it is decoding self-describing
media payloads inside a canonical object.

## 7. Point clouds

Point clouds use one dual-mode layer:

- `pj_scene3D/widgets/src/layers/pointcloud_layer.cpp`

The 3D dock registers both `kPointCloud` and `kCompressedPointCloud` to the same
`PointCloudLayer`.

### Raw canonical `PointCloud`

The canonical object already contains packed point bytes and a `fields` schema.

The viewer converts it through:

- `pj_scene3D/core/src/pointcloud_convert.cpp`
- `convertCanonical(...)`

That function:

1. Validates the cloud is little-endian.
2. Checks declared geometry against the backing byte buffer.
3. Finds `x`, `y`, `z` fields.
4. Validates field offsets before reading.
5. Reads positions.
6. Optionally reads a scalar field such as `intensity`.
7. Optionally reads RGB/RGBA color fields.
8. Computes a finite-point AABB in the same pass.
9. Returns a `DecodedPointCloud` for the render pass.

There is also a zero-copy GPU fast path for common packed layouts:

- contiguous float32 `x/y/z`
- optional compatible scalar field
- optional packed RGB/RGBA field

When the fast path is available, the layer can upload the canonical wire buffer
directly as a GL vertex buffer instead of rebuilding a CPU-side render buffer.

### Canonical `CompressedPointCloud`

A `CompressedPointCloud` contains:

- timestamp
- `frame_id`
- `format`
- compressed `data`

The supported formats are:

- `cloudini`
- `draco`

Decode dispatch lives in:

- `pj_scene3D/core/src/pointcloud_codecs.cpp`
- `decodeCompressedPointCloud(...)`

For Cloudini:

1. Decode the Cloudini header.
2. Validate field offsets before allowing Cloudini to write into the output
   buffer.
3. Decode the compressed blob into packed point bytes.
4. Convert Cloudini field types to PJ `PointField::Datatype`.
5. Build an `sdk::PointCloud`.

For Draco:

1. Decode the Draco point cloud.
2. Walk Draco attributes.
3. Recover field names from Draco metadata when present.
4. Pack attributes into a PJ-style interleaved point buffer.
5. Build an `sdk::PointCloud`.

The layer runs this compressed-cloud decode off the UI thread with
`QtConcurrent` and `QFutureWatcher`. When the result arrives, it is cached and
then sent through the same `pushCloud(...)` / `convertCanonical(...)` path as a
raw `PointCloud`.

That means the renderer only has one final point-cloud path.

```text
Mosaico compressed cloud
  -> sdk::CompressedPointCloud canonical bytes
  -> deserializeCompressedPointCloud(...)
  -> decodeCloudini/decodeDraco(...)
  -> sdk::PointCloud
  -> convertCanonical(...) or fast path
  -> PointcloudRenderPass
```

## 8. Poses and motion-state objects

Pose-style Mosaico objects are represented as `sdk::PosesInFrame` when they are
to be shown as object poses / pose arrays.

The canonical object contains:

- timestamp
- `frame_id`
- repeated poses

The layer is:

- `pj_scene3D/widgets/src/layers/poses_in_frame_layer.cpp`

The render expansion is:

- `pj_scene3D/core/src/poses_in_frame_render.cpp`

The path is:

```text
ObjectStore sample
  -> resolveObject(..., kPosesInFrame, ...)
  -> sdk::PosesInFrame
  -> buildPoseTriadInstances(...)
  -> PosesRenderPass
```

`buildPoseTriadInstances(...)` turns each pose into either:

- a full XYZ triad: three arrow instances per pose
- an X-only arrow: one arrow instance per pose

The styling is viewer-side:

- arrow size
- opacity
- X-only mode
- override color

The data schema carries pose geometry, not rendering style.

## 9. Frame transforms

Frame transforms are consumed by `TransformService`, not shown as ordinary object
layers.

Relevant code:

- `pj_scene3D/widgets/src/transform_service.cpp`
- `pj_scene3D/core/include/pj_scene3d_core/tf/tf_buffer.h`

For parser-less Mosaico canonical topics, `TransformService` classifies the
topic by metadata:

```text
metadata_json builtin_object_type == kFrameTransforms
```

It then reads new ObjectStore entries, decodes each entry with `resolveObject`,
casts to `sdk::FrameTransforms`, and inserts every transform edge into the
`TransformBuffer`.

This is what lets point clouds, depth clouds, poses, and other spatial objects be
placed in the selected fixed frame.

## 10. Images and depth images

Images are handled mainly by `pj_scene2D`, and depth images are also consumed by
`pj_scene3D` for back-projected depth clouds.

The canonical image object is `sdk::Image`.

It contains:

- timestamp
- width
- height
- encoding
- row step
- endian flag
- data bytes
- optional compressed-depth metadata
- frame id

The canonical image resolver is:

- `pj_scene2D/core/src/image_resolve.cpp`

The 2D image path recognizes parser-less canonical image blobs through the topic
metadata:

```json
{"builtin_object_type":"kImage","image_codec":"pj_image_v1"}
```

Then it calls `deserializeImage(...)` and decodes the resulting `sdk::Image`.

### Mosaico `serialization_format=image`

The branch adds a special recovery path for Mosaico-style image wrapping.

The observed shape is:

```text
sdk::Image.encoding = "rgb8" or "16UC1" or another raw logical encoding
sdk::Image.data     = PNG/JPEG container containing the flat raw byte buffer
```

For example, a `16UC1` depth frame may be stored as an 8-bit grayscale PNG whose
width is the byte stride. The pixels of that PNG are not the final RGB image.
They are the raw byte stream reshaped into an image container.

So the viewer must not do this:

```text
encoding says 16UC1
read PNG container bytes as uint16 depth samples
```

That produces black or garbage depth.

The correct path is:

```text
detect PNG/JPEG signature
decompress container
recover flat bytes
reinterpret flat bytes using sdk::Image.encoding and geometry
```

For 2D images, this logic lives in:

- `pj_scene2D/core/src/image_pipeline_source.cpp`,
  `decodeRawOrBayerImage(...)`

For 2D depth display, the reusable helper is:

- `pj_scene2D/core/src/codecs.cpp`,
  `recoverContainerRawSamples(...)`
- `pj_scene2D/core/src/depth_pipeline_source.cpp`,
  `toDepthImage(...)`

For 3D depth clouds, similar recovery is in:

- `pj_scene3D/widgets/src/layers/depth_cloud_layer.cpp`,
  `DepthCloudLayer::toDepthView(...)`

## 11. Depth-cloud visualisation

Depth clouds are built from `sdk::Image` objects whose encoding is a depth
encoding:

- `16UC1`
- `32FC1`
- `compressedDepth`

The 3D dock accepts `kImage`, but gates it by peeking the first sample's
`encoding`, because color images and depth images share the same canonical
object type.

The depth cloud layer path is:

```text
ObjectStore sample
  -> parser/canonical image resolution
  -> sdk::Image
  -> toDepthView(...)
  -> resolve CameraInfo intrinsics by frame_id
  -> depthToPoints(...)
  -> DecodedPointCloud
  -> PointcloudRenderPass
```

Back-projection is in:

- `pj_scene3D/core/src/depth_backproject.cpp`

`depthToPoints(...)`:

1. Checks intrinsics are valid.
2. Supports `16UC1` millimetres and `32FC1` metres.
3. Converts each valid pixel into an XYZ point in the camera optical frame.
4. Drops zero, non-finite, too-near, or too-far samples.
5. Builds a parallel depth scalar array for colormapping.
6. Computes the point-cloud AABB and scalar range in the same pass.

The resulting cloud is rendered by the same `PointcloudRenderPass` used for
normal point clouds.

## 12. Rendering summary

After canonicalization, rendering is type-specific but straightforward.

### `PointCloud`

```text
sdk::PointCloud
  -> fast path or convertCanonical(...)
  -> PointcloudRenderPass
  -> GL_POINTS or sphere/cube imposters
```

The shader can color by:

- solid color
- scalar field plus colormap
- fixed-frame X/Y/Z axis
- direct RGB/RGBA per point

### `CompressedPointCloud`

```text
sdk::CompressedPointCloud
  -> decodeCloudini/decodeDraco
  -> sdk::PointCloud
  -> normal point-cloud path
```

### `PosesInFrame`

```text
sdk::PosesInFrame
  -> buildPoseTriadInstances(...)
  -> PosesRenderPass
  -> instanced arrows
```

### `FrameTransforms`

```text
sdk::FrameTransforms
  -> TransformService
  -> TransformBuffer
  -> used by layers to transform source frame into fixed frame
```

### Depth `Image`

```text
sdk::Image with depth encoding
  -> recover wrapped raw samples if needed
  -> depthToPoints(...)
  -> DecodedPointCloud
  -> PointcloudRenderPass
```

## 13. Mental model for debugging

When a Mosaico object does not show up, debug the pipeline in this order.

### 1. Is there an ObjectStore topic?

Check the topic exists in `ObjectStore` and has entries.

The topic should have useful `metadata_json`, especially:

```json
{"builtin_object_type":"kPointCloud"}
```

or the appropriate builtin type.

### 2. Is the topic parser-less on purpose?

For Mosaico canonical topics, no parser binding is expected.

An empty parser binding is only a problem if the payload is raw source bytes. If
the payload is serialized canonical PJ bytes, an empty binding is correct.

### 3. Does `resolveObject(...)` support the type?

`hasCanonical3DCodec(...)` must return true for 3D parser-less topics.

If the type is unsupported there, the layer will reject it or `resolveObject`
will return an error.

### 4. Does the payload match the metadata?

If metadata says:

```json
{"builtin_object_type":"kPosesInFrame"}
```

then the payload must be the output of `serializePosesInFrame(...)`.

If the payload is still a Mosaico API response, JSON blob, CDR message, or any
other non-canonical format, parser-less rendering will fail. In that case either
the toolbox must convert it to a canonical object first, or the topic must use a
proper parser-backed ingest path.

### 5. For compressed point clouds, is the inner codec supported?

`CompressedPointCloud.format` must be one of:

- `cloudini`
- `draco`

Other formats are rejected by `decodeCompressedPointCloud(...)`.

### 6. For wrapped images, is the logical encoding still correct?

For `serialization_format=image`, the container unwrap only recovers flat bytes.
The `sdk::Image.encoding`, `width`, `height`, and `row_step` must still describe
how those recovered bytes should be interpreted.

For example:

```text
encoding = "16UC1"
data     = grayscale PNG wrapping the little-endian uint16 byte stream
```

After PNG decompression, the recovered bytes are interpreted as `16UC1`.

## 14. One sentence version

We can present Mosaico canonical objects without `parser_ingest` because the
toolbox converts Mosaico's typed cloud records into PlotJuggler canonical SDK
objects before writing them; the host then deserializes those canonical object
bytes directly from `ObjectStore`, decompresses only self-describing media
payloads such as PNG/JPEG, Cloudini, or Draco, and renders the resulting SDK
objects through the existing 2D and 3D scene layers.
