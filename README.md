<p align="center">
  <img src="data/icons/hicolor/256x256/apps/fcitx5-wetypex.png" width="112" alt="WeTypeX 图标">
</p>

<h1 align="center">WeTypeX</h1>

<p align="center">
  在 Linux 上通过 Fcitx5 使用微信输入法的兼容项目。
</p>

<p align="center">
  <img src="https://img.shields.io/badge/Linux-x86__64-FCC624?style=flat-square&logo=linux&logoColor=black" alt="Linux x86-64">
  <img src="https://img.shields.io/badge/Input-Fcitx5-27A7E7?style=flat-square" alt="Fcitx5">
  <a href="LICENSE"><img src="https://img.shields.io/badge/License-MIT-blue?style=flat-square" alt="MIT 许可证"></a>
</p>

> [!IMPORTANT]
> 可能包含微信输入法官方服务的遥测。

> [!IMPORTANT]
> 目前处于早期开发阶段，许多功能尚未经过测试。

> [!IMPORTANT]
> WeTypeX 是一个非官方的第三方开源项目，仅供个人学习交流使用，请于下载后 24 小时内删除。
>
> 本项目并非由腾讯公司（Tencent）或其关联公司开发、维护或认可。本项目中提及的“微信输入法”及其相关商标、标识均为腾讯公司（Tencent）的注册商标或财产。本项目的存在不意味着与腾讯公司（Tencent）存在任何关联、合作关系，也不代表腾讯公司（Tencent）对本项目有任何形式的认可或背书。
>
> 本项目的开发者尊重他人的知识产权。本项目仅旨在实现技术上的兼容与互通，不包含、不复制、不传播任何微信输入法的专有代码、二进制文件或受版权保护的资源。用户在使用本项目的过程中，需自行获取并遵守微信输入法官方软件的使用条款与许可协议。任何单位或个人如认为本项目中包含的内容（包括但不限于代码、文档、配置信息等）可能涉嫌侵犯其合法权益（包括但不限于著作权、商标权、专利权等），请及时[联系项目维护者](mailto:panxuc@panxuc.com)。项目维护者在收到通知后，将在合理期限内移除、下架或断开被控侵权内容的访问链接。
>
> 您使用本项目资料的风险完全由您自行承担。开发者不保证本项目资料的功能不会中断或没有错误，不保证缺陷会被修正，也不保证本项目资料兼容所有系统环境。

## 功能概览

| 能力 | Linux 集成状态 |
| --- | --- |
| 全拼、双拼、五笔 | 使用原版 macOS 2.2.3.657 输入核心和词典 |
| 智能拼写与云候选 | 原版本地候选与桌面云候选合并，云端新增候选带 ☁ 标记 |
| 微信表情 | 仅在可识别的微信客户端中推荐，候选显示用户安装包中的原图；普通 emoji 保留 |
| Fcitx5 输入体验 | 原生 input method 插件、内嵌预编辑、分页与选词 |
| 账户与设备 | 官方六位匹配码、确认配对、设备组和功能开关 |
| 跨设备剪贴板 | 文字与原始 PNG 图片双向同步 |
| 个人词库与常用语 | 本地持久化；设备组同步器已接入 |
| 语音输入 | PipeWire/PulseAudio 流式采集，原版语音引擎编码、VAD、识别、整理与实时预编辑 |
| 问 AI | `=` 上下文触发、真实在线回答和独立问答窗口 |
| 隔空传送 | 官方传输码与二维码、原版 Flurry LAN 与 WXP2P 直连/中继通道 |
| V 模式 | 原版计算候选，以及计算、剪贴板、常用语和符号快捷栏 |

目前仅在 Arch Linux、KDE Plasma 和 Wayland 上进行过测试。各项能力的支持范围见[功能状态](docs/feature-status.md)。

## 工作方式

WeTypeX 安装程序从官方包中提取输入核心与词典，生成固定地址的 Linux 可加载映像；运行时由一个小型 Mach-O ABI 兼容宿主提供原版核心实际使用的 Darwin C/C++ 接口，并将网络传输接到 Linux 的 TLS 与 WebSocket 实现。

```text
应用程序
   │ Fcitx5 input context
   ▼
WeTypeX 插件 ────────── 候选、预编辑、快捷键、剪贴板
   │ 私有管道
   ▼
受限引擎宿主 ───────── 原版输入核心、词典与 Flurry/WXP2P 文件通道
   │                    │
   │ Linux ABI 适配      └─ 用户词库 / 常用语
   ▼
原版业务服务 ────────── 云候选、配对、同步、语音、AI
```

