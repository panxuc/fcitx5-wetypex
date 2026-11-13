# 运行时更新、回退与清理

本文说明如何下载、更新、回退和清理用户目录中的原版运行时，以及适配层软件包的自动更新。ARM64 的执行方式见 [ARM64 支持](arm64.md)，维护者如何把新的官方版本加入兼容清单见[维护者自动化](maintainer-automation.md)。

## 两层更新

| 层 | 内容 | 位置 | 更新方式 | 授权 |
| --- | --- | --- | --- | --- |
| 适配层 | Fcitx5 插件、引擎宿主、设置界面、兼容清单 | `/usr` 或 Nix store | 发行版包管理器或 Nix | 需要管理员授权 |
| 原版运行时 | 输入核心、词典、传输模块、界面与语音资源 | `~/.local/share/fcitx5-wetypex` | `fcitx5-wetypex-setup` 或设置界面 | 当前用户即可 |

原版运行时从微信输入法官方下载服务直接获取，WeTypeX 的软件包不包含也不转发这些文件。适配层只激活自身兼容清单（`runtime-profiles.json`）中列出的版本：官网发布的新版本先由维护者验证并写入兼容清单，随新的适配层发布后才会被用户安装。

## 版本组成

兼容清单中的每个兼容档案规定各组件的来源。以当前默认档案为例：

