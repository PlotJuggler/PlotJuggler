# PlotJuggler 4 — Debian/Ubuntu packaging

Builds `plotjuggler4_<version>_amd64.deb` by repackaging the AppDir that
[`packaging/appimage/build_appimage.sh`](../appimage/README.md) already produced. Nothing
is compiled here.

## Build

```bash
./build.sh                                        # build the app
packaging/appimage/build_appimage.sh --plugins-registry     # populate build/AppDir
packaging/deb/build_deb.sh                                  # -> packaging/deb/plotjuggler4_<ver>_amd64.deb
```

Useful flags:

| Flag | Purpose |
|---|---|
| `--appdir <path>` | Package an AppDir somewhere other than `build/AppDir`. |
| `--binary <path>` | Substitute the app binary (release CI ships one stamped `PJ_INSTALLATION=deb`) and restore the deployed `RUNPATH`. Needs `patchelf`. |
| `--output-dir <dir>` | Where to write the `.deb` (default: this folder). |
| `--commit-hash <hash>` | Mark a non-tag build: version becomes `<ver>~<hash>`. |

## What kind of package this is

A **bundled ("vendor") package**, like Chrome, VS Code and Zoom: the whole Qt 6
and Conan runtime is installed under `/opt/plotjuggler4`, with only a launcher
in `/usr/bin` plus a desktop entry and icon. `lintian` reports bundled
libraries; that is inherent, not a defect to fix.

A distro-native package is not achievable: `versions.env` pins Qt 6.11.1 and no
Ubuntu release ships it, and much of the Conan graph (mcap, cloudini, nanoarrow,
nanocdr, backward-cpp) is not packaged by Debian at all.

Supported floor: **Ubuntu 22.04+ / Debian 12+** (glibc 2.35), matching the
jammy container the release build runs in.

## Layout

```
/opt/plotjuggler4/bin/          plotjuggler4, qt.conf
/opt/plotjuggler4/bin/3rdparty/retro/
                                separately-licensed retro payload, when the
                                AppDir was built with --retro-wad
/opt/plotjuggler4/lib/          Qt 6, Conan closure, CPython stdlib,
                                plotjuggler/plugins/<id>/ (bundled extensions)
/opt/plotjuggler4/plugins/      Qt platform + imageformat plugins
/opt/plotjuggler4/translations/ Qt translations
/opt/plotjuggler4/share/doc/    licenses of the bundled libraries
/usr/bin/plotjuggler4           launcher (packaging/deb/plotjuggler4.wrapper.in)
/usr/share/applications/        desktop entry
/usr/share/icons/hicolor/…      icons (256px PNG + scalable SVG)
```

`bin` and `lib` stay siblings because the app resolves its bundled-plugin seed
as `applicationDirPath()/../lib/plotjuggler/plugins`. Those plugins are seeded
into a per-user writable directory at startup, so marketplace install and
uninstall keep working against a root-owned, read-only `/opt`.

**Only the desktop entry and icon may be installed into `/usr/share`.**
linuxdeploy fills `AppDir/usr/share/doc/<distro-pkg>/` with the copyright file
of every bundled system library; inside an AppImage those paths are private, but
in a `.deb` they would collide with the real `libglib2.0-0t64`,
`libkrb5support0` and ~36 other packages. They stay under `/opt`.

## Bundled plugins (seeding)

The registry plugins under `/opt/plotjuggler4/lib/plotjuggler/plugins/<id>/`
are a **seed source, never a load path** — identical to the AppImage. On every
startup `ExtensionCatalogService::seedBundledPlugins()` syncs them into the
per-user extensions dir (`~/.local/share/PlotJuggler/PlotJuggler4/extensions`):
copy when absent, staged refresh when the bundled version is semver-newer,
never a downgrade (a marketplace update above the bundled version wins). The
app then loads only from the user dir, so a root-owned read-only `/opt` is by
design — marketplace install/uninstall never writes there. Bundled ids count
as *core* extensions: at or below the bundled version they cannot be
uninstalled; one upgraded above it can be, and the next launch re-seeds the
bundled version ("downgrade to bundled").

