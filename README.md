# claude-code-stopwatch

> Co-author: Claude Opus 4.7

把 **M5Stack Stopwatch Dev Kit (ESP32-S3)** 改造成 Claude Code 桌面伴侣。圆 AMOLED 显示当前会话状态、token 消耗、5h/7d 额度;表上按钮触发 Mac 微信输入法语音输入,文字直接注入 Claude Code 输入框;权限请求弹到表面,按按钮 allow / deny。

灵感和协议来源:Anthropic 的 [claude-desktop-buddy](https://github.com/anthropics/claude-desktop-buddy) (M5StickC Plus + Arduino) 和我自己的 [claude-code-buddy-bridge](../claude-code-buddy-bridge)。本项目复用 Nordic UART Service 协议,但目标硬件、UI 框架、HID 通路完全不同。

## 项目结构

```
claude-code-stopwatch/
├── src/clawd_watch/      # Mac 端 Python daemon + hook + statusline
├── firmware/             # ESP-IDF firmware (M5Stack Stopwatch)
├── resources/            # launchd plist 模板
├── install.sh            # 一键安装 Mac 端
├── uninstall.sh
└── pyproject.toml
```

## 状态

- [x] M1: Mac 端 daemon + statusline + hook + install — **就绪**
- [ ] M2: 固件骨架 + LVGL 占位 idle 页
- [ ] M3: 按钮事件机
- [ ] M4: BLE NUS 协议层(配对 + heartbeat)
- [ ] M5: BLE HID Profile 共存 — **项目硬闸门**
- [ ] M6: 6-8 个 motion graphic(Apple Watch 风格)
- [ ] M7: 端到端 dogfood
- [ ] M8: 安装文档 + 已知问题

详见 plan: `~/.claude/plans/eventual-weaving-meteor.md`

## 端到端交互

| 物理动作 | HID 输出 | 上层效果 |
|---|---|---|
| 右键长按 >500ms | `Shift+Space` | 微信输入法启动录音 |
| 右键单击 <200ms | `Shift` | 结束录音 + ASR 上屏 |
| 右键双击 <300ms | `Esc` | 退出 plan/vim/overlay |
| 左键单击 | `Backspace` | 删字符 |
| 左键双击 <300ms | `Ctrl+C` | 清空输入 / 中断 Claude |
| 左键长按 >500ms | `Return` | 提交 prompt |

权限待决期间(attention 状态),按键临时改语义:左键单击 = `allow`,右键单击 = `deny`。

## Mac 端安装

```bash
cd ~/Projects/claude-code-stopwatch
./install.sh
```

会做这些事:
1. 在 `.venv/` 建 Python venv,装 bleak + aiohttp
2. 把 4 个命令(`clawd-watch-daemon` / `clawd-watch-hook` / `clawd-watch` / `clawd-statusline`)软链到 `~/.local/bin/`
3. 复制 launchd plist 到 `~/Library/LaunchAgents/`,加载 daemon
4. 备份并合并 hooks + statusLine 配置到 `~/.claude/settings.json`

## 验证(无固件,纯 Mac 端)

```bash
clawd-watch test-statusline   # 推一条假 statusline 数据
clawd-watch status            # 'line: model=... cost=... ctx=...%'
clawd-watch tail              # 看 daemon 日志
```

## 已知限制

- **SSH 远程 Claude Code 不支持语音输入**。微信输入法上屏走 macOS `NSTextInputClient`(GUI 层文本注入),SSH 终端只透传按键字节,IME 上屏事件无法穿透。这是所有中文 IME 通用限制(微信/搜狗/系统拼音都一样),非本项目可修。本地 + tmux + Mac 息屏期间均工作正常。
- **微信输入法依赖**:语音方案绑定 Mac 输入法配置。他人 demo 拿 Mac 过去必须先装微信输入法 + 配 Shift+Space 快捷键。
- **Mac-only**。Linux/Windows 的 bleak 后端略有差异,未测试。
