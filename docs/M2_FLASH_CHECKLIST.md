# M2 烧机检查清单

> Co-author: Claude Opus 4.7

ESP-IDF 装完之后,按这份清单跑一遍。每一步标了"期望看到什么",方便对照。

## 0 前置

```bash
# ESP-IDF 的 install.sh 在 macOS 上默认不装 cmake/ninja。手动补:
brew install cmake ninja

# 让当前 shell 找到 idf.py
. ~/esp/esp-idf/export.sh

# 验证
idf.py --version    # 期望: ESP-IDF v5.5
which idf.py        # 期望: ~/esp/esp-idf/tools/idf.py
echo $IDF_PATH      # 期望: ~/esp/esp-idf
which cmake ninja   # 必须都在
```

## 1 应用我们的 patches 到 upstream

夜间已经 clone 过,现在 upstream 里应该已经有 app_claude/ + 改过的 main.cpp。验证:

```bash
cd ~/Projects/claude-code-stopwatch/firmware/upstream
git status
# 期望:
#   modified:   main/apps/apps.h
#   modified:   main/main.cpp
#   Untracked: main/apps/app_claude/
```

如果是干净的 clone(没有上述改动),跑:

```bash
cd ~/Projects/claude-code-stopwatch/firmware
./apply_to_upstream.sh
```

## 2 拉子模块依赖

```bash
cd ~/Projects/claude-code-stopwatch/firmware/upstream
python3 fetch_repos.py
```

期望看到 9 个 repo 被 clone 进 `components/`:
- mooncake, mooncake_log, smooth_ui_toolkit
- M5GFX, ArduinoJson, lvgl
- M5IOE1, M5PM1 (会自动 patch)
- BMI270_BMM150_Sensor

总下载 ~500MB。完成后 `ls components/` 应该看到这 9 个目录。

## 3 设置目标芯片

```bash
idf.py set-target esp32s3
```

期望最后一行:`Build complete (configuration successful)`。会生成 `sdkconfig`(继承自 `sdkconfig.defaults`)。

## 4 第一次 build

```bash
idf.py build
```

**首次约 5-10 分钟**(后续增量 30s-2min)。

期望看到:
- 大量 "Generating ld/sections.ld"、"Building C object" 行
- 最后是 binary size 报告:
  ```
  Project build complete. To flash, run: idf.py flash
  ```

如果失败,**最常见的两类错**:
1. **ArduinoJson include 找不到** → 检查 `components/ArduinoJson/` 是否存在
2. **app_claude.cpp 编译错** → 看具体 error。我夜里没烧机验证过,这是预期会出问题的地方

如果是 (2),把完整错误粘给我,我改 app_claude 代码。

## 5 烧录

设备已经通过 USB-C 连着电脑(你昨晚说过)。先确认串口:

```bash
ls /dev/cu.usbmodem* /dev/cu.usbserial* 2>/dev/null
```

期望看到一个端口,如 `/dev/cu.usbmodem11401`。

```bash
idf.py -p /dev/cu.usbmodem* flash monitor
```

期望:
- "Hard resetting via RTS pin..." 后串口接管,清屏
- 启动日志(很多 "I (xxxx) ...")
- 关键日志:
  ```
  I (xxxx) Claude: on create
  I (xxxx) Claude: on open
  ```
- 串口空闲(occasional INFO logs about HAL ticking)

**屏幕预期**:
- 黑底
- 顶部居中 "Claude" 字样
- 中央居中 "$0.00" 大字
- 中央下面浅灰 "no sessions"
- 三个空圆环:右上角(绿)、右下角(橙)、左下角(蓝)

按按钮没反应是预期的(M3 FSM 还没接进去,只有 mooncake 自带 KeyManager 在消化事件)。

## 6 退出 monitor

`Ctrl-]`(macOS 上是 control + 右方括号)。

## 如果哪步卡住

| 现象 | 怀疑 | 处理 |
|---|---|---|
| `idf.py: command not found` | 没 source | 重跑 `. ~/esp/esp-idf/export.sh` |
| `fetch_repos.py` 拉到一半停 | git 网络问题 | 重跑(脚本应该幂等) |
| `Failed to connect` 烧录卡住 | USB 串口冲突 / 设备没进 download mode | 长按 boot 键再插一次,或 `idf.py erase-flash` |
| 屏幕黑,日志正常 | LCD 驱动初始化早期失败 | 看 monitor 里有没有 error 行 |
| 屏幕花,日志正常 | LVGL 渲染问题 | 截图给我 |
| 串口看不到 "Claude: on create" | mooncake app 没 install | 看 main.cpp 有没有正确 include 我们的改动 |

把任何不在期望里的现象贴给我,我改代码。
