"""Audio output helpers for feeding a virtual microphone.

The first supported target is BlackHole 2ch. We write audio to it via
ffmpeg's AudioToolbox output device so WeChat IME can use BlackHole as
its microphone input.
"""
from __future__ import annotations

import asyncio
import os
import tempfile
from dataclasses import dataclass
from pathlib import Path


DEFAULT_TEST_PHRASE = (
    "这是 clawd watch 的 BlackHole 测试音频。"
    "如果微信输入法能听到虚拟麦克风，应该会输入这句话。"
)


@dataclass
class AudioSinkResult:
    ok: bool
    text: str
    device_index: int
    returncode: int
    stderr: str


class AudioSinkError(RuntimeError):
    pass


async def _run_command(*argv: str, timeout: float = 20.0) -> tuple[int, str, str]:
    proc = await asyncio.create_subprocess_exec(
        *argv,
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.PIPE,
    )
    try:
        stdout, stderr = await asyncio.wait_for(proc.communicate(), timeout=timeout)
    except asyncio.TimeoutError:
        proc.kill()
        await proc.wait()
        raise AudioSinkError(f"command timed out: {' '.join(argv)}")
    return proc.returncode, stdout.decode(errors="replace"), stderr.decode(errors="replace")


async def play_spoken_text_to_blackhole(
    text: str = DEFAULT_TEST_PHRASE,
    *,
    device_index: int | None = None,
    voice: str = "Ting-Ting",
) -> AudioSinkResult:
    """Speak text into BlackHole using macOS `say` and ffmpeg AudioToolbox.

    `device_index` is ffmpeg's AudioToolbox output index. On the verified
    machine BlackHole 2ch is index 0; callers can override with
    CLAWD_WATCH_AUDIO_DEVICE_INDEX or the HTTP/CLI payload.
    """
    if not text.strip():
        raise AudioSinkError("voice test text must not be empty")

    resolved_device_index = int(
        device_index
        if device_index is not None
        else os.environ.get("CLAWD_WATCH_AUDIO_DEVICE_INDEX", "0")
    )

    fd, path = tempfile.mkstemp(prefix="clawd-watch-blackhole-", suffix=".wav")
    os.close(fd)
    wav_path = Path(path)
    try:
        rc, _out, err = await _run_command(
            "say",
            "-v",
            voice,
            "-o",
            str(wav_path),
            "--data-format=LEF32@48000",
            text,
            timeout=20.0,
        )
        if rc != 0:
            return AudioSinkResult(False, text, resolved_device_index, rc, err)

        rc, _out, err = await _run_command(
            "ffmpeg",
            "-hide_banner",
            "-nostdin",
            "-re",
            "-i",
            str(wav_path),
            "-f",
            "audiotoolbox",
            "-audio_device_index",
            str(resolved_device_index),
            "-y",
            "dummy",
            timeout=30.0,
        )
        return AudioSinkResult(rc == 0, text, resolved_device_index, rc, err)
    finally:
        try:
            wav_path.unlink()
        except FileNotFoundError:
            pass
