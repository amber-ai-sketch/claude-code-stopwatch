# 模式二落地规划 — WiFi 推流 + 云端中文 STT

> Co-author: Claude Opus 4.8
> 定稿日期: 2026-06-01

模式二让 stopwatch 用**自己的麦克风**录音，转成文字注入 Claude Code 输入框。
本文是 [MODES.md](../MODES.md) 模式二章节的详细落地方案，两个待定轴已拍板：

- **传输层 = WiFi TCP 推流**（不走蓝牙，绕开同步采集盲区）
- **STT 后端 = 小米 MiMo 多模态音频理解**（`mimo-v2.5`，OpenAI 兼容 REST，整段上传）
- 转写文字 = Quartz `CGEventKeyboardSetUnicodeString` 直接注入 + 回显手表（不碰微信输入法）

## 为什么是这套组合

| 决策 | 理由 |
|---|---|
| WiFi 不走 BLE | 蓝牙同步模型有 15% 录音盲区（发送时麦没在录），根治要固件 DMA 双任务重构——真硬骨头。WiFi 带宽足，发送快到盲区可忽略，绕开重构。 |
| MiMo 不走端侧 | 云端最准、零本地算力、不用下模型。MiMo 按音频时长计费（≈6.25 token/秒），30s 录音 ≈ 188 token，极便宜。代价（网络 / key / 隐私）可接受。 |
| Unicode 注入不碰微信 | 模式一所有坑（键盘注入打断微信 IME 状态机）都因为依赖微信听写。模式二直接拿到文字串，用 `CGEventKeyboardSetUnicodeString` 注入任意中文，不依赖 keycode / IME，从根上避开。 |

### MiMo 不是专用 STT，是多模态 LLM 的「音频理解」——这点决定实现方式

MiMo 走 OpenAI 兼容的 `chat/completions`，本质是「听懂音频再回答」，不是 ASR 引擎。**专用的 `mimo-v2.5-asr` 只开源权重、无托管 API**（要本地部署，等于端侧方案），故先用多模态 API 凑合。以下全部已实测（2026-06-01）：

1. **endpoint 走 token plan 网关**：用的是 token plan（key 前缀 `tp-`），base URL = `https://token-plan-cn.xiaomimimo.com/v1`，**不是**官方 `api.xiaomimimo.com`（打官方域名一律 401）。
2. **鉴权头是 `api-key:`，不是 `Authorization: Bearer`**。实测同一请求体：`Bearer` 头路由到的配置会**死循环推理**（1024 token 烧光、content 空、拿不到结果）；`api-key` 头才正常出 content。
3. **靠 `<t></t>` 标记 prompt 逼稳定输出**。MiMo 是推理模型不是 ASR，结果字段不稳（content 时空、reasoning 里夹带）。**已验证有效的做法**：prompt 要求把结果包进 `<t></t>`，代码从 `content + reasoning_content` 拼起来 grep `<t>[^<]*</t>` 取最后一个。实测 content 干净输出 `<t>现在开始录音</t>`、不改写、completion 降到 317 token。**prompt 不要用 system role**（反而触发深推理），全塞 user message。
4. **不需要重采样**。MiMo 按音频时长计 token、不挑采样率，固件推什么 Mac 就原样包 WAV → base64 → POST，**省掉重采样和 soxr 依赖**。本地 WAV 走 base64 内联（50MB 上限，30s 录音绰绰有余）。
5. **已知残留风险**：复杂口语指令（带停顿/口误）会不会被改写成书面语，未实测，**接受为已知风险**。真用起来若发现改写严重，再换专用 ASR（火山/讯飞）或本地跑 mimo-v2.5-asr。

## 现状盘点：缺的只有「WiFi 链路 + STT」两环

