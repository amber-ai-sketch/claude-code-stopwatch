# 2026-05-29 Session 结束 — 重新规划

> Co-author: Claude Opus 4.7

## 当前状态

| Milestone | 状态 |
|---|---|
| M1 Mac 端 daemon + statusline + ccusage 共存 | ✅ 完整可用 |
| M2 圆 AMOLED LVGL idle 页 | ✅ 设备屏显 model+cost+arc |
| M3 按钮 FSM | ✅ host 单测 11/11 |
| M4 BLE NUS + 协议解析 | ✅ 设备实时显示 \$1.3 cost |
| M5 BLE HID(原方案) | ❌ 卡 macOS UI 配对环节,详见 docs/M5_STATUS.md |
| M5 B 方案: daemon 注入键盘 | 🟡 半成品,卡在 Accessibility 权限 + 设备端没改 |

## B 方案半成品状态

**已完成的 commit 没**(尚未 commit,工作区有未提交改动):

- `src/clawd_watch/keyinject.py`(新增)— Quartz CGEvent 键盘注入,提供 `key_tap` / `key_down` / `key_up` 三个函数
- `src/clawd_watch/daemon.py`(改) — `_on_ble_message` 加了 `cmd=key_tap/key_down/key_up` 分支
- `pyproject.toml` — 加了 `pyobjc-framework-Quartz>=10.0; sys_platform=='darwin'`
- `.venv` — 已经 reinstall 完,pyobjc 已装

**未做的事**:
- 设备端 `hid_dispatcher.cpp` 还在发 BLE HID notify,**没**改成发 NUS JSON 命令
- 设备端 `ble_hid` 还在 GATT 注册里(可以保留也可以下掉)
- Accessibility 授权: ghostty 已授权, daemon 二进制 (`~/Projects/claude-code-stopwatch/.venv/bin/clawd-watch-daemon`) **未授权** — 测试时 Quartz 注入静默失败
- 端到端没验证

## B 方案的剩余阻塞 — 你重新规划时要考虑的

1. **Accessibility 授权步骤是否可接受**: 用户首次安装必须手动给 daemon 二进制授权一次(在 System Settings → Privacy & Security → Accessibility)。这个步骤无法自动化。如果你接受,继续 B;如果觉得太突兀,想想方案 C/D。

2. **B 方案验证还没真正做过**: 我们没确认过 Quartz 注入能否正确触发微信输入法。即使授权对,**也可能微信输入法对程序模拟的 Shift+Space 不响应**(很多 IME 区分硬件键盘和软件注入)。这是 B 方案的最大未知。

3. **daemon 注入 vs HID 的真正差异**: 如果微信输入法接受 Quartz 注入,B 方案 100% 通;如果不接受,B 也死,得回去想方案 D(esp_hid 库重写 BLE HID)或者其它思路。

## 改动堆栈状态

`git status` 不太干净。如果新 session 想重置:

```bash
cd ~/Projects/claude-code-stopwatch
git status                          # 看 B 方案改动
git diff src/clawd_watch/daemon.py
git diff pyproject.toml
ls src/clawd_watch/keyinject.py     # 新增的
```

如果决定继续 B,改动保留即可。如果决定换方向:

```bash
git restore src/clawd_watch/daemon.py pyproject.toml
rm src/clawd_watch/keyinject.py
.venv/bin/pip install -e .          # 卸载 pyobjc
```

## 当前设备状态

- 烧的最新固件: M5 BLE HID + 双 service + random address + 主 adv = HID UUID + name + Appearance, scan rsp = NUS UUID
- 屏幕: idle 页, model="Opus 4.7", cost=最后一次接到的值, ctx 圆环约 50%
- daemon launchd 已 bootout, 设备空闲 advertising 中

## 重新规划时建议关注

按 office-hours review 的 Premise 排序:

- **P1**: 项目灵魂 = 长按按钮说话, Claude Code 出字 — 还没实现
- **P2**: Stopwatch 圆 AMOLED motion graphic — M2 占位 OK,M6 没做
- **P3**: 无线双 service — **M5 失败**,需要新方向
- 投资范围 2-4 周, 已用约 1 天

候选方向(按改动量从小到大):

- **B 完成**: 给 daemon 授权 + 改设备端发 NUS key cmd(剩 1-2 小时)
- **D 重写 BLE HID**: 用 ESP-IDF 的 `esp_hid_gap` 高层 API 替手写 NimBLE(2-3 小时)
- **C USB HID**: 违反 P3 但 100% 兼容(1 小时)
- **重新设计**: 也许语音输入这个交互本身有更好的形式

下次 session 见。

---

## Plan 文件还在
`~/.claude/plans/eventual-weaving-meteor.md` — 项目源真本
`~/Projects/claude-code-stopwatch/docs/M5_STATUS.md` — M5 原方案中断点详情
