{
  description = "A flake for building and running PlotJuggler";

  inputs = {
    nixpkgs.url = "github:nixos/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils, ... }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs {
          inherit system;
          config.allowUnfree = true;
          config.qt5.enable = true;
        };

        data-tamer-src = pkgs.fetchzip {
          url = "https://github.com/PickNikRobotics/data_tamer/archive/refs/tags/1.0.3.zip";
          sha256 = "sha256-hGfoU6oK7vh39TRCBTYnlqEsvGLWCsLVRBXh3RDrmnY=";
        };

        wasmer-release = {
          x86_64-linux = {
            archive = "wasmer-linux-amd64.tar.gz";
            sha256 = "10a55885b11eb51b06bb24ff184facde8c2a83c252782a0c04e7a46926630d72";
          };
          aarch64-linux = {
            archive = "wasmer-linux-aarch64.tar.gz";
            sha256 = "21d6968d33defa4a31d878022d261667a8fa8abbfe96007d5f4f28564b7fa372";
          };
          x86_64-darwin = {
            archive = "wasmer-darwin-amd64.tar.gz";
            sha256 = "3a0f44a3aae570b0870d4573fa663c7f0c96a2f9550e38eb22c3be7c77658a1e";
          };
          aarch64-darwin = {
            archive = "wasmer-darwin-arm64.tar.gz";
            sha256 = "3eff017389fb838b0b5af607a4d392edc6039e76343984fcd24307aa027d67ee";
          };
        }.${system};

        wasmer-archive = pkgs.fetchurl {
          url = "https://github.com/wasmerio/wasmer/releases/download/v7.0.1/${wasmer-release.archive}";
          inherit (wasmer-release) sha256;
        };

        plotjuggler-pkg = pkgs.qt5.mkDerivation {
          pname = "plotjuggler";
          version = "3.17.2";

          src = ./.;
          patches = [ ./nix/arrow.patch ];

          postPatch = ''
            substituteInPlace cmake/find_or_download_data_tamer.cmake \
              --replace-fail "URL" "SOURCE_DIR" \
              --replace-fail "https://github.com/PickNikRobotics/data_tamer/archive/refs/tags/1.0.3.zip" "${data-tamer-src}"

            substituteInPlace cmake/download_wasmer.cmake \
              --replace-fail 'URL ''${WASMER_URL}' 'URL ${wasmer-archive}'

            rm cmake/find_or_download_fmt.cmake

            substituteInPlace CMakeLists.txt \
              --replace-fail "include(cmake/find_or_download_fmt.cmake)" "find_package(fmt REQUIRED)" \
              --replace-fail "find_or_download_fmt()" ""

            substituteInPlace \
              cmake/find_or_download_lz4.cmake \
              plotjuggler_plugins/DataLoadMCAP/CMakeLists.txt \
              --replace-fail "LZ4::lz4_static" "LZ4::lz4_shared"

            substituteInPlace \
              cmake/find_or_download_zstd.cmake \
              plotjuggler_plugins/DataLoadMCAP/CMakeLists.txt \
              plotjuggler_plugins/DataStreamPlotJugglerBridge/CMakeLists.txt \
              --replace-fail "zstd::libzstd_static" "zstd::libzstd_shared"
          '';

          cmakeFlags = [
            "-DPLJ_USE_SYSTEM_LUA=ON"
            "-DPLJ_USE_SYSTEM_NLOHMANN_JSON=ON"
          ];


          nativeBuildInputs = [ pkgs.cmake pkgs.pkg-config pkgs.qt5.wrapQtAppsHook ];

          buildInputs = [
            pkgs.qt5.full
            pkgs.qt5.qtsvg
            pkgs.qt5.qtimageformats
            pkgs.qt5.qtdeclarative
            pkgs.zeromq
            pkgs.sqlite
            pkgs.lua
            pkgs.nlohmann_json
            pkgs.fmt
            pkgs.lz4
            pkgs.zstd
            pkgs.mosquitto
            pkgs.protobuf
            pkgs.xorg.libX11
            pkgs.xorg.libxcb
            pkgs.xorg.xcbutil
            pkgs.xorg.xcbutilkeysyms
            pkgs.arrow-cpp
          ];
          meta = with pkgs.lib; {
            description = "A tool to plot streaming data, fast and easy";
            homepage = "https://github.com/PlotJuggler/PlotJuggler";
            license = licenses.mpl20;
            platforms = platforms.linux ++ platforms.darwin;
          };
        };

      in
      {
        packages.default = plotjuggler-pkg;
        packages.plotjuggler = plotjuggler-pkg;

        apps.default = {
          type = "app";
          program = "${plotjuggler-pkg}/bin/plotjuggler";
        };
        apps.plotjuggler = self.apps.${system}.default;

        devShells.default = pkgs.mkShell {
          packages = [
            pkgs.cmake
            pkgs.qt5.full
            pkgs.qt5.qtsvg
            pkgs.qt5.qtimageformats
            pkgs.qt5.qtdeclarative
            pkgs.arrow-cpp
            pkgs.zeromq
            pkgs.sqlite
            pkgs.lua
            pkgs.nlohmann_json
            pkgs.fmt
            pkgs.lz4
            pkgs.zstd
            pkgs.mosquitto
            pkgs.protobuf
            pkgs.codespell
            pkgs.xorg.libX11
            pkgs.xorg.libxcb
            pkgs.xorg.xcbutil
            pkgs.xorg.xcbutilkeysyms
          ];
        };
      }
    );
}
