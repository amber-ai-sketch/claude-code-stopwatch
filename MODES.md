# 两种模式规划

踩坑教训催生的方向重定：把「能立刻当生产力工具用」和「啃 mic 无线链路硬骨头」拆开。
先拿到能用的（模式一），再慢慢演进硬的（模式二）。

## 背景：为什么拆

stopwatch mic 这条路接连踩坑——daemon 连错设备、烧录误判、BLE 背压、录音盲区——
说明把两件事绑死是错的。模式一几乎全是现成基建，今天就能用；模式二是真正的研究性硬骨头。

## 模式一：stopwatch 当遥控器（trigger-only）

stopwatch **不录音**，只当一个无线按钮。按键 → BLE 发 NUS 命令 → Mac 端 Quartz 注入快捷键
→ 触发 **Mac 自己的麦克风 + 微信输入法听写**。录音和转写全在 Mac，stopwatch 只是 trigger。

### 数据流
```
[右键长按] → 固件发 NUS {"cmd":"key_down","mod":"shift","key":"space"}
           → daemon keyinject.py Quartz 注入真实 Shift+Space（按住）
           → 微信输入法在前台开始听写
[松开]     → 固件发 {"cmd":"key_up",...} → 停止听写 → 微信把文字落到光标处
```

### 现状：基建 100% 现成
- ✅ `keyinject.py`：Quartz 注入，含微信必需的**真实左 Shift keycode 0x38**（记忆里那个坑早解决）
- ✅ `daemon.py`：已处理 `key_tap`/`key_down`/`key_up` 三个 NUS 命令
- ✅ 历史 commit `22e7705` 已端到端打通过这条路
- ⚠️ **唯一回退点**：本次为模式二把右键长按从「Shift+Space 喂微信」改成了「驱动 mic 录音」
  （`hid_dispatcher.cpp:41-63`）。回退这一处即可。

### 工作量：小。回退固件 1 处 + 实测微信听写能跳出。

## 模式二：stopwatch mic 真无线录音

stopwatch 板载 MEMS 麦录音 → 传到 Mac → STT 转文字。两个待定轴**已拍板**，
详细落地方案见 [docs/MODE2_PLAN.md](docs/MODE2_PLAN.md)。

### 轴 A：传输层 → **选定 WiFi TCP 推流**
- **WiFi（选定）**：家里 WiFi 吞吐足、盲区可忽略，ESP32 推流到 Mac，绕开蓝牙同步模型的录音盲区。
  代价：依赖局域网、要搭 socket 推流。
- ~~蓝牙~~：已验证能传通（0 丢包 / 120kbps / MTU 255），但同步「录一块→发一块」有 15% 录音盲区，
  根治要 I2S DMA 后台采集（真硬骨头）。**暂不走**，相关代码（`audio_recorder` / `audio_frame` / audio characteristic）保留备用。

### 轴 B：STT 后端 → **选定小米 MiMo 多模态（凑合）**
- **MiMo（选定）**：`mimo-v2.5`，走 token plan 网关 `token-plan-cn.xiaomimimo.com`，`api-key` 头，
  整段 WAV base64 上传。它是多模态 LLM 不是 ASR，靠 `<t></t>` 标记 prompt 逼稳定输出（已实测）。不挑采样率（免重采样）。
- ~~专用 `mimo-v2.5-asr`~~：质量最对口但**只开源无托管 API**，要本地部署，等于端侧方案，暂不走。
- ~~端侧中文模型 / 火山讯飞云 ASR~~：备选，MiMo 改写严重时再换。详见 [docs/MODE2_PLAN.md](docs/MODE2_PLAN.md)。

### 转写后：Unicode 直接注入（不碰微信输入法）
拿到文字后用 `CGEventKeyboardSetUnicodeString` 注入 Claude Code 输入框，绕开模式一所有「键盘注入打断微信 IME」的坑。

### 工作量：中。WiFi 链路 + STT 接入是新代码，但 PCM/WAV/transcript 回显基建已就位。分 4 阶段见 MODE2_PLAN.md。

## 模式切换 UI

两模式在固件按钮层会打架（右键长按只能一个意思），需要切换机制。两个方向：

### 方向 1：Mac 端菜单栏 App 切（推荐）
现成 `menubar.py` 已有完整骨架（连接状态/扫描/重连/忘记/重启/日志）。
加一个「模式」子菜单即可：
```
模式 ▸  ● 遥控器（微信听写）
        ○ 录音（stopwatch mic）
```
- ✅ 基建现成，加 ~20 行 rumps MenuItem
- ✅ 改一处就生效（daemon 持有当前模式，决定怎么解释按键事件）
- ✅ 用户不用碰 stopwatch，鼠标点一下
- 关键设计：**模式状态存在 daemon 端**。固件永远只上报「右键长按了/松开了」这类**原始按键事件**，
  由 daemon 按当前模式解释——模式一→注入微信快捷键；模式二→请求 mic 录音。
  固件不需要知道模式，切换零固件改动。

### 方向 2：stopwatch 上做设置页
另一 session 已加了 `overview_page`/`session_page`/`watch_face` 等页面。可加设置页选模式。
- ✅ 不依赖 Mac，表上自洽
- ❌ 小屏交互繁琐；且模式本质是「Mac 怎么解释按键」，状态放表上还要同步回 Mac，更绕

### 建议
**方向 1（Mac 菜单栏切 + 模式状态在 daemon）**。固件保持「只报原始按键事件」的哑终端角色，
模式纯粹是 daemon 的解释策略。这样切换零固件改动，也避免固件按钮语义打架。

## 落地顺序
1. **先写本规划**（本文档）✅
2. **落地模式一**：
   - daemon 持有 `mode` 状态（默认 trigger-only）
   - 固件右键长按改回「上报原始按键事件」（或直接发微信快捷键命令，看模式状态放哪更简）
   - 菜单栏加模式切换子菜单
   - 实测微信听写跳出
3. **模式二往后**：先评估 WiFi 传输可行性（大概率比蓝牙 DMA 重构更干净）