| 环节 | 状态 | 位置 |
|---|---|---|
| 固件录音 `AudioRecorder` | ✅ 写好但**未集成**（没人实例化，主循环没调用） | `firmware/app_claude/audio_recorder.{h,cpp}` |
| Mac 重组/排序/去重 | ✅ 完整，可直接复用 | `src/clawd_watch/audio_receiver.py` |
| 转写回显手表协议 | ✅ `{"cmd":"transcript","text":...}` + overlay UI 就绪 | `firmware/.../protocol_parse.cpp`, `watch_face.cpp` |
| Unicode 注入工具 | ⚠️ clipboard 辅助已备，缺 `type_unicode()` 主函数 | `src/clawd_watch/keyinject.py` |
| daemon mic 模式 | ⚠️ 是 stub：`log.warning("not implemented")` | `daemon.py:142-144` |
| **WiFi 推流（固件）** | ❌ 零代码 | 新增 |
| **WiFi 接收（Mac）** | ❌ 零代码 | 新增 |
| **STT 调用** | ❌ 零代码，仅注释占位 | 新增 |

结论：BLE 那套接收/重组代码**不直接复用于 WiFi**（传输边界不同），但 PCM 格式、WAV 落盘、transcript 回显协议全部沿用。

## 端到端数据流

```
[右键长按 down]
  固件: 发 NUS {"cmd":"btn","key":"right","edge":"down"}
  daemon: mode==mic → 发 NUS {"cmd":"mic","action":"start"} 回固件
  固件: 连 WiFi(已连则跳过) → TCP connect daemon:9878 → I2S 开录 → 边录边推 PCM

[说话中]
  固件: I2S 采 44100/16bit/mono，整块 TCP 推流（不抽取、不本地缓存）
  daemon: asyncio TCP server 累积 PCM bytes

[右键长按 up]
  固件: 发 NUS {"cmd":"btn","key":"right","edge":"up"}
  daemon: 发 {"cmd":"mic","action":"stop"} → 固件停录、关 TCP
  daemon: PCM 包成 WAV → base64 → POST MiMo chat/completions → 取 reasoning_content
        → CGEventKeyboardSetUnicodeString 注入前台 Claude Code
        → 发 NUS {"cmd":"transcript","text":...} 回显手表
```

关键设计点：

1. **录音起停仍由按钮驱动，但 daemon 下发 mic 命令给固件**。模式一固件长按直接被 daemon 解释成微信快捷键；模式二 daemon 改成回发 `mic start/stop` 命令。固件只在收到命令后才动麦——保持哑终端：固件不知道"模式"，只听命令。

2. **固件去掉抽取，直推 44100 全量**。当前 8820Hz 是为 BLE 省带宽硬抽的。WiFi 带宽够（44100×16bit×mono ≈ 705kbps，WiFi 轻松），固件回归最纯净：I2S 原生采、原样推。**Mac 端不重采样**——MiMo 不挑采样率，PCM 直接按 44100 包成 WAV 上传。固件少一段抽取代码，Mac 少一个 soxr 依赖，两头都更简。

3. **WiFi 只传音频，控制仍走 BLE NUS**。按钮事件、mic 起停命令、transcript 回显都走已通的 NUS。WiFi 是单向音频管道，断了不影响控制链路。这样 WiFi 失败可优雅降级（见风险章）。

## 分阶段落地

### 阶段 0：固件 WiFi 连通性验证（先证可行，再写链路）

- 固件加最小 WiFi STA 连接（SSID/密码暂时硬编码进 `app_claude`，验证期够用）。
- 连上后向一个固定 IP:port 发一行 "hello"，Mac 端 `nc -l 9878` 收到即通过。
- **验收**：Mac 终端看到固件发的 hello。不通则先解决 WiFi（信号/路由隔离/mDNS），不往下走。

### 阶段 1：WiFi 音频推流打通（不接 STT，先存 WAV）

固件端：

- 新增 `firmware/app_claude/net/wifi_audio.{h,cpp}`：WiFi STA + TCP client + I2S 推流。
- `app_claude.cpp` 主循环集成：收到 NUS `mic start` → 开 I2S + 连 TCP + 推流；`mic stop` → 停。
- I2S 配 44100/16bit/mono，按块（如 1024 样本）`send()` 到 TCP。

Mac 端：

- 新增 `src/clawd_watch/wifi_audio_server.py`：asyncio `start_server` 监听 `127.0.0.1:9878`
  （局域网则 `0.0.0.0:9878`），一个连接 = 一次录音，累积 bytes 到 buffer，连接关闭即一段录音完成。
