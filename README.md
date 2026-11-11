<!-- markdownlint-disable MD041 MD028 -->
<p align="center">
  <img src="data/icons/hicolor/256x256/apps/fcitx5-wetypex.png" width="112" alt="WeTypeX 图标">
</p>

<h1 align="center">WeTypeX</h1>

<p align="center">
  在 Linux 上通过 Fcitx5 使用微信输入法的兼容项目。
</p>

<p align="center">
  <img src="https://img.shields.io/badge/Linux-x86--64%20%7C%20ARM64-FCC624?style=flat-square&logo=linux&logoColor=black" alt="Linux x86-64 与 ARM64">
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

| 能力 | Linux 集成方式 |
| --- | --- |
| 全拼、双拼、五笔 | 原版输入核心与词典生成候选 |
| 智能拼写与云候选 | 原版本地候选与云候选合并，云端新增候选带 ☁ 标记 |
| 微信表情 | 在可识别的微信客户端中推荐，候选显示用户本机提取的原图；普通 emoji 照常可用 |
| Fcitx5 输入体验 | 原生输入法插件、内嵌预编辑、分页与选词 |
| 账户与设备 | 官方六位匹配码、确认配对、设备组和功能开关 |
| 跨设备剪贴板 | 文字与原始 PNG 图片双向同步 |
| 个人词库与常用语 | 本地持久化，接入原版设备组同步 |
| 语音输入 | PipeWire/PulseAudio 流式采集，原版语音引擎识别并实时显示预编辑 |
| 问 AI | `=` 以光标前文本提问，在独立窗口显示回答 |
| 隔空传送 | 官方传输码与二维码，原版 Flurry 局域网与 WXP2P 直连/中继通道 |
| V 模式 | 计算、剪贴板、常用语和符号快捷栏 |

每项功能的验证程度见[功能状态](docs/feature-status.md)。日常桌面验证主要在 Arch Linux、KDE Plasma 和 Wayland 上进行；CI 另在多个发行版容器中安装软件包并运行真实原版核心检查。

## 工作方式

WeTypeX 由两部分组成：

- **适配层**：本仓库构建的开源软件包，包括 Fcitx5 插件、ABI 兼容宿主、设置界面和辅助命令。由包管理器或 Nix 安装。
- **原版运行时**：首次设置时从微信输入法官方下载服务获取的专有组件，校验 SHA-256 后保存在用户目录。WeTypeX 的源码包和软件包都不含这些二进制文件或资源。

当前默认原版输入核心为 3.0.1.684；隔空传送模块、语音模型和界面资源继续取自经过验证的 2.2.3.657 安装包。设置中的“原版输入核心”版本只表示核心，适配层版本独立显示。

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

输入宿主和账户命令运行在 Bubblewrap 沙箱中。密码输入框不会进入原版核心、剪贴板记录或 AI。详见[架构说明](docs/architecture.md)。

## 安装

