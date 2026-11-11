# 发行与打包

本文面向打包者和维护者，说明发行物的内容边界、构建方法、版本规则和发布流程。用户安装说明见[安装指南](installation.md)，原版更新的自动化流程见[维护者自动化](maintainer-automation.md)。

## 内容边界

源码包和所有二进制包只包含 WeTypeX 自有代码、构建文件、文档、应用图标和许可文件。以下内容不得进入任何发行物或仓库：

- 原版可执行文件、动态库、输入词典和语言模型；
- 从原版安装包提取的设置、AI、传输界面资源和语音模型；
- 微信表情原图及由其生成的本地彩色字体；
- 账户身份、配对信息、用户词库、剪贴板和 AI 响应。

`tools/package_release.py` 生成固定 gzip 时间戳的源码包和带散列的 Arch `PKGBUILD`，并拒绝不属于源码发行范围的二进制文件。

## 构建全部格式

```bash
packaging/build-packages.sh
```

脚本通过统一的 `/usr` staging 目录生成便携 `tar.zst`，在 Ubuntu 22.04、Ubuntu 20.04 与 Fedora 40 容器中编译标准 deb、legacy deb 和 rpm，最后调用 makepkg 生成 Arch 软件包。需要预先安装 Docker、`nfpm`、`makepkg`、zstd 和本机构建依赖。`packaging/build-native-packages.sh` 在当前架构（x86-64 或 ARM64）上构建 deb、legacy deb 和 rpm。

只生成源码包与 Arch 构建文件：

```bash
python3 tools/package_release.py
cd dist/arch
makepkg -C -f --noconfirm
```

正式发布物请在干净环境中完成依赖检查和完整构建，不要使用 `makepkg -d`。

## 各格式的构建基线

| 格式 | 构建环境 | 关键依赖 |
| --- | --- | --- |
| 标准 deb | Ubuntu 22.04 (Jammy) | glibc 2.35、OpenSSL 3（`libssl3 \| libssl3t64`）、Qt5、Fcitx5 5.0.14+、LLVM unwinder |
| legacy deb | Ubuntu 20.04 (Focal) | glibc 2.31、Qt 5.12、稳定版 Fcitx5 5.0.14+、fcitx5-qt 5.0.10+、LibIME 1.0.11+ |
| rpm | Fedora 40，签名校验开启的官方归档仓库 | Qt6、LayerShellQt、`openssl-libs` |
| Arch | 当前 Arch Linux / Arch Linux ARM | makepkg 记录的实际 OpenSSL SONAME、Qt6、LayerShellQt |

Focal 原生仓库中的预览版 Fcitx5 框架 ABI 不兼容，因此 legacy 构建镜像从固定上游提交生成 SDK；发行包不捆绑也不替换用户的 Fcitx5 框架。运行时启动兼容 Focal 的 Bubblewrap 0.4：启动沙箱前清空环境，再显式传入所需变量。旧版 `notify-send` 没有操作按钮时，更新通知改为普通通知。验证旧系统时，请实际运行 `fcitx5-wetypex-setup --check --transport` 以及输入和语音流程。

Fcitx5 输入插件和 Qt5/Qt6 配置插件安装到 `FCITX_INSTALL_ADDONDIR`，私有引擎宿主位于 `/usr/lib/fcitx5-wetypex`。配置工具从 `qt5` 或 `qt6` 子目录加载对应 ABI 的插件。

## 网络与 TLS 依赖

网络桥接默认静态包含固定版本的 libcurl，启用 WS/WSS、OpenSSL 和线程 DNS 解析，不受发行版关闭 WebSocket 的影响。curl 的源码版本与 SHA-256 固定在 `cmake/BundledCurl.cmake` 和 Arch 构建文件中，更新时请同步维护。`-DWETYPE_BUNDLED_CURL=OFF` 改用系统 libcurl 8.9+，该构建必须启用 WS/WSS。

系统 OpenSSL 低于 3 时（legacy deb），构建私有静态 OpenSSL 3，源码与 SHA-256 固定在 `cmake/BundledOpenSSL.cmake`，许可证随包安装；系统的 OpenSSL 1.1 保持不变。CMake 3.16/3.17 使用 curl 发布源码中的 configure 构建，仍保持 WS/WSS、证书校验和 LTO 隔离。

Arch 包在 `package()` 中为 `locale.so` 和 `wcwss_bridge.so` 设置可执行权限，使 makepkg 能解析其 SONAME 依赖，从而记录实际链接的 OpenSSL ABI。AUR 包在用户机器上针对本地库构建。

## ARM64

ARM64 软件包由 `.github/workflows/packages-arm64.yml` 在原生 ARM64 运行器上构建：deb、legacy deb 与 rpm 在目标发行版容器中原生编译，Arch 包在固定摘要的 Arch Linux ARM 环境中构建。ARM64 构建额外编译使用 Apple 替代字符串布局的私有 libc++（LLVM 18.1.8 源码，散列固定在 `PKGBUILD.in`），需要 CMake 3.20+ 和 Clang 16+。Arch ARM 包把 `qemu-user` 列为可选依赖。运行方式见 [ARM64 支持](arm64.md)。

## 版本号

发布版本由两部分组成：

- **版本**：`CMakeLists.txt` 中 `project(... VERSION ...)`，与兼容清单默认原版输入核心版本一致；
- **修订**：`packaging/PKGBUILD.in` 的 `pkgrel`，表示同一版本的适配层打包修订。

发布时需同步更新：

- `CMakeLists.txt` 的版本；
- `packaging/PKGBUILD.in` 与 `packaging/aur/PKGBUILD` 的 `pkgver` 和 `pkgrel`；
- `README.md` 中的安装文件名。

