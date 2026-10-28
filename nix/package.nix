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
}:

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
  runtimePath = lib.makeBinPath [
    coreutils
    util-linux
    bubblewrap
    fontconfig
    pipewire
    pulseaudio
    ffmpeg
    wl-clipboard
    python
  ];
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

  patches = [ ./runtime.patch ];

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
    kdePackages.qtwebengine
    kdePackages.layer-shell-qt
  ];

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
    "-DWETYPE_LAYER_SHELL=ON"
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
