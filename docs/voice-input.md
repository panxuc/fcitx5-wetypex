# 流式语音输入

WeTypeX 边录音边识别。PipeWire 或 PulseAudio 采集 16 kHz、单声道、16 位 PCM，Fcitx5 在当前输入框中显示预编辑并提交最终文字。在线、离线识别共用快捷键、麦克风、标点、取消和上屏流程。

## 选择识别方式

在“语音输入 → 识别方式”中选择：

| 方式 | 行为 |
| --- | --- |
| 原版在线（默认） | 调用当前原版核心的 `voice_engine_*` 接口，保留原版 Opus、VAD、分包、重试、识别与智能整理；需要联网 |
| 本地离线 | 按需选择并下载模型，由原生 CPU 解码进程在本机识别；单机模式下也可使用 |

选择本地离线后，选择模型，再点击“下载模型”。已有腾讯模型继续使用原来的目录，无须重新下载。

| 模型 | 出字方式 | 下载 / 安装后大小（约） |
| --- | --- | --- |
| 微信输入法官方模型（默认） | 边说边出字 | 103 / 136 MB |
| X-ASR | 160 ms 流式模型，边说边出字 | 615 / 615 MB |
| Qwen3-ASR 0.6B INT8 | 停顿后分段出字 | 879 / 987 MB |
| FireRedASR2 CTC INT8 | 停顿后分段出字 | 521 / 776 MB |
| Fun-ASR-Nano INT8 | 停顿后分段出字 | 842 / 1010 MB |

X-ASR 及其后三个模型共用一份原生 sherpa-onnx / ONNX Runtime CPU 组件，首次额外下载约 26 MB（ARM64 约 29 MB），安装后约 32 / 40 MB。模型权重跨架构通用。没有 GPU、PyTorch 或 Python 包安装步骤。首次使用较大模型需要载入；后续会话复用已载入的模型，空闲两分钟后释放内存。

下载可取消和续传，每个文件均校验固定 SHA-256；实际加载成功后才启用新模型。成功后清理压缩包和旧代文件。删除所选模型保留其他模型；最后一个可选模型删除时同时清理共享组件。模型不随软件包分发，也不计入原版核心的版本代。设置会显示当前模型的来源、许可和大小。

离线进程使用独立网络命名空间，不调用原版联网语音宿主；模型缺失、损坏或处理失败会明确报错，不自动切换到在线识别。输入法的其他联网功能仍由“单机模式”等各自开关控制。智能整理只在原版在线识别时使用，切回在线后恢复原有设置。离线模型的语言覆盖、准确率、标点与在线引擎有所区别。

腾讯模型使用自带的原生 x86-64 / ARM64 解码器，不需要 ONNX Runtime。x86 按 CPU 能力选择 AVX2/AVX-VNNI 或基础指令路径；ARM64 使用基础 NEON。可选模型使用按需下载的原生 CPU 组件；两种方式均不需要 QEMU。本地繁体输出通过 OpenCC 转换，发行包提供相应依赖；源码安装时需安装 `opencc`。

升级后，空闲的旧版语音服务会自动退出并以新协议启动；正在进行的旧会话会提示先结束再重试。

软件包升级后仍须按安装流程重启 Fcitx5，使主输入法加载新插件，再选择新的识别方式；控制服务迁移不能代替插件重新加载。Flatpak 的独立运行实例按各自的进程命名空间启动录音任务；另一个实例正在识别时会提示结束后重试。

命令行管理同一套模型：

```bash
fcitx5-wetypex-voice model list
fcitx5-wetypex-voice model status
fcitx5-wetypex-voice model install --accept-upstream-license
fcitx5-wetypex-voice model verify
fcitx5-wetypex-voice model remove
# 也可显式指定：wetype / xasr / qwen3 / firered / funasr
fcitx5-wetypex-voice model install --model xasr --accept-upstream-license
```

可用 `model install --archive /path/to/model.apk --accept-upstream-license` 导入相同的官方资源包。校验清单固定于 `data/local-voice-model.json`，模型更新随兼容性验证后的适配层更新提供。腾讯模型目录是 `~/.local/share/fcitx5-wetypex/voice/local/`，可选模型与共享组件在 `voice/sherpa/` 下，删除模型保留语音偏好和原版在线功能。正在进行离线录音或识别时拒绝替换、删除；空闲解码器会先释放模型。

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

在“语音输入”或“快捷键”页面点击按键区域，再按想使用的快捷键即可修改，也支持单独的右 Alt。两个页面会同步显示；“清空”解除绑定，“默认”恢复上表中的原版快捷键。录入时按 Esc 取消。

普通按键形式的快捷键也支持按住使用，自动重复不会反复启动或取消录音。快捷键需要传递到正在使用 WeTypeX 的文本框；若被桌面环境或应用占用，请改用其他按键。

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
- PipeWire 录音流固定使用启动时选定的设备，避免 WirePlumber 恢复旧的应用路由、覆盖当前选择。修改麦克风后，下次录音生效。
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
| `native_ready` | 当前识别宿主已启动 |
| `provider` | `original` 为原版在线，`local` 为本地离线 |
| `vad` | 在线宿主运行时已加载原版 VAD 模型；宿主空闲释放后与 `native_ready` 一起变为 `false` |
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

用当前选择的识别方式转写已有音频文件：

```bash
fcitx5-wetypex-voice transcribe /path/to/recording.wav
```

加上 `--realtime --verbose` 可按音频实际时长送入并查看中间状态。`fcitx5-wetypex-account voice-file recording.opus` 使用同一引擎。

文件转写遵循当前选择的识别方式；加上 `--traditional` 输出繁体。离线单次输入上限为十分钟，达到时自动结束并保留已识别结果，可再启动下一段。腾讯模型的 `WETYPE_LOCAL_VOICE_CPU=scalar` 可用于排查旧 x86 CPU 的兼容性，通常保持自动选择即可。

文件超过上限时结果包含 `truncated: true`、`limit_reached: true` 与 `reason: duration_limit`，同时向标准错误输出提示；需要完整转写时请先分段。

日志位于 `~/.local/share/fcitx5-wetypex/state/voice/`：

| 文件 | 内容 |
| --- | --- |
| `capture.log` | 录音进程错误 |
| `backend.log` | 原版语音宿主错误 |
| `local-backend.log` | 本地解码器错误 |
| `daemon.log` | 控制服务日志 |

状态与日志保存在私有目录，可能包含识别文本或账户标识。分享时只提供状态字段、耗时和脱敏后的错误信息。

## 会话与资源管理

输入法激活时准备一个轻量控制服务，通过同用户的 Unix socket 接收快捷键命令，停止录音时无需等待 Python 进程启动。录音期间每 40 ms 检查一次识别结果，与设备同步的节拍相互独立。

原版宿主使用独立的语音业务缓存和账户身份快照，录音不会长时间占用设备同步所需的账户锁；主账户切换或退出后，下一次在线语音会重建对应缓存。本地识别不需要账户身份。识别宿主空闲两分钟后释放，控制服务空闲十分钟后退出。

会话以输入上下文、epoch 和唯一 token 关联。取消、快速松键、控制进程退出或宿主故障都会结束采集，旧会话的中间结果和最终结果被丢弃。

升级适配层后，可执行 `fcitx5-wetypex-voice shutdown` 结束旧控制服务，下次启动语音时会运行新版本。原版运行时切换会先让语音会话结束，见[运行时更新、回退与清理](runtime-updates.md)。
