# SPDX-License-Identifier: MPL-2.0
{
  lib,
  stdenv,
  fetchurl,
  cmake,
  ninja,
  pkg-config,
  python312,
  nasm,
  zlib,
  dav1d,
  libva,
  libdrm,
  lz4,
  zstd,
}:
let
  # Reuse the source pins of the existing native dependency recipes. These two
  # scalar fields are deliberately required; a recipe format change fails eval.
  recipeSource =
    name: urlFor:
    let
      recipe = builtins.readFile (../../recipes + "/${name}/recipe.yaml");
      version = builtins.head (builtins.match ".*\n  version: \"([^\"]+)\"\n.*" recipe);
      sha256 = builtins.head (builtins.match ".*\n  sha256: ([0-9a-f]+)\n.*" recipe);
    in
    {
      inherit version;
      src = fetchurl {
        url = urlFor version;
        inherit sha256;
      };
    };
in
{
  luau = stdenv.mkDerivation (
    {
      pname = "pj4-luau";
      nativeBuildInputs = [
        cmake
        ninja
      ];
      cmakeFlags = [
        "-DLUAU_BUILD_CLI=OFF"
        "-DLUAU_BUILD_TESTS=OFF"
        "-DLUAU_BUILD_WEB=OFF"
        "-DCMAKE_POSITION_INDEPENDENT_CODE=ON"
      ];
      ninjaFlags = [
        "Luau.Common"
        "Luau.Ast"
        "Luau.Compiler"
        "Luau.VM"
        "Luau.CodeGen"
      ];
      # Upstream has no install target for the embedding libraries.
      installPhase = ''
        runHook preInstall
        mkdir -p "$out/lib/cmake/Luau" "$out/include"
        for component in Common Ast Compiler VM CodeGen; do
          cp "libLuau.$component.a" "$out/lib/"
          cp -r "../$component/include/." "$out/include/"
        done
        cp ${../../recipes/luau/LuauConfig.cmake} "$out/lib/cmake/Luau/LuauConfig.cmake"
        runHook postInstall
      '';
      meta.license = lib.licenses.mit;
    }
    // recipeSource "luau" (v: "https://github.com/luau-lang/luau/archive/refs/tags/${v}.tar.gz")
  );

  nanoarrow = stdenv.mkDerivation (
    {
      pname = "pj4-nanoarrow";
      nativeBuildInputs = [
        cmake
        ninja
      ];
      # The app needs the IPC CMake targets and the matching flatcc runtime;
      # the Nixpkgs Meson package exposes a different installation layout.
      cmakeFlags = [
        "-DNANOARROW_IPC=ON"
        "-DCMAKE_POSITION_INDEPENDENT_CODE=ON"
      ];
      meta.license = lib.licenses.asl20;
    }
    // recipeSource "libnanoarrow" (
      v: "https://github.com/apache/arrow-nanoarrow/archive/refs/tags/apache-arrow-nanoarrow-${v}.tar.gz"
    )
  );

  cloudini = stdenv.mkDerivation (
    {
      pname = "pj4-cloudini";
      nativeBuildInputs = [
        cmake
        ninja
      ];
      propagatedBuildInputs = [
        lz4
        zstd
      ];
      cmakeDir = "../cloudini_lib";
      # Cloudini probes only the static zstd target; Nix supplies the shared one.
      postPatch = ''
        substituteInPlace cloudini_lib/CMakeLists.txt cloudini_lib/cmake/find_or_download_zstd.cmake \
          --replace-fail 'zstd::libzstd_static' 'zstd::libzstd_shared'
      '';
      cmakeFlags = [
        "-DCLOUDINI_FORCE_VENDORED_DEPS=OFF"
        "-DCLOUDINI_BUILD_TOOLS=OFF"
        "-DCLOUDINI_BUILD_BENCHMARKS=OFF"
        "-DBUILD_TESTING=OFF"
        "-DCMAKE_POSITION_INDEPENDENT_CODE=ON"
        "-DFETCHCONTENT_FULLY_DISCONNECTED=ON"
      ];
      postInstall = ''
        install -Dm644 ${../../recipes/cloudini/cloudini-config.cmake} \
          "$out/lib/cmake/cloudini/cloudini-config.cmake"
      '';
      meta.license = lib.licenses.asl20;
    }
    // recipeSource "cloudini" (
      v: "https://github.com/facontidavide/cloudini/archive/refs/tags/${v}.tar.gz"
    )
  );

  mcap = stdenv.mkDerivation (
    {
      pname = "pj4-mcap";
      propagatedBuildInputs = [
        lz4
        zstd
      ];
      dontConfigure = true;
      dontBuild = true;
      installPhase = ''
        runHook preInstall
        mkdir -p "$out/include"
        cp -r cpp/mcap/include/mcap "$out/include/"
        install -Dm644 ${../../recipes/libmcap/mcap-config.cmake} \
          "$out/lib/cmake/mcap/mcap-config.cmake"
        runHook postInstall
      '';
      meta.license = lib.licenses.mit;
    }
    // recipeSource "libmcap" (
      v: "https://github.com/foxglove/mcap/archive/refs/tags/releases/cpp/v${v}.tar.gz"
    )
  );

  ffmpeg = stdenv.mkDerivation (
    {
      pname = "pj4-ffmpeg";
      nativeBuildInputs = [
        pkg-config
        nasm
      ];
      buildInputs = [
        zlib
        dav1d
        libva
        libdrm
      ];
      dontConfigure = true;
      dontInstall = true;
      # Share the audited decode-only/LGPL feature selection with the pixi build.
      # This script configures, builds and installs into PREFIX in one invocation.
      buildPhase = ''
        runHook preBuild
        export PREFIX="$out" CPU_COUNT="$NIX_BUILD_CORES"
        bash ${../../recipes/ffmpeg/build_ffmpeg.sh}
        runHook postBuild
      '';
      doInstallCheck = true;
      nativeInstallCheckInputs = [
        python312
        cmake
        ninja
      ];
      installCheckPhase = ''
        runHook preInstallCheck
        PREFIX="$out" python ${../../recipes/ffmpeg/license_guard.py}
        PKG_CONFIG_PATH="$out/lib/pkgconfig:$PKG_CONFIG_PATH" \
          cmake -S ${../../recipes/ffmpeg/test_consumer} -B consumer -G Ninja
        cmake --build consumer
        ./consumer/consumer
        runHook postInstallCheck
      '';
      meta.license = lib.licenses.lgpl21Plus;
    }
    // recipeSource "ffmpeg" (v: "https://ffmpeg.org/releases/ffmpeg-${v}.tar.xz")
  );
}