| 组件 | 来源 |
| --- | --- |
| 原版输入核心与词典 | 默认档案对应的官方 ZIP（版本即 `--status` 输出的 `default`，当前值见[仓库首页](../README.md#工作方式)） |
| 隔空传送模块 Flurry 与 WXP2P | 2.2.3.657 官方 ZIP |
| 界面资源、微信表情原图和语音 VAD 模型 | 2.2.3.657 官方 ZIP |

新版官方安装包中的传输框架使用 Objective-C 与 Swift 接口，需要单独的兼容工作，因此传输模块和资源继续固定在 2.2.3.657。设置界面和 `--status` 显示的版本号指输入核心。准备默认档案需要两个官方 ZIP，`--download` 会自动获取缺少的那个。

查询官网当前稳定版：

```bash
fcitx5-wetypex-setup --query-upstream
```

该命令只读取官方安装信息。官网版本高于兼容清单默认版本时，说明维护者尚未完成验证，已安装的适配层不会使用它。

## 首次下载

```bash
fcitx5-wetypex-setup --download --accept-upstream-license
```

`--download` 必须与 `--accept-upstream-license` 一起使用，表示你已阅读并同意原版软件许可。在设置界面首次点击“下载并更新原版核心”时，也会先弹出许可确认。下载的 ZIP 经 SHA-256 校验后缓存到 `~/.cache/fcitx5-wetypex/archives/`。

完成后运行 `fcitx5-wetypex-setup --check` 验证，预期结果见[命令参考](#命令参考)。

## 离线准备

在可联网的机器上从官方下载服务获取所需 ZIP，复制到目标机器后逐个导入缓存，再准备默认档案：

```bash
fcitx5-wetypex-setup --cache-archive /path/to/WeType_2.2.3_657.zip
fcitx5-wetypex-setup --cache-archive /path/to/WeType_<默认核心版本>.zip
fcitx5-wetypex-setup --update
```

`--cache-archive` 只接受散列出现在兼容清单中的官方 ZIP。默认档案组合了两个官方版本的组件，所以需要导入两个文件；所需 ZIP 的下载地址和 SHA-256 列在已安装的 `runtime-profiles.json` 的 `archives` 中（源码仓库中为 `data/runtime-profiles.json`）。缓存不完整时，`--update` 会报告缺少哪个版本的 ZIP。

也可以直接用 `--archive` 指定单个 ZIP。未同时指定 `--update` 或 `--version` 时，WeTypeX 按 ZIP 的散列选择对应档案；例如只提供 `WeType_2.2.3_657.zip` 会安装 2.2.3.657 档案。

## 手动更新

在设置的“升级和反馈”页点击“下载并更新原版核心”，或运行：

```bash
fcitx5-wetypex-setup --update --download --accept-upstream-license
```

更新分两步：

1. **准备**：下载缺少的官方 ZIP，校验散列，为本机架构准备新的版本代并实际启动其核心。准备失败时当前运行时保持不变。
2. **切换**：输入法在输入、语音和未完成请求结束后切换到新版本代。隔空传送和账户命令持有共享运行时租约，切换会等到它们结束。切换前，WeTypeX 保存当前用户数据快照，分别用新旧核心读取，确认常用语在升级后保持一致；数据格式相同时，还确认旧核心能读回升级后的数据，以保证之后可以回退。切换期间可以继续打字。

设置界面中的操作在后台进程中执行，关闭设置窗口不会取消已经开始的下载、准备或清理；重新打开设置即可看到结果。命令行中的 `--update` 在空闲时立即切换；输入法正忙时，更新保持“已准备”状态，由输入法稍后应用。

指定兼容清单中的其他版本：

```bash
fcitx5-wetypex-setup --update --version <版本> --download --accept-upstream-license
```

`<版本>` 必须是兼容清单 `profiles` 中的键。

### 首次升级需要重启一次 Fcitx5

旧版插件不会在空闲时应用已准备的运行时。首次升级到支持运行时热更新的适配层后，设置页会显示“更新已准备。首次升级请重启 Fcitx5；之后可自动切换。”请按[重启 Fcitx5](installation.md#重启-fcitx5)操作一次，KDE Plasma Wayland 用户请使用虚拟键盘切换或重新登录。此后的兼容运行时更新无需重启。

## 自动更新

设置“升级和反馈”页中的“有新版本时自动更新”（`wetypex.json` 字段 `auto_update`，默认开启）同时控制两层更新。

| | 原版运行时 | 适配层软件包 |
| --- | --- | --- |
| 检查内容 | 已安装兼容清单的默认档案是否已安装 | GitHub 上是否有比当前适配层更高的完整正式版本 |
| 检查频率 | 每天最多一次；失败后约一小时重试 | 每天最多一次；失败后约一小时重试 |
| 执行动作 | 后台准备，空闲时切换 | 下载对应的软件包并按 `SHA256SUMS` 核对，桌面通知中的“安装更新”调用系统授权界面 |
| Nix | 照常工作 | 关闭，由 Nix 配置更新 |

适配层升级后，更新器下一次运行时会重新检查原版运行时，不受上次检查时间限制。单机模式不影响这两项检查；希望完全停止更新相关联网时，请关闭此开关。

自动更新遵守以下规则：

- 已安装的核心版本高于兼容清单默认版本时（例如手动选择了测试版档案），不会自动降级。
- 显式回退后，自动更新暂停推进到当时的默认版本；适配层带来新的默认版本或手动更新后恢复。
- 关闭开关只停止今后的检查，已经开始的下载或准备会继续完成。

### 适配层软件包的选择与校验

更新器读取项目的 GitHub Release 列表，跳过草稿和预发布版本，只考虑同时包含本机软件包和 `SHA256SUMS` 的完整发布，从中选择版本号最高的一个。因此重建历史版本或尚未上传完成的发布不会影响选择。本机软件包按 `/etc/os-release` 的发行版类型（Arch、Debian/Ubuntu、Fedora）和 CPU 架构确定；Debian 系 glibc 低于 2.35 时选择 legacy deb。

下载前，更新器先取得 `SHA256SUMS` 中该软件包的 SHA-256；缓存中已有散列相符的同名软件包时直接复用，否则下载并核对，不符即丢弃。通过核对的软件包保存在 `~/.cache/fcitx5-wetypex/updates/`，同一软件包只通知一次。旧版 `notify-send` 不支持操作按钮时，通知显示软件包的完整路径，请用包管理器手动安装；如果 `notify-send` 声明支持动作但实际调用失败（例如 libnotify 0.8.8 在 GNOME 50 下），更新器会改用 zenity 询问是否安装，zenity 不可用时仍显示路径。

立即检查一次，不受开关和检查间隔限制，并再次显示通知：

```bash
fcitx5-wetypex-update --force
```

更新器按发行版类型提供软件包。使用便携归档安装时，可以按通知改用包管理器安装，或继续手动解压新版本。

## 从 2.2.3 旧安装迁移

早期版本把 2.2.3.657 运行时直接写在 `runtime/` 目录中，可能没有 `runtime.json` 版本信息。WeTypeX 按核心映像的散列识别这类安装，首次更新时：

- 把原有运行时整理为一个可回退的版本代；
- 把原来的 `runtime/` 目录完整移到 `runtime-generations/original-<随机标识>/` 保存；
- 保留账户、自定义皮肤、用户词库和常用语。

迁移后 `runtime` 成为指向当前版本代的符号链接。无法识别的 `runtime/` 目录（例如非原版文件）不会被改动，命令会报错并保留它。

## 回退

在设置中点击“回退上个版本”，或运行：

```bash
fcitx5-wetypex-setup --rollback
```

回退切换到上一个版本代，并遵守以下规则：

- 当前配对身份保持不变。
- 两个版本的用户数据格式相同时，保留最新学习的词和常用语。
- 数据格式不同时，恢复升级前保存的数据快照；升级后新增的学习数据不会带回旧格式。
- 回退后，自动更新暂停推进到当时的默认版本，见[自动更新](#自动更新)。

回退需要存在上一个版本代。设置中的回退按钮在有待应用的更新时不可用；该更新应用失败后恢复可用。

## 清理与存储

每次成功切换后，WeTypeX 自动保留：

- 当前版本代；
- 一个可回退的上一版本代；
- 已准备但尚未应用的版本代；
- 回退所需的数据快照。

同一文件系统上，经过散列验证的只读文件（输入核心映像和词典）在版本代之间、乃至同一用户的多个安装之间共享存储；支持写时复制克隆的文件系统还能减少快照占用。皮肤和用户数据等可变内容在每个版本代中独立保存。

默认策略会删除不再需要的官方 ZIP：版本代完整准备后不再依赖 ZIP。需要离线复用时，在准备或清理时设置环境变量：

```bash
WETYPE_KEEP_ARCHIVES=1 fcitx5-wetypex-setup --update --download --accept-upstream-license
WETYPE_KEEP_ARCHIVES=1 fcitx5-wetypex-setup --cleanup
```

这会保留当前、回退和待应用档案所需的 ZIP。多个安装共享同一缓存目录时，清理会尊重其他安装的保留和待更新记录。

手动清理前先预览：

```bash
fcitx5-wetypex-setup --cleanup --dry-run
fcitx5-wetypex-setup --cleanup
```

清理只删除本安装创建且不再需要的版本代、数据快照、官方 ZIP 和无人引用的共享文件。迁移前保存的 `original-*` 目录、`asset-*` 资源备份、`interrupted-*` 中断恢复备份以及无法识别的文件都会保留，确认不再需要后可手动删除。更新正在进行或存在中断恢复记录时，清理会推迟并在输出中说明原因。设置中的“清理旧版本”执行相同操作。

## 命令参考

| 命令 | 作用 | 预期结果 |
| --- | --- | --- |
| `--status` | 查看运行时状态 | JSON：`current` 当前核心版本，`default` 兼容清单默认版本，`update_available`，`pending` 待应用更新，`history` 当前与上一版本代 |
| `--check` | 实际启动原版核心并检查文件 | 退出状态 0；`core_ready: true`，`missing_runtime` 与 `missing_ui` 为空 |
| `--check --transport` | 额外执行真实的本地 TLS 传输 | 同上，并含 `transport_checked: true` |
| `--query-upstream` | 读取官网稳定版信息 | JSON，含官网 `version` |
| `--download --accept-upstream-license` | 首次准备默认档案 | JSON 状态，`current` 为默认版本 |
| `--cache-archive FILE` | 导入官方 ZIP | `{"cached": true}` |
| `--update` | 准备并尝试切换到默认档案 | `changed: true`；`deferred: true` 表示等待空闲后由输入法应用；`changed: false` 且无待应用更新表示已是最新 |
| `--rollback` | 回退到上一版本代 | 同上 |
| `--cleanup --dry-run` | 预览清理 | `removed_generations`、`removed_archives`、`removed_bytes`，不删除文件 |
| `--icons-only` | 重新提取界面图标和微信表情原图 | 打印界面资源目录；可配合 `--archive` 或 `--download --accept-upstream-license` |
| `--repair-voice-resources` | 为当前已安装的核心恢复语音 VAD 模型，不切换核心 | 打印模型目录；先结束语音并运行 `fcitx5-wetypex-voice shutdown`，见[流式语音输入](voice-input.md#修复语音资源) |

## 状态文件与日志

以下文件位于 `~/.local/share/fcitx5-wetypex/`：

| 文件 | 内容 |
| --- | --- |
| `runtime-operation.log` | 设置界面启动的后台更新、回退或清理的输出 |
| `runtime-operation.json` | 后台操作的进度、结果和错误 |
| `runtime-pending.json` | 已准备但尚未应用的更新；`error` 字段记录应用失败原因 |
| `runtime-history.json` | 当前版本代、上一版本代和数据快照 |
| `runtime-policy.json` | 回退后暂停自动推进的默认版本 |
| `runtime-transaction.json` | 切换日志；断电等中断后，下次操作先据此恢复 |

适配层更新器的状态位于 `~/.cache/fcitx5-wetypex/updates/state.json` 和 `runtime-check.json`。

切换过程记录在日志中：意外中断后，下一次准备、切换或使用运行时前会先恢复到一致状态。下载或兼容性检查失败时，当前运行时保持选中。更多问题见[故障排查](troubleshooting.md#原版运行时)。
