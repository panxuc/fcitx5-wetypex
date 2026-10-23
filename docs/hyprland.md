# Hyprland 兼容

本文面向 Hyprland 0.55 及以上版本。0.55 起 Hyprland 的主配置改为 `hyprland.lua`，窗口规则使用 `hl.window_rule`；0.52–0.54 的 `windowrule` / `windowrulev2` 写法不再适用。

## 安装与加载

构建安装后，规则文件位于：

```text
/usr/share/fcitx5-wetypex/hyprland.lua
```

在 `~/.config/hypr/hyprland.lua` 中显式加载：

```lua
require("/usr/share/fcitx5-wetypex/hyprland.lua")
```

使用 `/usr/local` 等自定义前缀时，路径相应变为 `/usr/local/share/fcitx5-wetypex/hyprland.lua`。

Hyprland 0.55 早期版本如果不接受绝对路径 `require`，可以改用：

```lua
dofile("/usr/share/fcitx5-wetypex/hyprland.lua")
```

`dofile` 会直接在当前 Lua 作用域执行文件；`require` 会为模块建立独立作用域，推荐优先使用。

安装脚本不会自动修改用户配置，需要用户自行添加上面的加载语句。规则按书写顺序执行，建议把它放在自己的通用窗口规则之后，避免被宽泛的 `float` / `size` 规则覆盖。

## 规则内容

`data/hyprland.lua` 定义三个浮动窗口：

| 窗口 | app_id / class | 逻辑尺寸 |
| --- | --- | --- |
| 设置 | `fcitx5-wetypex-settings` | 715 × 569 |
| 隔空传送 | `fcitx5-wetypex-transfer` | 317 × 519 |
| 问 AI | `fcitx5-wetypex-ai` | 440 × 566 |

三条规则都启用了 `float = true` 和 `center = true`。`size` 使用逻辑像素，需要与 Qt 窗口中的初始尺寸保持一致；如果修改了界面尺寸，应同步更新 Lua 文件。

`fcitx5-wetypex-ai-viewer` 可执行文件和 AI 窗口共用同一份源码，app_id 固定为 `fcitx5-wetypex-ai`，因此不需要额外的 viewer 规则。

## 验证

确认 Hyprland 版本：

```bash
hyprctl version
```

重新加载配置：

```bash
hyprctl reload
hyprctl configerrors
```

`configerrors` 应无输出。规则只对重新打开的窗口生效，修改后需要关闭并重新启动对应窗口。

逐个打开设置、隔空传送和 AI 窗口，然后检查：

```bash
hyprctl clients -j |
  jq '.[] | select(.class | test("^fcitx5-wetypex-(settings|transfer|ai)$")) |
      {class, floating, size, at, monitor}'
```

预期 `floating` 为 `true`，`size` 分别为 `[715,569]`、`[317,519]`、`[440,566]`，`at` 位于当前聚焦显示器中央。

如果 `class` 不是表中的值，检查程序是否以 Wayland 后端启动，以及 `QGuiApplication::setDesktopFileName` 是否在窗口创建前执行。

## 标题栏拖拽

设置和 AI 窗口的标题栏在 Wayland 下使用 `QWindow::startSystemMove()`，由合成器处理移动，不再直接调用 `move()`。这需要 Qt 5.15 或 Qt 6。

使用 Qt 5.12 的 legacy 构建在 Wayland 下无法发起系统移动，可以用 Hyprland 的 `Super + 鼠标左键` 移动浮动窗口，或升级 Qt。

## V 工具栏

V 工具栏使用 `zwlr_layer_shell_v1`，不是普通 `xdg_toplevel`，因此 `hl.window_rule` 对它无效。它的 layer 命名空间是 `fcitx5-wetypex-vmode`，只应通过 layer rule（而非窗口规则）定制效果。本项目的 Hyprland 规则只负责设置、隔空传送和 AI 窗口，不要尝试用窗口规则移动 V 工具栏。

Wayland 下 Fcitx 给出的是相对输入表面的光标矩形，不是全局坐标。Hyprland 上工具栏通过 IPC 读取当前聚焦窗口的 `at`，加上光标矩形并除以缩放系数得到插入点，再按显示器 `reserved` 区域限制在屏幕内；读取失败时退回 `j/cursorpos`。`state/vmode.log` 的 `placement:` 行会记录坐标系来源、锚点、显示器和工作区，便于核对位置。

工具栏是 `KeyboardInteractivityNone` 的 overlay 层，不接收键盘焦点。合成器在打开或点击时短暂改变焦点，输入法会等待最多 500ms，原输入框恢复焦点后才执行工具栏动作。超时、切换输入框或停用输入法会结束该会话；工具栏进程异常退出且原输入框仍有焦点时，改用原生候选菜单。

LayerShellQt 或合成器协议不可用时，V 工具栏会回退到 Fcitx 原生候选菜单。Hyprland 下如果工具栏位置异常或闪退，请检查 `state/vmode.log`，并确认 LayerShellQt 的 Qt 主版本与 Fcitx5 使用的 Qt 版本一致。

## 快捷键冲突

Hyprland 大量使用 `Super` 作为窗口管理修饰键。WeTypeX 默认语音快捷键包含 `Super`，如果按键被 Hyprland 截获，可以在 WeTypeX 设置中改为 `Ctrl + Alt` 组合，或调整 Hyprland 的按键绑定。普通输入、候选和 V 模式菜单不受影响。

## 0.52–0.54

本仓库只维护 0.55+ 的 Lua 规则。旧版本需要使用 hyprlang 配置和 `windowrule`，例如：

```text
windowrule = float, class:^(fcitx5-wetypex-settings)$
```

旧格式不由安装包提供，也不应与 0.55+ 的 Lua 配置混用。

## 官方文档

- [Hyprland 0.55 Window Rules](https://wiki.hypr.land/0.55.0/Configuring/Basics/Window-Rules/)
- [Hyprland 0.56 Window Rules](https://wiki.hypr.land/0.56.0/Configuring/Basics/Window-Rules/)
- [Hyprland 0.56 Start](https://wiki.hypr.land/0.56.0/Configuring/Start/)
- [Hyprland 0.56 Autostart](https://wiki.hypr.land/0.56.0/Configuring/Basics/Autostart/)
