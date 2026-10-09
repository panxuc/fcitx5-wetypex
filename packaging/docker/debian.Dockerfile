FROM ubuntu:22.04

RUN apt-get update -qq && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
      build-essential cmake clang git pkg-config libboost-dev \
      libfcitx5core-dev libfcitx5config-dev libfcitx5utils-dev \
      fcitx5-modules-dev libfcitx5-qt-dev libimecore-dev libimepinyin-dev \
      libjson-c-dev libssl-dev ca-certificates libc++-dev libc++abi-dev \
      qtbase5-dev libqt5svg5-dev qtwebengine5-dev && \
    rm -rf /var/lib/apt/lists/*