- daemon 集成：`mic start` 时备好 server 接收，`mic stop` 后把这段 PCM 重采样 + 落 WAV 到
  `~/.claude/clawd-watch-audio/wifi-YYYYMMDD-HHMMSS.wav`（复用 `audio_debug.write_debug_wav`）。
- **验收**：长按录一句中文，WAV 文件能正常播放、听清、无盲区滴滴声。

### 阶段 2：接 MiMo 音频理解，出文字

- 新增 `src/clawd_watch/stt.py`：单函数 `transcribe(wav_bytes: bytes) -> str`，
  base64 内联 + POST MiMo token plan 网关，从 content+reasoning 抽 `<t>...</t>`（prompt 和抽取逻辑已实测定稿，见下文「MiMo STT 接入设计」）。
- daemon `mic stop` 流程：PCM 包 WAV（复用 `audio_debug` 的 wave 写法，写到内存 `BytesIO`）→ `transcribe()` → 拿文字。
- **验收**：长按说中文，daemon 日志打出转写文字，内容大致正确、无多余解释。

### 阶段 3：文字注入 + 回显闭环

- `keyinject.py` 加 `type_unicode(text: str)`：用 `CGEventKeyboardSetUnicodeString` 把整串中文
  注入前台 App（不依赖 keycode，绕开 IME）。
- daemon：转写后 `type_unicode(text)` 注入 Claude Code 输入框 + 发 `{"cmd":"transcript","text":text}` 回显手表。
- **验收**：长按说话 → 松手 → 文字出现在 Claude Code 输入框 + 同步显示在表盘 overlay。

## 文件改动清单

### 新增

| 文件 | 职责 |
|---|---|
| `firmware/app_claude/net/wifi_audio.h/.cpp` | WiFi STA + TCP client + I2S 推流 |
| `src/clawd_watch/wifi_audio_server.py` | asyncio TCP server，按连接收一段录音 |
| `src/clawd_watch/stt.py` | `transcribe(wav_bytes) -> str`，base64 + POST MiMo，从 content+reasoning grep `<t>` |

### 修改

| 文件 | 改动 |
|---|---|
| `firmware/app_claude/app_claude.cpp` | 主循环响应 NUS `mic start/stop`，驱动 wifi_audio |
| `firmware/.../protocol_parse.cpp` | 解析 `{"cmd":"mic","action":...}` 命令 |
| `daemon.py` | `_handle_button` 的 mic 分支：下发 mic 命令；新增 mic stop 后的 STT→注入→回显流程 |
| `keyinject.py` | 加 `type_unicode()` |
| `__init__.py` | 加 `WIFI_AUDIO_PORT = 9878` + `MIMO_ENDPOINT` 常量 |

### 不动

- `audio_recorder.{h,cpp}`、`audio_frame.h`、`ble_nus` 的 audio characteristic、`audio_receiver.py`：
  都是 BLE 音频链路的，模式二走 WiFi 不碰它们。**暂时保留**（BLE 链路是已验证资产，未来若回头做 BLE DMA 方案还要用），不删。

## MiMo STT 接入设计（已实测验证）

`stt.py` 暴露一个函数，失败抛异常（含响应体），不静默吞：

```python
def transcribe(wav_bytes: bytes) -> str:
    """整段 WAV → 转写文字。POST MiMo，从 content+reasoning 抽 <t>...</t>。"""
```

### 接口事实（全部实测确认）

- **endpoint**：`https://token-plan-cn.xiaomimimo.com/v1/chat/completions`（token plan 网关，**非**官方域名）
- **鉴权 header**：`api-key: $MIMO_API_KEY`（**不是** `Authorization: Bearer`——Bearer 会触发死循环推理）
- **model**：`mimo-v2.5`
- **音频传入**：`messages` 里的 `input_audio` content part，base64 内联：
  `"data": "data:audio/wav;base64,<BASE64>"`（本地 WAV 无公网 URL，必走 base64；上限 50MB）
- **转写结果**：从 `content + reasoning_content` 拼起来 grep `<t>[^<]*</t>` 取最后一个（靠 prompt 标记，见下）
- **格式/采样率**：收 WAV，不挑采样率，44100 直接传
- **计费**：≈6.25 token/秒，30s ≈ 188 token，极便宜

