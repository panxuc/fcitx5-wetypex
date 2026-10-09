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

先验证默认输入设备：

```bash
pw-record --rate 16000 --channels 1 --format s16 /tmp/wetypex-test.wav
```

录制几秒后按 `Ctrl+C`，确认 WAV 非空。随后检查 `state/voice/last-record.log`。识别需要联网、有效账户身份和 FFmpeg 的 Opus 编码器。

## AI 窗口空白或缺图标

重新提取界面资源：

```bash
fcitx5-wetypex-setup --archive /path/to/WeType_2.2.3_657.zip --icons-only
```

Qt WebEngine 无法启动时，检查 `qt6-webengine`、图形驱动和桌面沙箱配置。AI 结果保存在 `~/.local/share/fcitx5-wetypex/ai/`，其中可能含有用户问题，不应直接公开。

## 隔空传送无法连接

先运行：

```bash
fcitx5-wetypex-setup --check
```

检查结果应同时包含 `flurry` 与 `wxp2p`，并且 `core_ready` 为 `true`。原版 WXP2P 会依次尝试局域网直连、公网打洞和腾讯中继，因此两台设备不必位于同一局域网。受限网络需要允许出站 UDP，以及到上游中继的 TCP 80、8080 和 16285 端口。

传输日志位于 `~/.local/share/fcitx5-wetypex/state/transfer/transport.log`。手机点击重试时桌面窗口无需关闭；WeTypeX 会继续轮询同一传输码，并在官方服务返回新调度数据后重建会话。若只缺少传输图标或连接示意图，可从同一官方安装包重新提取：

```bash
fcitx5-wetypex-setup --archive /path/to/WeType_2.2.3_657.zip --icons-only
```

## 重新配对

普通故障不要删除身份文件。确需重置时先备份：

```bash
cp -a ~/.local/share/fcitx5-wetypex/account ~/wetypex-account-backup
```

再关闭 Fcitx5 与 WeTypeX 相关进程，移走 `account` 和 `state/sync-state.json`，重新打开设置窗口获取匹配码。
