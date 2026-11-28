# 2D and 3D scenes

Labels as the app shows them (tooltips for icon buttons); `objectName`s in backticks. Scenes show object topics
(images, video, depth, annotations, point clouds, TF, robot models, grids, scene entities) at the playhead. Which
plugin decodes each format: [formats.md](../formats.md).

## Open a scene and add topics

- **UI:**
  1. Drag an object topic from the Datasets (or Custom Topics) panel onto an empty dock: it creates the panel that
     shows that type. Or click "2D" (`buttonVizScene2D`) or "3D" (`buttonVizScene3D`) in the empty dock first.
  2. Drag more object topics onto the scene: each becomes a layer.
  3. Right-click the scene → "Open Settings" (or the title-bar "Toggle right panel") for its layer list and settings.
- **MCP:** `scene_view` `action: "create"` (`kind` `"3d"` or `"2d"`), then `"attach"` with `topics`. It works only in
  the agent's own views. Derived objects from `create_derived_object` attach the same way.
- **Gotchas:**
  - A 3D view takes only depth-encoded images ("Only depth-encoded images can be displayed in a 3D view. Use a 2D view
    for colour images.").
  - Scalars cannot go on a scene that already shows objects, and object topics cannot go on a plot.
  - An image annotation draws only over its own image: put the image in the same 2D view.

## Layers

In the right panel: "Layers" (2D) or "Topics" (3D) lists one row per layer.

- **Hide / remove:** "Toggle layer visibility" (`curveVisibilityToggle`), "Remove this layer" (`curveTrashToggle`).
- **Order:** drag rows (desktop only).
- **Settings:** select a row; its controls appear under "Settings". In 3D, "Copy parameters", "Paste parameters" and
  "Apply these parameters to all topics of the same type" share one layer's settings.
- **Status:** a row's tooltip shows a warning such as `Frame '<frame>' is not connected to fixed frame '<fixed>'`; a
  derived layer may read "computing…".
- **MCP:** `scene_view` `"detach"` on the agent's view. A layer's settings, in any scene dock, the user's too:
  `widget_state` `get` (the dock's XML and the values each attribute accepts), then `set_widget_state` with the edited element.

## 2D view: images, video, depth

- **Navigate:** wheel zooms about the cursor; left-drag pans once zoomed in; double-click resets.
- **Rotate:** "Rotation" (`scene2dRotation`): "0°", "90°", "-90°", "180°" ("Rotate every layer clockwise").
- **Pixel values:** with "Show point in plot" (`buttonShowpoint`, right toolbar) on, hovering shows the pixel under
  the cursor.
- **Image layer:** "Rectify" undistorts with the matching CameraInfo (same `frame_id`). It stays disabled until such a
  CameraInfo has been decoded; its tooltip says what is missing.
- **Depth image layer:** "Colormap" (Turbo, Viridis, Plasma, Grayscale), "Invert colormap", "Near" and "Far" (metres),
  "Auto-fit range" (from the current frame's depth distribution).
- **MCP:** `scene_view` with `kind: "2d"`; `screenshot` to see it.

## 3D view: camera and frames

Overlays on the view: the fixed-frame combo (`scene3dFixedFrameCombo`, top left) with "Reset view to default"
(`cameraHomeButton`) beside it, and the camera model (`cameraModelCombo`, top right): "Orbit", "XYOrbit" (pivot kept
on the ground plane), "Fly", "Top-down ortho".

| Gesture | Does |
|---|---|
| Left-drag | rotate / orbit |
| Shift + left-drag, or middle-drag | pan |
| Right-drag | zoom |
| Wheel | zoom towards the cursor |

- **Fixed frame:** choose it in `scene3dFixedFrameCombo`, or right-click a TF frame's axes → "Set as fixed frame".
  Every layer is drawn in it; a frame not connected to it is reported per layer.
- **Follow a frame:** "Camera" → "Follow frame" (`scene3dFollowFrameCombo`, "None" to stop), or right-click a frame →
  "Follow this frame".
- **Trail of a frame's motion:** right-click a frame → "Create trail for '<frame>'", or "Transforms and RobotModel" →
  "Create Trail" ("TF frame whose motion trail to draw") and "Add a motion trail for the selected frame".
- **MCP:** camera and fixed frame through `widget_state` / `set_widget_state` (`fixed_frame`, `camera_model`, `camera_state` on the
  `<scene3d>` element); while a frame is followed, `follow_offset` (the camera's focal point relative to that frame)
  places the camera, not `camera_state`'s focal; trails from the UI. `evaluate` with `pj.tf.lookup` reads a transform as numbers
  ([scripting.md](../scripting.md), "On-demand object scripts").

## 3D view: scene settings

The right panel, above the layer list:

- **"Camera":** "Follow frame", and "Recenter the camera on the followed frame".
- **"Grid":** "Style:" "Line grid" / "Checkerboard", "Show/hide the grid", "Size (m)", "Divisions".
- **"Transforms and RobotModel":** "Frames size (m)" with "Show/hide lines connecting each TF frame to its parent",
  "Frames opacity" with "Show/hide the TF frames", "Model/URDF" ("File", "Topic", "URL") with "Add a robot model from
  the selected source", one row per loaded model with "Remove this robot model", "Meshes opacity" ("Show/hide visual
  meshes"), "Collision opacity" ("Show/hide collision meshes"), "Create Trail".
- **MCP:** `widget_state` / `set_widget_state` on the dock: the grid, frames and mesh settings are attributes of `<scene_controls>`.

## Add a robot model (URDF)

- **UI:** "Transforms and RobotModel" → "Model/URDF": "Topic" (a robot description topic of the dataset), "File" (a
  `.urdf` / `.xml` file) or "URL" ("Load URDF from URL"), then "Add a robot model from the selected source".
- **Layer settings:** "Source", "Status", "Frame prefix", "Display mode" (Auto, Visual, Collision), "Color", "Ignore
  COLLADA up_axis".
- **MCP:** UI only.
- **Gotchas:** "No robot description topic in this dataset." when "Topic" finds none.

## Point clouds and other 3D layers

Per-layer settings, selected in the "Topics" list:

| Layer | Settings |
|---|---|
| Point cloud | "Shape:", "Point size:", "Color type:" (Solid, RGB, "Field: <name>"), "Color config:", "Range:" (auto), "Range Min:", "Range Max:", "Outside range:" |
| Depth cloud | "Colormap:", "Point size:", "Min depth:", "Max depth:" |
| Occupancy grid | "Colors:" ("Map (grayscale)", "Costmap"), "Opacity:" |
| Voxel grid | "Field:", "Colors:", "Draw:" (All cells, Non-zero, Threshold, In range), "Threshold:", "Range min:", "Range max:", "Opacity:" |
| Grid map | elevation, "Lighting:", "Opacity:" |
| Mesh map | "Color by:", "Lighting:", "Links:", "Opacity:" |
| Scene entities | "Opacity:", "Override color:", "Wireframe:" |
| Poses in frame | "Arrow length (m):", "Opacity:", "X Arrow only:", "Override color:", "Trail:" (the whole time range) |
| Trail | "Tracks:", "Thickness (px):" |

- **Colour a cloud by a field:** "Color type:" → "Field: intensity" (or another field), a colormap in "Color config:",
  "Range:" auto or "Range Min:" / "Range Max:".
- **MCP:** `create_derived_object` builds clouds, voxels and scene entities from scripts
  ([scripting.md](../scripting.md)); layer settings through `widget_state` / `set_widget_state` on the dock. `evaluate` with cloud ops (`count`, `bounds`,
  `crop_box`, ...) answers questions about a cloud without drawing it.
