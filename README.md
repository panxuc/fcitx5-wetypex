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
| 智能拼写与云候选 | 原版本地候选与桌面云候选合并 |
| Fcitx5 输入体验 | 原生 input method 插件、内嵌预编辑、分页与选词 |
| 账户与设备 | 官方六位匹配码、确认配对、设备组和功能开关 |
| 跨设备剪贴板 | 文字与原始 PNG 图片双向同步 |
| 个人词库与常用语 | 本地持久化；设备组同步器已接入 |
| 语音输入 | PipeWire 录音、Opus 编码、原版在线识别与回填 |
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
sudo pacman -U ./fcitx5-wetypex-2.2.3.657-2-x86_64.pkg.tar.zst
```

也可以从 AUR 安装：

```bash
paru -S fcitx5-wetypex
```

安装完成后，Pacman 会在终端中打印首次设置步骤。

发布包依赖 Fcitx5、LibIME、Qt 6、Bubblewrap、PipeWire、FFmpeg、libc++、curl、json-c、libnotify 和 Polkit。Pacman 会从已配置的软件仓库解析依赖。

### Debian 与 Ubuntu

```bash
sudo apt install ./fcitx5-wetypex_2.2.3.657-2_amd64.deb
```

Debian 软件包面向提供 Fcitx5 5.1、LibIME 和 Qt 6 WebEngine 的发行版；较早版本需要使用相应 backports 或升级系统组件。

### Fedora 与兼容的 RPM 发行版

```bash
sudo dnf install ./fcitx5-wetypex-2.2.3.657-2.x86_64.rpm
```

FFmpeg 及部分桌面依赖可能来自发行版启用的附加软件仓库。

### 便携归档

```bash
sudo tar --zstd -C / -xf ./fcitx5-wetypex-2.2.3.657-linux-x86_64.tar.zst
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
```

常用操作：

- `Shift`：切换中英文，默认开启。
- `Ctrl + .`：切换中英文标点。
- `-` / `=`：组合输入时向上、向下翻页。
- `=`：没有拼音组合时，使用光标前的文本打开问 AI。
- `Ctrl + Win + Shift`：启动语音输入，再按任意键结束。
- `Ctrl + Win`：按住说话，松开结束。

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
- 语音没有结果：确认 PipeWire 默认输入设备可用，且 `pw-record` 和 FFmpeg 已安装。
- AI 窗口缺少图标：执行 `fcitx5-wetypex-setup --archive ... --icons-only` 重新提取原版界面资源。

更多诊断步骤见[故障排查](docs/troubleshooting.md)。

## 文档

- [架构说明](docs/architecture.md)
- [功能状态](docs/feature-status.md)
- [配置参考](docs/configuration.md)
- [故障排查](docs/troubleshooting.md)
- [发行与打包](docs/distribution.md)

## 参与开发

提交问题时请附上桌面环境、显示协议、Fcitx5 版本和去除隐私信息后的日志。界面变更应注明参考平台与版本。具体要求见[贡献指南](CONTRIBUTING.md)和[安全策略](SECURITY.md)。

## 许可与声明

WeTypeX 自有适配代码使用 [MIT License](LICENSE)。libkqueue 保留其上游许可。原版程序、词典、模型、界面资源、服务和商标归各自权利人所有，不受本项目许可证覆盖；详见 [NOTICE](NOTICE)。

WeTypeX 不提供绕过账户验证、伪造同步状态或替代官方服务的实现。上游协议、接口或服务随时可能变化。