输入宿主和账户命令运行在 Bubblewrap 沙箱中。密码输入上下文不会触发原版核心、剪贴板记录或 AI；浏览器标记为敏感的普通输入仍可使用输入法。详细边界见[架构说明](docs/architecture.md)。

## 安装

### Arch Linux

从 [GitHub Releases](https://github.com/panxuc/fcitx5-wetypex/releases/latest) 下载软件包：

```bash
sudo pacman -U ./fcitx5-wetypex-2.2.3.657-13-x86_64.pkg.tar.zst
```

也可以从 AUR 安装：

```bash
paru -S fcitx5-wetypex
```

安装完成后，Pacman 会在终端中打印首次设置步骤。

发布包依赖 Fcitx5、LibIME、Qt 6、Bubblewrap、PipeWire、FFmpeg、libc++、curl、json-c、libnotify 和 Polkit；微信表情原图显示还使用 fontconfig、fontTools 和 Pillow。Pacman 会从已配置的软件仓库解析依赖。

### Debian 与 Ubuntu

```bash
sudo apt install ./fcitx5-wetypex_2.2.3.657-13_amd64.deb
```

从修订 `8` 起，标准 deb 使用 Ubuntu 22.04 的 glibc 2.35、Qt5 和稳定版 Fcitx5 构建，可用于 Ubuntu 22.04、24.04、26.04 及 Debian 12/13。LLVM unwinder 已显式列入依赖，Ubuntu 26.04 会安装 `llvm-libunwind1`，避免核心因缺少 `libunwind.so.1` 无法启动。

Ubuntu 20.04 和 glibc 2.31 的系统使用独立的 `fcitx5-wetypex-legacy_..._amd64.deb`。安装前需要稳定版 Fcitx5 5.0.14+、fcitx5-qt 5.0.10+ 与 LibIME 1.0.11+；20.04 原生仓库中的 `0.0~git` 预览版使用不同 ABI，不能直接加载此插件。可按 [Fcitx 上游安装说明](https://fcitx-im.org/wiki/Install_Fcitx_5) 安装稳定框架。旧系统包私有静态包含 OpenSSL 3，不替换系统的 OpenSSL 1.1。语音可使用现有的 PulseAudio 或 pipewire-pulse；现代 PipeWire 使用 `pw-record` 原始 PCM，旧版或缺少该工具时自动选择 `parec`，再回退到 FFmpeg 的 PulseAudio 输入。

源码构建支持 CMake 3.16、C++17、Python 3.8、Qt5.12+，也可自动使用完整的 Qt6 开发环境。用 `-DWETYPE_QT_VERSION=5` 或 `6` 指定界面工具链。配置插件采用发行版的 Fcitx5 安装目录；不同 Qt ABI 的配置入口按构建环境提供。完整设置始终可通过 `fcitx5-wetypex-settings` 打开。

### Fedora 与兼容的 RPM 发行版

```bash
sudo dnf install ./fcitx5-wetypex-2.2.3.657-13.x86_64.rpm
```

FFmpeg 及部分桌面依赖可能来自发行版启用的附加软件仓库。

RPM 以 Fedora 40 为构建基线。Arch 是滚动发行版，发布包跟随当前仓库；旧系统应保持库版本一致或在对应环境从源码构建。

### NixOS

Flake 提供 `packages.x86_64-linux.fcitx5-wetypex` 和同一包的 `default` 别名。
在现有系统 flake 中加入 input，并跟随系统的 Nixpkgs，以匹配 Fcitx5 插件 ABI：

```nix
inputs.wetypex = {
  url = "github:panxuc/fcitx5-wetypex";
  inputs.nixpkgs.follows = "nixpkgs";
};
```

在 `outputs` 中接收 `wetypex`，然后把模块加入现有 `nixosSystem.modules`：

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

Nix 包支持通过 `.override` 调整以下参数，四个布尔开关默认均为 `true`：

| 参数 | 默认值 | 作用 |
| --- | --- | --- |
| `enableAI` | `true` | 关闭时移除 AI 助手入口、查看器和 QtWebEngine 依赖；不影响随语音功能管理的智能润色。 |
| `enableVoice` | `true` | 关闭时隐藏语音页面和快捷键、阻断语音调用，并移除语音助手、采集脚本及其命令工具依赖。 |
| `enableLayerShell` | `true` | 关闭 V 模式的 LayerShell 浮动工具栏，改用 Fcitx 原生候选菜单；V 模式功能保留。 |
| `enableWeTypeTheme` | `true` | 关闭时隐藏皮肤和字号控件，并禁止 WeTypeX 改写共用的 `classicui.conf`，便于桌面主题统一管理。不会自动恢复之前写入的皮肤。 |
| `defaultSettings` | `{ }` | Nix 属性集，键沿用 `wetypex.json`；仅在偏好文件不存在时写入首次默认值。 |

例如，在 `fcitx5.addons` 中使用：

```nix
(wetypex.packages.${pkgs.stdenv.hostPlatform.system}.fcitx5-wetypex.override {
  enableAI = false;
  enableVoice = false;
  enableLayerShell = true;
  enableWeTypeTheme = false;
  defaultSettings = {
    page_size = 7;
    vertical = true;
  };
})
```

这些布尔参数在构建时生效，关闭的功能不能通过界面或已有偏好重新启用；修改后需重新构建、应用配置并重启 Fcitx5。关闭功能时保留其已有偏好，便于之后重新启用。设备同步与云候选不受这些开关控制。

`defaultSettings` 转换为 UTF-8 JSON，大小不得超过输入法支持的 16 KiB。初始化时以原子方式创建权限为 `0600` 的偏好文件；已有文件或符号链接不会覆盖、补写，之后仍可在界面修改。配置目录只读等原因导致无法创建文件时，输入法继续读取包内默认值；保存失败会提示，不会阻断输入核心启动。字段名和字段类型由上游设置读取逻辑解释，这里不提供逐字段校验。默认值会进入 Nix store，适合非敏感偏好。

应用系统配置后，按下方“首次设置”准备原版运行时。Nix 构建只安装社区适配层，原版安装包和资源仍由用户在接受上游许可后自行初始化。

包的安装路径、动态库、证书和 Bubblewrap 沙箱已适配 Nix store；Nix 包由 flake 锁文件管理更新，内置发行版更新器关闭。更新 input 后重新构建系统即可升级。

包会为录音和设置界面提供所需工具路径，并在沙箱中提供 DNS 配置和时区数据，不依赖 FHS 的 `/usr` 布局。CI 同时检查 flake 构建和源码包中的 Nix 入口；正式源码压缩包也包含 `flake.nix`、锁文件及 `nix/`。

在源码仓库中可单独运行 `nix build .#fcitx5-wetypex`。`nix flake check` 和 CI 会构建默认、全关闭、仅语音、仅 AI 四种组合，检查混合开关的兼容性。本 flake 仅提供 Linux x86_64 输出。

### 便携归档

```bash
sudo tar --zstd -C / -xf ./fcitx5-wetypex-2.2.3.657-13-linux-x86_64.tar.zst
```

便携归档不经过软件包管理器解析依赖，更适合已经准备好运行环境的系统。

### 从源码安装

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build --parallel
sudo cmake --install build
```

构建要求 CMake 3.24、Clang、C++20 编译器以及上述依赖的开发文件。完整依赖和打包流程见[发行与打包](docs/distribution.md)。

## 首次设置

准备官方运行时有两种方式。

使用已经下载的官方安装包：

固定版本的官方下载地址：[WeType_2.2.3_657.zip](https://download.weread.qq.com/app/wxkb/mac/2.2.3/WeType_2.2.3_657.zip)。

```bash
fcitx5-wetypex-setup --archive /path/to/WeType_2.2.3_657.zip
```

明确接受上游许可后，从脚本内固定的腾讯地址下载：

```bash
fcitx5-wetypex-setup --download --accept-upstream-license
```

检查提取结果：

```bash
fcitx5-wetypex-setup --check
```

随后打开 Fcitx5 配置工具，将 **WeTypeX** 添加到当前输入法列表。若桌面会话已经运行 Fcitx5，可执行：

```bash
fcitx5-remote -r
```

配置、原版运行时、配对身份、词库和同步状态都位于用户目录，与 `/usr` 下的软件文件分离；正常升级不会重建账户。

## 日常使用

```bash
fcitx5-wetypex-settings          # 打开完整设置窗口
fcitx5-wetypex-setup --check     # 检查原版运行时
fcitx5-wetypex-account group-info # 查看设备组和同步状态
fcitx5-wetypex-account pairing-code # 生成新的六位匹配码
fcitx5-wetypex-account network-info # 检查实际 libcurl 与 WS/WSS 支持
fcitx5-wetypex-account sync-now     # 请求个人词库与常用语同步
```

常用操作：

- `Shift`：切换中英文，默认开启。
- `Ctrl + .`：切换中英文标点。
- `-` / `=`：组合输入时向上、向下翻页。
- `=`：没有拼音组合时，使用光标前的文本打开问 AI。
- `V`：打开快捷工具栏，再按 `V`、`Esc` 或点击“关闭”结束；点击剪贴板、常用语或符号后，内容会插入原输入框。选择“计算”后直接输入表达式，按空格或回车选取结果。
- `Ctrl + Win + Shift`：启动或结束语音输入；`Ctrl + Win` 按住说话、松开结束。录音时可用空格结束、Enter 提交当前结果、Esc 取消，详见[流式语音输入](docs/voice-input.md)。
- `Ctrl + Win`：按住说话，松开结束。

外观中的“候选序号样式”可选择微信风格或 Fcitx 默认标签。Wayland 下的 V 工具栏优先使用 layer-shell 保持原输入框焦点，并尽量限制在当前窗口范围内，避免点击时焦点落到下方窗口；构建依赖与兼容回退见[故障排查](docs/troubleshooting.md#wayland-下-v-工具栏闪退或不显示)。Hyprland 0.55+ 的窗口规则与 app_id 配置见[Hyprland 兼容](docs/hyprland.md)。

所有快捷键、输入方式、候选数量、主题、模糊音和同步开关也会出现在 Fcitx5 自带的输入法配置页中；同一页面提供完整设置、账号与同步入口，采用与 Fcitx5 拼音词典管理器相同的 `ExternalOption` 扩展机制。配置项说明见[配置参考](docs/configuration.md)。

## 数据目录

| 内容 | 默认位置 |
| --- | --- |
| 原版核心与词典 | `~/.local/share/fcitx5-wetypex/runtime` |
| 账户身份与原版业务状态 | `~/.local/share/fcitx5-wetypex/account` |
| 用户词库、常用语与同步状态 | `~/.local/share/fcitx5-wetypex/state` |
| 从官方包提取的界面资源 | `~/.local/share/fcitx5-wetypex/ui` |
| WeTypeX 配置 | `~/.config/fcitx5/wetypex.json` |

卸载系统软件包不会自动删除这些用户数据。需要重新配对时，可先备份后自行删除 `account` 与 `state/sync-state.json`；普通升级不应删除它们。

## 故障排查

首先运行：

```bash
fcitx5-wetypex-setup --check
fcitx5-diagnose | less
```

常见问题：

- 看不到 WeTypeX：重新加载 Fcitx5，并用 `fcitx5-diagnose` 检查插件搜索路径。
- 核心启动失败：重新用固定版本官方包执行设置，检查 `manifest.txt` 和 `image.macho` 是否存在。
- 没有云候选：确认未开启单机模式，并检查设备身份是否已经建立。
- 语音没有结果：确认默认输入设备可用；旧版 PipeWire 可通过 `parec` 或 FFmpeg 使用现有 PulseAudio 接口。
- AI 窗口缺少图标：执行 `fcitx5-wetypex-setup --archive ... --icons-only` 重新提取原版界面资源。

更多诊断步骤见[故障排查](docs/troubleshooting.md)。

## 文档

- [架构说明](docs/architecture.md)
- [功能状态](docs/feature-status.md)
- [配置参考](docs/configuration.md)
- [Linux 微信表情原图与应用识别](docs/wechat-emoticons.md)
- [流式语音输入与诊断](docs/voice-input.md)
- [Hyprland 兼容](docs/hyprland.md)
- [故障排查](docs/troubleshooting.md)
- [发行与打包](docs/distribution.md)

## 参与开发

提交问题时请附上桌面环境、显示协议、Fcitx5 版本和去除隐私信息后的日志。界面变更应注明参考平台与版本。具体要求见[贡献指南](CONTRIBUTING.md)和[安全策略](SECURITY.md)。

## 许可与声明

WeTypeX 自有适配代码使用 [MIT License](LICENSE)。libkqueue 保留其上游许可。原版程序、词典、模型、界面资源、服务和商标归各自权利人所有，不受本项目许可证覆盖；详见 [NOTICE](NOTICE)。

WeTypeX 不提供绕过账户验证、伪造同步状态或替代官方服务的实现。上游协议、接口或服务随时可能变化。
