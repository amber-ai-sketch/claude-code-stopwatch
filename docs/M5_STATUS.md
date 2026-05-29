# M5 BLE HID 状态(2026-05-29 中断点)

> Co-author: Claude Opus 4.7
> 用户决定停下来重新想方案,这份是给新 session 的完整交接文档

## TL;DR

**M1-M4 全通,M5 卡在 macOS 配对环节**。
设备硬件 / 固件 / BLE 协议栈 / HID Service 全部就绪,**唯独 macOS Bluetooth UI 看到设备但点 Connect 没反应**。
nRF Connect 等手机 BLE 工具能扫到设备(证明设备 OK)。

## 当前已确定通过的环节

| 环节 | 状态 |
|---|---|
| Mac 端 daemon + statusline + hook + ccusage 共存 | ✅ M1 |
| 圆 AMOLED LVGL idle 页(model+cost+arc) | ✅ M2 |
| 按钮 FSM 单测 11/11 | ✅ M3 |
| ESP-IDF v5.5 + NimBLE + NUS 协议 + 配对 | ✅ M4 — 设备屏显 \$1.3 实时 cost |
| HID GATT Service + Input Report char | ✅ 注册通过 + Mac subscribed=1 |
| 按钮 → HID 键码 dispatcher | ✅ 长按右键日志显示 `Shift+Space press (rc=0)` |
| **macOS Bluetooth UI 配对完成 + 文字到 TextEdit** | ❌ **卡这里** |

## M5 失败的具体表现

### 路径 1:daemon 主动连(M4 的方式)
- daemon 通过 CoreBluetooth 直接扫描 + 连接(没经过 Mac UI 配对)
- ✅ NUS 通信正常,heartbeat / cost 推到设备屏幕
- ✅ 设备 log 显示 `connected`、`MTU=255`、`TX subscribed=1`、`HID input report subscribed=1`
- ✅ 按右键长按 → 设备 `ble_hid_press(Shift+Space) rc=0`(成功投递 GATT notify)
- ❌ **TextEdit 输入框没反应** — macOS 对未经用户 UI 配对的设备的 HID 输入直接丢弃

### 路径 2:让 Mac UI 配对(尝试解 macOS 的 HID 信任)
- 停了 daemon → 设备空闲 advertising
- 主 advertising 包加了 HID UUID 0x1812 + Appearance=0x03C1(Keyboard)
- 设备主动随机 BLE 地址
- 设备主动 `ble_gap_security_initiate` 在 connect 后
- ✅ Mac 设置面板"附近设备"列表能看到 ClawdWatch
- ❌ **点 Connect 没反应** — 设备串口 30s 内 0 个 connect 事件(macOS UI 的请求根本没发到设备)

## 关键调试事实

```
nRF Connect (手机 BLE 工具) 能扫到 ClawdWatch  RSSI=-47  uuids=[NUS_UUID]
↑ 证明设备 advertising 正常
```

```
macOS 蓝牙设置 → Nearby Devices: ClawdWatch 出现
但点 Connect 时设备串口监控 30 秒 0 个事件
↑ macOS UI 的 connect 请求黑洞,没发到 BLE 链路
```

## 已经尝试过、没解决问题的事

- ✅ `sudo pkill bluetoothd`(重启蓝牙服务) — 没用
- ✅ Bluetooth 关→开 — 没用
- ✅ 设备改用 `BLE_OWN_ADDR_RANDOM`(每次启动随机地址) — 没用
- ✅ 主 adv 包从"NUS+HID 共存"改成"只 HID,NUS 移到 scan response" — 没用
- ✅ 加 Appearance=0x03C1 + 主动 security_initiate — 没用

## 可能的根因(候选,未验证)

1. **HID Report Map 有坑**:8 字节 boot keyboard descriptor,我用的是标准模板,但 macOS 可能要求 Report Protocol 的特定字段(参考 NullYing/ESP32S3-BLE-Keyboard-Mouse 的处理)
2. **没启用 `ble_store_config_init`**:bonding 数据没持久化,macOS 期望的 SMP 流程走不通
3. **macOS 对纯 BLE HID 设备(没 BR/EDR fallback)的某种隐性要求**:Apple 的 Magic Keyboard 都是双模,我们是 single-mode BLE
4. **`adv_filter_policy` / `peer_addr_type` 设置和 macOS 期望的不一致**

## 重新设计的备选方案(值得讨论)

### 方案 A:USB HID 替代 BLE HID(违反 office-hours review 的 P3 但...)
- BLE 只走 NUS(已 100% 工作)
- USB-C 同时跑 HID(esp32-s3 内置 USB-OTG,标准 TinyUSB 库)
- **优点**:USB HID 在 macOS 100% 兼容,完全没有配对/信任问题
- **缺点**:需要插线(违反"无线"硬约束)
- 你 office-hours 时拒绝过这条路,但 8 小时调 BLE 配对失败后值得重新评估

### 方案 B:走 buddy 协议风格 — 让 daemon 接管 HID 注入
- 设备只发 NUS JSON 像 `{"key": "shift+space"}`
- Mac 端 daemon 收到后用 macOS API(Quartz Event Services)模拟键盘按键
- **优点**:绕过整个 BLE HID 兼容性地狱
- **缺点**:daemon 需要 Accessibility 权限(macOS 弹窗,你授权一次就好)
- 这个最像 buddy 项目的解法,基础设施全在,新增几十行 Python

### 方案 C:用 ESP32-S3 USB host 桥接 vs CoreBluetooth 走另一条路径
复杂,先不展开。

### 方案 D:换 BLE HID 库 — 改用 ESP-IDF 的 `esp_hid_gap` 高层 API
- 我之前选了手写 NimBLE service,因为担心 esp_hid 接管 advertising 跟 NUS 冲突
- 实际可能 esp_hid 处理 macOS 兼容性更全(它内部已经处理了几个特殊 quirks)
- 重写工作量约 2-3 小时

## 完整代码状态

- 主分支提交:`9181924`(M2 fix)、之后多个 commit 直到 HID + 双 service
- `firmware/app_claude/ble/ble_nus.cpp`、`ble_hid.cpp` 为最新
- daemon 端口 9877,launchd 已 bootout(避免抢占设备)
- 设备 BLE 名:ClawdWatch,advertising 主包含 HID UUID + Appearance=Keyboard,scan rsp 含 NUS UUID
- 设备物理状态:正在通过 USB-C 连 Mac,正在 advertising

## 对新 session 的建议起点

1. 读这份文档 + plan: `~/.claude/plans/eventual-weaving-meteor.md`
2. 决定走 A/B/C/D 哪个方向
3. **方案 B(daemon 注入)是改动最小、风险最低的选择**,值得最先讨论
