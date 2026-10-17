# SPDX-License-Identifier: MPL-2.0
{
  description = "PlotJuggler 4 — native Linux package and development environment";

  # Include the pinned ADS submodule in local and remote Git flake sources.
  inputs.self.submodules = true;
  # This revision supplies the Qt release required by versions.env.
  inputs.nixpkgs.url = "github:NixOS/nixpkgs/991eb3e01305e9549d0fc5504d034359ae0897a3";

  outputs =
    { self, nixpkgs }:
    let
      system = "x86_64-linux";
      pkgs = import nixpkgs { inherit system; };
      package = pkgs.callPackage ./packaging/nix/package.nix { source = self; };
    in
    {
      packages.${system} = {
        default = package;
        plotjuggler = package;
      };
      apps.${system} = rec {
        default = {
          type = "app";
          program = "${package}/bin/plotjuggler4";
          meta.description = "PlotJuggler 4";
        };
        plotjuggler = default;
      };
      checks.${system}.plotjuggler = package;
      devShells.${system}.default = pkgs.mkShell {
        inputsFrom = [ package ];
        packages = [
          pkgs.gtest
          pkgs.gbenchmark
        ];
        shellHook = ''
          # run.sh selects the Conan/.qt tree; this shell uses Nix's Qt instead.
          export QT_PLUGIN_PATH="${
            pkgs.lib.makeSearchPath pkgs.qt6.qtbase.qtPluginPrefix [
              pkgs.qt6.qtbase
              pkgs.qt6.qtsvg
              pkgs.qt6.qtwayland
            ]
          }"
          unset QT_IM_MODULE
          pj-configure() {
            cmake -S . -B build/nix -G Ninja \
              ${pkgs.lib.escapeShellArgs package.cmakeFlags} \
              -DCMAKE_BUILD_TYPE=RelWithDebInfo "$@"
          }
          export -f pj-configure
        '';
      };
    };
}
