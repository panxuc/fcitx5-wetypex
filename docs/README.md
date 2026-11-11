# WeTypeX 文档

本目录按读者任务组织。第一次使用请从仓库首页的[安装与首次设置](../README.md#首次设置)开始。

## 按任务查找

| 我想要…… | 阅读 |
| --- | --- |
| 在某个发行版、NixOS 或旧版 Ubuntu 上安装 | [安装指南](installation.md) |
| 下载、更新、回退或清理原版运行时 | [运行时更新、回退与清理](runtime-updates.md) |
| 在 ARM64 设备上使用 | [ARM64 支持](arm64.md) |
| 了解各项功能是否可用 | [功能状态](feature-status.md) |
| 修改输入方式、外观、快捷键或同步开关 | [配置参考](configuration.md) |
| 查询某个按键在不同模式下的行为 | [按键与输入模式](input-handling.md) |
| 使用或排查语音输入 | [流式语音输入](voice-input.md) |
| 在微信中使用原版表情 | [Linux 微信表情](wechat-emoticons.md) |
| 输入日期和时间 | [日期、时间候选](date-time-input.md) |
| 在 Hyprland 中配置窗口规则 | [Hyprland 兼容](hyprland.md) |
| 解决具体故障 | [故障排查](troubleshooting.md) |
| 理解进程、沙箱和数据流 | [架构说明](architecture.md) |
| 构建软件包或发布版本 | [发行与打包](distribution.md) |
| 维护原版更新的自动 PR 与发布 | [维护者自动化](maintainer-automation.md) |
| 提交代码或报告安全问题 | [贡献指南](../CONTRIBUTING.md)、[安全策略](../SECURITY.md) |

## 术语

文档统一使用以下名称。

| 术语 | 含义 |
| --- | --- |
| 适配层 | WeTypeX 自有代码构建的系统软件包：Fcitx5 插件、引擎宿主、设置界面和辅助命令。通过发行版包管理器或 Nix 安装和更新。 |
| 原版运行时 | 用户从微信输入法官方下载服务获取并在本机准备的专有组件：输入核心、词典、隔空传送模块、界面资源和语音资源。保存在用户目录，不随适配层分发。 |
| 原版输入核心 | 原版运行时中负责候选、词库和业务服务的主程序映像。设置界面和 `--status` 显示的版本号指它。 |
| 兼容清单 | 随适配层安装的 `runtime-profiles.json`，列出经过验证的官方 ZIP、组件散列和兼容档案。 |
| 兼容档案 | 兼容清单中的一个版本条目，规定输入核心、词典、传输模块和资源分别取自哪个官方 ZIP。 |
| 版本代 | `runtime-generations/` 中一份完整准备好的原版运行时。`runtime` 指向当前版本代。 |
| 数据快照 | 切换版本代前保存的用户词库、常用语、账户和语音状态副本，用于跨数据格式回退。 |
| 单机模式 | 设置中的开关，关闭云候选、设备同步、语音和问 AI 等联网能力。 |

命令示例中的 `fcitx5-wetypex-setup`、`fcitx5-wetypex-account` 等程序由适配层安装到 `PATH`。路径中的 `~/.local/share` 和 `~/.cache` 分别对应未设置 `XDG_DATA_HOME`、`XDG_CACHE_HOME` 时的默认位置。
