# ARM64 支持

WeTypeX 在 Linux ARM64（aarch64）上加载官方安装包中普通 `arm64` 架构的原版映像，与 x86-64 使用同一套兼容清单、更新和回退流程。

## 执行方式

ARM64 适配层把 Darwin 的调用约定、线程局部存储、回调和 C++ ABI 桥接到 Linux。原版映像使用的 libc++ 采用 Apple 的替代字符串布局，因此 WeTypeX 私有构建一份对应 ABI 的 libc++，与 Qt 程序使用的系统 C++ 运行库相互独立。`arm64e` 映像会被拒绝加载。

原版 ARM64 映像面向 Apple Silicon 编译。加载前，宿主通过 `AT_HWCAP` 检查以下 CPU 特性：

`ASIMD`、`AES`、`PMULL`、`SHA1`、`SHA2`、`CRC32`、`ATOMICS`、`FPHP`、`ASIMDHP`、`LRCPC`、`ASIMDDP`

| CPU 情况 | 执行方式 |
| --- | --- |
| 具备全部特性 | 直接原生执行 |
| 缺少任一特性，已安装 `qemu-user` | 通过 QEMU 用户态模拟执行同一份 ARM64 映像，不替换 ABI |
| 缺少任一特性，未安装 `qemu-user` | `fcitx5-wetypex-setup --check` 报告需要兼容执行组件 |

QEMU 模拟器从 `/usr/bin/qemu-aarch64` 或 `/usr/bin/qemu-aarch64-static` 查找。请通过发行版安装 `qemu-user` 软件包；Arch Linux ARM 软件包已将它列为可选依赖。NixOS 用户在构建参数中设置 `enableCompatibilityProvider = true`，见[安装指南](installation.md#构建参数)，默认关闭以免具备特性的设备把 QEMU 拉入闭包。

## 检查执行方式

```bash
fcitx5-wetypex-account network-info
```

该命令不加载原版核心，也不联网。ARM64 上关注以下字段：

| 字段 | 含义 |
| --- | --- |
| `native_cpu_supported` | CPU 是否具备全部所需特性 |
| `execution_provider` | 实际使用的执行方式：`native` 或 `qemu` |
| `execution_ready` | 所选执行方式是否可用；为 `false` 时需安装 `qemu-user` |

环境变量 `WETYPE_ENGINE_PROVIDER` 可取 `auto`、`native` 或 `qemu`。日常使用保持默认的 `auto`，其余取值用于排查。经 QEMU 执行时，`--check` 等待核心启动的时间更长。

## 软件包

正式版本提供 ARM64 的 Arch Linux 软件包、标准 deb、legacy deb、rpm 和便携归档，文件名见[安装指南](installation.md#选择发行格式)。legacy deb 面向 glibc 2.31，在 Ubuntu 20.04 上同样需要稳定版 Fcitx5 框架的回移植包。

## 验证范围

原生 ARM64 CI 运行器覆盖：

- 在目标发行版中原生构建全部 ARM64 软件包，并核对架构、版本和安装后的启动；
- 在两个旧系统基线上运行真实原版核心和本地 TLS 传输检查；
- 原版更新验证中的真实核心、词典选词、TLS 传输、匿名配对和语音识别（ASR）请求。

平板设备上的桌面体验、实体键盘和麦克风的实际使用尚未在物理 ARM 设备上测试。欢迎在 Issue 中报告设备型号、发行版、`network-info` 输出和使用结果。
