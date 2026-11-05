# 故障排查

## 输入法没有出现在列表中

确认以下文件来自同一个系统级安装：

```text
/usr/lib/fcitx5/wetypex.so
/usr/share/fcitx5/addon/wetypex.conf
/usr/share/fcitx5/inputmethod/wetypex.conf
```

运行 `fcitx5-remote -r`。仍然不可见时检查：

```bash
fcitx5-diagnose | less
```

## 提示核心不可用

```bash
fcitx5-wetypex-setup --check
```

输出中的 `core_ready` 应为 `true`。若报告文件缺失或核心验证失败，重新准备固定版本：

```bash
fcitx5-wetypex-setup --archive /path/to/WeType_2.2.3_657.zip
```

散列不匹配时不要绕过校验。下载或版本变化可能意味着 ABI 不兼容。

## 界面正常，但中文没有候选或无法上屏

运行 `fcitx5-wetypex-account network-info`，检查 `json_c_version` 和 `utf8_json`。后者应为 `true`。json-c 0.18 的严格模式存在将中文 UTF-8 字节误判为控制字符的问题，WeTypeX 在解析前做兼容转换并保留严格语法、UTF-8 和尾随数据校验。升级到包含该修复的构建即可使用系统原有 json-c；`fcitx5-wetypex-setup --check` 也会检查该兼容链路。

## 本地候选正常，但没有云候选

先运行 `fcitx5-wetypex-account network-info`。默认构建应显示 `provider: bundled-static`、`ws: true`、`wss: true`；输出不需要读取配对身份，也不会联网。Ubuntu 24.04 的系统 libcurl 8.5 关闭了 WebSocket，修复版桥接使用自己的 libcurl。连接采用 Happy Eyeballs，并在网络/TLS 建连失败时于同一超时预算内尝试 IPv4；无需修改 `/etc/hosts` 固定上游 IP。

1. 在设置中确认“单机模式”关闭。
2. 运行 `fcitx5-wetypex-account group-info`。
3. 检查 `~/.local/share/fcitx5-wetypex/account/identity.json` 是否存在。
4. 查看 `account/last-network.log` 中最后一次业务初始化结果，分享日志前删除账户标识。

## Ubuntu 24.04 设置窗口无法启动

若出现 `Qt_6.8 not found`，请升级兼容构建包。修订 `6/7` 的 deb 以 Noble 构建，修订 `8` 的标准 deb 以 Jammy/Qt5 构建。修订 `5` 的 deb 不能通过重装 Noble 的 Qt 6.4 修复。不要把 Qt6 插件软链到 Qt5 配置工具目录；构建按可用 SDK 提供对应 ABI 的配置入口，并沿用发行版的 Fcitx5 多架构目录。完整设置可直接运行 `fcitx5-wetypex-settings`。

若日志提示 `libunwind.so.1` 缺失，Ubuntu 26.04 可安装 `llvm-libunwind1` 后重启 Fcitx5；修订 `8` 的 deb 已声明 LLVM unwinder 依赖。GNU 的 `libunwind8` 提供不同 SONAME，不要靠手动软链替代。核心无法加载时，中英文切换看似失效或只能输出英文字母；先运行 `fcitx5-wetypex-account network-info` 和 `fcitx5-wetypex-setup --check` 恢复核心，再检查切换快捷键。

Ubuntu 20.04 使用 legacy 包，且必须先具备稳定版 Fcitx5 5.0.14+、fcitx5-qt 5.0.10+、LibIME 1.0.11+。系统仓库的 `0.0~git` 预览框架 ABI 不兼容；不能使用 `--force-depends` 绕过要求。自动更新器按 glibc 基线区分标准包与 legacy 包。

## 个人词库与常用语同步

先用 `fcitx5-wetypex-account group-info` 确认设备已关联，功能位 `4` 表示个人词库、`2` 表示常用语。启用 WeTypeX 并关闭单机模式后，可在完整设置中点击“立即请求同步”，或运行 `fcitx5-wetypex-account sync-now`。命令读取最新设备组并把请求交给当前原版输入核心；`requested: true` 表示核心接受了请求，下载与跨设备完成情况还需通过关联设备确认。

