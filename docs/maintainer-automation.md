# 维护者自动化

本文面向仓库维护者，说明官网发布新的原版稳定版后，如何把它加入兼容清单并发布新的适配层版本。自动化由三个工作流和 `tools/update_upstream.py` 组成，默认关闭，可以逐级开启。用户侧的更新行为见[运行时更新、回退与清理](runtime-updates.md)，发布流程本身见[发行与打包](distribution.md#github-release)。

## 流程概览

1. **发现**：每天读取官网稳定版信息，为新版本生成规范的更新提交并开启机器人 PR。
2. **验证**：在只读工作流中构建全部软件包，并在 x86-64 与原生 ARM64 上运行真实原版核心、传输、语音和数据迁移检查。
3. **完成**：受信任的 `main` 代码复核候选内容与全部检查结果，以快进方式把这一个已验证提交并入 `main`，创建标签并启动发布。

| 工作流 | 文件 | 触发方式 |
| --- | --- | --- |
| 发现原版更新 | `.github/workflows/upstream-discover.yml` | 每天 UTC 03:19；手动触发（默认 `dry_run=true`） |
| 验证原版更新 | `.github/workflows/upstream-verify.yml` | 由发现流程以指定的 head 与 base 触发 |
| 合并并发布通过验证的原版更新 | `.github/workflows/upstream-complete.yml` | 验证成功后自动触发（仅 `release` 模式）；手动触发 |

## 前提条件

- **仓库变量** `WETYPEX_UPSTREAM_AUTOMATION`：决定自动化模式，默认未设置，即关闭。
- **允许 GitHub Actions 创建 Pull Request**：在仓库设置“Actions → General → Workflow permissions”中开启。`pr` 和 `release` 模式开启 PR 时需要它。
- **`main` 的推送规则**：完成流程用 `GITHUB_TOKEN` 向 `main` 快进推送已验证的提交，并创建 `v*` 标签。若为 `main` 或标签配置了分支保护或规则集，需要允许 GitHub Actions 执行这两项操作。
- **凭据**：GitHub 侧的全部操作使用工作流自带的 `GITHUB_TOKEN`，无需个人访问令牌或部署密钥。发布到 AUR 需要仓库机密 `AUR_SSH_PRIVATE_KEY`。

## 模式

| `WETYPEX_UPSTREAM_AUTOMATION` | 定时发现 | 验证成功后 | 手动操作 |
| --- | --- | --- | --- |
| 未设置或其他值 | 跳过 | 不自动完成 | 发现流程可做只读预演；完成流程可做只读复核 |
| `pr` | 创建、rebase 机器人 PR 并请求验证 | 不自动完成，由维护者审阅后合并、打标签、发布 | 同上，另可执行写入式发现 |
| `release` | 同 `pr`，并恢复未完成的发布 | 自动完成：集成、打标签、发布 | 同上，另可手动恢复发布（`publish=true`） |

删除该变量即可回到关闭状态。已经开始的工作流运行会继续完成。

## 发现与 PR 维护

发现流程只在受信任的 `main` 检出上运行 `tools/update_upstream.py --reconcile`。每次运行比较官网稳定版与 `main` 上兼容清单的默认版本：

| 情况 | 处理 |
| --- | --- |
| 官网版本等于默认版本，官方 ZIP 地址与 MD5 也一致 | 已是最新。`release` 模式下检查该版本的发布是否完成，未完成时触发完成流程续做 |
| 官网版本等于默认版本，但官方 ZIP 已被替换 | 停止并报错，等待人工审查 |
| 官网版本更高，尚无该版本的 PR 与分支 | 生成更新提交，推送到 `upstream/runtime-<版本>`，开启机器人 PR 并请求验证 |
| 机器人 PR 已存在且基于当前 `main` | 保持原提交，必要时请求验证 |
| 机器人 PR 已存在但 `main` 已前进 | 确认现有分支内容规范后，在当前 `main` 上重新生成提交并更新分支，再请求验证 |
| 分支已推送但 PR 尚未创建 | 确认分支内容规范后复用或重新生成，补开 PR |
| 同名分支对应人工创建、草稿或已关闭的 PR | 保持原样，不做任何修改 |

### 规范的更新提交

每个机器人提交都是当前 `main` 上的**一个**提交，只修改以下五个普通文件：

| 文件 | 改动 |
| --- | --- |
| `data/runtime-profiles.json` | 新增官方 ZIP 与核心组件条目；以当前默认档案为模板新增档案，替换核心与词典来源；`default` 指向新版本 |
| `CMakeLists.txt` | 项目版本改为新版本 |
| `packaging/PKGBUILD.in`、`packaging/aur/PKGBUILD` | `pkgver` 改为新版本，`pkgrel` 重置为 `1` |
| `README.md` | 旧的“版本-修订”替换为“新版本-1”，其余旧版本字符串替换为新版本 |

生成器下载官方 ZIP，核对官方安装信息给出的 MD5，计算 ZIP 的 SHA-256、核心映像及其 `x86_64`/`arm64` 切片散列和每个词典的散列。新档案沿用模板的传输模块、界面与语音资源来源。兼容清单中已有的条目不可变：同一条目内容变化，或同版本号对应不同的核心，生成器都会报错。

> [!NOTE]
> 生成器对 `README.md` 做全文字符串替换。编辑 README 时，只在表示当前默认核心版本或当前发布文件名的地方书写当前版本号；其他文档使用 `<版本>` 占位或引用 `fcitx5-wetypex-setup --status`。

### 更新机器人分支

更新已有机器人分支前，工具先确认分支内容完全规范：分支是其 base 上的单个提交，只修改上述五个文件且保持普通文件权限，内容与依据其记录的官方信息独立生成的结果逐字一致，原 base 仍在 `main` 历史中，记录的官方版本、地址和 MD5 与官网当前信息一致。任何一项不符都会停止，由维护者处理。

确认后，新提交以 `--force-with-lease` 推送，只覆盖该机器人分支上刚检查过的那个提交。人工编辑过的分支、草稿或已关闭的 PR 不会被改写；关闭 PR 表示维护者暂不采用该版本，需要恢复时重新打开 PR 即可。

## 验证

验证流程的运行名称为 `upstream <head> base <base>`，记录被验证的确切提交。每个运行作业先确认检出的提交等于 head、其唯一父提交等于 base。验证由 13 个作业组成：

| 作业 | 内容 |
| --- | --- |
| `packages / arch-linux` | Arch 软件包构建与脚本检查 |
| `packages / ubuntu-jammy-deb` | Ubuntu 22.04 标准 deb，并在该旧基线上运行真实原版核心与 TLS |
| `packages / ubuntu-focal-legacy` | Ubuntu 20.04 legacy deb，并运行真实原版核心与 TLS |
| `packages / arm64 / packages` | 全部 ARM64 软件包，并在旧基线上运行真实 ARM64 核心与 TLS |
| `packages / nix (ubuntu-24.04)`、`packages / nix (ubuntu-24.04-arm)` | 两种架构的 flake 检查与 Nix 包真实运行检查 |
| `packages / deb-runtime (…)`（5 个） | 在 Debian 12、13 与 Ubuntu 22.04、24.04、26.04 容器中安装 deb 并启动 |
| `runtime (ubuntu-24.04)`、`runtime (ubuntu-24.04-arm)` | 见下文 |

两个 `runtime` 作业分别在 x86-64 和原生 ARM64 上：

- 下载默认档案并运行 `fcitx5-wetypex-setup --check --transport`；
- 运行 `--verify-upgrade-from 2.2.3.657`，在隔离目录中确认常用语从旧版升级后保持一致，且旧版能读回升级后的数据；
- 用临时匿名身份申请匹配码，并用合成语音完成一次真实语音识别；
- 结束后删除临时身份和数据。

新的官方版本只有通过这些检查才会进入发布。官方在新版本中改变了原版 ABI、资源布局或数据格式时，验证会失败，更新停在 PR 阶段，需要维护者进行兼容性工作。

同一 head 与 base 的验证最多自动请求 3 次。已有进行中或成功的运行时不再请求；3 次都失败后，发现流程报错并停止为该提交重试，维护者检查失败原因后可在 Actions 中手动重新运行。

## 完成与集成

完成流程只检出受信任的 `main`，通过 GitHub API 读取候选内容，不运行候选分支的代码或产物。它依次核对：

1. 验证运行已成功完成，来自本仓库的 `upstream-verify.yml`，分支为 `upstream/runtime-<版本>`，且分支版本与提交记录的版本一致；
2. 存在唯一对应、由 `github-actions[bot]` 创建、以 `main` 为目标的非草稿 PR，其 head 与验证运行一致；
3. head 是 base 上的单个提交，`main` 仍指向该 base；
4. PR 只修改规范的五个文件；
5. 官网当前稳定版信息与提交记录一致；完成流程重新下载官方 ZIP，在 base 上独立生成补丁，结果与 PR 内容逐字一致；
6. 13 个必需作业全部存在，且运行中的每个作业都成功。

集成采用**线性单提交**方式：完成流程以非强制推送把已验证的 head 快进到 `main`。这次推送只有在 `main` 仍等于 base 时才会成功，任何抢先进入 `main` 的提交都会使它被拒绝；`main` 上不会出现合并提交，也不会被强制改写。推送前后，完成流程都再次确认 PR 的 head 未被修改。随后它确认集成的提交以 base 为唯一父提交、文件树与验证内容相同且仍在 `main` 历史中，再创建 `v<版本>-1` 标签并启动发布。

维护者手动处理的普通 PR 仍可按常规方式 squash 或 rebase 合并。

### 标签与发布

- 标签不可变。标签已存在时，完成流程解析它（包括附注标签）指向的提交：与集成的提交相同则继续，不同则停止。
- 由 `GITHUB_TOKEN` 创建的标签不会自动触发工作流，完成流程因此显式启动 `release.yml`。
- 发布已在运行时等待；发布已完整且有成功的运行时跳过。只重试 AUR 的运行不计为完整发布。
- 最近一次发布运行失败时，只重新运行失败的作业及其依赖作业，已成功的构建保持不变；同一运行最多自动尝试 3 次。被取消的发布需要维护者手动恢复。

### 恢复中断的发布

提交已进入 `main` 但标签或发布未完成时，完成流程可以续做。它从已合入的兼容清单中记录的官方版本、地址和 MD5 重新下载并核对官方 ZIP，无需依赖旧的安装信息地址。续做只在已验证的提交仍位于 `main` 历史中时进行；官网撤回该版本或替换了同版本的官方 ZIP 时停止。

`release` 模式下，每天的发现流程会检查最新已合入版本的发布状态，在找到对应的成功验证、发布未完成且没有进行中的发布时，自动触发完成流程续做。

## 手动操作

以下示例使用 GitHub CLI，也可以在 Actions 页面填写相同的输入。

**预演发现**（任何模式下均可，不写入任何内容）：

```bash
gh workflow run upstream-discover.yml -f dry_run=true
```

运行日志输出官网版本、目标分支、当前 base、计划动作（`create`、`rebase`、`up_to_date` 或 `manual_pr_left_unchanged`）和将要修改的文件。

**执行写入式发现**（需要 `pr` 或 `release` 模式）：

```bash
gh workflow run upstream-discover.yml -f dry_run=false
```

**复核某次验证**（只读，任何模式下均可）：

```bash
gh workflow run upstream-complete.yml -f verification_run=<验证运行 ID>
```

全部核对通过时，日志输出 PR 编号、head 和计划的发布版本，不做任何修改。

**恢复集成与发布**（需要 `release` 模式）：

```bash
gh workflow run upstream-complete.yml -f verification_run=<验证运行 ID> -f publish=true
```

**在本地运行工具**需要设置 `GH_TOKEN` 与 `GITHUB_REPOSITORY`：

| 命令 | 作用 |
| --- | --- |
| `python3 tools/update_upstream.py --reconcile --dry-run` | 预演发现与 PR 维护 |
| `python3 tools/update_upstream.py --reconcile --dry-run --base-ref <完整提交 SHA>` | 以指定的不可变提交作为 base 预演 |
| `python3 tools/update_upstream.py --complete-run <ID>` | 只读复核某次验证 |
| `python3 tools/update_upstream.py --release-complete <标签>` | 发布完整时以状态 0 退出 |
| `python3 tools/update_upstream.py --write` | 无需 GitHub 凭据，在本地工作区写入规范改动，用于人工 PR |

`--publish` 只能与 `--complete-run` 一起使用，并且总是重新下载官方 ZIP。

## 需要维护者处理的情形

| 情形 | 处理 |
| --- | --- |
| 验证失败（包括新版本改变了原版 ABI） | 检查失败的作业；需要兼容性改动时提交常规 PR |
| 同一提交的验证连续 3 次失败 | 排除原因后在 Actions 中手动重新运行 |
| 官网替换了同版本的官方 ZIP，或撤回了已合入的版本 | 人工审查后决定是否调整兼容清单 |
| 机器人分支内容被改动，或同名 PR 为人工、草稿、已关闭状态 | 自动化保持原样；关闭或修正后重新打开，或继续人工流程 |
| 已有标签指向其他提交 | 核对标签与发布历史后人工处理 |
| 发布被取消，或同一发布运行 3 次尝试后仍失败 | 检查运行后手动重新运行或重新触发 `release.yml` |
| 兼容宿主、ABI 处理或资源来源需要修改 | 走常规 PR 审查；自动化只生成兼容清单与版本元数据 |
