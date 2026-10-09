FROM ubuntu:20.04

# Focal's 0.0~git packages predate the stable Fcitx5 ABI. Build a pinned SDK;
# the resulting WeTypeX package still requires a stable Fcitx5 installation.
RUN apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
    build-essential cmake clang git pkg-config ca-certificates gettext \
    extra-cmake-modules libboost-dev libboost-iostreams-dev libboost-filesystem-dev \
    libboost-regex-dev libjson-c-dev libssl-dev libc++-dev libc++abi-dev \
    qtbase5-dev qtbase5-private-dev libqt5svg5-dev qtwebengine5-dev \
    libfmt-dev libdbus-1-dev libevent-dev libxkbcommon-dev libxkbcommon-x11-dev \
    libxcb-ewmh-dev libxcb-xkb-dev libxcb-randr0-dev libxcb-util-dev \
    libxcb-keysyms1-dev libxcb-icccm4-dev libxcb-xinerama0-dev \
    libxcb-xfixes0-dev libxcb-shape0-dev libxkbfile-dev \
    libcairo2-dev libpango1.0-dev libgdk-pixbuf2.0-dev \
    libwayland-dev wayland-protocols libegl1-mesa-dev iso-codes xkb-data \
    appstream appstream-util python3 && rm -rf /var/lib/apt/lists/*

RUN git clone https://github.com/fcitx/xcb-imdkit.git /tmp/xcb-imdkit && \
    git -C /tmp/xcb-imdkit checkout 30e2f16f9a8b0e338e25ce5e3643809a07ad41f0 && \
    cmake -S /tmp/xcb-imdkit -B /tmp/xcb-build -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib/x86_64-linux-gnu && \
    cmake --build /tmp/xcb-build --parallel 2 && cmake --install /tmp/xcb-build
RUN git clone https://github.com/fcitx/fcitx5.git /tmp/fcitx5 && \
    git -C /tmp/fcitx5 checkout 769928a30313329ca1a440210383e62856843b13 && \
    cmake -S /tmp/fcitx5 -B /tmp/fcitx-build -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib/x86_64-linux-gnu \
      -DENABLE_TEST=OFF -DENABLE_EMOJI=OFF -DENABLE_ENCHANT=OFF -DENABLE_LIBUUID=OFF && \
    cmake --build /tmp/fcitx-build --parallel 2 && cmake --install /tmp/fcitx-build && ldconfig
RUN git clone https://github.com/fcitx/fcitx5-qt.git /tmp/fcitx5-qt && \
    git -C /tmp/fcitx5-qt checkout 814bc3980fa4f1e0451242c723b437459f82f8cb && \
    cmake -S /tmp/fcitx5-qt -B /tmp/qt-build -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib/x86_64-linux-gnu \
      -DENABLE_QT4=OFF -DENABLE_QT6=OFF && \
    cmake --build /tmp/qt-build --parallel 2 && cmake --install /tmp/qt-build
RUN git clone https://github.com/fcitx/libime.git /tmp/libime && \
    git -C /tmp/libime checkout f21f48b01dc6b84c10e009b876c39e768b21905d && \
    git -C /tmp/libime submodule update --init --recursive && \
    cmake -S /tmp/libime -B /tmp/libime-build -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib/x86_64-linux-gnu -DENABLE_TEST=OFF && \
    cmake --build /tmp/libime-build --target IMEPinyin --parallel 2 && \
    cmake --install /tmp/libime-build/src/libime/core && \
    cmake --install /tmp/libime-build/src/libime/pinyin && ldconfig && \
    rm -rf /tmp/xcb-imdkit /tmp/xcb-build /tmp/fcitx5 /tmp/fcitx-build \
      /tmp/fcitx5-qt /tmp/qt-build /tmp/libime /tmp/libime-build
