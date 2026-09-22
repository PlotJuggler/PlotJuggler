# PlotJuggler 4 — Windows installer (Qt Installer Framework)

A self-contained Windows installer for PJ4, built with the **Qt Installer
Framework** (IFW) — the same mechanism PlotJuggler 3 uses. Output is a single
offline `.exe`: a wizard that installs the app with a Start-Menu + Desktop
shortcut and a maintenance/uninstall tool. Tag-triggered releases name it
`PlotJuggler-<version>-Windows-x64.exe` (`-CleanReleaseName`, matching the
Linux AppImage's plain `PlotJuggler-<version>-<arch>.AppImage`); other builds
default to `PlotJuggler-<version>-Windows-x64.<short-commit>.exe`, stamped
with the built commit's short hash so otherwise-identical-looking dev
artifacts stay distinguishable.

There is **no portable-zip path** — the deployment target is this installer.

## Layout

```
packaging/installer/
├── config.xml                                     # installer identity — version tokenised as __PJ_VERSION__
├── build_windows_installer.ps1                    # staging + binarycreator orchestrator (run on Windows)
└── packages/
    └── io.plotjuggler.application/
        └── meta/
            ├── package.xml                        # component metadata — version tokenised
            ├── installscript.qs                   # shortcuts, per-user install, custom target-dir page
            ├── targetwidget.ui                    # the target-dir wizard page
            ├── license_mpl.txt                    # MPL-2.0 (PlotJuggler)
            └── license_lgpl.txt                   # LGPL (Qt)
```

The **source tree is templated** (`__PJ_VERSION__` / `__PJ_RELEASE_DATE__`
placeholders) and never mutated by a build: the script renders the XMLs into
a scratch staging directory under `%TEMP%\pj4-installer-stage` and points
`binarycreator -p` at that staged `packages/` tree.

### Installed layout (prefix tree)

The staged payload mirrors the prefix layout the app discovers plugins from —
the same convention the Linux AppImage uses (`bin` beside `lib`):

```
<TargetDir>/                         # @ApplicationsDirX64@/PlotJuggler4
├── bin/
│   ├── PlotJuggler4.exe             # + Qt DLLs, Qt plugins, MSVC runtime, FFmpeg, CPython (+ Lib/, DLLs/, ._pth)
│   └── platforms/ …
└── lib/plotjuggler/plugins/         # verified published plugins; app resolves bin/../lib/plotjuggler/plugins
    └── <registry-id>/
        ├── manifest.json
        └── *_plugin.dll
```

The app scans `bin/../lib/plotjuggler/plugins` (via `applicationDirPath`), so
bundled plugins MUST live there — dropping them next to the exe is not scanned.
Plugins load their Qt/FFmpeg/CPython DLLs from `bin/` (the exe's directory, which
Windows searches at load time); `windeployqt --dir bin` puts any plugin-only Qt
modules there too.

## Prerequisites on the Windows build host

- **PJ4 already built** (RelWithDebInfo) with CPython enabled:
  ```powershell
  conan install . --output-folder=build --build=missing `
      -s build_type=RelWithDebInfo -s compiler.cppstd=20 `
      -o "cpython/*:shared=True" -s "cpython/*:build_type=Release"
  cmake --preset ... ; cmake --build build --config RelWithDebInfo
  ```
  The `cpython/*:shared=True` override matches Windows CI and produces
  `python3XX.dll` — the local default is static on MSVC and the installer
  would then fail to find the runtime.
- **Qt 6.11.1 msvc2022_64** — for `windeployqt.exe`. Auto-detected under `.qt`
  (the `scripts/install_qt6.sh` / aqt layout); otherwise pass `-QtDir`.
- **Qt Installer Framework tools** — for `binarycreator.exe`. Install via the Qt
  Maintenance Tool, or `aqt install-tool --outputdir .qt windows desktop tools_ifw`.
  Auto-detected next to `-QtDir` (aqt `.qt\Tools`) and under `C:\Qt\Tools`.
- **Network access to the plugin registry and the selected release artifacts.**
  Plugins are downloaded from `pj-plugin-registry` and their registry SHA-256 is
  mandatory. Use `-SkipPlugins` for an app-only installer, or point
  `-PluginRegistryUrl` at a local registry mirror.

## GitHub Actions Conan repository

The Windows CI and release workflows first resolve Conan 2 packages through the
shared `plotjuggler-conan` repository at
`https://plotjuggler.jfrog.io/artifactory/api/conan/plotjuggler-conan`. Configure
these repository-level GitHub Actions settings before dispatching a build:

- Variable `JFROG_USER`: the username shown by the JFrog profile menu (often the
  account email address). An Actions secret with the same name is also accepted
  for organizations that provision both JFrog values as secrets.
- Secret `JFROG_TOKEN`: a scoped JFrog identity/access token with read and
  deploy/write access to the repository.

These may be repository- or organization-level settings. Organization-level
settings must grant `PlotJuggler/PJ4` access.

Trusted push and manual jobs authenticate, install with
`-r=plotjuggler-conan -r=conancenter`, and publish only missing Conan revisions
to JFrog. This requires one local Conan repository in JFrog; an Artifactory
remote proxy and virtual repository are not required. Pull-request and
noncanonical-repository jobs never reference the JFrog credential; Windows CI
uses public ConanCenter for those jobs and never uploads. Release jobs require
the JFrog settings and fail early with an actionable error when either is
missing. No workflow archives or restores the complete `~/.conan2` directory.

## Usage

From the repo root, after a Windows build of the app — a plain run downloads the
curated published plugins and produces the full installer:

```powershell
.\packaging\installer\build_windows_installer.ps1
```

`-QtDir` / `-IfwDir` are auto-detected (`.qt` first, then `C:\Qt`); pass them only
if Qt/IFW live elsewhere. `-Version` defaults to `PJ_APP_VERSION` from repo-root
`versions.env`; override with `-Version <x.y.z>`.

The script:

1. Locates the built `plotjuggler4.exe` / `pj_app.exe` under `-BuildDir`; stages
   it as `bin\PlotJuggler4.exe` in a scratch tree under `%TEMP%`.
2. Resolves every id in `-PluginIds` against `-PluginRegistryUrl` for
   `windows-x86_64`, downloads the published marketplace ZIP, verifies its
   mandatory SHA-256, checks the package manifest id/version, and unpacks it under
   `lib\plotjuggler\plugins\<registry-id>`. A missing or malformed whitelisted
   plugin fails the release; nothing is silently skipped.
3. Runs `windeployqt` over the exe **and every bundled plugin DLL** (`--dir bin`,
   so plugin-only Qt modules land in `bin`).
4. Copies the FFmpeg + CPython runtime DLLs from the Conan cache (both the
   downloaded `~/.conan2/p/<hash>/p/bin` and built-from-source
   `~/.conan2/p/b/<hash>/p/bin` layouts; everything else in the graph is static).
5. Copies the CPython `Lib/` + `DLLs/` (from the DLL's own dir — the Conan package
   nests them next to `python3XX.dll` on Windows) and either reuses the package's
   `python3XX._pth` or synthesizes one, so the embedded interpreter resolves its
   stdlib relative to the DLL instead of the build-machine `PYTHONHOME` baked into
   the binary.
6. Runs the staged app's `--validate-plugins` mode. This loads every downloaded
   DLL through the real runtime catalog and checks the embedded C-ABI manifest,
   instance/capability probe, dialog-vtable contract, exact registry id/version,
   and absence of non-whitelisted plugins. Any failure stops before packaging.
7. Renders `config.xml` / `package.xml` into the stage tree (version +
   release-date tokens substituted).
8. Runs `binarycreator --offline-only` against the stage.

Options: `-SkipPlugins` (fast core-only installer), `-PluginIds a,b,c` (override
the curated registry ids), `-PluginRegistryUrl <url-or-json-path>` (registry
source), `-PluginPlatform <key>` (defaults to `windows-x86_64`), and
`-CleanReleaseName` (name the output `PlotJuggler-<Version>-Windows-x64.exe`
instead of the commit-stamped default — used by release CI on tag builds).

The default whitelist bundles these 18 published plugins:
`mcap-loader`, `csv-loader`, `parquet-loader`, `ulog-loader`, `mp4-loader`,
`pointcloud-3d-loader`, `dummy-streamer`, `foxglove-bridge`,
`plotjuggler-bridge`, `webrtc-client`, `ros-parser`, `protobuf-parser`,
`json-parser`, `data-tamer-parser`, `toolbox-quaternion`,
`toolbox-transform-editor`, `toolbox-mosaico`, and `arrow-parser`.

## Install-time behaviour

- **Per-user install by default** — the wizard offers `%LOCALAPPDATA%\PlotJuggler4`
  and does not call `gainAdminRights()`. Corporate machines that block UAC
  prompts install without an escalation dialog. Admin-write locations still
  work; the user just picks one.
- **Replace-existing** — if the target directory already contains a PJ4
  install, the wizard **asks before** running `maintenancetool purge`. The
  earlier flow purged silently.
- **Shortcuts** — Start Menu and Desktop link to `PlotJuggler4.exe`.

## Remote updates (maintenance tool)

Off unless a release opts in. `build_windows_installer.ps1` takes two
parameters that are set together:

| Parameter | Effect |
|---|---|
| `-UpdateUrl <url>` | Injects a `<RemoteRepositories>` entry into the staged `config.xml`, so the installed maintenance tool gains a "Update components" path pointing at `<url>`. |
| `-RepoOutDir <dir>` | Also runs `repogen` over the *same* staged packages tree, writing an IFW update repository there. |

They are generated from one staging run on purpose: the offline installer and
the update repository can then never describe different payloads for the same
version.

`publish_update_repo.ps1` uploads that repository to the R2 bucket that also
hosts the [apt repository](../deb/README.md#publishing-to-the-apt-repository),
under the path of the update URL (`windows/` in production). Release CI runs
both from `windows-release.yml`, gated on the repository variable
`PJ4_WINDOWS_UPDATE_URL` and using the same R2 secrets as the Linux side.
Without that variable the installer is built exactly as before and nothing is
published.

### Rehearsing on a staging prefix

A tag is the only production trigger, so the path is rehearsed with a manual
run whose `update_url` input points somewhere users never look (the workflow
refuses the production URL):

```bash
gh workflow run windows-release.yml --ref <branch> \
  -f update_url=https://updates.plotjuggler.io/windows-staging -f version=4.0.1
# after it finishes, with <run-id> of the first run:
gh workflow run windows-release.yml --ref <branch> \
  -f update_url=https://updates.plotjuggler.io/windows-staging -f version=4.0.2 \
  -f update_check_from_run=<run-id>
```

The first run proves `repogen`, the `Updates.xml` assertion and the upload.
The second publishes 4.0.2, then its `update-check` job installs the first
run's 4.0.1 installer headlessly on a Windows runner and updates it through the
maintenance tool's command line (`check-updates`, `update`), asserting that
`components.xml` reports 4.0.2 afterwards. No Windows machine is needed. The
two runs share a concurrency group, so start the second only once the first
has finished. Delete `windows-staging/` from the bucket afterwards.

The maintenance tool runs the component's `installscript.qs` too, so anything
there that only makes sense for a first install (the target-directory page,
the replace-existing-installation prompt) must return early unless
`installer.isInstaller()`; during an update, `TargetDir` is the installation
being updated.

`config.xml` itself stays clean — the block is injected at render time rather
than templated, so a build without `-UpdateUrl` is byte-identical to one from
before this existed. An empty `<Url/>` would leave every maintenance tool
polling nothing on each launch.

### Upload order

The maintenance tool reads `Updates.xml` and then fetches the component
archives it names, so the publisher uploads archives first and `Updates.xml`
last. A tool polling mid-publish then sees an index older than the payload,
never one promising an archive that has not landed. (The apt publisher follows
the same rule for the same reason.)

### Three things to know before relying on it

- **The URL is compiled in at install time.** Only machines that installed a
  build already carrying `-UpdateUrl` can ever check for updates — turning
  this on does nothing for anyone who installed an earlier release. That is an
  argument for enabling it sooner rather than later.
- **An installer installs the newest published version.** With
  `<RemoteRepositories>` set, IFW consults the update repository while
  installing, so running an older installer after a newer release is published
  installs the newer release (downloaded from the repository, checksummed).
  The rehearsal's `update-check` pins the fetch with `--set-temp-repository`
  to reproduce a machine that installed earlier.
- **No delta updates.** The app is a single component of several hundred MB,
  so an update re-downloads the whole payload. The `.deb` behaves the same
  way; do not expect Chrome-style patching.
- **Integrity rests on TLS.** IFW has no signing mechanism comparable to the
  apt repository's detached GPG signature, so the trust anchor is the HTTPS
  certificate of the update host. Authenticode-signing the installer and
  maintenance tool is the usual complement — see the code-signing gap below.

## Known gaps (first version)

- **No code signing.** The `.exe` is unsigned, so SmartScreen warns on first
  run ("More info" → "Run anyway").
- **ROS 2 subscriber.** The ros2-stream build system is Linux-only, so ROS
  streaming is absent from the Windows installer for this release.
- **Assistant Agent.** `toolbox-assistant-agent` is published for Windows, but
  its backends drive the `claude`/`codex` CLI through POSIX-only code, so it
  is bundled on Linux only (AppImage/.deb) and left out of the whitelist here.