从 [GitHub Releases](https://github.com/panxuc/fcitx5-wetypex/releases/latest) 下载与系统和架构匹配的软件包。下列示例使用 x86-64 文件名，ARM64 文件名在注释中给出。

### Arch Linux

```bash
sudo pacman -U ./fcitx5-wetypex-3.0.1.684-2-x86_64.pkg.tar.zst   # ARM64: ...-aarch64.pkg.tar.zst
# 或从 AUR 在本机构建
paru -S fcitx5-wetypex
```

### Debian 与 Ubuntu

标准包支持 Ubuntu 22.04 及更新版本、Debian 12/13。

```bash
sudo apt install ./fcitx5-wetypex_3.0.1.684-2_amd64.deb          # ARM64: ..._arm64.deb
```

### Ubuntu 20.04

需先安装稳定版 Fcitx5 框架，见[安装指南](docs/installation.md#ubuntu-2004-legacy-deb)。

```bash
sudo apt install ./fcitx5-wetypex-legacy_3.0.1.684-2_amd64.deb   # ARM64: ..._arm64.deb
```

### Fedora 及兼容的 RPM 发行版

```bash
sudo dnf install ./fcitx5-wetypex-3.0.1.684-2.x86_64.rpm          # ARM64: ...-2.aarch64.rpm
```

### 便携归档

```bash
sudo tar --zstd -C / -xf ./fcitx5-wetypex-3.0.1.684-2-linux-x86_64.tar.zst   # ARM64: ...-linux-aarch64.tar.zst
```

NixOS flake、源码构建、各格式的构建基线与依赖说明见[安装指南](docs/installation.md)。ARM64 的 CPU 要求和旧 CPU 兼容方式见 [ARM64 支持](docs/arm64.md)。

## 首次设置

1. 阅读并接受微信输入法的上游许可后，下载并准备默认原版运行时：

   ```bash
   fcitx5-wetypex-setup --download --accept-upstream-license
   ```

   离线环境可先导入官方 ZIP，步骤见[离线准备](docs/runtime-updates.md#离线准备)。

2. 检查运行时和本地传输通道：

   ```bash
   fcitx5-wetypex-setup --check --transport
   ```

   命令以状态 0 退出，输出中 `core_ready` 为 `true`、`missing_runtime` 和 `missing_ui` 为空即表示准备完成。

3. 重启 Fcitx5，让它加载新安装的插件：
   - KDE Plasma Wayland：在“系统设置 → 键盘 → 虚拟键盘”中先选择“无”并应用，再选择“Fcitx 5”并应用；也可以注销后重新登录。
   - X11 会话或非 KWin 托管的 Fcitx5：运行 `fcitx5 -r -d`。

   原因和验证方法见[重启 Fcitx5](docs/installation.md#重启-fcitx5)。软件包安装完成时，终端也会打印这些步骤。

4. 打开 `fcitx5-configtool`，把 **WeTypeX** 添加到当前输入法列表。

配置、原版运行时、配对身份、词库和同步状态都保存在用户目录，与 `/usr` 下的适配层分离。升级适配层不会重建账户。

## 日常使用

| 按键 | 作用 |
| --- | --- |
| `Shift` | 单独按下并松开时切换中英文 |
| `Ctrl + .` | 在当前输入框临时切换中英文标点；默认标点在设置“输入”页中选择 |
| `-` / `=`、`[` / `]` | 组合输入时上下翻页 |
| `=` | 没有组合输入时，以光标前文本打开问 AI |
| `v` 后按空格 | 中文全拼下打开 V 快捷栏；`v1+1` 等算式直接计算 |
| `Ctrl + Win + Shift` | 启动或结束语音输入 |
| `Ctrl + Win` | 按住说话，松开结束 |

全部快捷键、输入方式、外观和同步开关可在 `fcitx5-wetypex-settings` 或 Fcitx5 自带的输入法配置页中修改，说明见[配置参考](docs/configuration.md)和[按键与输入模式](docs/input-handling.md)。Hyprland 用户请参阅 [Hyprland 兼容](docs/hyprland.md)。

常用命令：

```bash
fcitx5-wetypex-settings                  # 打开完整设置窗口
fcitx5-wetypex-setup --status            # 查看当前、默认和待应用的原版运行时
fcitx5-wetypex-setup --check --transport # 检查运行时，并测试本地 TLS 传输通道
fcitx5-wetypex-account network-info      # 检查网络组件与 ARM64 执行方式，不联网
fcitx5-wetypex-account group-info        # 查看设备组和同步功能位
fcitx5-wetypex-account pairing-code      # 生成新的六位匹配码
fcitx5-wetypex-account sync-now          # 请求同步个人词库与常用语
```

## 更新

更新分为两层：

- **适配层**由包管理器管理。开启“有新版本时自动更新”后，WeTypeX 每天最多检查一次 GitHub Release，下载并校验对应软件包，再通过系统授权界面安装。Nix 用户通过 Nix 配置更新适配层。
- **原版运行时**在用户目录中更新，无需管理员授权。WeTypeX 只准备适配层兼容清单中列出的版本，并在输入、语音和请求结束后切换。

首次升级到支持运行时热更新的适配层后，需要按上文方法重启一次 Fcitx5。手动更新、回退、清理和离线导入见[运行时更新、回退与清理](docs/runtime-updates.md)。

## 数据目录

| 内容 | 默认位置 |
| --- | --- |
| 当前原版运行时（指向当前版本代） | `~/.local/share/fcitx5-wetypex/runtime` |
| 运行时版本代、数据快照与迁移前的原目录 | `~/.local/share/fcitx5-wetypex/runtime-generations` |
| 账户身份与原版业务状态 | `~/.local/share/fcitx5-wetypex/account` |
| 用户词库、常用语、语音与同步状态 | `~/.local/share/fcitx5-wetypex/state` |
| 原版界面资源 | `~/.local/share/fcitx5-wetypex/ui` |
| 官方 ZIP 与共享只读文件缓存 | `~/.cache/fcitx5-wetypex/archives` |
| 已下载的适配层更新包 | `~/.cache/fcitx5-wetypex/updates` |
| WeTypeX 配置 | `~/.config/fcitx5/wetypex.json` |

卸载软件包会保留这些用户数据。需要重新配对时，按[故障排查](docs/troubleshooting.md#重新配对)先备份再操作。

## 故障排查

```bash
fcitx5-wetypex-setup --check
fcitx5-diagnose | less
```

| 现象 | 处理 |
| --- | --- |
| 输入法列表中没有 WeTypeX | 重启 Fcitx5，再用 `fcitx5-diagnose` 检查插件搜索路径 |
| 提示核心不可用 | 运行 `fcitx5-wetypex-setup --status` 和 `--check`，按输出重新准备运行时 |
| 没有云候选 | 确认单机模式关闭，并用 `fcitx5-wetypex-account group-info` 检查设备身份 |
| 语音没有结果 | 确认默认输入设备可用，再运行 `fcitx5-wetypex-voice status`；VAD 模型缺失时按[修复语音资源](docs/voice-input.md#修复语音资源)恢复 |
| 界面缺少图标 | 运行 `fcitx5-wetypex-setup --icons-only --download --accept-upstream-license` |

更多情形见[故障排查](docs/troubleshooting.md)。

## 文档

完整目录见[文档索引](docs/README.md)。

## 参与开发

提交问题时请附上发行版、架构、桌面环境、显示协议、Fcitx5 版本和去除隐私信息后的日志。代码与文档要求见[贡献指南](CONTRIBUTING.md)，安全问题请按[安全策略](SECURITY.md)私密报告。

## 许可与声明

WeTypeX 自有适配代码使用 [MIT License](LICENSE)。libkqueue 等第三方组件保留各自的上游许可。原版程序、词典、模型、界面资源、服务和商标归各自权利人所有，不受本项目许可证覆盖；详见 [NOTICE](NOTICE)。

WeTypeX 调用官方服务完成账户、同步和联网功能，不提供绕过账户验证、伪造同步状态或替代官方服务的实现。上游协议、接口和服务可能随时变化。
