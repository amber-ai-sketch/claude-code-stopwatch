# 明天回来要做的事

> Co-author: Claude Opus 4.7

## 当前进度

| Milestone | 状态 |
|---|---|
| **M1** Mac 端 daemon + statusline | ✅ 完成,端到端验证通过 |
| **M2** 固件骨架 + LVGL idle 占位 | 🟡 代码就绪,等 ESP-IDF 安装 |
| M3 按钮事件机 | ⬜ 未开始 |
| M4 BLE NUS 协议层 | ⬜ 未开始 |
| M5 BLE HID 双 service | ⬜ 未开始(项目硬闸门) |
| M6 motion graphic | ⬜ 未开始 |

## 明早 Step 1: 修 LaunchAgents 权限

`~/Library/LaunchAgents/` 当前是 root 拥有,launchd 自动加载失败。修一次永久解决:

```bash
sudo chown -R $USER:staff ~/Library/LaunchAgents
chmod 700 ~/Library/LaunchAgents
```

然后重跑 install:

```bash
cd ~/Projects/claude-code-stopwatch
./install.sh
```

预期看到:
```
==> Loading com.claude-code.clawd-watch into launchd
... (no permission error)
✓ Install complete.
```

接着:

```bash
clawd-watch test-statusline   # 推一条假数据
clawd-watch status            # 看到 line: model=Opus 4.7 cost=$1.23 ctx=42% ...
```

## 明早 Step 2: 装 ESP-IDF v5.5 (M2 必须)

这一步**未在我夜间动手时执行**(用户环境改动,需要你确认)。两个安装路径:

### 路径 A:官方安装脚本(推荐)

```bash
mkdir -p ~/esp && cd ~/esp
git clone -b v5.5 --recursive https://github.com/espressif/esp-idf.git
cd esp-idf
./install.sh esp32s3
```

下载约 300MB-1GB,装完后每个新 shell 需要:

```bash
. ~/esp/esp-idf/export.sh
```

把上面这行加进 `~/.zshrc` 一劳永逸,或者起 alias `getidf`。

### 路径 B:VS Code ESP-IDF 扩展(GUI)

如果你不想动 shell,VS Code 装 `espressif.esp-idf-extension` 一键搞定。但 CLI 仍然推荐路径 A。

### 验证 ESP-IDF 装好

```bash
idf.py --version    # 期望: ESP-IDF v5.5
echo $IDF_PATH      # 期望: ~/esp/esp-idf
```

## 明早 Step 3: M2 烧机

夜间 clone 的 `firmware/upstream/` 已经 patch 好,但因为它本身是另一个 git repo,被主 repo 的 `.gitignore` 排除。**如果你删掉它从头来,我留了一键复原脚本**:

```bash
cd ~/Projects/claude-code-stopwatch/firmware

# (可选,只在 upstream/ 不存在或被弄乱时跑)
rm -rf upstream
git clone --depth 1 https://github.com/m5stack/M5StopWatch-UserDemo.git upstream
./apply_to_upstream.sh   # 应用 main.cpp/apps.h patch + 复制 app_claude/
```

正常情况(upstream/ 还在,夜间状态完好)直接进 build:

```bash
cd ~/Projects/claude-code-stopwatch/firmware/upstream

# 1. 拉 demo 的子模块依赖(M5GFX、LVGL v9.5、mooncake、smooth_ui_toolkit 等)
python3 fetch_repos.py

# 2. 设置目标芯片
idf.py set-target esp32s3

# 3. build (首次约 5-10 分钟)
idf.py build

# 4. 烧录(USB-C 已连)
idf.py -p /dev/cu.usbmodem* flash monitor
```

**期望现象**:开机 → 黑屏 → 加载 → 圆屏中央显示 "$0.00",顶部 "Claude" 字样,右上 / 右下 / 左下三个空圆环。

监控串口看到 `Claude` 这个 app 的日志:
```
INFO Claude: on create
INFO Claude: on open
```

按按钮(GPIO 1/2)mooncake 自带 KeyManager 会消化事件,不会做任何动作(这是预期 — M3 会替换 FSM)。

## 已写好的固件代码骨架

主 repo 视角(这些是 git 里的真实文件):

```
firmware/
├── upstream/                    (git-ignored — 上游 demo,patch 已应用)
├── app_claude/                  (源真本,你 git 里能 diff 它)
│   ├── app_claude.h / .cpp      AppAbility 容器
│   ├── ui/idle_page.h / .cpp    M2 占位 idle 页
│   ├── ble/                     (空,M4 填充)
│   └── input/                   (空,M3 填充)
├── patches/
│   └── 0001-only-install-AppClaude.patch   只装 AppClaude+AppSetup
└── apply_to_upstream.sh         一键复原脚本
```

`apply_to_upstream.sh` 会:
1. 把 `patches/*.patch` 应用到 `upstream/`
2. 把 `app_claude/` 整个 cp 进 `upstream/main/apps/app_claude/`

## 跑通 M2 之后

立即上 M3 + M4(按钮 FSM + BLE NUS 协议)。这两步并行做。
跑通后立刻做 M5 (BLE HID),这是项目硬闸门。

详细方案看 plan: `~/.claude/plans/eventual-weaving-meteor.md`

## 已知踩坑

- M5StopWatch demo 的 `sdkconfig.defaults` **没启 BLE**。M4 阶段要加 NimBLE 配置。
- demo 用 `GLOB_RECURSE` 收集 cpp,所以 `app_claude/` 子目录的 cpp 都会自动入 build,不用改 CMakeLists.txt。
- 圆 AMOLED 466×466 物理是圆形,但 LVGL 默认用方形 framebuffer。靠近边缘的内容会被物理屏切掉,UI 设计要把重要元素压在 380px 圆内。

## 改动汇总(可 git diff 查看)

```bash
cd ~/Projects/claude-code-stopwatch
git status
# 你会看到:
#   firmware/upstream/  (clone 的 demo,是 git submodule 候选)
#   src/clawd_watch/    (Mac 端代码)
#   resources/          (launchd plist 模板)
#   install.sh / uninstall.sh
#   pyproject.toml / README.md / .gitignore
#   docs/NEXT_STEPS.md  (本文档)
```

`firmware/upstream/` 是 fresh clone 里加了 4 处 patch:
1. `main/main.cpp` — 只 install AppClaude + AppSetup
2. `main/apps/apps.h` — 加 #include
3. `main/apps/app_claude/` — 全新目录(代码骨架)
