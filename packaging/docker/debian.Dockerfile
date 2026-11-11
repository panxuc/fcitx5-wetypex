FROM ubuntu:22.04

RUN apt-get update -qq && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
      build-essential cmake clang git pkg-config libboost-dev \
      libfcitx5core-dev libfcitx5config-dev libfcitx5utils-dev \
      fcitx5-modules-dev libfcitx5-qt-dev libimecore-dev libimepinyin-dev \
      libjson-c-dev libssl-dev libfontconfig1-dev libwayland-dev ca-certificates libc++-dev libc++abi-dev \
      qtbase5-dev libqt5svg5-dev qtwebengine5-dev && \
    rm -rf /var/lib/apt/lists/*

# LLVM 18 libc++ requires Clang 16+. Keep Jammy's glibc while using a newer
# build compiler for the private ARM64 ABI library.
COPY packaging/keys/llvm-snapshot.asc /tmp/llvm.asc
RUN if [ "$(dpkg --print-architecture)" = arm64 ]; then \
      apt-get -o Acquire::Retries=3 update -qq && \
      apt-get -o Acquire::Retries=3 install -y -qq gnupg && \
      gpg --show-keys --with-colons /tmp/llvm.asc | grep -q ':6084F3CF814B57C1CF12EFD515CF4D18AF4F7421:' && \
      gpg --dearmor -o /usr/share/keyrings/llvm.gpg /tmp/llvm.asc && \
      echo 'deb [signed-by=/usr/share/keyrings/llvm.gpg] https://apt.llvm.org/jammy/ llvm-toolchain-jammy-18 main' \
        > /etc/apt/sources.list.d/llvm.list && \
      apt-get -o Acquire::Retries=3 update -qq && \
      apt-get -o Acquire::Retries=3 install -y -qq clang-18 && \
      ln -sf /usr/bin/clang-18 /usr/local/bin/clang && \
      ln -sf /usr/bin/clang++-18 /usr/local/bin/clang++ && \
      rm -rf /var/lib/apt/lists/*; \
    fi && rm -f /tmp/llvm.asc
