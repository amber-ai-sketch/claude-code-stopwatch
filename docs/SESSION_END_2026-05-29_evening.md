# 2026-05-29 晚 Session 结束 — 三大功能全部打通

> Co-author: Claude Opus 4.8
> 这是给明天新 session 的交接文档。读完即可接上。

## TL;DR

**三大目标功能(像素宠物 / cost 显示 / 语音输入)今天全部打通并真机验证。** 早上那份 `SESSION_END_2026-05-29.md` 记录的 M5 死局(BLE HID 被 macOS 拒)已用 B 方案绕过解决。代码已提交并推送到 GitHub(`origin/main`, 最新 `6aeea75`)。

仓库: https://github.com/amber-ai-sketch/claude-code-stopwatch

## 三大功能现状

| 功能 | 状态 | 实现位置 |
|---|---|---|
| ① 像素宠物动画 | ✅ 真机验证 | bufo idle GIF 居中循环, `firmware/app_claude/ui/idle_page.cpp` + `assets/bufo_idle_gif.c` |
| ② cost 信息 | ✅ 真机验证 | 底部 cost + model + 三圆环(5h/7d/ctx), daemon heartbeat 实时推 |
| ③ 语音输入按键 | ✅ 真机验证 | 见下方交互表 |

### 按键交互表(已全部真机验证命令到达 daemon)

| 物理动作 | 效果 |
|---|---|
| 右键长按 >500ms 按住/松开 | 左 Shift+Space 按下/松开 → 微信输入法录音 |
| 右键短按 (<500ms) | Esc |
| 左键单击 | Backspace |
| 左键长按 | Return |

## 今天的关键突破(都已存进记忆 + commit message)

1. **B 方案绕过 BLE HID 死局**: 设备发 NUS JSON `{"cmd":"key_down/up/tap",...}`,Mac daemon 用 Quartz CGEvent 本地注入键盘。**仍是纯无线**(设备↔Mac 走 BLE NUS),只是注入在 Mac 本地完成。

2. **微信 IME 注入根因**(最关键): 必须发**真实的左 Shift 键** keyDown/keyUp (keycode 0x38) 包住 space,**不能**给 space 事件贴 shift flag。微信只认真实修饰键按下事件 + 只认左侧 Shift。见 `src/clawd_watch/keyinject.py`。

3. **按键"读不到"是采样时机假象**: 调试时一度以为按键硬件/引脚坏了(查了 IOE/CH442E MUX 一大圈),真相是 1Hz 日志采样错过了短按的下降沿。按键 KEYA=GPIO2=btnA, KEYB=GPIO1=btnB,**直接接 ESP32,接线本来就对**。

## 今天的 commit(都已 push)

- `22e7705` feat(M5-B): 语音输入端到端打通 — NUS key 命令 + Quartz 注入
- `e8356bd` feat: 补全按键映射 — 右键短按 Esc + 左键 Backspace/Return
- `6aeea75` feat: idle 页加像素宠物 — bufo idle GIF + cost 降角标 + sdkconfig patch 固化

## ⚠️ 明天第一件事: 验证 daemon Accessibility 授权是否持久

**这是今天唯一没收尾的事,可能影响语音功能。**

- 今天语音能用,但 Quartz 注入的 Accessibility 授权是临时借来的(可能借了某个已授权进程的权限)。
- daemon 进程真实可执行文件是 Homebrew Python:
  `/opt/homebrew/Cellar/python@3.14/3.14.5/.../Python.app/Contents/MacOS/Python`
- **风险**: launchd 重启 daemon 或 Mac 重启后,语音注入可能静默失效(未授权时 Quartz 不报错直接丢弃)。
- **明天验证步骤**:
  1. `launchctl kickstart -k gui/$(id -u)/com.claude-code.clawd-watch` 强制重启 daemon
  2. 等 10s, `curl -s http://127.0.0.1:9877/status` 确认 `connected:true`
  3. 光标进微信输入框, 长按设备右键说话, 看是否还能上屏
  4. 若失效: 去 系统设置→隐私与安全性→辅助功能, 手动加上面那个 Python 二进制路径并打勾。注意路径含版本号 3.14.5, Homebrew 升级 Python 后会失效。

## 可选的后续方向(非必须)

- **换成 Claude 本尊形象**: 当前宠物是 bufo 青蛙(Anthropic 官方 claude-desktop-buddy 的演示素材,不是 Claude 本尊)。Claude 没有现成宠物素材,只有橙色星芒 logo。想要的话得自己像素化一版星芒动画。
- **宠物状态联动**: 图集/素材支持 idle/busy/attention 等多状态, daemon heartbeat 已带 `tool`/`running` 字段。现在只做了单一 idle 动画。要联动只需多嵌几个 GIF + 根据 `_state.running` 切 `lv_gif_set_src`。
- **审批功能**: 早上的 replan 决定砍掉了"权限请求弹到表面 allow/deny",代码里现有审批逻辑保留未动。

## 烧机/构建备忘(新 session 必读)

- **源真身在 `firmware/app_claude/`**, build 用的是 git-ignored 的 `firmware/upstream/main/apps/app_claude/`。**改完源码必须跑 `firmware/apply_to_upstream.sh` 同步**,否则 build 是旧的。
- **新增 .c 文件后** CMake 的 GLOB_RECURSE 不会自动感知, 需 `touch main/CMakeLists.txt` 再 build。
- ESP-IDF 环境: `. ~/esp/esp-idf/export.sh` (当前 shell 默认没加载)。
- 设备串口: `/dev/cu.usbmodem1101`。烧录: `idf.py -p /dev/cu.usbmodem1101 flash`。
- daemon 由 launchd 托管(`com.claude-code.clawd-watch`), HTTP 端口 9877, 日志 `~/.claude/clawd-watch.log`。
- 读设备串口日志别用 miniterm `--raw`(会失败), 直接 pyserial 读。
