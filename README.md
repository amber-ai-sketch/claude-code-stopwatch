# claude-code-stopwatch

把 **M5Stack Stopwatch Dev Kit (ESP32-S3)** 改造成 Claude Code 桌面伴侣。圆 AMOLED 表盘可左右滑动:总览页显示会话数、额度、像素小宠物;每个 Claude 会话各占一页,显示模型、token 进出量、缓存命中、cost、上下文占用。表上按钮通过蓝牙发命令给 Mac,触发语音输入、删字、提交 prompt 等。

灵感和协议来源:Anthropic 的 [claude-desktop-buddy](https://github.com/anthropics/claude-desktop-buddy) (M5StickC Plus + Arduino) 和我自己的 [claude-code-buddy-bridge](../claude-code-buddy-bridge)。本项目复用 Nordic UART Service 协议,但目标硬件、UI 框架、输入通路完全不同。

## 架构:设备当哑终端,Mac 决定一切

设备**不内置任何业务逻辑**:按钮只上报"哪个键、按下还是松开"这类原始事件,蓝牙发给 Mac 端的 daemon。daemon 按当前模式解释这些事件,并通过 macOS Quartz 把键盘事件注入到前台 App——和真实键盘走同一条输入管线。

这样切换模式、改键位都只动 Mac 端,不用重烧固件。

macOS 不接受未经蓝牙 UI 配对的 BLE-HID 设备输入,所以没走 HID profile,改由设备发 NUS 命令、daemon 本地注入。

## 两种使用模式

详见 [MODES.md](MODES.md)。模式状态存在 daemon,可在菜单栏切换。

- **模式一 — 遥控器(已可用)**:长按右键 → daemon 模拟按下电脑的 `Shift+Space` → 触发 Mac 微信输入法语音输入,松开即停。录音用的是**电脑麦克风**,手表只当无线触发器。
- **模式二 — 手表麦克风(开发中)**:用手表自己的麦克风录音再转文字。蓝牙上行传音已验证可行(连续录音零丢包),但同步采集模型有录音空档,待改为后台连续采集或改走 WiFi。转文字可接云端 API 或本地中文识别模型。

## 按键映射

| 物理动作 | 命令 | 效果 |
|---|---|---|
| 右键长按 >500ms | 原始 btn 事件(down/up) | 模式一:按住触发微信语音输入,松开停止 |
| 右键短按 | `Esc` | 退出 plan/vim/overlay |
| 左键单击 | `Backspace` | 删字符 |
| 左键长按 >500ms | `Return` | 提交 prompt |

## 项目结构

```
claude-code-stopwatch/
├── src/clawd_watch/      # Mac 端 Python daemon + hook + statusline + 菜单栏
├── firmware/             # ESP-IDF firmware (M5Stack Stopwatch)
│   ├── app_claude/       # 我们的应用代码(UI 页、BLE、按键、录音)
│   ├── patches/          # 对上游 demo 的改动
│   └── apply_to_upstream.sh
├── resources/            # launchd plist 模板
├── install.sh / uninstall.sh
├── MODES.md              # 两种使用模式的规划
└── pyproject.toml
```

## Mac 端安装

```bash
cd ~/Projects/claude-code-stopwatch
./install.sh
```

会做这些事:
1. 在 `.venv/` 建 Python venv,装 bleak + aiohttp + rumps
2. 把命令(`clawd-watch-daemon` / `clawd-watch-ui` / `clawd-watch-hook` / `clawd-watch` / `clawd-statusline`)软链到 `~/.local/bin/`
3. 复制 launchd plist 到 `~/Library/LaunchAgents/`,加载 daemon 和菜单栏程序
4. 备份并合并 hooks + statusLine 配置到 `~/.claude/settings.json`

菜单栏程序显示连接状态,可扫描/重连设备、切换模式、看日志。

## 固件编译烧录

```bash
cd firmware
bash apply_to_upstream.sh                 # 把 app_claude 同步进上游 demo
cd upstream && source ~/esp/esp-idf/export.sh
idf.py build
idf.py -p /dev/cu.usbmodemXXXX flash
```

## 验证(无固件,纯 Mac 端)

```bash
clawd-watch status            # 连接状态 + 当前模式 + 会话数据
clawd-watch test-statusline   # 推一条假 statusline 数据
clawd-watch tail              # 看 daemon 日志
```

## 已知限制

- **SSH 远程 Claude Code 不支持语音输入**。微信输入法上屏走 macOS `NSTextInputClient`(GUI 层文本注入),SSH 终端只透传按键字节,IME 上屏事件无法穿透。这是所有中文 IME 通用限制(微信/搜狗/系统拼音都一样),非本项目可修。本地 + tmux + Mac 息屏期间均工作正常。
- **微信输入法依赖**(模式一):语音方案绑定 Mac 输入法配置。他人拿 Mac 过去必须先装微信输入法 + 配 Shift+Space 快捷键。
- **桌上有多块 ESP32 时需注意**:daemon 按设备名 `ClawdWatch` 精确匹配,避免误连其他也广播 NUS 服务的板子。
- **Mac-only**。Linux/Windows 的 bleak 后端略有差异,未测试。
