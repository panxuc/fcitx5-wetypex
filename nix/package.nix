{
  lib,
  stdenv,
  fetchFromGitHub,
  writeText,
  cmake,
  ninja,
  pkg-config,
  kdePackages,
  fcitx5,
  libime,
  boost,
  json_c,
  fontconfig,
  wayland,
  curl,
  llvmPackages,
  python3,
  bash,
  coreutils,
  util-linux,
  bubblewrap,
  cacert,
  tzdata,
  pipewire,
  pulseaudio,
  ffmpeg,
  wl-clipboard,
  desktop-file-utils,
  src ? lib.cleanSource ../.,
  enableAI ? true,
  enableVoice ? true,
  enableLayerShell ? true,
  enableWeTypeTheme ? true,
  defaultSettings ? { },
}:

assert lib.assertMsg (builtins.isAttrs defaultSettings)
  "WeTypeX defaultSettings must be an attribute set";
assert lib.assertMsg (builtins.all builtins.isBool [
  enableAI enableVoice enableLayerShell enableWeTypeTheme
]) "WeTypeX feature switches must be booleans";

let
  projectLine =
    lib.findFirst (lib.hasPrefix "project(") (throw "WeTypeX project version is missing")
      (lib.splitString "\n" (builtins.readFile ../CMakeLists.txt));
  upstreamVersion = builtins.head (builtins.match "project\\([^ ]+ VERSION ([0-9.]+) .*" projectLine);
  releaseLine = lib.findFirst (lib.hasPrefix "pkgrel=") (throw "WeTypeX package release is missing") (
    lib.splitString "\n" (builtins.readFile ../packaging/PKGBUILD.in)
  );
  version = "${upstreamVersion}-${lib.removePrefix "pkgrel=" releaseLine}";
  # WeTypeX requires WS/WSS, disabled in the default nixpkgs curl build.
  transportCurl = curl.override { websocketSupport = true; };
  python = python3.withPackages (ps: [
    ps.fonttools
    ps.pillow
  ]);
  defaultSettingsJSON = builtins.toJSON defaultSettings;
  defaultSettingsFile =
    assert lib.assertMsg (builtins.stringLength defaultSettingsJSON <= 16384)
      "WeTypeX defaultSettings exceeds the 16 KiB preference limit";
    writeText "fcitx5-wetypex-default-settings.json" defaultSettingsJSON;
  runtimePath = lib.makeBinPath (
    [
      coreutils
      util-linux
      bubblewrap
      fontconfig
      wl-clipboard
      python
    ]
    ++ lib.optionals enableVoice [
      pipewire
      pulseaudio
      ffmpeg
    ]
  );
  nsswitch = writeText "wetypex-nsswitch.conf" "hosts: files dns\n";
  # Keep the same fixed revision as CMake, with no configure-time download.
  kqueueSource = fetchFromGitHub {
    owner = "mheily";
    repo = "libkqueue";
    rev = "46a3e130f88b0b0742575dcb01d77e336538024b";
    hash = "sha256-bUtjzGxQHJkd9SxlXpA8xJ4hT0IeMHrlcfRLMTdT59k=";
  };