`state/sync-runtime.json` 的 `remote_version`、`local_kind` 和 `local_digest` 仅描述剪贴板，不是个人词库下载状态。核心的 `state/user/persistent_data`、`state/user/network` 与 `state/userDict` 在重启时保留；不要为排查普通连接故障删除这些目录。源码修复前的启动脚本曾自动删除前两个缓存目录。

## 跨设备剪贴板没有同步

确认设备组的 `func_switch` 包含剪贴板位 `1`。Wayland 需要 `wl-copy` 与 `wl-paste`；X11 需要可用的 Qt 剪贴板。同步守护进程使用文件锁，避免手工同时运行多个实例。

## 语音没有识别结果

Ubuntu 22.04 的 `pw-record` 0.3.48 不支持 `--raw` 和 `--properties`。辅助程序会检测实际选项，旧版或缺少 `pw-record` 时自动使用 `parec`，再回退到 FFmpeg 的 PulseAudio 输入；也支持 pipewire-pulse。已有系统可继续使用现有音频服务。

根据当前音频服务选择一条命令，验证默认输入设备：

```bash
# PipeWire
pw-record --rate 16000 --channels 1 --format s16 /tmp/wetypex-test.wav
# PulseAudio / pipewire-pulse
parec --file-format=wav --rate=16000 --channels=1 --format=s16le > /tmp/wetypex-test.wav
```

录制几秒后按 `Ctrl+C`，确认 WAV 非空。随后运行 `fcitx5-wetypex-voice status`，检查 `capture_backend`、`native_ready`、`vad`、`state` 和时间字段。录音错误见 `state/voice/capture.log`，原版宿主错误见 `state/voice/backend.log`。识别需要联网；首次使用会初始化设备身份。编码和分块由原版引擎完成，FFmpeg 用于已有音频解码以及缺少 `parec` 时的旧音频接口采集。原版 VAD 模型可用 `--icons-only` 更新，具体操作见[流式语音输入](voice-input.md)。

## 语音结果没有出现在应用里

语音最终结果通过输入法的提交通道写入输入框，同时（默认）写入系统剪贴板。终端类窗口通常只处理真实按键事件，提交文本可能被丢弃，表现是“语音面板显示已识别，但终端里没有字”。先按 `Ctrl + Shift + V` 粘贴验证：如果剪贴板里有完整结果，说明识别和提交都已执行，被吞掉的是终端一侧。

在 VS Code 集成终端中可先用以下方式区分原因：

- 先在终端按一个空格再启动语音，若此时文本能正常上屏，说明该终端只在已有输入事件后才处理提交文本；临时方案是保留空格后用退格或方向键删除。
- 确认 VS Code 使用的输入协议：原生 Wayland 需 `--ozone-platform=wayland --enable-wayland-ime`，否则走 XWayland 的 GTK 输入法模块。具体参数见下面的 Electron 小节。
- 关闭“识别后复制到剪贴板”后对比，可以确认文本是在提交阶段丢失，而不是根本没有识别结果。

## 输入框周期性失焦，候选框消失并留下拼音

Ubuntu 26.04、Plasma/KWin 6.6.6、wl-clipboard 2.2.1 下曾观察到，设备剪贴板同步轮询时 `wl-paste` 的临时窗口获得焦点，打断未完成的拼音；普通 Fcitx 拼音和浏览器也会受影响。旧版 wl-clipboard 只支持 `wlr-data-control`，而部分 KWin 版本只公布 `ext-data-control`；wl-clipboard 从 2.3 开始支持后者。只在日志里看到 `data_control` 字样不足以确认两者兼容。

同步守护进程通过剪贴板助手查询 Wayland registry，再核对 `wl-paste`、`wl-copy` 的版本。探测只列举协议，不读取剪贴板、不创建窗口。兼容时继续使用原生 Wayland；缺少共同协议或连接状态不明时，通过 Qt/XWayland 无窗口读写。无窗口连接失败会跳过本轮并稍后重试，不退回可能抢焦点的 `wl-paste`/`wl-copy`。X11 连接使用 `DISPLAY` 和实际可用的认证配置，也支持未显式设置 `XAUTHORITY` 的会话。 文字读取优先选择 UTF-8 格式；同步进程管理的剪贴板助手会持续持有内容，直到所有权被替换或同步进程退出。

可单独查看当前桌面的协议（空输出表示未公布支持的 data-control 协议）：