Consequences specific to the `.deb` lifecycle: a package **upgrade** refreshes
each user's copies on their next launch (per-user, on demand — no maintainer
script touches home directories); package **removal** leaves the seeded
per-user copies behind (`postrm` deliberately spares user data), and being
self-contained they keep loading like any marketplace-installed extension.

The `ros2-topic-subscriber` bundle needs the same special treatment here as in
the release glibc audit: its per-distro inners under `dist/<distro>/` bind to
the user's *sourced* ROS 2 installation by design, so `build_deb.sh` excludes
that subtree from the `Depends` scan — otherwise librclcpp & co. would become
hard package dependencies no desktop machine satisfies.

## Dependencies

`Depends` is derived on every build from the **staged package payload** (after
the optional `--binary` substitution, so what is scanned is exactly what
ships): each ELF's `DT_NEEDED` minus the sonames the package itself provides
(ELF files only — a stray non-ELF named like a library cannot satisfy a real
dependency), mapped through the `SONAME_PKG` table in `build_deb.sh`. **An
unmapped soname is a hard error** — a dependency bump that introduces a new
external library fails the build instead of shipping a package that cannot
start on a clean machine. When that fires, add the soname to the table after
confirming the package name exists on Ubuntu 22.04.

`ca-certificates` is added unconditionally: the wrapper's gRPC CA export and
Qt's TLS (marketplace downloads, update check) need the system bundle, a
non-ELF requirement the `DT_NEEDED` scan structurally cannot see.

## Testing

`smoke_test.sh` installs the package in a bare container and runs it:

```bash
docker run --rm -v "$PWD/deb:/pkg:ro" ubuntu:22.04 sh /pkg/smoke_test.sh
```

It checks what a static dependency list cannot: `apt install` succeeding proves
the declared `Depends` resolve, but a too-new glibc or libstdc++ version symbol
is burned into the ELF and no package name expresses it. The test therefore,
in order: asserts the CA bundle landed; runs an **`ldd` closure gate** over
every shipped ELF *before* Xvfb is installed, so the linkable closure is proven
against the declared `Depends` alone (Xvfb pulls X11 libraries that would
otherwise mask a missing GUI dependency); runs `--selftest-python`
(display-free, and proves the wrapper's `PYTHONHOME` reaches the bundled
stdlib); and finally a real GUI startup under Xvfb — the bundled Qt ships only
the `xcb` platform plugin, so an X server is required, and this phase is what
covers `dlopen`'d libraries that `ldd` cannot see.

Release CI runs it on `ubuntu:22.04` and `ubuntu:24.04`, and the `.deb` is
attached to a GitHub Release only after it passes. The same container then
re-runs the runtime checks against the AppImage (`packaging/appimage/smoke_test.sh`),
using the libraries this package's `Depends` pulled in.

## Supported distros

amd64 with glibc ≥ 2.35 and dpkg: Ubuntu 22.04 LTS and newer, Debian 12
"bookworm" and newer, and their derivatives (Mint 21+, Pop!\_OS 22.04+, KDE
neon, Zorin 17+, elementary 7.1+). Every `Depends` entry has existed unchanged
since Ubuntu 22.04 / Debian 12 (the Ubuntu 24.04 `t64` rename touched none of
them). Older releases fail at run time on the glibc floor; non-dpkg distros
(Fedora, Arch, openSUSE) should use the AppImage.

## Distribution

The package is a **release asset**, not an apt repository — install it with
`apt install ./plotjuggler4_<version>_amd64.deb`. `apt upgrade` therefore does
not update PlotJuggler; the in-app update check behaves as it does for the
AppImage. Publishing a signed apt repository is a separate piece of work.
