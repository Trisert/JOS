{
  description = "RedPill PocketQube OBSW — STM32L496VGTx + FreeRTOS";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = nixpkgs.legacyPackages.${system};
        inherit (pkgs) gcc-arm-embedded-13 stlink;
      in
      {
        devShells.default = pkgs.mkShell {
          name = "jos-dev";

          packages = [
            gcc-arm-embedded-13
            stlink
            pkgs.openocd
          ];

          shellHook = ''
            echo "JOS dev shell"
            echo "  arm-none-eabi-gcc:  $(arm-none-eabi-gcc --version | head -1)"
            echo "  st-flash:           $(st-flash --version 2>&1 | head -1 || echo available)"
            echo ""
            echo "  make all    — build firmware"
            echo "  make flash  — flash via ST-LINK"
            echo "  make clean  — clean build dir"
          '';
        };
      });
}
