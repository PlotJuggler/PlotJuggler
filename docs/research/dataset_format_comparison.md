# Multimodal Robotics Dataset Format Comparison

## MCAP (ROS 2 / Foxglove) vs RLDS vs LeRobot vs Zarr

A survey of the formats' public documentation and ecosystems as of May 2026; not re-checked since.

| Aspect | **MCAP** | **RLDS** | **LeRobot (v2/v3)** | **Zarr** |
|---|---|---|---|---|
| **Primary purpose** | Recording & replay of live robot data streams | Storing RL/IL episodes for ML training | Storing robot demonstrations for policy training | Storing chunked N-dimensional arrays |
| **Origin / ecosystem** | Foxglove / ROS 2 (default since Iron) | Google Research / TensorFlow Datasets | HuggingFace / PyTorch | Zarr community (framework-agnostic) |
| **Typical role in pipeline** | **Upstream** — raw recording format, converted to training formats | Training format (OXE, OpenVLA, Octo, RT-X) | Training format (SmolVLA, GR00T, π0, ACT) | Training format (Diffusion Policy, UMI) |

### Data Model

| Aspect | **MCAP** | **RLDS** | **LeRobot (v2/v3)** | **Zarr** |
|---|---|---|---|---|
| **Core abstraction** | Channels of timestamped messages | Episodes → Steps (nested `tf.data.Dataset`) | Episodes → Frames (Parquet rows + MP4 videos) | Flat N-dimensional arrays with shared time axis |
| **Data organization** | Multiple named topics/channels in a single file | Nested: Dataset of episodes, each containing a Dataset of steps | Parquet files (tabular) + MP4 files (video) + JSON metadata | Hierarchical groups of arrays (`/data/`, `/meta/`) |
| **Episode boundaries** | No native concept — continuous recording; episodes defined by external metadata or topic flags | Structural: each episode is a separate nested Dataset with `is_first`/`is_last`/`is_terminal` flags | `episode_index` column in Parquet; `meta/episodes/` metadata files | `meta/episode_ends` integer array storing end indices |
| **Schema** | Embedded in file (ROS msg defs, Protobuf, JSON Schema, Flatbuffers) | `FeaturesDict` defined in TFDS builder code | `info.json` with feature names, shapes, dtypes | Zarr array metadata (dtype, shape, chunks, compressor) |

### Timestamp & Synchronization

| Aspect | **MCAP** | **RLDS** | **LeRobot (v2/v3)** | **Zarr** |
|---|---|---|---|---|
| **Timestamp model** | Each message has `log_time` (nanoseconds) + optional `publish_time`; **wall-clock timestamps** | **None per step** — implicit from step ordering; some datasets add optional episode-level `timestamp` | `timestamp` (float32, seconds) per Parquet row; `VideoFrame = {path, timestamp}` for images | **None** — pure integer index alignment |
| **Timestamp resolution** | Nanosecond (uint64) | N/A | Float32 seconds (~microsecond effective) | N/A |
| **Multi-modal sync** | **By timestamp join** — each channel has independent timestamps; consumers must align by time | **By construction** — all fields (image, state, action) are co-located in the same step record | **By timestamp seek** — Parquet timestamp used to seek into MP4 video via PTS | **By shared index** — row N in all arrays corresponds to the same timestep |
| **Misalignment risk** | Possible — channels can have different rates; requires interpolation or nearest-timestamp matching | Zero — data is atomically recorded per step | Non-zero — depends on MP4 PTS alignment with Parquet timestamps; sub-frame drift can degrade policy performance | Zero — if recording was truly synchronous; no mechanism to detect if it wasn't |
| **Variable-rate support** | Native — each channel can publish at its own rate | No — fixed step structure | Partially — FPS declared in `info.json`, `delta_timestamps` for temporal queries | No — all arrays must share the same time dimension |
| **Temporal queries** | Index-based seeking by time range and channel | Iterate steps sequentially (or batch with RLDS transforms) | `delta_timestamps` API (e.g., `[-1.0, -0.5, 0]` to get history) | Native numpy/zarr slicing (`array[start:end]`) |

### Image & Video Storage

| Aspect | **MCAP** | **RLDS** | **LeRobot (v2/v3)** | **Zarr** |
|---|---|---|---|---|
| **Image storage** | Per-message: `sensor_msgs/Image` (raw) or `sensor_msgs/CompressedImage` (JPEG/PNG) as individual messages | Per-step: raw image tensor `(H, W, 3) uint8` inline in TFRecord | MP4 video files (H.264/H.265); one file per camera per episode (v2) or per chunk (v3) | Per-frame in chunked array with configurable codec (JPEG-XL, Blosc, etc.) |
| **Inter-frame compression** | No (each message is independent) | No (each step is independent) | **Yes** — full video codec with temporal compression | No (each chunk/frame is independent) |
| **Image access pattern** | Seek to timestamp → decompress single message | Read Nth step record → decode image tensor | Seek to PTS in MP4 → decode frame (via torchcodec or pyav) | Read chunk at index N → decompress |
| **Storage efficiency (images)** | Poor to moderate (per-frame JPEG/PNG) | Poor (~70x larger than video) | **Best** (inter-frame video compression) | Moderate (per-frame JPEG-XL, ~10-20x better than raw, worse than video) |
| **Video playback** | Natural — sequential message replay | Requires reconstruction from step images | Natural — MP4 files are standard video | Requires reconstruction from array frames |

