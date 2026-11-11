# 贡献指南

感谢你帮助改进 WeTypeX。提交 Issue 或 Pull Request 时，请先说明要解决的具体行为，并指出它属于哪一类：

- **Linux 集成**：Fcitx5、桌面环境、显示协议、音频、剪贴板、打包；
- **原版兼容**：特定原版版本或兼容档案下的 ABI、映像加载和运行时管理；
- **上游服务变化**：官方接口、账户或服务行为的变化。

项目结构见[架构说明](docs/architecture.md)，全部文档见[文档索引](docs/README.md)。

## 基本要求

- 不提交原版安装包、二进制、词典、模型、界面资源、账户数据或由它们生成的文件，边界见[发行与打包](docs/distribution.md#内容边界)。
- 如实反映候选、登录、配对、同步、语音、AI 和传输的结果，不伪造成功状态。
- 界面变更请注明参考的原版平台和版本，并写明平台差异。
- ABI 相关改动请注明适用的兼容档案或 `abi` 标识、兼容范围和失败时的行为。
- 日志、截图和示例数据请删除 UIN、设备码、传输码、剪贴板内容及私人文本。

## 代码风格

- C/C++：项目以 C++17 为基线，Fcitx5 插件在新版 Fcitx SDK 下以 C++20 编译；两空格缩进，沿用现有命名风格。
- Python 与 Shell：保持依赖少、出错即退出，为创建的账户或用户内容设置严格权限（目录 `0700`、文件 `0600`）。
- 文档：使用中文撰写，命令、文件名、字段名和程序名保持原文，术语遵循[文档索引](docs/README.md#术语)。

## 提交前检查

以下命令与 CI 的脚本检查一致：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
for script in scripts/*.sh.in; do
  case "$script" in *-syncd.sh.in|*-update.sh.in|*-sync.sh.in) continue;; esac
  sh -n "$script"
done
python3 -m py_compile scripts/setup_runtime.py scripts/wechat_emoticons.py scripts/voice_stream.py tools/*.py \
  build/fcitx5-wetypex-syncd build/fcitx5-wetypex-update build/fcitx5-wetypex-sync
```

修改 Nix 打包时，另运行 `nix flake check`。涉及原版运行时、传输或语音时，请在实际环境中运行：

```bash
fcitx5-wetypex-setup --download --accept-upstream-license
fcitx5-wetypex-setup --check --transport
```

## Pull Request

请按 PR 模板填写：

- **改动**：具体问题和改动后的行为；
- **验证**：实际执行的构建、运行和界面验证，以及所用的发行版、架构和桌面环境；
- **兼容性与限制**：适用的原版版本、Linux 环境、兼容范围和仍存在的限制。

改动用户可见的行为、命令或配置字段时，请同步更新对应文档。`README.md` 中的当前版本号由原版更新自动化统一替换，编辑时请遵循[维护者自动化](docs/maintainer-automation.md#规范的更新提交)中的约定。

安全问题请按[安全策略](SECURITY.md)私密报告，不要提交公开 Issue。
