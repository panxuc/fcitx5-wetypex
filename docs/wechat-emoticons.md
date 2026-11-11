# Linux 微信表情

微信的 `[微笑]`、`[捂脸]` 等内置表情有专属图片和文本别名，替换为外观相近的 Unicode emoji 会改变含义。WeTypeX 在候选窗口中显示原版微信输入法安装包里的原图，并向应用提交 Linux 微信能够识别的文本别名，由微信客户端渲染为表情。

## 安装资源

准备原版运行时时会自动安装表情资源。表情图片取自兼容档案的资源包（当前为 2.2.3.657）。只需重新提取界面资源时运行：

```bash
fcitx5-wetypex-setup --icons-only --download --accept-upstream-license
# 或使用本地官方 ZIP
fcitx5-wetypex-setup --icons-only --archive /path/to/WeType_2.2.3_657.zip
```

安装器校验官方 ZIP 的 SHA-256，把 PNG 提取到 `~/.local/share/fcitx5-wetypex/ui/wechat-emoticons/`，并生成彩色字体 `~/.local/share/fonts/fcitx5-wetypex-wechat.ttf`（设置了 `XDG_DATA_HOME` 时以其为根）。生成字体需要 fontTools、Pillow 和 fontconfig，发行包已声明这些依赖；源码环境缺少 fontTools 或 Pillow 时，安装器给出提示，候选显示文本名称。

安装后重启 Fcitx5，让候选界面加载新字体；使用独立桌面候选面板时，也请重启该面板。

原图和派生字体只保存在用户数据目录，不进入仓库、源码包或发行包。仓库中的 `data/wechat-emoticons.json` 只记录别名、图片文件名和显示码位的映射。

## 显示方式

Fcitx 的候选文本接口没有图片字段。WeTypeX 生成的本地字体把私用码位 `U+10F000–U+10F0FF` 映射到原图，Classic UI 或支持相同字体的候选界面即可绘制图片，无需另开会抢焦点的窗口。私用字符只用于候选显示，提交时使用引擎候选 ID 和文本别名。图片旁保留名称；字体未就绪时只显示名称。

## 生效范围

插件根据 Fcitx 输入上下文提供的程序标识，精确识别 `wechat`、`weixin`、`xwechat`、`com.tencent.WeChat` 和 `com.tencent.Weixin`（忽略大小写和 `.desktop` 后缀）。只有这些客户端会收到微信表情候选；WeChatAppEx 浏览器、网页微信和其他应用照常使用普通 emoji 与颜文字，这些开关相互独立。

部分原生 Wayland 输入协议只提供共享上下文，没有程序身份。此时微信专属候选保持关闭，WeTypeX 不根据窗口标题或“上一个窗口”推测应用。通过客户端输入法接口提供应用身份的微信会话可正常识别。密码输入框仍然禁用输入法处理。

后端在发布候选快照前过滤专属或不受支持的别名，并保留原引擎的候选 ID，因此过滤后的翻页、数字选词和云候选刷新都对应正确的候选。

## 别名映射依据

映射以原生 Linux 微信 `4.1.13.26` 中的经典表情别名表和资源 `:/emoji/newemoji-config.xml` 为准，按资源文件名与原版输入法的 PNG 对应，共确认 149 张图片、379 个受支持或可规范化的别名。

- 旧表与新表重名时采用新表的名称和图片，例如 `[Blush]` 使用 `[囧]`，`[再见]` 使用 `smiley_39b.png`。原版包中保留的旧图片仍生成字体字形，但不覆盖新表的映射。
- 提交时统一使用客户端支持的中文别名。例如原版输入法的 `[折磨]` 对应同一张经典图片，当前 Linux 微信的名称为 `[疯了]`，因此提交后者；`[Bah!L]`、`[Bah!R]` 使用对应的中文名称。
- 13 个未确认能被该 Linux 客户端识别的额外别名不作为候选发布。

微信后续版本调整支持的名称时，需要核对映射并更新 `data/wechat-emoticons.json`。