```bash
fcitx5-wetypex-clipboard-set --wayland-clipboard-protocols
wl-paste --version
wl-copy --version
```

升级后需要让旧的 `fcitx5-wetypex-syncd` 进程退出并启动新版本。KDE Wayland 可保存工作后注销并重新登录；只重新加载配置不一定会替换仍在运行的旧同步进程。重新测试时，在浏览器或编辑器中输入拼音、等待至少 10 秒，确认焦点和候选框保持。

## 首次输入先出现拼音小框，再扩展成候选面板

“拼音显示位置”设为“候选框内”，或应用不支持内嵌预编辑时，乐观更新的拼音可能早于本地候选，造成首次窗口先小后大的跳变。

首次面板现在由本地核心回复触发，候选一到立即显示；输入框内的预编辑仍即时更新。核心慢于约一帧时显示组合文字兜底，后续按键不会延长这段等待。取消、失焦、切换模式或重启核心会清除等待状态。

云候选由核心回调主动推送，不再等 100ms 轮询，也不会阻塞本地候选。兼容不支持推送的旧版辅助核心时保留轮询。冷启动还会受到字体、主题和桌面首次绘制耗时影响。

排查时可在真实编辑器中对比首次输入、连续输入、Esc、失焦，以及“输入框内”和“候选框内”两种位置；窗口尺寸变化和按键到首次显示的耗时应分开观察。

## Electron 在 Wayland 下候选框固定在左上角

先在同一个应用中对照 Fcitx 拼音，并确认 KDE“系统设置 → 键盘 → 虚拟键盘”选择了 Fcitx5。WeTypeX 使用 Fcitx 的面板定位；客户端提供的输入表面和光标矩形决定面板位置。

对支持原生 Wayland 和 text-input-v3 的 Electron 应用，可仅在该应用的启动命令中使用：

```bash
env -u GTK_IM_MODULE -u QT_IM_MODULE application --ozone-platform=wayland --enable-wayland-ime --wayland-text-input-version=3
```

这里的 `application` 换成应用实际可执行文件。KWin 6.8 提供 text-input-v2/v3，旧版本上建议的 v1 参数不适用于这个组合。已用 KWin 6.8 的独立会话和原生 Wayland VS Code 验证 v3 路线：应用发送的光标矩形随编辑位置更新，候选框跟随插入点。

部分应用内置的 Electron/Chromium 版本较旧，或启动器会覆盖参数。此时对该应用使用 XWayland，并提供对应的 Gtk 输入法模块：

```bash
env GTK_IM_MODULE=fcitx application --ozone-platform=x11
```

Arch 的模块包为 `fcitx5-gtk`，Debian/Ubuntu 通常使用 `fcitx5-frontend-gtk3`；版本应与当前 Fcitx 框架匹配。可把上述命令写入该应用的用户级 `.desktop` 启动项，保留原命令中的路径和占位参数。环境调整只用于对应应用，其他 Qt/Gtk 应用继续使用各自适合的输入方式。

## Wayland 下 V 工具栏闪退或不显示

V 工具栏在支持 `zwlr_layer_shell_v1` 的 Wayland 合成器上使用不接收键盘焦点的 layer-shell 表面，避免打开工具栏时原输入框失焦。Arch 和 rpm 构建包含 Qt6 的 LayerShellQt；源码构建可用 `-DWETYPE_LAYER_SHELL=ON` 要求该支持，或用 `OFF` 禁用，默认 `AUTO` 按 SDK 可用性选择。运行时还会检查对应 Qt 主版本的 shell 插件是否存在。

缺少匹配的 LayerShellQt 运行时或合成器不支持该协议时，工具栏回退到 Fcitx 原生候选菜单，不创建会抢焦点的普通 Wayland 窗口。主菜单提供计算、剪贴板、常用语与符号，可用鼠标或上下键加空格/回车选择；直接输入数字开始计算，保留 `v1+2` 的输入方式。子菜单支持数字选词与翻页，退格返回主菜单，V / Esc 关闭。

标准 deb 使用 Qt5 并保留跨发行版基线；Ubuntu 26.04 和 Debian 13 已不再提供 Qt5 的 LayerShellQt，所以 deb 在 Wayland 会话使用原生候选菜单，避免新增不可解析的旧库依赖。Qt5.12 的 legacy 包采用相同回退。X11 会话仍使用独立工具栏；显式指定 `QT_QPA_PLATFORM=xcb` 时也保留该选择。原生 Wayland 的本机源码构建可安装匹配 Qt 主版本的 LayerShellQt 开发包、运行时 shell 插件及 `wayland-client`，启用独立工具栏。

