{
  description = "WeTypeX Fcitx5 input method";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs =
    { self, nixpkgs }:
    let
      system = "x86_64-linux";
      pkgs = import nixpkgs { inherit system; };
      wetypex = pkgs.callPackage ./nix/package.nix { src = self.outPath; };
    in
    {
      packages.${system} = {
        fcitx5-wetypex = wetypex;
        default = wetypex;
      };
      checks.${system} = {
        package = wetypex;
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
      };
    };
}
