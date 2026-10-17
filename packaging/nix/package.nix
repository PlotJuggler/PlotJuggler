# SPDX-License-Identifier: MPL-2.0
{
  lib,
  stdenv,
  callPackage,
  fetchurl,
  runCommand,
  cmake,
  ninja,
  pkg-config,
  qt6,
  qt6Packages,
  fmt,
  robin-map,
  glm,
  nlohmann_json,
  kissfft,
  python312,
  python312Packages,
  fast-float,
  libarchive,
  libjpeg_turbo,
  libpng,
  draco,
  assimp,
  backward-cpp,
  elfutils,
  libGL,
  source,
}:
let
  versions = builtins.listToAttrs (
    lib.concatMap (
      line:
      let
        match = builtins.match "(PJ_[A-Z_]+)=(.*)" line;
      in
      lib.optional (match != null) {
        name = builtins.elemAt match 0;
        value = builtins.elemAt match 1;
      }
    ) (lib.splitString "\n" (builtins.readFile ../../versions.env))
  );
  dependencies = callPackage ./dependencies.nix { };
  sdkVersion = builtins.head (
    builtins.match ".*\nplotjuggler_sdk/([0-9.]+)\n.*" (builtins.readFile ../../conanfile.txt)
  );
  sdkArchive = fetchurl {
    url = "https://github.com/PlotJuggler/plotjuggler_sdk/archive/refs/tags/v${sdkVersion}.tar.gz";
    sha256 = builtins.head (
      builtins.match ".*SHA256=([0-9a-f]+).*" (builtins.readFile ../../CMakeLists.txt)
    );
  };
  # FetchContent consumes an unpacked tree, but the authoritative hash in CMake
  # describes the archive. Verify those same bytes before unpacking them in Nix.
  sdkSource = runCommand "plotjuggler-sdk-${sdkVersion}-source" { } ''
    mkdir "$out"
    tar -xf ${sdkArchive} --strip-components=1 -C "$out"
  '';
in
assert lib.assertMsg (
  qt6.qtbase.version == versions.PJ_QT_VERSION
) "The locked Nixpkgs Qt does not match versions.env; select a matching Nixpkgs revision.";
stdenv.mkDerivation {
  pname = "plotjuggler4";
  version = versions.PJ_APP_VERSION;
  src = lib.cleanSource source;

  nativeBuildInputs = [
    cmake
    ninja
    pkg-config
    qt6.wrapQtAppsHook
  ];
  buildInputs = [
    qt6.qtbase
    qt6.qtsvg
    qt6.qttools
    qt6.qtwayland
    qt6Packages.qwt
    fmt
    robin-map
    glm
    nlohmann_json
    (kissfft.override {
      datatype = "float";
      enableStatic = true;
    })
    python312
    python312Packages.pybind11
    fast-float
    libarchive
    libjpeg_turbo
    libpng
    draco
    assimp
    backward-cpp
    elfutils
    libGL
    dependencies.luau
    dependencies.nanoarrow
    dependencies.cloudini
    dependencies.mcap
    dependencies.ffmpeg
  ];

  cmakeFlags = [
    # kissfft's installed config still declares a pre-3.5 CMake policy version.
    "-DCMAKE_POLICY_VERSION_MINIMUM=3.5"
    "-DPJ_BUILD_TESTS=OFF"
    "-DPJ_BUILD_DEMOS=OFF"
    "-DPJ_BUILD_RASTER_HELPER=OFF"
    "-DPJ_DEBUG_INFO=none"
    "-DPJ_SDK_FORCE_SOURCE=ON"
    "-DFETCHCONTENT_SOURCE_DIR_PLOTJUGGLER_SDK=${sdkSource}"
    "-DFETCHCONTENT_FULLY_DISCONNECTED=ON"
    "-DPJ_SYSTEM_QWT=ON"
    "-DPython_EXECUTABLE=${python312}/bin/python3"
  ];
  ninjaFlags = [ "pj_app" ];

  # PJ4's other packagers stage these two binaries directly; there is no app
  # install target. Keep the admission helper beside the executable for lookup.
  installPhase = ''
    runHook preInstall
    install -Dm755 pj_app/plotjuggler4 "$out/bin/plotjuggler4"
    install -Dm755 pj_app/pj-plugin-check "$out/bin/pj-plugin-check"
    install -Dm644 ../packaging/appimage/plotjuggler4.desktop \
      "$out/share/applications/plotjuggler4.desktop"
    install -Dm644 ../resources/svg/plotjuggler.svg \
      "$out/share/icons/hicolor/scalable/apps/plotjuggler4.svg"
    runHook postInstall
  '';
  qtWrapperArgs = [
    "--unset"
    "QT_IM_MODULE"
  ];

  doInstallCheck = true;
  installCheckPhase = ''
    runHook preInstallCheck
    export HOME="$TMPDIR/home" QT_QPA_PLATFORM=offscreen
    mkdir -p "$HOME"
    "$out/bin/plotjuggler4" --version
    "$out/bin/plotjuggler4" --help
    "$out/bin/plotjuggler4" --selftest-python
    test -x "$out/bin/pj-plugin-check"
    runHook postInstallCheck
  '';

  meta = {
    description = "Visualize, transform and analyze time series";
    homepage = "https://github.com/PlotJuggler/PlotJuggler";
    license = lib.licenses.mpl20;
    mainProgram = "plotjuggler4";
    platforms = [ "x86_64-linux" ];
  };
}
