# Compile in Linux

On Ubuntu (20.04/22.04), the dependencies can be installed with the command:

```shell
sudo apt -y install qtbase5-dev libqt5svg5-dev libqt5websockets5-dev \
      libqt5serialport5-dev libqt5opengl5-dev libqt5x11extras5-dev libprotoc-dev libzmq3-dev \
      liblz4-dev libzstd-dev python3-dev
```

`python3-dev` enables the embedded Python custom-function feature. If you
omit it, PlotJuggler still builds — only the Python tab in the function
editor will be disabled (Lua remains available).

To enable the Parquet, CSV-toolbox and Mosaico Flight plugins, also install
Apache Arrow (Ubuntu 22.04 doesn't ship it, so use Apache's APT repo):

```shell
wget https://packages.apache.org/artifactory/arrow/$(lsb_release --id --short | tr 'A-Z' 'a-z')/apache-arrow-apt-source-latest-$(lsb_release --codename --short).deb
sudo apt install -y -V ./apache-arrow-apt-source-latest-$(lsb_release --codename --short).deb
sudo apt update
sudo apt install -y -V libarrow-dev libarrow-flight-dev libparquet-dev
```

On Fedora (42):

```shell
sudo dnf install qt5-qtbase-devel qt5-qtsvg-devel qt5-qtwebsockets-devel \
      qt5-qtserialport-devel qt5-qtx11extras-devel python3-devel
```

Clone the repository into **~/plotjuggler_ws**:

```shell
git clone https://github.com/PlotJuggler/PlotJuggler.git ~/plotjuggler_ws/src/PlotJuggler
cd ~/plotjuggler_ws
```

Then compile using cmake (qmake is NOT supported):

```shell
cmake -S src/PlotJuggler -B build/PlotJuggler -DCMAKE_INSTALL_PREFIX=install
cmake --build build/PlotJuggler --config RelWithDebInfo --target install
```

## Optional: build with Conan

If you want to use [conan](https://conan.io/) to manage the dependencies,
follow this instructions instead.

```shell
conan install src/PlotJuggler --output-folder build/PlotJuggler \
      --build missing -pr:b=default -s build_type=RelWithDebInfo

export CMAKE_TOOLCHAIN=$(pwd)/build/PlotJuggler/conan_toolchain.cmake

cmake -S src/PlotJuggler -B build/PlotJuggler \
      -DCMAKE_TOOLCHAIN_FILE=$CMAKE_TOOLCHAIN  \
      -DCMAKE_INSTALL_PREFIX=install \
      -DCMAKE_POLICY_DEFAULT_CMP0091=NEW \
      -DBUILDING_WITH_CONAN=ON

cmake --build build/PlotJuggler --config RelWithDebInfo --target install
```

## Deploy as an AppImage

The recommended path is the Docker-based builder, which produces an AppImage
that runs on any Linux distro regardless of the host's Python version (it
bundles the Python 3.10 stdlib that the embedded interpreter needs):

```shell
./appimage/build_in_docker.sh
```

This builds an Ubuntu 22.04 image once (cached afterwards), then runs the
in-container build script which calls `cmake` + `linuxdeploy` and copies
the matching Python stdlib + an apprun-hook into the AppDir. The output
lands at `./PlotJuggler-<arch>.AppImage`.

If you prefer to run the same script directly on an Ubuntu 22.04 host
(skipping Docker):

```shell
./appimage/build_appimage.sh
```

The plain manual `linuxdeploy` invocation still works but produces an
AppImage that crashes (`init_fs_encoding` segfault) on hosts whose system
Python doesn't match the builder's. Avoid unless you've manually disabled
the Python feature with `-DPython3_FOUND=OFF`.

> The root-level `Dockerfile` (`docker buildx build -o . .`) is the legacy
> path; it doesn't bundle Python and will produce a non-portable AppImage
> identical to the one shipped in 3.17.0. Use `appimage/build_in_docker.sh`
> instead.

# Compile in macOS

