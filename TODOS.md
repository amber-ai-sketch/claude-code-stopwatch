# TODOS

## ✅ 已完成

- **按键去抖职责重复** — 删除 ButtonFsm 内的重复去抖（30ms），左键去抖统一由 Button_Class（10ms）负责。右键不受影响（本就不走 FSM）。
- **NumberFlow 滚动动画** — session 页 token 四宫格改用 NumberFlow（spring 物理逐位滚动），cost 用 NumberFlowFloat（$前缀 + 2 位小数动画）。footer 拆分为 cost + tool 两行。设计稿已同步。
- **Celebrate 换 spring 缓动** — 庆祝跳跃改用 smooth_ui_toolkit::Spring（500ms/0.5 bounce），替换 lv_anim_path_ease_in_out。呼吸/招手不受影响。

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

## 按键去抖职责重复（M5IOE1/M5PM1 源码对照后发现）

- **What**: 去抖逻辑写了两处且分布不均。`button_fsm.cpp:15-24` 有完整去抖（`kDebounceMs`），但 `hid_dispatcher.cpp:40` 右键**根本不走 FSM**（直读 raw level），左键走 FSM 但喂进去的 `btnA.isPressed()` 已是 upstream `Button_Class` 去过抖的值——等于**左键去抖两遍、右键零去抖**。
- **Why**: 正中记忆 [[clawd-watch-button-timing-not-hardware]]。`updateButtonStates()` 在 `_key_manager->update()` 内刷新（`app_claude.cpp:86`），`_hid.tick()` 在其后读 `isPressed()`，某帧 KeyManager 没刷新右键就会错过下降沿。
- **确认（读 HAL 源码）**: 按钮走 **ESP32 原生 GPIO 1/2 + 上拉**（`hal_button.cpp:16-29`），**不是** M5IOE1 扩展口——原生 GPIO 读取无 I2C 延迟，这条路是对的，别改接 M5IOE1。
- **Start**: 确认 `Button_Class` 去抖够用后，删 `button_fsm.cpp:15-24` 重复去抖，让 FSM 只管语义（single/double/long）、去抖归 `Button_Class`；右键补一套轻量边沿检测或也接 `Button_Class.wasPressed/wasReleased`。
- **Cons**: 当前能用，属健壮性优化非 bug。

## 充电状态无迟滞，可能导致息屏闪烁

- **What**: `isBatteryCharging(strict=false)` 只判 `vin_mv > 4000`（`hal_pmic.cpp:160-166`），VIN 在 4000mV 附近会反复跳变。该值在 `app_claude.cpp:114` 驱动「充电时永不息屏」，抖动会让息屏态闪烁。
- **Start**: charging 加迟滞（如 >4100 才算插入、<3900 才算拔出），或复用电池电量那套 7:1 滤波。
- **Cons**: 改的是 upstream HAL，要进 patch；没遇到充电屏闪就先不动（YAGNI）。

## 电池百分比线性映射不贴合锂电曲线

- **What**: `battery_millivolts_to_percent()` 用 3300–4200mV **线性**插值（`hal_pmic.cpp:19-34`）。锂电压-电量非线性，线性映射会「高电量掉得慢、低电量掉得快」，新做的电池 gauge 若百分比跳得不自然根因在此。
- **Start**: 换分段折线 LUT（如 4.2V=100 / 3.9V=75 / 3.7V=50 / 3.5V=20 / 3.3V=0 线性插值）。
- **Cons**: 同样要进 upstream patch；够用就别动（YAGNI）。

## 会话页 token/cost 数字换 NumberFlow 滚动动画（P1，对照 smooth_ui_toolkit 发现）

- **What**: `session_page` 的 token 进出量 / cost / context% 当前用 `lv_label_set_text` 直接刷，数字跳变是硬切。换成 `smooth_ui_toolkit::lvgl_cpp::NumberFlow`（`components/smooth_ui_toolkit/src/lvgl/number_flow/number_flow.hpp`）做逐位里程表式滚动 + spring 物理。
- **Why**: 该库是 Forairaaaaa 专门给 M5 表盘做的（参考 number-flow.barvian.me），token 这种频繁变动的数字滚动最抓眼；常变数字最值得上，静态标题/项目名不需要。
- **接入成本低**: `app_claude.cpp:13` 已 include `smooth_lvgl.hpp`，C++ 封装层编译链路已通。
- **Start**: `setPrefix("$")` / `setSuffix("k")` / `setValue(int)`，每帧 `update()`；`animationType` 默认 Spring。先读 `session_page.cpp` 确认数字怎么刷的再接。
- **Cons**: NumberFlow 只吃 int，cost 浮点要先定标（如 ×100 显示分）或看 `number_flow_float.hpp`。

## Celebrate 庆祝跳跃换 spring 缓动（P2）

- **What**: clawd 庆祝跳跃（`clawd_pet.cpp:279-290`）现用 `lv_anim_path_ease_in_out`，换成官方 `AnimateValue` + spring generator（`src/core/animation/generators/spring/`）做带回弹的弹性跳。
- **Why**: Celebrate 是一次性、要张力的动作，spring 比 ease_in_out 更「蹦」、更有生命力。
- **关键约束**: **只换 Celebrate**。呼吸 / 招手是「平静」基调（代码注释「peace, not urgency」），ease_in_out 更对，**别动**。
- **Cons**: 局部改动，风险低。

## 翻页加滑动过渡动画（P3，非刚需）

- **What**: `_show_page()`（`watch_face.cpp:359`）直接 `lv_obj_set_pos` 瞬移页面，翻页无滑动。给 `next_page` 加 `AnimateValue` 驱动的 x 位移补间（~200ms 缓动横移）。
- **Why**: 圆屏滑动翻页带缓动会高级很多。
- **Cons / 排序**: 体验提升明显但非刚需，且要处理动画期间触摸重入。刚做完触摸手势重构（commit 56de27f），**排在 NumberFlow 之后再做**，避免重入风险叠加。
