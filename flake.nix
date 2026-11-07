{
  description = "Plugins for the Shady 3D Wayland compositor";

  inputs.shady.url = "github:gloaming-dusk/shady";
  inputs.nixpkgs.follows = "shady/nixpkgs";

  outputs = { self, nixpkgs, shady }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f system nixpkgs.legacyPackages.${system});
      plugin = system: pkgs: name: extra: pkgs.stdenv.mkDerivation {
        pname = "shady-plugin-${name}";
        version = "0-unstable";
        src = ./plugins + "/${name}";
        strictDeps = true;
        nativeBuildInputs = with pkgs; [ meson ninja pkg-config ];
        # Only Shady's headers; the compositor provides everything else at runtime.
        buildInputs = [ shady.packages.${system}.default pkgs.libxkbcommon ] ++ extra;
        doCheck = true;
        meta.license = pkgs.lib.licenses.asl20;
      };
    in {
      packages = forAllSystems (system: pkgs: {
        astral-loom = plugin system pkgs "astral-loom" [ ];
        bad-apple = plugin system pkgs "bad-apple" [ pkgs.libGL ];
        black-hole = plugin system pkgs "black-hole" [ ];
        close-burn = plugin system pkgs "close-burn" [ ];
        frozen-window = plugin system pkgs "frozen-window" [ ];
        magnetic-windows = plugin system pkgs "magnetic-windows" [ ];
        water-windows = plugin system pkgs "water-windows" [ ];
        window-constellation = plugin system pkgs "window-constellation" [ ];
        window-portal = plugin system pkgs "window-portal" [ ];
      });
      # `nix flake check` builds and unit-tests them all.
      checks = self.packages;
    };
}
