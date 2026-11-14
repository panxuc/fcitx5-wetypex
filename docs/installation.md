# 安装指南

本文说明各发行格式的适用系统、安装方法和安装后的首次设置。快速安装命令见[仓库首页](../README.md#安装)。

## 选择发行格式

每个正式版本同时提供 x86-64 与 ARM64 两种架构的以下格式。

| 格式 | 文件名形式 | 构建基线 | 适用系统 |
| --- | --- | --- | --- |
| Arch 软件包 | `fcitx5-wetypex-<版本>-<修订>-{x86_64,aarch64}.pkg.tar.zst` | 当前 Arch Linux / Arch Linux ARM 仓库 | Arch Linux 及其滚动衍生版 |
| AUR | `fcitx5-wetypex` | 在本机按本地库构建 | Arch Linux |
| 标准 deb | `fcitx5-wetypex_<版本>-<修订>_{amd64,arm64}.deb` | Ubuntu 22.04：glibc 2.35、OpenSSL 3、Qt5、Fcitx5 5.0.14 | Ubuntu 22.04、24.04、26.04，Debian 12、13 |
| legacy deb | `fcitx5-wetypex-legacy_<版本>-<修订>_{amd64,arm64}.deb` | Ubuntu 20.04：glibc 2.31、Qt 5.12 | Ubuntu 20.04 及同基线系统，需要稳定版 Fcitx5 框架 |
| rpm | `fcitx5-wetypex-<版本>-<修订>.{x86_64,aarch64}.rpm` | Fedora 40，Qt6 | Fedora 及兼容的 RPM 发行版 |
| 便携归档 | `fcitx5-wetypex-<版本>-<修订>-linux-{x86_64,aarch64}.tar.zst` | 与标准 deb 相同的 `/usr` 布局 | 已自行准备依赖的系统 |
| Nix flake | `packages.<system>.fcitx5-wetypex` | 跟随系统 Nixpkgs | NixOS 与使用 Nix 的系统 |

`<版本>` 与默认原版输入核心版本一致，`<修订>` 是适配层的打包修订号。所有格式都只包含适配层；原版运行时在[首次设置](#首次设置)时下载到用户目录。

## Arch Linux

```bash
sudo pacman -U ./fcitx5-wetypex-<版本>-<修订>-x86_64.pkg.tar.zst
```

Pacman 从已配置的仓库解析依赖，包括 Fcitx5、LibIME、Qt 6、Bubblewrap、PipeWire、FFmpeg、libc++、curl、json-c、libnotify、Polkit，以及显示微信表情原图所需的 fontconfig、fontTools 和 Pillow。发布包按 makepkg 解析出的实际 OpenSSL SONAME 记录依赖，因此只能安装在提供相同 OpenSSL ABI 的系统上。系统库落后于发布包时，请先升级系统或改用 AUR。

AUR 包在本机编译，链接本机的库：

```bash
paru -S fcitx5-wetypex
```

原生包按需要使用 `qemu-user`，Arch 包将其列为可选依赖；Flatpak 扩展自带当前架构的兼容执行器。输入和语音按各自需要的 CPU 指令集分别选择执行方式，见 [CPU 指令集与兼容执行](architecture.md#cpu-指令集与兼容执行)。

## Debian 与 Ubuntu

### 标准 deb

```bash
sudo apt install ./fcitx5-wetypex_<版本>-<修订>_amd64.deb
```

标准 deb 在 Ubuntu 22.04 上用 glibc 2.35、OpenSSL 3、Qt5 和稳定版 Fcitx5 构建，CI 在 Ubuntu 22.04、24.04、26.04 与 Debian 12、13 容器中安装它并启动网络诊断和设置窗口。软件包显式依赖 LLVM unwinder（`llvm-libunwind1` 或 `libunwind-x.y`），Ubuntu 26.04 会自动安装 `llvm-libunwind1`。

标准 deb 使用 Qt5。Ubuntu 26.04 和 Debian 13 不再提供 Qt5 版 LayerShellQt，因此 Wayland 会话中的 V 快捷栏使用 Fcitx 原生候选菜单，见[故障排查](troubleshooting.md#wayland-下-v-快捷栏不显示或闪退)。

### Ubuntu 20.04 legacy deb

Ubuntu 20.04 和其他 glibc 2.31 系统使用独立的 legacy 包：

```bash
sudo apt install ./fcitx5-wetypex-legacy_<版本>-<修订>_amd64.deb
```

安装前需要稳定版框架：

- Fcitx5 5.0.14 或更新版本
- fcitx5-qt 5.0.10 或更新版本
- LibIME 1.0.11 或更新版本

Ubuntu 20.04 官方仓库中的 `0.0~git` 预览版 Fcitx5 使用不同的 ABI，无法加载 WeTypeX 插件，也不能用 `--force-depends` 绕过。请按 [Fcitx 上游安装说明](https://fcitx-im.org/wiki/Install_Fcitx_5)安装上述稳定版本的回移植包后再安装 legacy deb。

legacy 包的网络传输组件私有静态链接 OpenSSL 3，系统继续使用自身的 OpenSSL 1.1。语音采集可使用现有的 PulseAudio 或 pipewire-pulse：现代 PipeWire 使用 `pw-record` 原始 PCM，旧版或缺少该工具时自动选择 `parec`，再回退到 FFmpeg 的 PulseAudio 输入。Qt 5.12 的设置窗口在 Wayland 下使用系统窗口边框拖动和缩放。

适配层自动更新器按 glibc 版本选择标准包或 legacy 包。

## Fedora 与兼容的 RPM 发行版

```bash
sudo dnf install ./fcitx5-wetypex-<版本>-<修订>.x86_64.rpm
```

RPM 以 Fedora 40 为构建基线，使用 Qt6 和 LayerShellQt。FFmpeg 等依赖可能来自发行版需要单独启用的附加仓库。

## 便携归档

```bash
sudo tar --zstd -C / -xf ./fcitx5-wetypex-<版本>-<修订>-linux-x86_64.tar.zst
```

便携归档不经过包管理器解析依赖，适合已经具备标准 deb 同等运行环境的系统。适配层自动更新器按发行版类型下载 Arch、deb 或 rpm 软件包，便携归档的升级请手动解压新版本。

## NixOS

Flake 提供 `packages.x86_64-linux.fcitx5-wetypex`、`packages.aarch64-linux.fcitx5-wetypex` 和各架构的 `default` 别名。在系统 flake 中加入 input，并让它跟随系统的 Nixpkgs，以匹配 Fcitx5 插件 ABI：

```nix
inputs.wetypex = {
  url = "github:panxuc/fcitx5-wetypex";
  inputs.nixpkgs.follows = "nixpkgs";
};
```

在 `outputs` 中接收 `wetypex`，再把模块加入现有的 `nixosSystem.modules`：

```nix
outputs = { nixpkgs, wetypex, ... }: {
  nixosConfigurations.YOUR_HOST = nixpkgs.lib.nixosSystem {
    # 保留已有的 system、hardware-configuration.nix 和其他 modules。
    modules = [
      ({ pkgs, ... }: {
        i18n.inputMethod = {
          enable = true;
          type = "fcitx5";
          fcitx5.addons = [
            wetypex.packages.${pkgs.stdenv.hostPlatform.system}.fcitx5-wetypex
          ];
        };
      })
    ];
  };
};
```

也可通过默认 overlay 使用 `pkgs.fcitx5-wetypex`，让包直接跟随当前系统的 Nixpkgs 和其他 overlays：

```nix
{ pkgs, ... }: {
  nixpkgs.overlays = [ wetypex.overlays.default ];
  i18n.inputMethod = {
    enable = true;
    type = "fcitx5";
    fcitx5.addons = [ pkgs.fcitx5-wetypex ];
  };
}
```

将这段模块放入上例的 `modules`，其中 `wetypex` 来自 flake 的 `outputs` 参数。包的 `.override` 参数同样可用于 overlay 提供的包。

### 构建参数

Nix 包支持通过 `.override` 调整以下参数：

| 参数 | 默认值 | 作用 |
| --- | --- | --- |
| `enableAI` | `true` | 设为 `false` 时移除问 AI 入口、查看器和 QtWebEngine 依赖。随语音功能管理的智能整理不受影响。 |
| `enableVoice` | `true` | 设为 `false` 时隐藏语音页面和快捷键、阻断语音调用，并移除语音辅助程序、采集脚本及其工具依赖。 |
| `enableLayerShell` | `true` | 设为 `false` 时 V 快捷栏改用 Fcitx 原生候选菜单，V 模式功能保留。 |
| `enableWeTypeTheme` | `true` | 设为 `false` 时隐藏皮肤和字号控件，WeTypeX 不再改写共用的 `classicui.conf`，由桌面主题统一管理。之前写入的皮肤设置需自行恢复。 |
| `enableCompatibilityProvider` | `false` | 设为 `true` 时只把当前架构的 QEMU 用户态模拟器加入闭包，用于缺少原版所需指令的 x86-64 或 ARM64 CPU，见 [CPU 指令集与兼容执行](architecture.md#cpu-指令集与兼容执行)。 |
| `defaultSettings` | `{ }` | Nix 属性集，键名沿用 `wetypex.json`，仅在偏好文件不存在时写入首次默认值。 |

示例：

```nix
(wetypex.packages.${pkgs.stdenv.hostPlatform.system}.fcitx5-wetypex.override {
  enableAI = false;
  enableVoice = false;
  enableWeTypeTheme = false;
  defaultSettings = {
    page_size = 7;
    vertical = true;
  };
})
```

布尔参数在构建时生效：关闭的功能无法通过界面或已有偏好重新启用，修改后需要重新构建、应用配置并重启 Fcitx5。关闭功能时保留其已有偏好，重新启用后继续生效。设备同步与云候选不受这些参数控制。

`defaultSettings` 转换为 UTF-8 JSON，大小上限为 16 KiB。初始化时以原子方式创建权限为 `0600` 的偏好文件；已有文件或符号链接保持原样，之后仍可在界面修改。配置目录只读而无法创建文件时，输入法读取包内默认值继续运行。字段名和类型由设置读取逻辑解释，构建时只检查类型为属性集和大小。默认值会进入 Nix store，请只放非敏感偏好。

### Nix 下的更新

Nix 包的安装路径、动态库、证书、DNS 配置、时区数据和 Bubblewrap 沙箱均已适配 Nix store。适配层由 flake 锁文件管理：更新 input 后重新构建系统即可升级，内置的适配层软件包更新器在 Nix 构建中关闭。用户目录中的原版运行时照常通过 `fcitx5-wetypex-setup` 和设置界面更新，见[运行时更新、回退与清理](runtime-updates.md)。

在源码仓库中可运行 `nix build .#fcitx5-wetypex` 单独构建。`nix flake check` 和 CI 在 x86-64 与 ARM64 上构建默认、全关闭、仅语音、仅 AI 四种组合。正式源码压缩包也包含 `flake.nix`、`flake.lock` 和 `nix/`。

## 从源码构建

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build --parallel
sudo cmake --install build
```

构建要求 CMake 3.16+、Clang、C++17 编译器、Python 3.8+ 以及 Fcitx5、LibIME、Qt 5.12+ 或 Qt6、json-c、Boost、OpenSSL、Wayland 客户端库等开发包；Fcitx5 插件在新版 Fcitx SDK 下以 C++20 编译。ARM64 的私有 libc++ 需要 CMake 3.20+ 和 Clang 16+。

常用 CMake 选项：

| 选项 | 取值 | 说明 |
| --- | --- | --- |
| `WETYPE_QT_VERSION` | `AUTO`（默认）、`5`、`6` | 选择界面工具链。`AUTO` 优先使用完整的 Qt6 开发环境，否则使用 Qt 5.12+。 |
| `WETYPE_LAYER_SHELL` | `AUTO`（默认）、`ON`、`OFF` | Wayland V 快捷栏是否使用 LayerShellQt。`ON` 在缺少 SDK 时报错。 |
| `WETYPE_BUNDLED_CURL` | `ON`（默认）、`OFF` | 默认静态链接固定版本、支持 WS/WSS 的 libcurl。`OFF` 改用系统 libcurl 8.9+，且该构建必须启用 WebSocket。 |

插件和 Qt 配置插件安装到发行版的 Fcitx5 插件目录（`FCITX_INSTALL_ADDONDIR`），私有引擎宿主位于 `<prefix>/lib/fcitx5-wetypex`。软件包构建流程见[发行与打包](distribution.md)。

## 首次设置

软件包安装完成时会在终端打印以下步骤，并链接到本文。请以日常使用输入法的用户身份（不使用 `sudo`）准备原版运行时：

```bash
fcitx5-wetypex-setup --download --accept-upstream-license
fcitx5-wetypex-setup --check --transport
```

`--accept-upstream-license` 表示你已阅读并同意微信输入法的上游许可。第一条命令从官方下载服务获取兼容清单默认档案所需的官方 ZIP，校验 SHA-256，并为本机架构准备运行时。第二条命令启动原版核心并测试本地 TLS 传输，以状态 0 退出、报告 `core_ready: true` 即表示准备完成。离线导入、指定版本和输出说明见[运行时更新、回退与清理](runtime-updates.md)。

随后[重启 Fcitx5](#重启-fcitx5)，让它加载新插件，再在 `fcitx5-configtool` 中添加 **WeTypeX**。

## 重启 Fcitx5

新安装或升级适配层后，Fcitx5 需要以新进程启动才能加载新插件。仅更新用户目录中的原版运行时时无需重启。重启方法取决于 Fcitx5 由谁启动。

### KDE Plasma Wayland（由 KWin 管理）

在 KDE Plasma Wayland 中，“系统设置 → 键盘 → 虚拟键盘”选择 Fcitx 5 后，Fcitx5 由 KWin 作为合成器管理的输入法启动，并通过继承的 `WAYLAND_SOCKET` 连接输入法协议。此时 Fcitx5 的自重启能力可能不可用（`CanRestart` 为 `false`），在终端启动的新 Fcitx5 进程也拿不到这条连接，应用会失去输入法。

请让 KWin 重新启动它：

1. 在“系统设置 → 键盘 → 虚拟键盘”中先选择“无”并应用，再选择“Fcitx 5”并应用。
2. 或者保存工作后注销并重新登录。

### X11 与独立启动的 Fcitx5

在 X11 会话，以及由桌面自启动项或用户命令（而非合成器）启动 Fcitx5 的会话中，替换当前实例：

```bash
fcitx5 -r -d
```

`-r` 替换正在运行的实例，`-d` 在后台运行。

### 验证

重启后运行 `fcitx5-diagnose`，确认 WeTypeX 插件已加载。存在待应用的运行时更新时，设置“升级和反馈”页显示“已准备就绪，将在输入结束后切换”表示新插件正在运行；仍显示“首次升级请重启 Fcitx5”表示重启尚未生效。

## 卸载

使用安装时的包管理器卸载。卸载只移除 `/usr`（或 Nix store）中的适配层，用户目录中的原版运行时、配对身份、词库和配置保持不变，重新安装后可继续使用。需要彻底删除时，先确认不再需要配对身份和词库，再手动删除[数据目录](../README.md#数据目录)中列出的路径。
