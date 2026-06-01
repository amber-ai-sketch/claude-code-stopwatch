# TODOS

## seq 2 字节绕回边界（v2 长录音前必须处理）

- **What**: 音频帧 `seq` 用 2 字节（max 65536）。`audio_frame.h` / `audio_receiver.py` 共享此宽度。
- **Why**: 阶段一单次录音封顶 30s（@40ms 块 ≈ 750 帧），远不到绕回，不触发。但 v2 取消 30s 上限做长录音时，seq 绕回会让 daemon `reassemble()` 按 seq 排序错乱（旧帧 seq 与新帧重叠），表现为漏字/错位。
- **Pros**: 修掉后长录音不会静默错乱。
- **Cons**: 极低成本（几行），但当前不触发，提前做违背 YAGNI。
- **Context**: 决策来自 `/plan-eng-review` 的 outside voice 评审。阶段一刻意保持 seq 2 字节以省 MTU 紧张时的帧头开销。
- **Depends on**: v2 取消 30s 录音上限的决策。
- **Start**: 要么 seq 扩到 4 字节（`audio_frame_pack` + `parse_frame` 同步改 `<BHI`→`<BII` 之类），要么 `reassemble()` 加序号取模回绕检测。

## 录音结束后把上屏文字回显到手表屏幕

- **What**: 模式一语音录入结束后，把微信听写实际上屏的文字发回手表，显示在表盘上，让用户不用看电脑就能确认听写内容对不对。
- **Why**: 当前确认听写结果必须扭头看电脑屏幕，手表当遥控器用时这一步很别扭。
- **关键约束（已实测，别再走这条死路）**: daemon 不知道微信打了什么字——转写在微信输入法内部完成，daemon 只按了 Shift+Space。最干净的"读焦点输入框文本（Accessibility API）"方案在用户环境下**走不通**：用户用 Ghostty 终端（GPU 自绘），`AXUIElementCopyAttributeValue(focused)` 返回 `-25204 kAXErrorNoValue`，读不到输入框文本。这不是权限问题（权限已授权），是 Ghostty 架构决定的，Alacritty/Kitty 同类。
- **选定方案**: **剪贴板中转**。松手后 daemon 模拟 `Cmd+A` 全选 + `Cmd+C` 复制，从剪贴板读文字发手表，再恢复剪贴板原内容。不依赖终端是否暴露 AX，Ghostty 也能用。
- **Cons / 已知代价**:
  - 会瞬间改变输入框选中态 + 覆盖剪贴板（用完恢复，但有竞态窗口）。
  - 读到的是输入框**全部**内容，不只是新听写那句（除非能定位增量）。
  - 466px 圆屏放不下长句，需截断或滚动显示。
- **已实测失败（2026-06-01）**:
  - 方案 A：500ms 延迟后 `Cmd+A` + `Cmd+C`。结果：`Cmd+A` 选中了终端整个 scrollback（30K 字符），不是听写文字。
  - 方案 B：改用 `Cmd+Shift+Left`（选当前行）替代 `Cmd+A`。结果：键盘注入打断了微信语音输入过程，导致听写被中断。即使延迟 500ms，额外的按键事件仍会干扰 WeChat 的转写流程。
  - **根因推测**：`CGEventPost` 注入的按键事件和 WeChat IME 的内部状态有冲突。WeChat 可能还在处理 Shift+Space 的释放，此时注入 Cmd 系列按键会破坏其状态机。
  - **结论**：在 button up 之后注入任何键盘事件都有风险干扰 WeChat。纯键盘注入方案走不通，需要找完全不碰键盘的读取方式。
- **Depends on**: 找到不干扰 WeChat 的文本读取方案。
- **备选方向**: (1) Accessibility API 读终端文本（已知 Ghostty 不支持）；(2) 直接读 Ghostty 的 GPU buffer（需要 Ghostty 自己暴露接口）；(3) 换用支持 AX 的终端（iTerm2/Terminal.app）；(4) 不做回显，改为在手表上显示"录音完成"确认。

## 手表在 Claude 等待用户输入时刷成"等待"态

- **What**: Claude 用 AskUserQuestion 弹选项让用户选时，手表应刷成"等待你输入"态，而不是干等无反馈。
- **Why（已查清根因）**: 手表的 attention/waiting 态当前**只由工具权限请求 `PreToolUse` hook 驱动**（daemon 的 `waiting = len(self.pending)`，pending 只在 `_on_pre_tool` 填充）。而 AskUserQuestion 弹选项**不触发任何已装 hook**，daemon 完全感知不到，所以手表不刷。这是架构盲区，非 bug。
- **线索（已查官方文档）**: Claude Code 有 `Notification` hook，matcher 支持 `idle_prompt`（"waiting and needs attention"）/`permission_prompt` 等类型。理论上 `idle_prompt` 能让 daemon 感知"Claude 在等你"。当前 6 个 hook（PreToolUse/PostToolUse/Stop/SessionStart/SessionEnd/UserPromptSubmit）里**没装 Notification**。
- **未验证（做之前先实测）**: AskUserQuestion 到底会不会触发 `idle_prompt`、什么时机触发（很可能要空闲超时几秒才发，不是弹出即发——若如此体验会延迟）。先装个只打日志的 Notification hook 实测信号是否存在、时机如何，再决定接不接手表。
- **Depends on**: 实测确认 Notification/idle_prompt 行为。
- **Start**: settings.json 加 `Notification` hook（matcher `idle_prompt`）→ 复用现有 `clawd-watch-hook` 加个事件类型 → daemon 加一类"waiting for input"状态（区别于 permission 的 attention）→ 经 heartbeat 下发 → 固件加显示。
