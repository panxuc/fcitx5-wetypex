{
  description = "WeTypeX Fcitx5 input method";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs =
    { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forSystems = nixpkgs.lib.genAttrs systems;
      packageFor = system: (import nixpkgs {
        inherit system;
        overlays = [ self.overlays.default ];
      }).fcitx5-wetypex;
    in
    {
      overlays.default = final: _prev: {
        fcitx5-wetypex = final.callPackage ./nix/package.nix { src = self.outPath; };
      };
      packages = forSystems (system: let wetypex = packageFor system; in {
        fcitx5-wetypex = wetypex;
        default = wetypex;
      });
      checks = forSystems (system: let wetypex = packageFor system; in {
        package = wetypex;
        compatibility = wetypex.override { enableCompatibilityProvider = true; };
        minimal = wetypex.override {
          enableAI = false;
          enableVoice = false;
          enableLayerShell = false;
          enableWeTypeTheme = false;
          defaultSettings = {
            page_size = 7;
            vertical = true;
            standalone = true;
          };
        };
        voiceOnly = wetypex.override {
          enableAI = false;
          enableVoice = true;
          enableLayerShell = false;
          enableWeTypeTheme = false;
        };
        aiOnly = wetypex.override {
          enableAI = true;
          enableVoice = false;
        };
      });
    };
}
