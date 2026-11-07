{
  description = "Plugins for the Shady 3D Wayland compositor";

  inputs.shady.url = "github:gloaming-dusk/shady";
  inputs.nixpkgs.follows = "shady/nixpkgs";

  outputs = { self, nixpkgs, shady }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f system nixpkgs.legacyPackages.${system});
      plugin = system: pkgs: name: pkgs.stdenv.mkDerivation {
        pname = "shady-plugin-${name}";
        version = "0-unstable";
        src = ./plugins + "/${name}";
        strictDeps = true;
        nativeBuildInputs = with pkgs; [ meson ninja pkg-config ];
        # Only Shady's headers; the compositor provides everything else at runtime.
        buildInputs = [ shady.packages.${system}.default pkgs.libxkbcommon ];
        doCheck = true;
        meta.license = pkgs.lib.licenses.asl20;
      };
    in {
      packages = forAllSystems (system: pkgs: {
        black-hole = plugin system pkgs "black-hole";
      });
      # `nix flake check` builds and unit-tests them all.
      checks = self.packages;
    };
}
