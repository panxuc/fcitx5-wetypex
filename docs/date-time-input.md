# 日期、时间候选

中文全拼输入 `riqi` / `rq` / `date`，或 `shijian` / `sj` / `time`，即可选择当前日期或时间。双拼、五笔由原版核心按候选词匹配“日期”“时间”，不在前端硬编码全拼字符串。

日期候选包括年月日、连字符、斜杠、点分隔、紧凑数字、中文数字和农历七种格式；时间候选包括三种日期时间及纯时间四种格式。格式保持原版输出，普通“日期”“时间”候选也保留。候选按既有空格、数字、翻页、鼠标路径选择，Enter 提交原编码，Esc 取消。

## 原版依据

研究使用[微信输入法官网](https://z.weixin.qq.com/)提供的 macOS 2.2.3.657 和 Windows 3.0.2.22 安装包。Windows 包仅解包检查，没有在本轮运行 Windows 界面。

- macOS 原版包含 `Calendar::Impl::MatchWord`、`TimeFormat::FormatAllDate`、`TimeFormat::FormatAllTime` 和农历转换函数。日历包装器已用于拼音、双拼和五笔；会话配置 `+0x127` 控制额外的英文 `date` / `time` 入口。
- 原版 C 接口的候选 `+0x30` 指向附加信息链表。`NewAndCopyCandidateInfoToCStruct` 将 `TimeFormatInfo` 转成类型 9 的节点：类型 `+0`、下一节点 `+8`、UTF-8 文本 `+16`、字节长度 `+24`。每种格式占一个节点，基础候选仍可能只写“日期”或“时间”。
- Windows 安装包的 `libwxhld.dll` 同样包含 `TimeFormatInfo`、`CalendarCandidateID`、`TimeFormatProfile<64>`、`TimeFormatType`、`riqi` 和 `shijian`。它与 Mac 版本使用同类原版核心能力。

Linux 宿主复制附加信息里的格式文本并保留原版候选 ID、覆盖长度及选词调用。格式排在普通候选之前；时区数据通过只读挂载系统 `/etc/localtime` 进入后端沙箱，显式 `TZ` 优先。无需额外日历库或每次输入再开一个后端会话。

## 验证方式

通过真实原版后端 IPC 验证候选生成、逐项选择、普通候选、原编码提交、退格清空、过期候选拒绝、时间快照和再次输入刷新；V 模式沿用原版结果。另用真实 Fcitx5 和 Wayland 编辑器核对键盘选词。所有格式与农历内容均由固定版本原版核心负责。