Hyprland 0.55+ 使用 Lua 配置。安装的 `hyprland.lua` 只包含设置、隔空传送和 AI 窗口的浮动/居中规则；V 工具栏是 layer-shell 表面，不受 `hl.window_rule` 影响。Wayland 光标矩形是相对输入表面的坐标，Hyprland 下由 `state/vmode.log` 中的 `placement:` 行记录解析后的锚点、显示器、工作区和当前窗口范围，可用来确认定位来源。工具栏会优先限制在当前窗口内，避免点击时焦点落到下方窗口；窗口过小时退回显示器工作区。工具栏位置异常时不要用窗口规则尝试修复，应检查 LayerShellQt、合成器协议和该日志。加载方式和验证命令见 [Hyprland 兼容](hyprland.md)。

单实例守卫使用 `flock`。`state/vmode.lock` 文件保留在磁盘上，进程退出（包括 SIGTERM、SIGKILL）后内核自动释放锁；文件存在不表示工具栏仍在运行，无需手工删除。删除活实例使用的锁文件会破坏互斥，应先关闭工具栏。

仍有异常时检查 `state/vmode.log`，其中记录最近一次启动使用的窗口协议、定位参数、按钮动作和轮询关闭原因，不记录输入内容。

## AI 窗口空白或缺图标

重新提取界面资源：

```bash
fcitx5-wetypex-setup --archive /path/to/WeType_2.2.3_657.zip --icons-only
```

Qt WebEngine 无法启动时，检查 `qt6-webengine`、图形驱动和桌面沙箱配置。AI 结果保存在 `~/.local/share/fcitx5-wetypex/ai/`，其中可能含有用户问题，不应直接公开。

## 隔空传送无法连接

接收关联设备主动发送的文件时，保持“隔空传送”窗口打开，并在邀请提示中选择“接收”。设备列表会自动加载，也可以选中设备后主动连接。只有当前账户的关联设备邀请会显示；已处理的邀请在本地记录，重新打开窗口不会反复提示同一次邀请。

接收文件保存在系统“下载”目录下的 `WeTypeX` 文件夹，可点“打开接收目录”查看。传完后点“返回”可继续等待另一台设备；对方取消、拒绝或邀请过期时也会恢复等待状态。目录传输支持普通文件和目录，保留中文名称与空目录，不跟随符号链接。

先运行：

```bash
fcitx5-wetypex-setup --check
```

检查结果应同时包含 `flurry` 与 `wxp2p`，并且 `core_ready` 为 `true`。原版 WXP2P 会依次尝试局域网直连、公网打洞和腾讯中继，因此两台设备不必位于同一局域网。受限网络需要允许出站 UDP，以及到上游中继的 TCP 80、8080 和 16285 端口。

传输日志位于 `~/.local/share/fcitx5-wetypex/state/transfer/transport.log`。手机点击重试时桌面窗口无需关闭；WeTypeX 会继续轮询同一传输码，并在官方服务返回新调度数据后重建会话。若只缺少传输图标或连接示意图，可从同一官方安装包重新提取：

```bash
fcitx5-wetypex-setup --archive /path/to/WeType_2.2.3_657.zip --icons-only
```

## Qt5 设置窗口偏小

Qt5 的设置、AI、隔空传送和 V 工具窗口会在创建应用前启用系统 DPI 缩放及高分辨率图标，适用于 X11 分数缩放桌面。Qt6 沿用框架自己的缩放机制。显式设置的 Qt 缩放环境变量继续生效；例如 `QT_ENABLE_HIGHDPI_SCALING=0` 会关闭 Qt5 的自动缩放。

候选窗由 Fcitx5 Classic UI 独立缩放，不依赖上述窗口设置。

## 重新配对

普通故障不要删除身份文件。确需重置时先备份：

```bash
cp -a ~/.local/share/fcitx5-wetypex/account ~/wetypex-account-backup
```

再关闭 Fcitx5 与 WeTypeX 相关进程，移走 `account` 和 `state/sync-state.json`，重新打开设置窗口获取匹配码。
