# pj_custom_widgets requirements

What the module must do. How it is built is in [../CLAUDE.md](../CLAUDE.md).

## Panel

- R1. A Custom Widget panel hosts one widget type of one Widget plugin library, identified as `plugin_id/type`. Each open panel is an independent plugin instance.
- R2. The widget's QWidget is created by the plugin as a child of the panel and fills it. Only the opaque pointer crosses the ABI.
- R3. A panel whose widget cannot be created shows a placeholder that states the reason, and keeps its saved state so the widget returns when the cause is fixed. Causes: library not installed or disabled, no such widget type, Qt build of the library not compatible with the host (the plugin's `create_qwidget` is never called), creation failed (`create_qwidget` returned null, `bind` failed), an error during creation (R18).

## Inputs and wiring

- R4. The widget declares named inputs (name, label, unit, kind number/text/object, `multiple`). The user wires series to them; the plugin never names a topic.
- R5. A drop goes to the first compatible input with no wire; if none is empty, to the first compatible `multiple` input (a new slot); otherwise it replaces the wire of the first compatible input. Several series in one drop do not replace each other on a normal input.
- R6. A number input accepts plottable scalar series, a text input accepts string series, an object input accepts object topics.
- R7. The input list is re-read after creation, after every `load_config` and when the widget announces a change. Wires are kept for names that survive, dropped for the others; new inputs start empty. A kind change under the same name drops the wire. When the list cannot be read (the widget reports a failure, or returns a list the host refuses), the wires are not reconciled: the panel is disabled as in R18 and its layout element keeps every wire.

## Values

- R8. On every tracker tick of a visible panel the host resolves, per wire, the latest sample at or before the tracker time (zero-order hold) on the display time axis of the wire's dataset, and calls `on_update` with one value per wire. A wire with no sample, or not resolved yet, is delivered with kind NONE. Every value, NONE included, carries the display name of the wired series.
- R9. `on_update` is not called when no wired sample changed since the previous call. A change of bindings, settings, inputs or catalog content forces the next delivery; a display offset change delivers only when it moves a wired dataset. A panel receives one delivery right after its widget is created, wired or not.
- R10. History is not pushed: the widget asks for it through `pj.widget_host.v1` (`query_history`).

## Settings

- R11. The host keeps the full settings JSON of the panel, sends it whole to `load_config` on every change and on restore, and saves it in the layout. What `load_config` receives holds every key the widget type declares in its manifest `settings`, with the manifest `default` for a key the user never set, then the user's values, then the widget's own `save_config` state (a colliding state key wins); a new panel therefore starts with the full defaults. A rejection by the widget does not discard what was stored.

## Persistence

- R12. The panel saves as `<CustomWidget plugin type plugin_version>` with one `<input>` per wire and a `<config>` CDATA. Each wire stores its dataset identity (id, source, path) and is re-resolved at restore. Like a plot curve, a layout file stores the path relative to the layout and keeps no id it cannot validate, and a generic layout keeps none of the three.
- R13. A wire that does not resolve at restore is kept and retried when the catalog adds items; it is saved unchanged meanwhile.
- R14. Loading a `<CustomWidget>` element always succeeds, including when the plugin is missing; the element is then written back verbatim.

## Demand

- R15. A panel holds one demand reference per distinct (dataset, topic) of its resolved wires, updated whenever its wires change and released when the panel is destroyed. A pending wire (restored before its topic exists) holds a reference on its topic too, through the shell's pending-display binder, released exactly once: when the wire resolves or goes away, when the panel is destroyed, or when the pending bindings are cleared. Resolved or pending, a wire's topic is subscribed on a demand-driven source.

## Lifetime

- R16. The plugin instance and the QWidget it created are destroyed together, synchronously, with the widget detached from its parent first. This happens when the dock discards the panel (`IDataWidget::aboutToBeDiscarded`, required because the panel's own deletion is deferred) and in the panel's destructor, which covers window teardown: the window destroys its docks before the session.
- R17. Nothing in the host schedules deferred deletion for an object created by a plugin.

## Errors and crashes

- R18. Every call into the plugin runs inside a catch. An exception that reaches the host (one thrown while the host converts what the plugin returned; the ABI slots are noexcept, so one thrown inside the plugin never crosses back) disables that panel: the plugin instance is destroyed at once, the placeholder says "This widget was disabled after an error: <what>", an error diagnostic is reported, the panel calls its plugin no more and holds no demand, and its layout element is kept as the panel was. Choosing the widget again or reloading the layout re-enables it. A rejection through the contract's own error channel (`load_config` refusing settings) is not an error of this kind. A crash (SIGSEGV) cannot be caught in the process; R19 covers it.
- R19. Before its first call into a plugin instance the panel records `plugin_id/type` in the active-widgets record of its process (`pj_runtime`'s `ActiveWidgetsMarker`, one file per process id) and removes it once that instance is destroyed; a clean exit deletes that process's record and no other. At the next start the shell reviews only the records of processes that are no longer alive (an instance still running is never reported as a crash, nor its record erased), reports their widgets together and, when someone can answer, offers to disable those plugins before any layout is restored.

## Out of scope here

The picker, the side panel (wire table, settings form), the icon, the freeze watchdog and publishing to the robot.
