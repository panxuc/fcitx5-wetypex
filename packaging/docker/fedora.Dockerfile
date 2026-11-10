FROM fedora:40

RUN dnf --disablerepo='*' \
      --repofrompath='fedora-archive,https://archives.fedoraproject.org/pub/archive/fedora/linux/releases/40/Everything/$basearch/os/' \
      --repofrompath='updates-archive,https://archives.fedoraproject.org/pub/archive/fedora/linux/updates/40/Everything/$basearch/' \
      --setopt=fedora-archive.gpgcheck=1 --setopt=updates-archive.gpgcheck=1 \
      --setopt='fedora-archive.gpgkey=file:///etc/pki/rpm-gpg/RPM-GPG-KEY-fedora-40-$basearch' \
      --setopt='updates-archive.gpgkey=file:///etc/pki/rpm-gpg/RPM-GPG-KEY-fedora-40-$basearch' \
      install -y -q \
      gcc gcc-c++ cmake clang git pkgconf-pkg-config boost-devel \
      fcitx5-devel fcitx5-qt-devel libime-devel json-c-devel openssl-devel fontconfig-devel ca-certificates libcxx-devel \
      qt6-qtbase-devel qt6-qtsvg-devel qt6-qtwebengine-devel layer-shell-qt-devel wayland-devel && \
    dnf clean all