### File Format & I/O

| Aspect | **MCAP** | **RLDS** | **LeRobot (v2/v3)** | **Zarr** |
|---|---|---|---|---|
| **Container format** | Single binary file (`.mcap`) with append-only row-oriented layout | TFRecord files (`.tfrecord`) partitioned by episode groups | Parquet files + MP4 files + JSON/JSONL metadata | Directory tree of chunks + JSON metadata (or `.zarr.zip`) |
| **Single file?** | Yes — all channels in one file | No — multiple TFRecord shards | No — separate Parquet, MP4, and metadata files | No — directory of chunk files |
| **Compression** | Chunk-level: LZ4 or Zstandard | Per-record: PNG for images, TFRecord built-in | Video codec for images; Parquet compression for tabular data | Per-chunk: configurable (Blosc, JPEG-XL, LZ4, Zstd, none) |
| **Write pattern** | Append-only streaming (crash-safe) | Batch write via TFDS builder or EnvLogger | Batch write via `LeRobotDataset.add_frame()` + `save_episode()` | Random or append; parallel chunk writes possible |
| **Random read** | Fast — built-in index by timestamp and channel | Read Nth record in partition | Parquet: memory-mapped; Video: seek to keyframe + decode | Direct chunk addressing by index |
| **Cloud/streaming** | Indexed remote read via HTTP range requests | `tfds.load()` with GCS/S3 support | HuggingFace Hub streaming (`StreamingLeRobotDataset` in v3) | Native cloud support (S3, GCS) via chunk-per-object |
| **Self-describing** | Yes — schemas embedded in file | Partially — schema in TFDS builder code, not in data file | Yes — `info.json` with full feature schema | Yes — metadata JSON alongside chunks |

### Ecosystem & Tooling

| Aspect | **MCAP** | **RLDS** | **LeRobot (v2/v3)** | **Zarr** |
|---|---|---|---|---|
| **Language support** | C++, Python, Go, Rust, Swift, TypeScript | Python (TensorFlow) | Python (PyTorch) | Python, Java, JavaScript, C++, Rust, Julia |
| **ML framework** | None (recording format) | TensorFlow | PyTorch / HuggingFace | Framework-agnostic |
| **Visualization** | Foxglove Studio, PlotJuggler | TensorBoard (limited) | Rerun.io via HF Visualize space | Custom (no standard viewer) |
| **Key consumers** | ROS 2 ecosystem, Foxglove, PlotJuggler | OXE, OpenVLA, Octo, RT-1-X, RT-2-X | SmolVLA, GR00T (N1/N1.5/N1.6), π0, ACT, X-VLA | Diffusion Policy, UMI |
| **Community** | ROS Tooling WG + Foxglove | Google DeepMind | HuggingFace | Open Microscopy Environment, climate/geo, ML |
| **Dataset sharing** | No standard hub (files shared directly) | TFDS catalog + GCS | HuggingFace Hub (thousands of datasets) | No standard hub |

### Strengths & Weaknesses Summary

| Aspect | **MCAP** | **RLDS** | **LeRobot (v2/v3)** | **Zarr** |
|---|---|---|---|---|
| **Greatest strength** | Faithful, lossless recording of heterogeneous live data with nanosecond timestamps; crash-safe streaming writes | Perfect sync by construction; mature OXE ecosystem with 60+ unified datasets | Best storage efficiency (video compression); largest community dataset hub; PyTorch-native | Simplest data model; numpy-like random access; framework-agnostic |
| **Greatest weakness** | Not a training format — must be converted; no episode/step structure | Terrible storage efficiency (no video compression); TensorFlow-only | Timestamp-based video sync introduces subtle alignment risks | No inter-frame compression; no timestamps; no standard sharing platform |
| **Best suited for** | Robot data recording, logging, replay, debugging, fleet data collection | Large-scale VLA pre-training in the Google/TF ecosystem | General-purpose robot learning with PyTorch; dataset sharing and collaboration | Small-to-medium manipulation datasets that fit in RAM; Diffusion Policy workflows |
| **Scalability** | Excellent for recording (append-only); files can be terabytes | Good for training (sharded TFRecords); poor for storage (8.9 TB for OXE uncompressed) | Good (v3 scales to millions of episodes with chunked files) | Moderate (excellent for cloud I/O; limited by per-frame compression for large video datasets) |

### Typical Pipeline Position

```
[Robot HW] → [MCAP recording] → [conversion] → [RLDS / LeRobot / Zarr] → [VLA/Policy training]
                 ↑                                        ↑
          live, lossless                          training-optimized
         multi-rate streams                    fixed-rate, synchronized
        nanosecond timestamps                  episode/step structure
```