源码包和 nFPM 元数据从 CMake 与 `pkgrel` 读取版本，Nix 包也从这两处读取。原版核心更新由 `tools/update_upstream.py` 自动生成上述改动，见[维护者自动化](maintainer-automation.md#规范的更新提交)。

## GitHub Release

推送 `v<版本>-<修订>` 标签会启动 `release.yml`。也可以为已有标签手动触发：

```bash
gh workflow run release.yml -f tag=v<版本>-<修订>
gh workflow run release.yml -f tag=v<版本>-<修订> -f only_aur=true   # 只发布 AUR 元数据
```

原版更新自动化在 `release` 模式下会自行创建标签并触发发布，见[维护者自动化](maintainer-automation.md#标签与发布)。

### 发布物

每个版本包含以下 12 个文件：

| 文件 | 架构 |
| --- | --- |
| `fcitx5-wetypex-<版本>.tar.gz`（源码包） | 通用 |
| `fcitx5-wetypex-<版本>-<修订>-{x86_64,aarch64}.pkg.tar.zst` | 两种 |
| `fcitx5-wetypex_<版本>-<修订>_{amd64,arm64}.deb` | 两种 |
| `fcitx5-wetypex-legacy_<版本>-<修订>_{amd64,arm64}.deb` | 两种 |
| `fcitx5-wetypex-<版本>-<修订>.{x86_64,aarch64}.rpm` | 两种 |
| `fcitx5-wetypex-<版本>-<修订>-linux-{x86_64,aarch64}.tar.zst` | 两种 |
| `SHA256SUMS` | 以上全部文件的 SHA-256 |

### 发布步骤

1. 核对标签与源码版本、修订号一致，不一致时直接失败。
2. 在原生 ARM64 运行器上构建全部 ARM64 软件包，在 x86-64 上构建其余格式和源码包。
3. 运行 `tools/update_upstream.py --verify-release-dir dist --tag <标签>`：确认 11 个发布文件齐全、无重复或空文件，并逐个核对 deb、rpm 和 Arch 包内记录的版本与架构，以及便携归档中引擎宿主的 ELF 架构。全部通过后才写入 `SHA256SUMS`。
4. 发布到 GitHub Release：
   - 该标签的发布已完整公开时，保留现有文件和校验和，不再上传；
   - 已有未完成的发布时，先转为草稿再覆盖上传；不存在时以草稿创建；
   - 上传完成后才公开草稿。存在更高版本的正式发布时，本次发布不标记为“最新”，重建历史版本不会影响用户看到的最新版本；
   - 公开后再次确认发布完整。
5. 用 `packaging/aur/PKGBUILD` 更新 `fcitx5-wetypex` AUR 软件包及其 `.SRCINFO`。AUR 上已有更高版本时保留它，不会降级；内容无变化时不提交；推送为普通推送，不改写 AUR 历史。

`only_aur=true` 跳过全部构建，只执行第 5 步，用于单独重试 AUR。它不计为一次完整发布。

AUR 发布需要仓库机密 `AUR_SSH_PRIVATE_KEY`，其公钥需登记在 AUR 账户中。GitHub Release 使用工作流自带的 `GITHUB_TOKEN`。

## 发布检查

1. 在 Debian/Ubuntu、Fedora 和 Arch 构建环境中完成 Release 构建。
2. 确认软件包中没有原版运行时或账户数据。
3. 确认脚本中的安装前缀已由 CMake 替换。
4. 在新用户目录中运行 `fcitx5-wetypex-setup --download --accept-upstream-license` 和 `fcitx5-wetypex-setup --check --transport`。
5. 在真实 Fcitx5 会话中验证全拼输入、候选选择和设置入口。
6. 检查 deb、rpm、Arch 和便携归档的依赖与文件布局。
7. 确认 GitHub Release 已公开，且包含上表的 12 个文件。

## 安装布局

系统级文件安装到 `/usr`，原版运行时与用户状态写入 XDG 用户目录。软件包卸载脚本不得删除用户的账户、词库和配置。

## 星火应用商店投递

投递使用项目现有的 deb：软件名 WeTypeX，主页和维护入口为本仓库，启动入口为 `fcitx5-wetypex-settings`。软件介绍应说明它是 Fcitx5 输入法兼容项目，原版运行时与词典由用户在首次设置时从官方获取。

请按目标 Deepin/UOS 版本实际提供的依赖选择包：标准 deb 要求 glibc 2.35+，legacy deb 要求 glibc 2.31+，两者都要求稳定版 Fcitx5 5.0.14+、fcitx5-qt 5.0.10+ 和 LibIME 1.0.11+。发行版名称不能代替依赖检查，低于这些基线的系统需要单独适配。

正式投递前，在目标系统上用包管理器安装 deb，确认依赖可从其仓库解析，并验证首次运行时设置、添加输入法、候选上屏、V 快捷栏、升级与卸载。投递材料包括软件包、对应 SHA-256、图标、设置和候选界面截图，以及首次设置说明。商店版本沿用 `fcitx5-wetypex` 包名和项目版本，便于与现有安装互相升级。

星火官方的 [Git Releases 自动更新服务](https://gitee.com/spark-building-service/github) 可以跟随本仓库的 Release 更新。其 `file_list` 使用文件名模板：标准包为 `fcitx5-wetypex_{version}_amd64.deb`（ARM64 为 `_arm64.deb`），legacy 包为 `fcitx5-wetypex-legacy_{version}_amd64.deb`；`{version}` 是去掉 `v` 前缀、包含打包修订号的完整版本。请在目标系统验证后为每个架构选定其中一种包，不要同时配置两套模板。

本节提供投递材料与验证清单；商店上架是单独的发布操作。
