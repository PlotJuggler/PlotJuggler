# Mosaico Cloud Server

Mosaico is a server for robotics recordings, browsed over Apache Arrow Flight. The "Mosaico Cloud Server" toolbox
(`toolbox-mosaico`, desktop only) lists its sequences and topics and downloads a time range of chosen topics into
PlotJuggler. It needs the **Arrow IPC Parser** (`arrow-parser`) for both scalar and object topics.

## Connect

1. Sources panel → Cloud (`tabCloud`) → pick "Mosaico Cloud Server" in the combo (`comboCloud`) → "Open cloud
   browser" (`buttonCloudOpen`).
2. "Server:" (`comboUri`, editable): type a URI such as `grpc+tls://host:port`, or pick one from the history; a demo
   server is listed.
3. Credentials, when the server needs them: "Cert / API Key…" (`buttonCert`) → "Server Certificate / API Key":
   "Certificate:" (file), "API Key:", "Allow insecure / plaintext connection", OK. Saved per server; without a saved
   key the `MOSAICO_API_KEY` environment variable is used.
4. "Connect" (`buttonConnect`). Success and failure arrive as notifications ("Mosaico connection failed: ...").

## Browse and download

1. **Sequences** (`seqTable`, columns Name / Date / Size): filter with "Filter…" (".*" toggles regular expressions),
   narrow by date with the date picker ("All", "Past 24h", "Last 7 Days", "Last Month", custom).
2. **Metadata Filter:** a query of the form `<key> <op> <value>` in the query bar (`lua_queryBar`), clauses joined
   with `and`, or build it from the "Key" / "Op" / "Value" combos and "+". Operators `==`, `~=`, `<`, `>`, `<=`,
   `>=`; `and`, `or`, `not`.
3. **Topics** (`topicTable`): select one or more topics of the chosen sequence. "Info" shows their metadata and
   Arrow schema.
4. Narrow the time window on the range slider (`rangeSlider`).
5. "Download" (`buttonFetch`). "Cancel" (`buttonCancel`) stops it. The panel closes after a successful import
   ("Imported N/M topics"); the download becomes one dataset named after the sequence.

## Agents

- **MCP:** UI only for connecting and downloading. Once downloaded, the dataset is ordinary data: `list_topics`,
  `read_series`, `plot_tab`, `scene_view` work on it.
- A layout saved bound to its sources restores a Mosaico download from the source cache when the cache holds it
  (Preferences → Recording, "Capture downloads for offline restore:"), without contacting the server.
- **Gotchas:** without the Arrow IPC Parser the topics do not decode.