### 请求骨架（实测能稳定出 `<t>` 标记的版本）

```python
# 关键：prompt 全塞 user，不要用 system role（system 会触发深推理）
PROMPT = (
    "你是语音转写引擎。把音频里的中文逐字转写，原样输出说话人说的话，"
    "不要改写成书面语、不要补全、不要解释、不要纠错。如果一句话重复多次，"
    "也照实重复输出。最终把转写结果放进 <t></t> 标记里，例如 <t>你好世界</t>。"
    "标记外不要写任何分析过程。"
)
messages = [
    {"role": "user", "content": [
        {"type": "input_audio",
         "input_audio": {"data": f"data:audio/wav;base64,{b64}"}},
        {"type": "text", "text": PROMPT},
    ]},
]
# POST {model: "mimo-v2.5", messages, max_completion_tokens: 1024}
# 用 aiohttp（daemon 已依赖），不引 openai SDK——一个 POST 不值得多一个依赖

# 抽取：拼 content+reasoning，正则取最后一个 <t>...</t>
import re
blob = (msg.get("content") or "") + (msg.get("reasoning_content") or "")
m = re.findall(r"<t>(.*?)</t>", blob, re.S)
text = m[-1].strip() if m else ""   # 抽不到 → 空串，daemon 据此跳过注入
```

### 鉴权走环境变量（不写进代码、不进 git）

```
MIMO_API_KEY=...    # token plan key，前缀 tp-
```

daemon 启动时读环境变量；缺失则 fail fast（明确报"未设置 MIMO_API_KEY"），不静默降级。

### 残留风险（接受为已知）

复杂口语指令（带停顿/口误）会不会被改写成书面语，未实测。简单句（"现在开始录音"）已验证不改写。
真用起来若发现改写严重，再换专用 ASR（火山/讯飞）或本地跑开源 `mimo-v2.5-asr`。

## 验收点汇总

1. 阶段0：Mac `nc` 收到固件 WiFi hello。
2. 阶段1：录一句中文，WAV 能听清、无盲区滴滴声（这是 WiFi 相对 BLE 的核心收益，必须实测确认）。
3. 阶段2：daemon 日志打出大致正确的转写文字（无多余解释/幻觉）。
4. 阶段3：松手后文字落进 Claude Code 输入框 + 表盘 overlay 同步显示。

## 风险与坑

- **WiFi 连接延迟**：ESP32 冷启 WiFi 关联可能数秒，长按一开始就连会让首句丢头。
  对策：进 mic 模式时 daemon 提前下发 `mic start` 预连，或固件常驻 WiFi（耗电换体验，按需）。
- **局域网隔离**：很多家用路由开了 AP 隔离 / 访客网络，ESP32 连不上 Mac。
  阶段0 必须先验证同网可达，不通别往下做。
- **Mac IP 漂移**：固件硬编码 Mac IP 不稳。首版可硬编码验证，后续用 mDNS（`_clawdwatch._tcp`）让固件发现 daemon。
- **隐私**：音频上云。文档/README 需明确告知；公司内网环境优先考虑内网网关 STT 端点。
- **录音空档已被 WiFi 绕开但未根治**：固件仍是"采一块推一块"，只是 WiFi 推得快到空档可忽略。
  若实测仍有空档，才需要回头做 I2S DMA 双任务（见 MODES.md 轴 A）。**先实测再决定，别提前重构（YAGNI）**。
- **30s 上限**：`audio_recorder.h` 的 30s cap 是 BLE 链路的；WiFi 链路新写的推流要不要上限单独定。
  长录音还会触发 seq 绕回（仅 BLE 链路），见 [TODOS.md](../TODOS.md)，WiFi 链路无 seq 概念不受影响。

## 落地顺序

1. 本规划定稿 ✅
2. 阶段 0 WiFi 连通性（不通则止）
3. 阶段 1 推流 + WAV（验证无盲区，这是换 WiFi 的核心赌注）
4. 阶段 2 接 MiMo + 调 prompt 到逐字转写
5. 阶段 3 注入 + 回显闭环
6. 收尾：mDNS 发现、隐私说明
