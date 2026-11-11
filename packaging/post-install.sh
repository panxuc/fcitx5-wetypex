#!/bin/sh
set -eu

command -v update-desktop-database >/dev/null 2>&1 && \
  update-desktop-database /usr/share/applications >/dev/null 2>&1 || true
command -v gtk-update-icon-cache >/dev/null 2>&1 && \
  gtk-update-icon-cache -q -t /usr/share/icons/hicolor >/dev/null 2>&1 || true

cat <<'EOF'

WeTypeX 已安装。请以日常使用输入法的用户身份完成设置：

  1. 准备官方运行时（先阅读并接受上游软件许可）：
     fcitx5-wetypex-setup --download --accept-upstream-license

  2. 检查核心和本地传输：
     fcitx5-wetypex-setup --check --transport

  3. 重启 Fcitx5，让它加载新插件：
     KDE Plasma Wayland：在系统设置的“虚拟键盘”中先选择“无”，再选择 Fcitx 5；
                         也可注销并重新登录。
     X11 或独立启动的 Fcitx5：fcitx5 -r -d

  4. 打开 fcitx5-configtool，将 WeTypeX 加入输入法列表。

离线安装、旧版迁移和其他桌面环境：
https://github.com/panxuc/fcitx5-wetypex/blob/main/docs/installation.md
EOF
