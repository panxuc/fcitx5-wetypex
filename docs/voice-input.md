# 流式语音输入

WeTypeX 边录音边识别。PipeWire 或 PulseAudio 采集 16 kHz、单声道、16 位 PCM，交给当前原版输入核心中的 `voice_engine_*` C 接口；原版引擎完成 Opus 编码、VAD、分包、重试、中间结果、最终结果和智能整理。Fcitx5 负责在输入框中显示预编辑和提交文字。

语音识别需要联网。单机模式下语音快捷键不会启动录音。

## 使用

| 操作 | 方法 |
| --- | --- |
| 按住说话 | 按住 `Ctrl + Win`，松开结束 |
| 切换式录音 | 按 `Ctrl + Win + Shift` 开始，再按一次结束；按住说话期间按它会转为持续录音 |
| 结束录音 | 空格 |
| 立即提交当前结果 | Enter |
| 取消 | Esc |

输入其他字符会取消语音，并按正常输入或应用快捷键处理该字符。离开输入框、切换输入法、重置上下文或重新加载配置时语音取消，迟到的结果不会写入另一个输入框。

中间结果显示为预编辑，最终文本只提交一次。显示位置遵循外观中的“拼音显示位置”，输入框不支持内嵌预编辑时使用 Fcitx 候选面板。启动语音前若有未完成的拼音，会先按原始拼写提交。

快捷键可在设置中修改。普通按键形式的快捷键也支持按住使用，自动重复不会反复启动或取消录音。

### 提交与剪贴板

“语音输入 → 识别后复制到剪贴板”（字段 `voice_copy_to_clipboard`）默认开启。当前输入上下文确认接收最终结果后，插件异步把文字复制到系统剪贴板；部分终端无法接收异步提交时，可以直接粘贴。

- 中间结果、失焦或取消的会话、过期结果和文件转写不会复制。
- 复制失败不影响正常提交。
- 开启跨设备剪贴板同步时，这份内容也会作为一次本地复制参与同步。
- Wayland 下先核对 data-control 协议与 `wl-copy` 版本，无法静默复制时尝试 XWayland 剪贴板助手；没有可用通道时跳过复制，不创建窗口或抢焦点。

## 麦克风与音频服务

在设置中选择麦克风，或使用“自动检测”采集默认输入源。录音通过内存管道流式传给原版引擎，不保存整段音频文件。

| 环境 | 采集方式（`capture_backend`） |
| --- | --- |
| `pw-record` 支持 `--raw`，且 PipeWire 服务有可用音源 | `pipewire` |
| 旧版 PipeWire（如 Ubuntu 22.04 的 `pw-record` 0.3.48）、只运行屏幕共享的 PipeWire，或纯 PulseAudio | `pulse`（`parec`） |
| 缺少 `parec` | `ffmpeg-pulse`（FFmpeg 的 PulseAudio 输入） |
| 转写已有音频文件 | `file` |

PulseAudio 客户端路径同样适用于 pipewire-pulse。设置页可以枚举旧版 PulseAudio 麦克风。

- 已选设备断开后显示“当前不可用”，可重新选择设备或改用“自动检测”。
- 明确指定 PipeWire 设备时不会改用默认麦克风；连续 5 秒没有收到音频时结束录音并提示检查设备和音频服务。静音的设备仍会返回静音数据，不触发这项超时。
- 音频后端变化后，旧的数字设备编号不会被当作另一个后端的同编号设备，请重新选择麦克风或使用“自动检测”。

## 原版语音资源

原版 VAD 配置和模型随原版运行时一起准备，取自兼容档案的资源包（当前为 2.2.3.657），保存在当前版本代中，并通过 `~/.local/share/fcitx5-wetypex/voice/vad/` 访问。缺少 VAD 文件时，仍可用空格或快捷键手动结束录音；资源完整后才能使用原版的本地语音检测。模型和原版二进制都不随 WeTypeX 分发。

### 修复语音资源

`fcitx5-wetypex-voice status` 中宿主运行时 `vad` 始终为 `false`，或 `backend.log` 报告 VAD 文件缺失时，为当前已安装的核心恢复经过校验的模型：

1. 结束正在进行的语音输入，然后停止语音控制服务：

   ```bash
   fcitx5-wetypex-voice shutdown
   ```

2. 恢复模型（联网时下载缺少的官方 ZIP）：

   ```bash
   fcitx5-wetypex-setup --repair-voice-resources --download --accept-upstream-license
   ```

   离线时，用 `--archive` 指定资源包，或先用 `--cache-archive` 导入：

   ```bash
   fcitx5-wetypex-setup --repair-voice-resources --archive /path/to/WeType_2.2.3_657.zip
   ```

成功时命令打印模型目录。该操作只重写 VAD 文件：当前核心版本、配对身份和配置保持不变。语音服务仍在运行时命令会提示先完成第 1 步。下次启动语音时，控制服务自动以新模型启动。

`fcitx5-wetypex-setup --icons-only` 只刷新界面图标和微信表情，不处理语音模型。

## 诊断

查看本地语音状态：

```bash
fcitx5-wetypex-voice status
```

| 字段 | 含义 |
| --- | --- |
| `native_ready` | 原版语音宿主已启动 |
| `vad` | 宿主运行时已加载原版 VAD 模型；宿主空闲释放后与 `native_ready` 一起变为 `false` |
| `capture_backend` | 使用的采集方式，见上表 |
| `capture_pid` | 采集进程运行时的进程号 |
| `state` | 当前会话状态 |
| `timings` | 采集启动、首个文字结果、停止到最终结果的耗时，用于区分设备、建连和识别延迟 |

单独验证默认输入设备（按当前音频服务选择一条，录几秒后按 `Ctrl+C`，确认文件非空）：

```bash
# PipeWire
pw-record --rate 16000 --channels 1 --format s16 /tmp/wetypex-test.wav
# PulseAudio / pipewire-pulse
parec --file-format=wav --rate=16000 --channels=1 --format=s16le > /tmp/wetypex-test.wav
```

转写已有音频文件（会连接原版语音服务）：

```bash
fcitx5-wetypex-voice transcribe /path/to/recording.wav
```

加上 `--realtime --verbose` 可按音频实际时长送入并查看中间状态。`fcitx5-wetypex-account voice-file recording.opus` 使用同一引擎。

日志位于 `~/.local/share/fcitx5-wetypex/state/voice/`：

| 文件 | 内容 |
| --- | --- |
| `capture.log` | 录音进程错误 |
| `backend.log` | 原版语音宿主错误 |
| `daemon.log` | 控制服务日志 |

状态与日志保存在私有目录，可能包含识别文本或账户标识。分享时只提供状态字段、耗时和脱敏后的错误信息。

## 会话与资源管理

输入法激活时准备一个轻量控制服务，通过同用户的 Unix socket 接收快捷键命令，停止录音时无需等待 Python 进程启动。录音期间每 40 ms 检查一次识别结果，与设备同步的节拍相互独立。

原版宿主使用独立的语音业务缓存和账户身份快照，录音不会长时间占用设备同步所需的账户锁；主账户切换或退出后，下一次语音会重建对应缓存。原版宿主空闲两分钟后释放，控制服务空闲十分钟后退出。

会话以输入上下文、epoch 和唯一 token 关联。取消、快速松键、控制进程退出或宿主故障都会结束采集，旧会话的中间结果和最终结果被丢弃。

升级适配层后，可执行 `fcitx5-wetypex-voice shutdown` 结束旧控制服务，下次启动语音时会运行新版本。原版运行时切换会先让语音会话结束，见[运行时更新、回退与清理](runtime-updates.md)。
