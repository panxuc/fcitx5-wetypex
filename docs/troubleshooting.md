# 故障排查

本文按现象分类。排查前先收集以下信息：

```bash
fcitx5-wetypex-setup --status
fcitx5-wetypex-setup --check
fcitx5-wetypex-account network-info
fcitx5-diagnose | less
```

提交 Issue 时请附上发行版、架构、桌面环境、显示协议、Fcitx5 版本和上述输出，并删除其中的账户标识、设备码、传输码和私人文本。

## 安装与加载

### 输入法列表中没有 WeTypeX

确认以下文件来自同一次系统级安装（Arch 与部分发行版的路径，Debian 系为多架构目录）：

```text
/usr/lib/fcitx5/wetypex.so
/usr/share/fcitx5/addon/wetypex.conf
/usr/share/fcitx5/inputmethod/wetypex.conf
```

新安装或升级适配层后，需要按[重启 Fcitx5](installation.md#重启-fcitx5)中的方法重启 Fcitx5 进程。仍然不可见时，用 `fcitx5-diagnose` 检查插件搜索路径。Ubuntu 20.04 请确认已安装稳定版 Fcitx5 框架，见[安装指南](installation.md#ubuntu-2004-legacy-deb)。

### Ubuntu 24.04 设置窗口无法启动

出现 `Qt_6.8 not found` 时，说明安装的是早期以 Ubuntu 24.04 环境构建的 deb。请安装当前发布的标准 deb，它以 Ubuntu 22.04 和 Qt5 构建。不要把 Qt6 配置插件软链到 Qt5 配置工具目录；构建会按可用 SDK 提供对应 ABI 的配置入口。完整设置始终可以直接运行 `fcitx5-wetypex-settings`。

### 提示缺少 `libunwind.so.1`

当前标准 deb 声明了 LLVM unwinder 依赖；Ubuntu 26.04 上请安装 `llvm-libunwind1` 后重启 Fcitx5。GNU 的 `libunwind8` 提供不同的 SONAME，不能用手动软链代替。核心无法加载时，中英文切换看起来失效或只能输出英文字母，请先按[原版运行时](#原版运行时)恢复核心，再检查切换快捷键。

## 原版运行时

### 提示核心不可用

```bash
fcitx5-wetypex-setup --check
```

`core_ready` 应为 `true`。根据输出处理：

| 输出 | 处理 |
| --- | --- |
| `missing_runtime` 非空，或 `runtime_version` 为空 | 运行 `fcitx5-wetypex-setup --update --download --accept-upstream-license` 重新准备默认档案 |
| `missing_ui` 非空 | 运行 `fcitx5-wetypex-setup --icons-only --download --accept-upstream-license` |
| `core_error` 提到需要兼容执行组件 | ARM64 CPU 缺少所需特性，安装 `qemu-user`，见 [ARM64 支持](arm64.md) |
| 报告 SHA-256 不匹配或没有兼容档案 | 文件不是兼容清单中的官方版本。保持校验开启，改用 `--download` 或官方原始 ZIP |

### 旧 CPU 在配对或联网初始化时出现 SIGILL

先运行 `fcitx5-wetypex-account network-info`，查看 `native_cpu_supported`、`execution_provider` 和 `execution_ready`。部分老 x86-64 CPU 同时缺少 AES-NI 和 AVX2，无法直接执行原版的软件 AES 初始化。默认 `auto` 模式会改用同架构 QEMU；语音另有 AVX2/FMA/F16C 要求，诊断结果中的 `voice_execution_ready` 表示语音执行方式是否可用。请安装支持所需指令集的 `qemu-user`，Nix 用户启用 `enableCompatibilityProvider = true`。

若已安装 QEMU 而 `execution_ready` 仍为 `false`，需要更新模拟器。诊断会实际检查其指令集能力；Ubuntu 22.04 自带的 QEMU 6.2 不满足这里的 x86 要求。详见 [CPU 指令集与兼容执行](architecture.md#cpu-指令集与兼容执行)。

### 更新一直显示“已准备”

- 显示“更新已准备。首次升级请重启 Fcitx5”：当前运行的是旧插件，请[重启 Fcitx5](installation.md#重启-fcitx5)一次。KDE Plasma Wayland 请用虚拟键盘切换或重新登录。
- 显示“已准备就绪，将在输入结束后切换”：输入、语音、隔空传送窗口或账户命令仍在使用运行时。结束这些操作后片刻即会切换。
- 显示“更新未应用”及错误：`~/.local/share/fcitx5-wetypex/runtime-pending.json` 的 `error` 字段记录原因，当前运行时保持不变。修复后重新点击“下载并更新原版核心”，或运行 `fcitx5-wetypex-setup --update --download --accept-upstream-license`。

后台操作的完整输出位于 `~/.local/share/fcitx5-wetypex/runtime-operation.log`。

### 没有收到适配层更新通知

1. 确认设置“升级和反馈”页中的“有新版本时自动更新”已开启。Nix 构建通过 Nix 配置更新适配层，不显示此类通知。
2. 查看 `~/.cache/fcitx5-wetypex/updates/state.json`：`latest_version` 是最近一次选中的版本，`error` 记录上次失败原因，例如没有适合本机发行版与架构的完整发布。
3. 运行 `fcitx5-wetypex-update --force` 立即检查，并重新显示已下载软件包的通知。

同一软件包只通知一次。已下载的软件包保存在 `~/.cache/fcitx5-wetypex/updates/`，也可以直接用包管理器安装。详见[适配层软件包的选择与校验](runtime-updates.md#适配层软件包的选择与校验)。

### 清理释放的空间少于预期

清理保留当前版本代、一个回退版本代、待应用的版本代和回退所需的数据快照，并保留迁移前的 `original-*` 目录等无法自动判断的文件。先运行 `fcitx5-wetypex-setup --cleanup --dry-run` 查看计划，再参阅[清理与存储](runtime-updates.md#清理与存储)。输出含 `deferred` 时，说明有更新正在进行，稍后再试。

## 输入与候选

### 界面正常，但中文没有候选或无法上屏

运行 `fcitx5-wetypex-account network-info`，检查 `json_c_version` 与 `utf8_json`，后者应为 `true`。json-c 0.18 的严格模式会把中文 UTF-8 字节误判为控制字符；WeTypeX 在解析前做兼容转换，同时保留严格语法、UTF-8 和尾随数据校验。`fcitx5-wetypex-setup --check` 也检查这条链路。

### 首次输入时先出现拼音小框，再扩展成候选面板

首个面板由本地核心回复触发，候选一到即显示；核心慢于约一帧时先显示组合文字兜底，后续按键不会延长这段等待。“拼音显示位置”设为“候选框内”，或应用不支持内嵌预编辑时，最容易看到这种变化。云候选由核心主动推送，不阻塞本地候选。冷启动时，字体、主题和桌面的首次绘制也会增加耗时。

排查时请在真实编辑器中对比首次输入、连续输入、Esc、失焦，以及“输入框内”和“候选框内”两种位置，并分别记录窗口尺寸变化和按键到首次显示的耗时。

### Electron 应用在 Wayland 下候选框固定在左上角

先在同一应用中对照 Fcitx 拼音，并确认 KDE“系统设置 → 键盘 → 虚拟键盘”选择了 Fcitx 5。WeTypeX 使用 Fcitx 的面板定位，面板位置由客户端提供的输入表面和光标矩形决定。

支持原生 Wayland 和 text-input-v3 的 Electron 应用，可只在该应用的启动命令中使用：

```bash
env -u GTK_IM_MODULE -u QT_IM_MODULE application --ozone-platform=wayland --enable-wayland-ime --wayland-text-input-version=3
```

`application` 换成应用实际的可执行文件。KWin 6.8 提供 text-input-v2/v3；在 KWin 6.8 独立会话中，原生 Wayland 的 VS Code 使用这组参数时，光标矩形随编辑位置更新，候选框跟随插入点。支持读取 `~/.config/code-flags.conf` 的 Arch 启动脚本也可以在该文件中配置这些参数。修改后请完整退出应用再打开。

应用内置的 Electron/Chromium 较旧，或启动器会覆盖参数时，对该应用改用 XWayland 并提供 Gtk 输入法模块：

```bash
env GTK_IM_MODULE=fcitx application --ozone-platform=x11
```

Arch 的模块包为 `fcitx5-gtk`，Debian/Ubuntu 通常为 `fcitx5-frontend-gtk3`，版本应与当前 Fcitx 框架匹配。可把命令写入该应用的用户级 `.desktop` 启动项，保留原命令中的路径和占位参数。这些调整只作用于对应应用。

### Wayland 下 V 快捷栏不显示或闪退

V 快捷栏在支持 `zwlr_layer_shell_v1` 的合成器上使用不接收键盘焦点的 layer-shell 表面，打开时原输入框保持焦点。Arch 和 rpm 构建包含 Qt6 的 LayerShellQt；运行时还会检查对应 Qt 主版本的 shell 插件是否存在。

缺少匹配的 LayerShellQt 运行时或合成器不支持该协议时，快捷栏改用 Fcitx 原生候选菜单，不创建会抢焦点的普通 Wayland 窗口。主菜单提供计算、剪贴板、常用语与符号，可用鼠标或上下键加空格/回车选择；直接输入数字开始计算，`v1+2` 等输入方式保持不变。子菜单支持数字选词与翻页，退格返回主菜单，`V` 或 Esc 关闭。

标准 deb 使用 Qt5。Ubuntu 26.04 和 Debian 13 已不提供 Qt5 版 LayerShellQt，因此 deb 在 Wayland 会话中使用原生候选菜单；Qt 5.12 的 legacy 包相同。X11 会话以及显式设置 `QT_QPA_PLATFORM=xcb` 时使用独立快捷栏窗口。需要在原生 Wayland 中使用独立快捷栏时，可安装匹配 Qt 主版本的 LayerShellQt 开发包、运行时 shell 插件和 `wayland-client`，以 `-DWETYPE_LAYER_SHELL=ON` 从源码构建。

`~/.local/share/fcitx5-wetypex/state/vmode.log` 记录最近一次启动使用的窗口协议、定位参数、按钮动作和关闭原因，不记录输入内容。单实例守卫使用 `flock`：`state/vmode.lock` 文件始终保留在磁盘上，进程退出后内核自动释放锁，无需手动删除；快捷栏运行时删除它会破坏互斥。Hyprland 的定位方式见 [Hyprland 兼容](hyprland.md#v-快捷栏)。

## 网络与同步

### 本地候选正常，但没有云候选

运行 `fcitx5-wetypex-account network-info`。默认构建应显示 `provider: bundled-static`、`ws: true`、`wss: true`；该命令不读取配对身份，也不联网。Ubuntu 24.04 等系统的 libcurl 关闭了 WebSocket，默认构建因此静态包含自己的 libcurl。连接采用 Happy Eyeballs，并在建连失败时于同一超时预算内尝试 IPv4，无需在 `/etc/hosts` 中固定上游地址。

1. 在设置中确认“单机模式”关闭。
2. 运行 `fcitx5-wetypex-account group-info`。
3. 确认 `~/.local/share/fcitx5-wetypex/account/identity.json` 存在。
4. 查看 `account/last-network.log` 中最后一次业务初始化结果；分享前删除账户标识。

### 个人词库与常用语没有同步

先用 `fcitx5-wetypex-account group-info` 确认设备已关联，功能位 `4` 表示个人词库、`2` 表示常用语。启用 WeTypeX 并关闭单机模式后，在完整设置中点击“立即请求同步”，或运行：

```bash
fcitx5-wetypex-account sync-now
```

`requested: true` 表示原版输入核心接受了请求；下载与跨设备结果请在关联设备上确认。

`state/sync-runtime.json` 中的 `remote_version`、`local_kind` 和 `local_digest` 只描述剪贴板状态。原版核心的 `state/user/persistent_data`、`state/user/network` 与 `state/userDict` 在重启后保留，排查普通连接故障时请保持这些目录原样。

### 跨设备剪贴板没有同步

确认设备组的 `func_switch` 包含剪贴板位 `1`。Wayland 需要 `wl-copy` 与 `wl-paste`，X11 需要可用的 Qt 剪贴板。同步守护进程使用文件锁保证单实例，请勿手动另行运行。

### 输入框周期性失焦，候选框消失并留下拼音

在 Ubuntu 26.04、Plasma/KWin 6.6.6、wl-clipboard 2.2.1 上曾观察到：剪贴板同步轮询时 `wl-paste` 的临时窗口获得焦点，打断未完成的拼音，普通 Fcitx 拼音和浏览器也受影响。原因是旧版 wl-clipboard 只支持 `wlr-data-control`，部分 KWin 版本只公布 `ext-data-control`；wl-clipboard 从 2.3 开始支持后者。

当前同步守护进程先通过剪贴板助手查询 Wayland registry，再核对 `wl-paste`、`wl-copy` 的版本。探测只列举协议，不读取剪贴板，也不创建窗口。两者兼容时使用原生 Wayland；缺少共同协议或状态不明时，通过 Qt/XWayland 以无窗口方式读写；无窗口连接失败时跳过本轮稍后重试，不会退回可能抢焦点的 `wl-paste`/`wl-copy`。X11 连接使用 `DISPLAY` 和实际可用的认证配置，也支持未显式设置 `XAUTHORITY` 的会话。

查看当前桌面公布的协议（空输出表示没有受支持的 data-control 协议）：

```bash
fcitx5-wetypex-clipboard-set --wayland-clipboard-protocols
wl-paste --version
wl-copy --version
```

升级适配层后，需要让旧的 `fcitx5-wetypex-syncd` 进程退出、启动新版本；仅重新加载配置不一定替换仍在运行的同步进程。KDE Wayland 可保存工作后注销并重新登录。重新测试时，在浏览器或编辑器中输入拼音并等待至少 10 秒，确认焦点和候选框保持。

## 语音

### 语音没有识别结果

1. 确认单机模式关闭，语音识别需要联网；首次使用会初始化设备身份。
2. 按[流式语音输入](voice-input.md#诊断)中的命令单独录制几秒，确认默认输入设备有声音。
3. 运行 `fcitx5-wetypex-voice status`，检查 `capture_backend`、`native_ready`、`vad`、`state` 和 `timings`。
4. 查看 `~/.local/share/fcitx5-wetypex/state/voice/capture.log`（录音错误）和 `backend.log`（原版宿主错误）。
5. 宿主运行时 `vad` 仍为 `false`，或日志报告 VAD 文件缺失时，先运行 `fcitx5-wetypex-voice shutdown`，再运行 `fcitx5-wetypex-setup --repair-voice-resources --download --accept-upstream-license` 恢复模型。离线修复与说明见[修复语音资源](voice-input.md#修复语音资源)。

Ubuntu 22.04 的 `pw-record` 0.3.48 不支持 `--raw` 和 `--properties`；WeTypeX 会检测实际选项，自动改用 `parec` 或 FFmpeg 的 PulseAudio 输入。

### 语音已识别，但应用没有显示文字

部分终端和编辑器对输入法的异步提交处理不同。语音面板显示了结果而输入框没有文字时，开启“语音输入 → 识别后复制到剪贴板”，识别后在目标应用中粘贴。剪贴板中有文字说明识别和复制成功，应用是否接收了提交需要单独确认。

Wayland 下复制优先使用兼容的 data-control 协议；旧版 wl-clipboard 与桌面协议不匹配时，尝试通过 XWayland 静默复制；都不可用时跳过复制。可检查 `wl-copy --version`、`fcitx5-wetypex-clipboard-set --wayland-clipboard-protocols` 以及应用实际使用的输入法通道。VS Code 等 Electron 应用也可以按[上文](#electron-应用在-wayland-下候选框固定在左上角)尝试原生 Wayland 或 XWayland。

## 界面

### 候选字体被重置

候选字体由 Fcitx5 的“经典用户界面”设置管理。WeTypeX 保存输入选项或切换主题时会保留现有字体和字号；只有主动调整“候选字大小”才修改字号，并保留字体、字重、斜体及其他字体属性。菜单和托盘字体分别沿用 Fcitx5 的设置。

如果字体已被旧版本覆盖，请在更新适配层后，到 Fcitx5 的经典用户界面中重新选择一次所需字体。

### Qt5 设置窗口偏小

Qt5 的设置、AI、隔空传送和 V 快捷栏窗口在创建应用前启用系统 DPI 缩放和高分辨率图标，适用于 X11 分数缩放桌面；Qt6 使用框架自身的缩放机制。显式设置的 Qt 缩放环境变量继续生效，例如 `QT_ENABLE_HIGHDPI_SCALING=0` 会关闭 Qt5 的自动缩放。候选窗由 Fcitx5 Classic UI 独立缩放。

### AI 窗口空白或缺少图标

重新提取界面资源：

```bash
fcitx5-wetypex-setup --icons-only --download --accept-upstream-license
```

Qt WebEngine 无法启动时，检查 QtWebEngine 软件包（如 Arch 的 `qt6-webengine`）、图形驱动和桌面沙箱配置。AI 结果保存在 `~/.local/share/fcitx5-wetypex/ai/`，其中可能含有用户问题，请勿直接公开。

## 隔空传送无法连接

接收关联设备主动发送的文件时，保持“隔空传送”窗口打开，并在邀请提示中选择“接收”。设备列表会自动加载，也可以选中设备后主动连接。窗口只显示当前账户关联设备的邀请；已处理的邀请记录在本地，重新打开窗口不会重复提示。

接收的文件保存在系统“下载”目录下的 `WeTypeX` 文件夹，可点击“打开接收目录”查看。传输完成后点击“返回”可继续等待其他设备；对方取消、拒绝或邀请过期时也会回到等待状态。目录传输支持普通文件和子目录，保留中文名称与空目录，不跟随符号链接。

检查运行时：

```bash
fcitx5-wetypex-setup --check --transport
```

`core_ready` 应为 `true`，`missing_runtime` 中不应出现 `flurry` 或 `wxp2p` 文件。原版 WXP2P 依次尝试局域网直连、公网打洞和腾讯中继，两台设备不必位于同一局域网。受限网络需要允许出站 UDP，以及到上游中继的 TCP 80、8080 和 16285 端口。

传输日志位于 `~/.local/share/fcitx5-wetypex/state/transfer/transport.log`。手机点击重试时无需关闭桌面窗口；WeTypeX 继续轮询同一传输码，并在官方服务返回新的调度数据后重建会话。只缺少传输图标或连接示意图时，运行上文 `--icons-only` 命令。

## 重新配对

普通故障无需删除身份文件。确需重置配对时：

1. 备份账户目录：

   ```bash
   cp -a ~/.local/share/fcitx5-wetypex/account ~/wetypex-account-backup
   ```

2. 退出 Fcitx5 和 WeTypeX 相关进程。
3. 移走 `~/.local/share/fcitx5-wetypex/account` 和 `~/.local/share/fcitx5-wetypex/state/sync-state.json`。
4. 重新打开设置窗口获取匹配码。
