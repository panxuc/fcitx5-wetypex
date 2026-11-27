# 快捷键语音

这份说明只描述 [marsxw/fcitx5-wetypex](https://github.com/marsxw/fcitx5-wetypex) 相对上游的语音改动：在任何窗口按一下开始、再按一下结束，文字先出现在屏幕底部的悬浮条里，原版引擎整理完成后再粘贴一次。输入法自己的「按住 Ctrl+Win 说话」仍见 [流式语音输入](voice-input.md)。

当前这套行为在提交 `94a9856` 里。快捷键本身不在输入法软件包里，而在 [keymapper](https://github.com/houmain/keymapper) 的配置里。

## 使用时会发生什么

按 `Ctrl+F11`。再按一次做的是结束，不是另一条命令。

1. 第一次按下开始录音。屏幕底部正中出现一条胶囊形悬浮条，此时条里还没有字。
2. 说话期间，识别出的文字只写在这条悬浮条上，不写进当前输入框，也不走输入法候选条。这就是不做「边说边上屏」。
3. 第二次按下只是停止麦克风，并让原来的在线引擎做智能整理。悬浮条继续留着。不要在整理完成前粘贴。
4. 引擎给出最终结果（状态是 `final`）之后，悬浮条消失，这句话被复制并粘贴一次。
5. 没识别到内容时，悬浮条直接消失。不弹「没有识别到语音」，也不发系统通知。

两次按键之间如果不到 0.35 秒，第二次会被丢掉。按得太快时看起来像没停住，等一下再按结束。

正在录音时再按一次发送的是停止，不是「立刻采用还没整理完的字」。整理要等多久，和输入法自己的语音一样，时间主要花在引擎上。悬浮条发现结果的间隔大约 10 毫秒。

### 悬浮条

悬浮条是一个不抢焦点的 X11 窗口，程序名是 `wetypex-voice-overlay`。

- 形状是胶囊：圆角半径等于高度的一半。没有另外描一圈边，避免右侧出现双线。
- 文字变长时条变宽；超过一行后固定到最大宽度，并只保留最后三行。
- 字号跟 Fcitx 经典界面的 `Font` 走。`~/.config/fcitx5/conf/classicui.conf` 里的磅值乘以 `Xft.dpi / 72` 得到像素。字体由 fontconfig 选择，抗锯齿和轻微微调与输入法候选条一致，不会比候选条更粗。
- 颜色取当前选中的 Fcitx 主题。`wetypex-dark` 是背景 `#252525`、文字 `#eeeeee`、强调色 `#2bd89b`。

这条快捷键路径在 X11 上验证过。Wayland 会话没有这条悬浮条和粘贴路径。

### 粘贴

普通窗口发送 `Shift+Insert`。Moonlight 窗口发送 `Ctrl+Alt+Shift+V`，这是 Moonlight 交给远端的粘贴键。

复制是立刻完成的。粘贴键发出去之后，剪贴板程序还会再留 0.3 秒，好让对方读到内容，然后才退出。延迟不是因为复制慢。

对 Moonlight 有两条不能做的事：

- 不把 Moonlight 窗口重新激活。再激活一次会丢掉它已经抓住的键盘。
- 不使用 `xdotool --clearmodifiers`。它会多发出按键抬起，把 Moonlight 的快捷键弄乱。

窗口是不是 Moonlight，在开始录音时看一次窗口类名，结束时用这个结果，不再查一次。

### 为什么是两个程序

录音、识别和整理在 `fcitx5-wetypex-voice` 里，也就是 `voice_stream.py`。悬浮条和粘贴在 `wetypex-voice-pad` 里。输入法自己的语音本来也是这样拆开的：Fcitx5 是一个进程，语音服务是另一个进程。

开始和停止直接发给语音服务已经开着的套接字，不再为了停一下而重新启动 Python。整理好的句子写在 `~/.local/share/fcitx5-wetypex/state/voice-inbox.json`。悬浮条在这次录音期间每 10 毫秒看一次这个文件。两个程序之间剩下的等待就是这一下。

快捷键这次录音使用的标记是「悬浮条进程号 + 时间」。输入法只处理自己发起的那次录音，所以不会把这句话再写进输入框。输入法自带的语音卡片如果也在运行，看到这个标记后会让开，避免粘贴两次。

## 和上游相比改了什么

上游的语音是输入法内部的按住说话：结果进当前输入框，快捷键必须能到达 Fcitx。Moonlight 会抢走键盘，输入法收不到那组键。

这个 fork 在上游语音服务外面加了一条不经过输入框的路。

| 部分 | 改动 |
| --- | --- |
| `scripts/wetypex-voice-pad` | 新增。响应 Ctrl+F11，显示悬浮条，整理完成后粘贴一次 |
| `scripts/wetypex-voice-overlay.c` | 胶囊条、主题色、与候选条相同的字体粗细，不再描边 |
| `scripts/voice_stream.py` | 没识别到内容时记为取消，错误文字留空；快捷键录音带上来源标记 |
| `scripts/voice_card.py` | 遇到快捷键录音时不显示、不粘贴，避免和悬浮条各贴一次 |
| `src/plugin/wetype.cpp` | 语音进行中不把半成品放进候选条；最终文字由语音卡片粘贴，插件不再提交第二次 |
| `data/wetypex-voice-pad.service.in` | 登录图形会话后启动悬浮条服务 |
| `CMakeLists.txt` | 安装上面的程序、服务，并让悬浮条链接 fontconfig |

输入法设置里的在线引擎、智能标点、麦克风和跨设备剪贴板没有换成另一套。跨设备剪贴板要先在设置里和手机配成同一组才会打开，单独一台电脑时那一项是关着的。

## 安装

给另一台电脑用的是 deb，不是上游 [panxuc/fcitx5-wetypex](https://github.com/panxuc/fcitx5-wetypex) 的安装包。上游包里没有这条快捷键语音。系统要是 X11，架构是 amd64，Ubuntu 22.04 及相同基线的 Debian 可以装。

当前这包在打包那台电脑上：

```text
dist/debian/fcitx5-wetypex_3.0.1.684-10_amd64.deb
```

把它复制到目标电脑后安装。`apt` 会装上包里声明的依赖，包括 Fcitx5、Bubblewrap、PipeWire 和 FFmpeg：

```bash
sudo apt install ./fcitx5-wetypex_3.0.1.684-10_amd64.deb
```

粘贴和窗口判断还要用这三个程序，deb 的依赖里没有它们：

```bash
sudo apt install xdotool xclip x11-utils
```

装好后这几个文件在固定位置：

| 文件 | 路径 |
| --- | --- |
| 快捷键程序 | `/usr/bin/wetypex-voice-pad` |
| 语音服务 | `/usr/bin/fcitx5-wetypex-voice` |
| 悬浮条 | `/usr/share/fcitx5-wetypex/wetypex-voice-overlay` |
| 用户服务 | `/usr/lib/systemd/user/wetypex-voice-pad.service` |

`PATH` 里如果还有旧的 `~/.local/bin/fcitx5-wetypex-voice` 或 `~/.local/bin/wetypex-voice-pad`，会盖住刚装上的程序。装完这个 deb 后删掉这两份本地副本。

### 从源码安装

没有这份 deb 时，在 Ubuntu 22.04 上安装编译依赖后自行构建。Qt 6 的开发包按 [从源码构建](installation.md#从源码构建) 替换。

```bash
sudo apt install git cmake clang build-essential pkg-config rustc \
  libboost-dev libfcitx5core-dev libfcitx5config-dev libfcitx5utils-dev \
  fcitx5-modules-dev libfcitx5-qt-dev libimecore-dev libimepinyin-dev \
  libjson-c-dev libssl-dev libfontconfig1-dev libwayland-dev \
  libc++-dev libc++abi-dev qtbase5-dev libqt5svg5-dev qtwebengine5-dev \
  libcairo2-dev libx11-dev libxrender-dev libxext-dev libxinerama-dev \
  xdotool xclip x11-utils

git clone https://github.com/marsxw/fcitx5-wetypex.git
cd fcitx5-wetypex
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build --parallel
sudo cmake --install build
```

作者以后加了功能，把上游合并进这个 fork，再重新打包或重新编译安装。不要改回上游安装包，否则快捷键语音会被盖掉。合并时如果悬浮条、`wetypex-voice-pad` 或空识别结果这几处冲突，保留 fork 里的版本。账户和词库不用重新下载。

```bash
git fetch https://github.com/panxuc/fcitx5-wetypex.git main
git merge FETCH_HEAD
cmake --build build --parallel
sudo cmake --install build
fcitx5 -r -d
systemctl --user restart wetypex-voice-pad.service
```

## 配置

deb 装上之后，输入法运行时、任务栏图标和 `Ctrl+F11` 还要在每台电脑上各配一次。账号、词库和下面这些用户配置都在用户目录，重装 deb 不会清掉。

### 输入法

用平时打字的那个用户执行，不要加 `sudo`：

```bash
fcitx5-wetypex-setup --download --accept-upstream-license
fcitx5-wetypex-setup --check --transport
fcitx5 -r -d
```

然后打开 `fcitx5-configtool`，把 **WeTypeX** 加进输入法列表，并登录微信输入法账号。`--accept-upstream-license` 表示已经阅读并同意微信输入法的上游许可。

`~/.config/fcitx5/config` 里不要把 `notificationitem` 放进 `DisabledAddons`。关掉它之后，GNOME 任务栏不会再显示空闲时的键盘图标和输入时的 WeTypeX 图标。

识别方式、标点、麦克风仍在 WeTypeX 设置的「语音输入」里。默认是原来的在线引擎和智能整理。跨设备剪贴板要先和手机配成同一组才会打开。

### 悬浮条外观

悬浮条跟着 Fcitx 经典界面走，不另存一套字体和颜色。改 `~/.config/fcitx5/conf/classicui.conf`：

| 项 | 作用 |
| --- | --- |
| `Font` | 磅值。像素大小是磅值乘以 `Xft.dpi / 72` |
| `Theme` | 背景、文字和强调色。`wetypex-dark` 是深色胶囊 |

改完后重新加载 Fcitx。下一次显示悬浮条时用新的字体和颜色。

### 语音服务

登录图形会话后启动悬浮条服务：

```bash
systemctl --user enable --now wetypex-voice-pad.service
```

看是否在跑，以及日志：

```bash
systemctl --user status wetypex-voice-pad.service
```

日志文件是 `~/.local/share/fcitx5-wetypex/voice-pad.log`。服务没起来时，第一次按下 `Ctrl+F11` 也会自己拉起一份。

### Ctrl+F11

快捷键不在 WeTypeX 里，在 [keymapper](https://github.com/houmain/keymapper)。发布包在 <https://github.com/houmain/keymapper/releases/latest>。装好后：

```bash
sudo systemctl enable --now keymapperd
```

再让登录时启动 `keymapper -u`，这样改配置文件会自己重新加载。GNOME 上可以写成 `~/.config/autostart/keymapper.desktop`：

```ini
[Desktop Entry]
Type=Application
Name=keymapper
Exec=keymapper -u
X-GNOME-Autostart-enabled=true
```

在 `~/.config/keymapper.conf` 里、所有 `[class = ...]` 窗口块的外面，加上这一行：

```ini
# 任何窗口：Ctrl+F11 开始或结束 WeTypeX 语音
ControlLeft {F11} >> $(/usr/bin/wetypex-voice-pad trigger)
```

路径用 `/usr/bin/wetypex-voice-pad`。不要写成某一台电脑主目录里的路径，也不要绑定单独的 `F10`。不要把打包那台电脑的整份 `keymapper.conf` 复制过去，里面还有只属于那台机器的命令。

保存后如果 `keymapper -u` 已经在跑，新快捷键会生效。没有 `-u` 时重新执行一次。设备抓取异常时再执行 `sudo systemctl restart keymapperd`。只重启服务、用户侧的 `keymapper` 仍拿着旧配置时，新快捷键不会出现。键盘被改乱时，同时按 `Shift+Esc+K` 退出 `keymapperd`。

如果还装着 just-talk，并且它也占用 `Ctrl+F11`，两个程序会抢同一个键。只用 WeTypeX 语音时不要再启动 just-talk。`~/.config/just-talk/` 里可能有账号信息，不需要时再删。

## keymapper 为什么要单独配

[keymapper](https://github.com/houmain/keymapper) 在按键进入窗口之前就把键拿到，可以换成别的键，也可以运行一条命令。它不是输入法。Moonlight 把键盘独占之后，Fcitx 收不到 `Ctrl+F11`，keymapper 仍然接得到，所以语音快捷键放在这里。

它由两个进程组成。`keymapperd` 用 root 抓住键盘设备。普通用户的 `keymapper` 读 `~/.config/keymapper.conf`，并执行 `$()` 里的命令。键名区分大小写，`F11` 不能写成 `f11`。

```ini
ControlLeft {F11} >> $(/usr/bin/wetypex-voice-pad trigger)
```

`ControlLeft {F11}` 表示按住左边的 Ctrl，再按 F11。`$()` 表示运行里面的命令。`trigger` 通知已经在后台的悬浮条：没在录音就开始，正在录音就结束。

`[class = 'moonlight']` 只对 Moonlight 生效，下一段 `[default]` 结束这段条件。语音这一行要放在所有这种块之后，任何窗口才会走到。这一行没有在结尾加 `^`，按住不放可能连着触发几次；0.35 秒内的重复会被丢掉。点按即可。