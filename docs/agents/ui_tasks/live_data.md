# Live data: streaming, pause, Record, cache

Sources and the plugin each needs: [formats.md](../formats.md), "Live sources". Concepts: [concepts.md](../concepts.md),
"Streaming" and "Record and the source cache".

## Start a stream

- **UI:** Sources panel → "Stream" (`tabStream`) → pick the source in the combo (`comboStreaming`) → "Start
  streaming" (`buttonStreamingOptions`, the "+"). The source's own dialog opens (address, topics, encoding...); OK
  starts it. A new live dataset appears in the Datasets panel and grows.
- **Show data:** drag topics or fields onto a plot as with files. With sources that subscribe on demand (PlotJuggler
  Bridge, ROS 2...) an advertised topic reads "Not subscribed. Drop it into a view to start streaming." until shown.
- **MCP:** `start_stream` (`source` from `get_status` `stream_sources`; no options: the settings the user last
  confirmed in that source's dialog; `config`; `ask_user: true` opens the dialog). Topics appear as the source
  advertises them: poll `list_topics`. Ask the user before starting a stream they did not mention.
- **Gotchas:** the encoding chosen in a UDP, MQTT or ZeroMQ dialog must match a parser that is installed. In the
  browser only `wss://` bridges connect from an `https://` page.

## Keep a topic streaming without showing it

- **UI:** right-click the topic in the Datasets panel → "Force topic streaming" (again: "Stop forced streaming").
  Only for live sources that subscribe per topic, and only with a single row selected.
- **MCP:** UI only.

## Pause and resume

- **UI:** Stream page → "Pause streaming" (`buttonStreamingPause`; then "Resume streaming"). The plots freeze; data
  keeps arriving into a side buffer and merges on resume.
- **MCP:** UI only. `playback` `pause` pauses playback, not the stream.

## How much history is kept

- **UI:** Stream page → "Streaming buffer length (seconds)" (`streamingSpinBox`, suffix " sec"). It applies to every
  live stream when editing ends; older samples are dropped.
- **MCP:** UI only. Ask the user to raise it before a long capture they want to analyse.
- **Gotchas:** the buffer is saved only in a layout bound to its streams. A layout never shrinks the buffer of a
  stream that was already running.

## Stop a stream

- **UI:** there is no Stop button on the Stream page. Right-click the live dataset → "Remove" stops the stream and
  removes its data; "Remove all Datasets" stops every stream.
- **MCP:** `stop_stream` (`dataset`) stops it and **keeps** the data; `job_kill` on the stream's job does the same.

## Record what is streaming

- **UI:** Stream page → record button (`buttonStreamingRecord`, tooltip "Record the subscribed topics of every active
  stream, one MCAP file per stream"); press again ("Stop recording") to finish. While recording a label shows
  elapsed time, bytes written and dropped messages. The toast "Recording saved to ..." names the folder.
- **Files:** `<Recordings folder>/pj_<UTC yyyyMMdd_HHmmss>/<source>.mcap`. Set the folder on File →
  Preferences... → Recording → "Recordings folder:". Open a recording later with File → Open Data File... like any
  MCAP.
- **MCP:** UI only.
- **Gotchas:** desktop only. Only subscribed topics are recorded. A stream started after Record was pressed is not
  recorded. Sources that write decoded samples directly (Dummy, LSL) cannot be recorded ("Nothing to record: ...").
  When the disk falls behind, the largest queued messages are dropped; "Write queue per source (MiB):" sets the
  queue.

## Source cache (cloud downloads)

- **UI:** File → Preferences... → Recording: "Source cache folder:", "Source cache budget (GB):" (both apply at the
  next launch) and "Capture downloads for offline restore:" (applies at once). There is no other control: a layout
  bound to a cached download restores it from the cache automatically.
- **MCP:** UI only.
- **Gotchas:** two PlotJuggler windows restoring the same download at once: the second reports "another PlotJuggler
  instance is currently downloading this request" and should be reloaded once the first finishes.

## Follow the live edge, replay

- While streaming, playback follows the live edge of the stream launched last; when it stops, the last-launched
  stream still running takes over.
- **MCP:** `wait_for` waits for a live series to cross a threshold; `playback` `seek` moves the cursor.
