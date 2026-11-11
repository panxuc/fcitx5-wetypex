# Hyprland 兼容

本文面向 Hyprland 0.55 及以上版本。从 0.55 起，Hyprland 的主配置改为 `hyprland.lua`，窗口规则使用 `hl.window_rule`。

## 加载规则文件

适配层安装的规则文件位于：

```text
/usr/share/fcitx5-wetypex/hyprland.lua
```

使用 `/usr/local` 等其他安装前缀时，路径相应变为 `<prefix>/share/fcitx5-wetypex/hyprland.lua`。安装过程不会修改用户配置，请在 `~/.config/hypr/hyprland.lua` 中显式加载：

```lua
require("/usr/share/fcitx5-wetypex/hyprland.lua")
```

`require` 为模块建立独立作用域，推荐优先使用。早期 0.55 版本不接受绝对路径的 `require` 时，改用在当前作用域执行文件的 `dofile`：

```lua
dofile("/usr/share/fcitx5-wetypex/hyprland.lua")
```

规则按书写顺序执行。请把加载语句放在自己的通用窗口规则之后，避免被宽泛的 `float` / `size` 规则覆盖。

## 规则内容

规则文件（源码中为 `data/hyprland.lua`）把三个窗口设为浮动并居中（`float = true`、`center = true`）：

| 窗口 | app_id / class | 逻辑尺寸 |
| --- | --- | --- |
| 设置 | `fcitx5-wetypex-settings` | 715 × 569 |
| 隔空传送 | `fcitx5-wetypex-transfer` | 317 × 519 |
| 问 AI | `fcitx5-wetypex-ai` | 440 × 566 |

`size` 使用逻辑像素，与 Qt 窗口的初始尺寸一致；修改界面尺寸时请同步更新 Lua 文件。`fcitx5-wetypex-ai-viewer` 与 AI 窗口共用源码和 app_id `fcitx5-wetypex-ai`，无需额外规则。

## 验证

```bash
hyprctl version
hyprctl reload
hyprctl configerrors
```

`configerrors` 应无输出。规则只作用于新打开的窗口，请关闭并重新打开对应窗口，然后检查：

```bash
hyprctl clients -j |
  jq '.[] | select(.class | test("^fcitx5-wetypex-(settings|transfer|ai)$")) |
      {class, floating, size, at, monitor}'
```

预期 `floating` 为 `true`，`size` 分别为 `[715,569]`、`[317,519]`、`[440,566]`，`at` 位于当前聚焦显示器中央。`class` 与表中不同时，检查程序是否以 Wayland 后端启动，以及 `QGuiApplication::setDesktopFileName` 是否在窗口创建前执行。

## 标题栏拖动

设置和 AI 窗口的标题栏在 Wayland 下调用 `QWindow::startSystemMove()`，由合成器处理移动，需要 Qt 5.15 或 Qt 6。使用 Qt 5.12 的 legacy 构建无法发起系统移动，可用 Hyprland 的 `Super + 鼠标左键` 移动浮动窗口。

## V 快捷栏

V 快捷栏是 `zwlr_layer_shell_v1` 表面，不是普通的 `xdg_toplevel`，因此 `hl.window_rule` 对它无效。它的 layer 命名空间是 `fcitx5-wetypex-vmode`，需要定制效果时使用 layer rule。

定位方式：

- Fcitx 在 Wayland 下给出相对输入表面的光标矩形。快捷栏通过 Hyprland IPC 读取当前聚焦窗口的 `at`，加上光标矩形并除以缩放系数得到插入点，再按显示器的 `reserved` 区域限制在屏幕内；读取失败时改用 `j/cursorpos`。
- 快捷栏优先限制在当前窗口范围内，点击时焦点不会落到下方窗口；窗口过小时使用显示器工作区。
- `~/.local/share/fcitx5-wetypex/state/vmode.log` 的 `placement:` 行记录坐标来源、锚点、显示器、工作区和当前窗口范围，用于核对定位。

快捷栏是 `KeyboardInteractivityNone` 的 overlay 层，不接收键盘焦点。合成器在打开或点击时短暂改变焦点，输入法最多等待 500 ms，原输入框恢复焦点后才执行快捷栏动作。超时、切换输入框或停用输入法会结束本次会话；快捷栏进程异常退出且原输入框仍有焦点时，改用 Fcitx 原生候选菜单。

LayerShellQt 或合成器协议不可用时，快捷栏使用 Fcitx 原生候选菜单。位置异常或闪退时，请检查 `vmode.log`，并确认 LayerShellQt 的 Qt 主版本与 WeTypeX 界面使用的 Qt 版本一致；更多步骤见[故障排查](troubleshooting.md#wayland-下-v-快捷栏不显示或闪退)。

## 快捷键冲突

Hyprland 常用 `Super` 作为窗口管理修饰键，而 WeTypeX 默认的语音快捷键包含 `Super`（`Win`）。按键被 Hyprland 截获时，可在 WeTypeX 设置中改用 `Ctrl + Alt` 等组合，或调整 Hyprland 的按键绑定。普通输入、候选和 V 快捷栏不受影响。

## Hyprland 0.52–0.54

安装包只提供 0.55+ 的 Lua 规则。旧版本使用 hyprlang 配置和 `windowrule` 语法，例如：

```text
windowrule = float, class:^(fcitx5-wetypex-settings)$
```

请按上表自行编写，不要与 0.55+ 的 Lua 配置混用。

## 参考

- [Hyprland 0.55 Window Rules](https://wiki.hypr.land/0.55.0/Configuring/Basics/Window-Rules/)
- [Hyprland 0.56 Window Rules](https://wiki.hypr.land/0.56.0/Configuring/Basics/Window-Rules/)
- [Hyprland 0.56 Start](https://wiki.hypr.land/0.56.0/Configuring/Start/)
- [Hyprland 0.56 Autostart](https://wiki.hypr.land/0.56.0/Configuring/Basics/Autostart/)