On macOS, the dependencies can be installed using [brew](https://brew.sh/) with the following command:

```shell
brew install cmake qt@5 protobuf mosquitto zeromq zstd git-lfs
```

If a newer version of qt is installed, you may need to temporarily link to qt5

```shell
brew link qt@5 --overwrite
# brew link qt --overwrite  # Run once you are done building to restore the original linking
```

Add CMake into your env-vars to be detected by cmake

```shell
echo  'QT_HOME=$(brew --prefix qt@5) \
export CPPFLAGS="-I $QT_HOME/include" \
export PKG_CONFIG_PATH="$QT_HOME/lib/pkgconfig" \
export LDFLAGS="-L$QT_HOME/lib"' >> $HOME/.zshrc
```

If you don't want to permanently add them into your main file, you can try by just exporting locally in the current terminal with:

```shell
QT_HOME=$(brew --prefix qt@5)
export CPPFLAGS="-I $QT_HOME/include"
export PKG_CONFIG_PATH="$QT_HOME/lib/pkgconfig"
export LDFLAGS="-L$QT_HOME/lib"
```

Clone the repository into **~/plotjuggler_ws**:

```shell
git clone https://github.com/PlotJuggler/PlotJuggler.git ~/plotjuggler_ws/src/PlotJuggler
cd ~/plotjuggler_ws
```

Then compile using cmake:

```shell
cmake -S src/PlotJuggler -B build/PlotJuggler -DCMAKE_INSTALL_PREFIX=install -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build/PlotJuggler --config RelWithDebInfo --target install
```

# Compile in Windows

Dependencies in Windows are managed either using
[conan](https://conan.io/) or [vcpkg](https://vcpkg.io/en/index.html)

The rest of this section assumes that you installed
You need to install first [Qt](https://www.qt.io/download-open-source) and
[git](https://desktop.github.com/).

**Visual studio 2019 (16)**, that is part of the Qt 5.15.x installation,
 will be used to compile PlotJuggler.

Start creating a folder called **plotjuggler_ws** and cloning the repo:

```batch
cd \
mkdir plotjuggler_ws
cd plotjuggler_ws
git clone https://github.com/PlotJuggler/PlotJuggler.git src/PlotJuggler
```

## Build with Conan

Note: the Arrow/Parque plugin is not supported in Conan. Use vcpkg instead, if you need
that specific plugin.

```batch
conan install src/PlotJuggler --output-folder build/PlotJuggler ^
      --build=missing -pr:b=default -s build_type=Release

set CMAKE_TOOLCHAIN=%cd%/build/PlotJuggler/conan_toolchain.cmake

cmake -G "Visual Studio 16" ^
      -S src/PlotJuggler -B build/PlotJuggler ^
      -DCMAKE_TOOLCHAIN_FILE=%CMAKE_TOOLCHAIN%  ^
      -DCMAKE_INSTALL_PREFIX=%cd%/install ^
      -DCMAKE_POLICY_DEFAULT_CMP0091=NEW ^
      -DBUILDING_WITH_CONAN=ON


cmake --build build/PlotJuggler --config Release --target install
```

## Build with vcpkg

Change the path where **vcpkg.cmake** can be found as needed.

```batch
set CMAKE_TOOLCHAIN=/path/vcpkg/scripts/buildsystems/vcpkg.cmake

cmake -G "Visual Studio 16" ^
      -S src/PlotJuggler -B build/PlotJuggler ^
      -DCMAKE_TOOLCHAIN_FILE=%CMAKE_TOOLCHAIN%  ^
      -DCMAKE_INSTALL_PREFIX=%cd%/install

cmake --build build/PlotJuggler --config Release --target install
```

## Create a Windows installer

Change the **Qt** and **QtInstallerFramework** version as needed.

```batch
xcopy src\PlotJuggler\installer installer\ /Y /S /f /z
xcopy install\bin\*.* installer\io.plotjuggler.application\data /Y /S /f /z

installer\windeploy_pj.bat C:\QtPro\5.15.16\msvc2019_64\bin\windeployqt.exe
```

For the installer to work on machines that don't have the matching Python
installed, you must also bundle the embeddable Python distribution next
to `plotjuggler.exe`. The pinned version MUST match the `Python3` that
CMake found during the build (so the import-library and the runtime DLL
share the same ABI):

```powershell
$pyVersion = "3.12.7"   # must match what was used at compile time
$pyTag = "312"          # major+minor concatenated, no dot
$dataDir = "installer\io.plotjuggler.application\data"

Invoke-WebRequest "https://www.python.org/ftp/python/$pyVersion/python-$pyVersion-embed-amd64.zip" -OutFile python-embed.zip
Expand-Archive python-embed.zip -DestinationPath "$dataDir\python" -Force
Remove-Item "$dataDir\python\python.exe","$dataDir\python\pythonw.exe" -ErrorAction Ignore

# Comment out `import site` in python._pth so isolated mode stays on
# (otherwise Python would search the user's site-packages and registry).
$pth = "$dataDir\python\python$pyTag._pth"
(Get-Content $pth) -replace '^\s*import site','#import site' | Set-Content $pth

# Copy pythonXX.dll next to the EXE so the loader picks the bundled one.
Copy-Item "$dataDir\python\python$pyTag.dll" $dataDir -Force
```

Then create the installer:

```batch
C:\QtPro\Tools\QtInstallerFramework\4.6\bin\binarycreator.exe --offline-only -c installer\config.xml -p installer  PlotJuggler-Windows-installer.exe
```

The same flow runs in CI (`.github/workflows/windows.yaml` — see the
"Bundle embeddable Python" step) which is the canonical reference.
