# pj_custom_widgets architecture

How the module works today. Per-method contracts live in the headers; the module's invariants are in
[../CLAUDE.md](../CLAUDE.md).

A Custom Widget panel is a dock whose content is a Qt widget created by a Widget plugin (the SDK family `Widget`, ABI
in `pj_plugins/widget_protocol`). The module owns what the host does for such a panel: wiring series to the widget's
named inputs, resolving each value at the tracker time, pushing it to the plugin, persisting the panel in the layout,
reporting plugin failures, and destroying the plugin's objects in a safe order. The plugin never sees topics, datasets
or the data store; it declares inputs by name and receives values.

## Components

- `core/` (`pj_custom_widgets_core`, no widget class): `WireSample` (the latest sample of one wire at an instant, its
  identity hash and its conversion to `PJ_widget_value_t`), `SettingsSchema` (the type's manifest `settings`
  declaration), `WidgetBindings` (the wires of one panel, reconciliation, resolution, the `<input>` XML).
- `widget/` (`pj_custom_widgets`): `CustomWidgetDockWidget` (`QWidget` + `IDataWidget`) and
  `MissingWidgetPlaceholder`.
- Outside the module: `pj_runtime`'s `WidgetRuntimeHost` (the `pj.widget_host.v1` service: history queries, object
  reads), `PlaybackControlHost` (`pj.playback.v1`), `canonicalObjectEncoder` and `ActiveWidgetsMarker`; `pj_app`'s
  `CustomWidgetController` (the picker menu and config page, the marker's owner), `PendingDisplayBinder`,
  `TopicDemandController::registerCustomWidgetDock`, `LayoutXml`, and `MainWindow::makeSceneDock` /
  `createCustomWidgetDock` (kind `"CustomWidget"`), which construct the dock.

## A panel and its widget

A panel hosts one widget type of one plugin library, identified as `plugin_id/type`; each open panel is its own
plugin instance. The plugin creates the `QWidget` as a child of the panel and it fills the panel; only the opaque
pointer crosses the ABI.

Binding (`bindWidget`) runs in this order: find the library and type, record the panel in the active-widgets record,
`createHandle`, register the host services (widget host, and `pj.playback.v1` when the dock has a `PlaybackEngine`),
`bind`, `load_config` with the settings, read the inputs, load the saved wires, `create_qwidget`. The wires load before
`create_qwidget` so a widget can query their history while building its view. A new panel's first `load_config` carries
the full defaults ("Settings").

`pj.playback.v1` (play, pause, seek, rate, state, display-time conversions) is offered to every widget. Its time
conversions are the mappers the shell passes to `setPlaybackTimeMappers` before the bind; unset, the widget is told they
are unsupported. A value's `time` is `rawToDisplaySecondsAtOrAfter` of its sample, so seeking to it shows that sample
even where a double cannot hold every nanosecond.

A widget that cannot be created shows `MissingWidgetPlaceholder` with the reason and keeps its layout element, so the
widget returns once the cause is fixed. Causes: library not installed or disabled, no such type, a Qt build of the
library the host cannot use (`qt_compatible == false`: `create_qwidget` is never called), creation failed
(`create_qwidget` returned null, `bind` failed), or an error during creation or later ("Errors and crashes").
`isMissing()` is true and `missingReason()` holds the text. A panel's layout element is written back verbatim in all of
these (`preserved_`), so a layout saved without the plugin loses nothing.

## Inputs and wiring

The widget declares named inputs (name, label, unit, kind number / text / object, `multiple`). The user wires series to
them (drag and drop, or `setBinding` from the side panel).

- **Matching is by name.** `WidgetBindings::setInputs` is the only reconciliation point. The input list is re-read after
  creation, after every `load_config` and when the widget announces a change. Wires are kept for names that survive and
  dropped for the others; new inputs start empty; a kind change under the same name drops the wire; a normal input
  keeps slot 0 only. Position-based matching would rewire a widget whose input order changes between versions.
- **A failed input list is never reconciled against.** The plugin reported an error, or returned a list
  `WidgetHandle::inputs` refuses (a null array with a count, an empty or duplicate name, an unknown kind, an impossible
  string): reconciling an empty list would drop every wire. `readInputs()` disables the panel instead, and the disabled
  panel's preserved element keeps the wires.
- **Which input a drop goes to** (`pickInput`): the first compatible input with no wire; else the first compatible
  `multiple` input (a new slot); else the first compatible input, whose wire is replaced. Several series in one drop do
  not replace each other on a normal input.
- **Kinds.** A number input accepts plottable scalar series, a text input string series, an object input object topics
  (`kindOfKey`). A drop that wires nothing tells the user why (`showDropRejected`, opened with `show()` so the drop
  handler does not nest an event loop); unknown keys stay silent. Object drops hover-check through
  `IDataWidget::canAcceptObjectTopic`.

## Delivering values

On every tracker tick of a visible panel (`onTrackerTime`) the dock resolves, per wire, the latest sample at or before
the tracker time (zero-order hold) on the display time axis of the wire's dataset, and calls `on_update` with one value
per wire. A wire with no sample, or not resolved yet, is delivered with kind NONE. Every value, NONE included, carries
the wired series' label (`"topic/field"`).

- **`on_update` runs only when a wired sample changed.** The dock hashes the identity of every wire (stamp and number,
  the stamp alone for text, the entry uid for objects, the display offset: `mixWireSample`) and skips the call when the
  hash is unchanged. A change of bindings, settings, inputs or catalog content invalidates the key and forces the next
  push; a display-offset change delivers only when it moves a wired dataset. A panel gets one delivery right after its
  widget is created, wired or not.
- **Two events deliver the unchanged values once more** (`requestUpdate`, from the event loop): playback starting or
  stopping (`PlaybackEngine::playingChanged`), and entries added to or evicted from a wired object topic wherever they
  fall relative to the tracker (a widget showing a whole log needs them). The second is detected through each wire's
  `ObjectStore::contentIdentity` at the last delivery and triggered by `SessionManager::dataPublished` (which covers
  object-only ingest that `samplesIngested` does not), `ingestEnded` (a load's last entries land with its end) and
  `derivedEntriesPublished`. A `CoalescingTrigger` lets the first arrival through, then at most one per
  `kEntriesLandedInterval`, because a progressive load commits many times a second and a widget reloading its whole
  topic on each would stall. Requests collapse into one, and the update is dropped when another `on_update`
  reached the widget first.
- **Object payloads are fetched lazily.** The first pass resolves only an object's entry id; the payload is fetched
  after the hash changed, and only for an entry the wire does not already hold. A text string is read only when its row
  stamp moved. The dock keeps each wire's previous sample until the wiring changes. The per-tick path reuses member
  scratch vectors and copies no string.
- **Widgets receive canonical object bytes.** The per-tick value and `query_history` both pass each object entry
  through the panel's `ObjectPayloadEncoder` (`canonicalObjectEncoder`): a parser-backed topic's wire message is
  decoded and re-serialized with the `pj_base` codec, a topic without a parser passes its stored bytes through. An
  entry that does not decode is no value (current) or is skipped (history), like a failed lazy fetch.
- **History is not pushed.** The widget asks for it through `pj.widget_host.v1` (`query_history`).
- **Re-entrancy.** All plugin calls are on the GUI thread, and a widget's code can spin an event loop (a modal dialog)
  or move the tracker from inside `on_update`. A push asked for while a plugin call is on the stack is not run nested:
  it sets `update_wanted_`, and `callPlugin` delivers it once the outermost call returned (nothing is scheduled
  meanwhile, so a nested event loop does not spin on it). A tracker move a widget causes from its own code arrives
  synchronously from the engine (`PlaybackControlHost::callInProgress()`) and is deferred to the event loop with the
  same unchanged-sample rule. `inputs_changed` arriving from inside a plugin call is deferred (`deferOnce`, a
  `QTimer::singleShot(0)` latch) so the input list never changes under a running `on_update`; `config_changed` from
  inside one is dropped ("Settings"). The values handed to `on_update` point into the dock's samples and labels, which
  an edit during that call (a rewire from the widget's nested event loop) would free: `invalidateRenderKey` only marks
  them stale, and the next push clears them.

## Settings

A widget type declares user-editable settings in its manifest `settings`; `parseSettingsSchema` reads them once per
bind. The host keeps the whole settings JSON and sends it whole through `load_config` on every change and on restore,
with the manifest `default` filled in for every declared key the stored JSON lacks (`withDefaults`). A brand-new
panel's first `load_config` is therefore the full defaults object, never `{}` (a type that declares no defaults and
has no stored settings gets no `load_config` at creation). A rejection by the widget does not discard what was stored:
the JSON is kept either way so the layout keeps what the user wrote.

`settingsJson()` is what the layout persists: the defaults, then the stored values, then the plugin's own `save_config`
keys laid over them (a colliding state key wins). A declared key with no `default` appears only once the user set it.
The side panel edits one key of that JSON and passes it to `applySettings`.

A change the user makes to the widget's own state in its UI (a filter, a splitter), which the widget reports with
`notify_config_changed` (SDK 1.1), is a change of the workspace like a side-panel edit: one undo step. The dock only
emits `workspaceChanged`, which the shell debounces into an undo snapshot; that snapshot calls `save_config` from the
event loop, never inside the plugin's callback, and an undo rebuilds the panel from the snapshot's `<config>`. A report
that arrives from inside one of the plugin's own calls (what `load_config` or `on_update` brings) is dropped: it is the
host's doing, and reporting it would turn an undo into a new step.

## Persistence

One fixed tag, `CustomWidget`, with attributes `plugin`, `type`, `plugin_version`; one
`<input name slot kind dataset_id dataset_source dataset_path topic field/>` per wire; and
`<config><![CDATA[json]]></config>`. A wire's identity is a `SavedSeriesPath`, the type the layout uses for curves.

- The panel writes the full `dataset_path` itself, and every `<CustomWidget><input>` then goes through `LayoutXml`'s
  three passes like a transform input (`forEachCustomWidgetInput`): a file save makes the path layout-relative and
  strips an id it cannot validate, and a generic layout drops all three qualifiers.
- A wire re-resolves through `CatalogModel::resolveCurveKey` (scalars) or `SessionManager::resolveObjectDatasetIdentity`
  (objects). An object wire with no qualifier left (a generic layout, or a layout applied to the current data, e.g.
  `-d file -l layout`) binds to the one loaded dataset that has its topic, as a scene layer does, and stays pending
  while several have it, so a second recording never makes the choice by load order.
- **Pending wires.** A wire that does not resolve stays pending, is written back verbatim, and is retried when the
  catalog adds or changes items (a field reclassified on its first data adds no key, hence the change hook), and on
  `SessionManager::datasetSourcePathChanged` while any wire is pending: an object wire resolves its dataset by source
  path, which an asynchronous file load registers only at commit, after the topic and its entries exist, and no catalog
  change reports it.
- A saved dataset id that no `DatasetId` can hold decays to an unqualified identity instead of wrapping into another
  dataset.
- Loading a `<CustomWidget>` element always succeeds, including when the plugin is missing; the element is then written
  back verbatim.

## Demand

A panel holds one demand reference per distinct (dataset, topic) of its resolved wires (`displayedTopics()`), updated
whenever its wires change and released when the panel is destroyed. A pending wire needs its topic subscribed on a
demand-driven stream too: `pendingDemands()` lists one `PendingWireDemand` (the topic, plus the dataset its saved
identity names when that one is loaded) per distinct pending topic, and the shell's `PendingDisplayBinder` holds a
reference for each (`addPendingCustomWidgetWire`, from `MainWindow::collectPendingDisplayBindings`). The dock keeps
its own retry; the binder only holds the reference and drops it once no pending wire of the dock names the topic
(checked on `bindingsChanged`), when the dock is destroyed, or on `clear()`. A reference held and never dropped would
keep a stream subscribed to a topic nobody reads.

## Lifetime and destruction

The plugin instance and the QWidget it created are destroyed together, synchronously, with the widget detached from its
parent first (`releasePlugin()`: `setParent(nullptr)`, then the handle, the registry, the playback and widget host
services, in that order, which is the reverse of the members' declaration). A parent must not free an object whose
destructor lives in the plugin's library. Nothing in the host schedules deferred deletion for an object a plugin
created.

`releasePlugin()` runs from two places and both are needed:

- `aboutToBeDiscarded()`, because a panel closed during the session is scheduled for deferred deletion: only this ends
  the plugin before the session or the event loop can reach it. `pj_plotting`'s `DockWidget::clearCurrentContent`
  calls it right before scheduling the content's deletion; `releaseObjectWidget` (the content moves to another dock)
  does not, so the dock must tolerate its own later deletion.
- The destructor, which covers window teardown: `~MainWindow` deletes the docks before the session unloads anything.

The library is never unloaded (the SDK loader keeps it mapped for the life of the process), so the plugin's code stays
mapped for as long as its widget can be called.

## Errors and crashes

Every plugin call goes through `callPlugin` (or `callOrDisable`, which disables on an exception): a call-depth
rollback and a catch, with no allocation on the success path. An exception it returns makes the dock
`disablePlugin(reason)`: `releasePlugin()`, the layout element kept in `preserved_` (built from the dock's own state,
never by asking the plugin), wires and inputs cleared, the placeholder "This widget was disabled after an error: ...",
a `kError` to the diagnostic sink, `inputsChanged` and `bindingsChanged`. The panel then calls its plugin no more and
holds no demand; choosing the widget again or reloading the layout re-enables it. From a const path (`settingsJson`
during a layout save) the disable is deferred to the event loop and skipped if the dock was rebound in between
(`bind_generation_`).

The ABI slots are noexcept, so what this catches in practice is the host's own conversion of what the plugin returned
(a string length no `std::string` can hold, an allocation failure); a plugin built on `WidgetPluginBase` contains its
own exceptions in the SDK trampolines (an exception in `onUpdate` is swallowed there and the panel stays live).
`destroy` and `~WidgetHandle` are noexcept, so `releasePlugin` has no catch. A rejection through the contract's own
error channel (`load_config` refusing settings) is not an error of this kind: a warning, and the settings stay stored.

**The active-widgets record.** A crash (SIGSEGV) inside the plugin cannot be caught in the process. Before its first
call into a plugin instance (right before `createHandle`) the dock adds `plugin_id/type` to the record of its process
(`setActiveWidgetsMarker`, a `pj_runtime` `ActiveWidgetsMarker`, one `active_widgets.<pid>.json` per process id,
shared from `pj_app`'s `CustomWidgetController`) and removes it in `releasePlugin` after the handle died, so the file
names exactly the panels whose plugin code was live. A clean exit deletes that process's record and no other. At the
next start the shell reviews only the records of processes that are no longer alive (an instance still running is
never reported as a crash, nor its record erased), reports their widgets together and, when someone can answer, offers
to disable those plugins before any layout is restored. A null marker records nothing (the module's own tests, the
browser).

## Surface used by the shell

`CustomWidgetDockWidget`: `setActiveWidgetsMarker`, `setDiagnosticSink`, `setPlaybackTimeMappers`, `bindToWidget`,
`inputs()`, `bindings()`, `setBinding` (`WidgetBindings::kNewSlot` appends), `canBind` (what the side panel's series
chooser offers), `removeBinding`, `settingsDeclarations()`, `settingsJson()`, `applySettings`, `isMissing()` /
`missingReason()`, `displayedTopics()`, `pendingDemands()`; signals `bindingsChanged`, `inputsChanged`,
`workspaceChanged`. The picker, the side panel (wire table, settings form) and the icon belong to `pj_app`.

## Tests

`pj_custom_widgets_tests` drives the dock against an in-process plugin (`tests/in_process_widget_plugin.h`: a static
vtable on the SDK base class that creates a real `QLabel`, records every call in `g`, and has switches for each error
path). `tests/test_widget_plugin.cpp` is a real Qt plugin library for the end-to-end runs, registered from
`pj_app/CMakeLists.txt` through `tests/E2E.cmake` (the `pj_app` target must exist). Every end-to-end variant is Linux
only: the runs redirect the user profile through `XDG_CONFIG_HOME` / `XDG_DATA_HOME` / `HOME`, which only Linux honours.
`tests/run_custom_widget_e2e.cmake` documents the variants, including how `after_kill` hard-kills a run and checks the
crash record.

## Known gaps

- A plugin call that never returns freezes the GUI thread: calls are synchronous and the host has no watchdog or
  timeout.
- A SIGSEGV inside the plugin takes the whole process down; the active-widgets record only reports it at the next start.
- The end-to-end runs are Linux only until the app has a profile-directory override.
