# TODOS

## seq 2 字节绕回边界（v2 长录音前必须处理）

- **What**: 音频帧 `seq` 用 2 字节（max 65536）。`audio_frame.h` / `audio_receiver.py` 共享此宽度。
- **Why**: 阶段一单次录音封顶 30s（@40ms 块 ≈ 750 帧），远不到绕回，不触发。但 v2 取消 30s 上限做长录音时，seq 绕回会让 daemon `reassemble()` 按 seq 排序错乱（旧帧 seq 与新帧重叠），表现为漏字/错位。
- **Pros**: 修掉后长录音不会静默错乱。
- **Cons**: 极低成本（几行），但当前不触发，提前做违背 YAGNI。
- **Context**: 决策来自 `/plan-eng-review` 的 outside voice 评审。阶段一刻意保持 seq 2 字节以省 MTU 紧张时的帧头开销。
- **Depends on**: v2 取消 30s 录音上限的决策。
- **Start**: 要么 seq 扩到 4 字节（`audio_frame_pack` + `parse_frame` 同步改 `<BHI`→`<BII` 之类），要么 `reassemble()` 加序号取模回绕检测。