in
stdenv.mkDerivation {
  pname = "fcitx5-wetypex";
  inherit version;
  src = lib.cleanSourceWith {
    inherit src;
    filter =
      path: type:
      lib.cleanSourceFilter path type
      && !(builtins.elem (builtins.baseNameOf path) [
        "build"
        "dist"
        ".cache"
      ]);
  };

  patches = [
    ./runtime.patch
    ./feature-options.patch
  ]
  ++ lib.optional (!enableWeTypeTheme) ./preserve-desktop-theme.patch;

  nativeBuildInputs = [
    cmake
    ninja
    pkg-config
    kdePackages.extra-cmake-modules
    kdePackages.wrapQtAppsHook
    python
  ];

  buildInputs = [
    fcitx5
    libime
    boost
    json_c
    fontconfig
    wayland
    transportCurl
    llvmPackages.libcxx
    kdePackages.fcitx5-qt
    kdePackages.qtbase
    kdePackages.qtsvg
    kdePackages.qtwayland
  ]
  ++ lib.optional enableAI kdePackages.qtwebengine
  ++ lib.optional enableLayerShell kdePackages.layer-shell-qt;

  postPatch = ''
    # Distribution installers cannot update an immutable Nix store package.
    cat > scripts/fcitx5-wetypex-update.sh.in <<'PY'
    #!@NIX_PYTHON@
    print("WeTypeX 由 Nix 管理，请更新 Nix 配置中的版本并重新构建。")
    PY
    while IFS= read -r -d "" file; do
      substituteInPlace "$file" \
        --replace-quiet '@NIX_RUNTIME_PATH@' '${runtimePath}' \
        --replace-quiet '@NIX_PYTHON@' '${python}/bin/python3' \
        --replace-quiet '@NIX_BASH@' '${bash}/bin/bash' \
        --replace-quiet '@NIX_CA_BUNDLE@' '${cacert}/etc/ssl/certs/ca-bundle.crt' \
        --replace-quiet '@NIX_NSSWITCH@' '${nsswitch}' \
        --replace-quiet '@NIX_TZDIR@' '${tzdata}/share/zoneinfo' \
        --replace-quiet '@NIX_LIBCXX@' '${lib.getLib llvmPackages.libcxx}/lib/libc++.so.1' \
        --replace-quiet '@NIX_LIBCXXABI@' '${lib.getLib llvmPackages.libcxx}/lib/libc++abi.so.1' \
        --replace-quiet '@NIX_DEFAULT_SETTINGS@' '${defaultSettingsFile}' \
        --replace-quiet '@NIX_VOICE_ENABLED@' '${if enableVoice then "1" else "0"}' \
        --replace-quiet '@NIX_OUT@' "$out"
    done < <(grep -rlZ '@NIX_' scripts src tools)
    patchShebangs scripts tools
  '';

  preFixup = ''
    # Qt programs also invoke audio and other runtime tools directly. Their
    # environment must work without relying on the user's system profile.
    qtWrapperArgs+=(--prefix PATH : '${runtimePath}'
      --set-default TZDIR '${tzdata}/share/zoneinfo'
      --set-default SSL_CERT_FILE '${cacert}/etc/ssl/certs/ca-bundle.crt')
  '';

  cmakeFlags = [
    "-DCMAKE_INSTALL_LIBDIR=lib"
    "-DCMAKE_POLICY_VERSION_MINIMUM=3.5"
    "-DFETCHCONTENT_SOURCE_DIR_KQUEUE=${kqueueSource}"
    "-DFETCHCONTENT_FULLY_DISCONNECTED=ON"
    "-DWETYPE_QT_VERSION=6"
    "-DWETYPE_LAYER_SHELL=${if enableLayerShell then "ON" else "OFF"}"
    "-DWETYPE_ENABLE_AI=${lib.boolToString enableAI}"
    "-DWETYPE_ENABLE_VOICE=${lib.boolToString enableVoice}"
    "-DWETYPE_DEFAULT_SETTINGS_FILE=${defaultSettingsFile}"
    "-DWETYPE_BUNDLED_CURL=OFF"
    "-DWETYPE_CLANGXX=${llvmPackages.libcxxStdenv.cc}/bin/clang++"
  ];

  doCheck = true;
  checkPhase = ''
    runHook preCheck
    for script in ../scripts/*.sh.in; do
      case "$script" in
        *-syncd.sh.in|*-update.sh.in|*-sync.sh.in) continue ;;
      esac
      ${bash}/bin/bash -n "$script"
    done
    export PYTHONPYCACHEPREFIX="$TMPDIR/wetypex-pycache"
    ${python}/bin/python3 -m py_compile \
      ../scripts/*.py ../tools/*.py \
      fcitx5-wetypex-syncd fcitx5-wetypex-update fcitx5-wetypex-sync
    runHook postCheck
  '';

  postInstall = ''
    install -Dm444 ${defaultSettingsFile} "$out/share/fcitx5-wetypex/default-settings.json"
  '';

  nativeInstallCheckInputs = [ desktop-file-utils ];
  doInstallCheck = true;
  installCheckPhase = ''
    runHook preInstallCheck
    test -f "$out/lib/fcitx5/wetypex.so"
    test -f "$out/lib/fcitx5/qt6/libwetypexconfig.so"
    test -f "$out/share/fcitx5/inputmethod/wetypex.conf"
    desktop-file-validate "$out/share/applications/fcitx5-wetypex-settings.desktop"
    "$out/bin/fcitx5-wetypex-setup" --help >/dev/null
    # Load the transport without an account, graphical session, or network I/O.
    "$out/bin/fcitx5-wetypex-account" network-info > network-info.json
    ${python}/bin/python3 -c \
      'import json; d=json.load(open("network-info.json")); assert all(d[k] for k in ("utf8_json", "ws", "wss")), d'
    "$out/bin/fcitx5-wetypex-init-settings" --help >/dev/null
    ${lib.optionalString (!enableAI) ''
      test ! -e "$out/bin/fcitx5-wetypex-ai"
      test ! -e "$out/bin/fcitx5-wetypex-ai-viewer"
    ''}
    ${lib.optionalString (!enableVoice) ''
      test ! -e "$out/bin/fcitx5-wetypex-voice"
      test ! -e "$out/share/fcitx5-wetypex/voice_stream.py"
      code=0
      "$out/bin/fcitx5-wetypex-account" voice-file /nonexistent > /dev/null 2>voice-disabled.txt || code=$?
      test "$code" -eq 78
      grep -q 'disabled by the package configuration' voice-disabled.txt
    ''}
    runHook postInstallCheck
  '';

  meta = {
    description = "Fcitx5 compatibility layer for the WeType macOS input engine";
    homepage = "https://github.com/panxuc/fcitx5-wetypex";
    license = with lib.licenses; [
      mit
      isc
      bsd2
    ];
    platforms = [ "x86_64-linux" ];
    mainProgram = "fcitx5-wetypex-settings";
  };
}
