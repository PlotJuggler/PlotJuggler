# Packaging

Run packaging commands from the repository root after building the application.

| Format | Guide | Command |
| --- | --- | --- |
| Linux AppImage | [AppImage and Docker builds](appimage/README.md) | `packaging/appimage/build_appimage.sh` |
| Debian / Ubuntu | [Debian package](deb/README.md) | `packaging/deb/build_deb.sh` |
| Windows installer | [Qt Installer Framework](installer/README.md) | `.\packaging\installer\build_windows_installer.ps1` |

AppImages and `.deb` files are written into their respective packaging folders.
The Debian builder consumes `build/AppDir`, produced by the AppImage builder.
The Windows installer writes to `-OutDir` (the current directory by default).

`packaging/deb/publish_apt.sh` pushes a built `.deb` to the apt repository that
users add to `sources.list`; see [Distribution](deb/README.md#distribution).
