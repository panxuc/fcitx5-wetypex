# Flatpak 扩展

WeTypeX 可作为 Flatpak 版 Fcitx5 的扩展安装，扩展 ID 为 `org.fcitx.Fcitx5.Addon.WeTypeX`。它与宿主机安装的 deb、rpm、Arch 包分别保存配置和用户数据。当前构建目标为 Flathub 的 Fcitx5 `stable` 分支和 KDE 6.11 运行时。

扩展包含适配层及必要的开源依赖。原版输入核心、词典、界面和语音资源在接受上游许可后另行准备。已安装 Flatpak 版 Fcitx5 的用户无需安装开发 SDK；两种架构均只携带当前架构的 QEMU 用户态程序，供缺少原版所需指令的 CPU 使用；执行方式按输入、语音分别选择，兼容组件由 Flatpak 一并更新。

## 安装和首次设置

当前先提供开发构建和 CI 安装包，尚未上架 Flathub。以下以用户级安装为例；已有 Flatpak 版 Fcitx5 时跳过第一条：

```bash
flatpak install --user flathub org.fcitx.Fcitx5
flatpak install --user ./fcitx5-wetypex-flatpak-x86_64.flatpak
# ARM64 使用 fcitx5-wetypex-flatpak-aarch64.flatpak
```

完整退出并重新启动 Flatpak 版 Fcitx5，在其配置工具的输入法列表中添加 WeTypeX，再打开 WeTypeX 设置准备原版运行时：

```bash
flatpak run org.fcitx.Fcitx5
flatpak run --command=fcitx5-configtool org.fcitx.Fcitx5
```

也可直接打开扩展设置：

```bash
flatpak run --command=/app/addons/WeTypeX/bin/fcitx5-wetypex-settings org.fcitx.Fcitx5
```

桌面接入沿用 Flatpak 版 Fcitx5 的配置。宿主机 Gtk/Qt 应用若通过输入法模块接入，仍需宿主机提供相应模块，见 [Fcitx 官方安装说明](https://fcitx-im.org/wiki/Install_Fcitx_5)。安装扩展不会切换正在使用的宿主机输入法。

## 麦克风和文件接收

扩展继承 Fcitx5 父应用的权限。使用麦克风前，为父应用允许 PulseAudio 兼容接口；原生 PipeWire 桌面可通过 pipewire-pulse 提供该接口：

```bash
flatpak override --user --socket=pulseaudio org.fcitx.Fcitx5
```

隔空传送需要将接收文件写入下载目录时，可只授权下载目录：

```bash
flatpak override --user --filesystem=xdg-download org.fcitx.Fcitx5
```

修改权限后重新启动 Flatpak 版 Fcitx5。扩展使用 Fcitx 原生 V 菜单提供计算、剪贴板、常用语和符号入口。

## 更新和数据位置

适配层由 Flatpak 更新。开发安装包通过重新安装较新的 `.flatpak` 文件更新；正式仓库分发后使用 `flatpak update`。扩展不会调用宿主机的 deb、rpm 或 Arch 安装器。

原版运行时继续通过 WeTypeX 设置更新、回退和清理。数据位于：

- 配置：`~/.var/app/org.fcitx.Fcitx5/config/fcitx5/wetypex.json`
- 运行时、账户和用户数据：`~/.var/app/org.fcitx.Fcitx5/data/fcitx5-wetypex/`
- 下载缓存：`~/.var/app/org.fcitx.Fcitx5/cache/fcitx5-wetypex/`

这些目录不与原生输入法自动共享。运行时更新的数据保护与清理规则见[运行时更新、回退与清理](runtime-updates.md)。

## 从源码构建

安装 Flatpak Builder、Flathub 的 Fcitx5、KDE SDK 6.11 和 LLVM 21 SDK 扩展，然后在仓库根目录执行：

```bash
flatpak-builder --user --install-deps-from=flathub --jobs=2 \
  --repo=build/flatpak/repo build/flatpak/app \
  packaging/flatpak/org.fcitx.Fcitx5.Addon.WeTypeX.json
flatpak build-bundle --runtime \
  --runtime-repo=https://dl.flathub.org/repo/flathub.flatpakrepo \
  build/flatpak/repo fcitx5-wetypex-flatpak-x86_64.flatpak \
  org.fcitx.Fcitx5.Addon.WeTypeX stable
```

原生包保持使用 Bubblewrap。Flatpak 构建启用 `WETYPE_FLATPAK`，通过 Flatpak 的原生子沙箱接口挂载只读运行时、独立状态目录和可选的网络命名空间。运行时及用户路径不依赖 `/input`、`/work` 等固定根目录；账户、语音和 AI 请求沿用同一启动接口。
