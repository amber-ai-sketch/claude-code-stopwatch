"""macOS keyboard injection via Quartz Event Services.

This is the "Plan B" path for clawd-watch: instead of running a BLE HID
keyboard profile (which macOS won't accept input from without UI
pairing), the device sends a NUS JSON command and the daemon injects
the keystroke locally.

Quartz events go through the same input pipeline as a real keyboard, so
WeChat IME / focused app behavior is identical to typing on the
built-in keyboard.

First call requires Accessibility permission. macOS will prompt the
user once; after that the grant persists across restarts.
"""
from __future__ import annotations

import logging
from typing import Optional

log = logging.getLogger("clawd_watch.keyinject")

# Quartz key codes. Values from
# /System/Library/Frameworks/Carbon.framework/.../HIToolbox/Events.h
KEY_CODES = {
    "space":      0x31,
    "backspace":  0x33,
    "enter":      0x24,  # Return
    "esc":        0x35,
    "tab":        0x30,
    "c":          0x08,
}

# Real key codes for the LEFT modifier keys. WeChat's hold-to-talk hotkey
# is locked to the *left* Shift specifically, and it watches for an actual
# modifier key-down event — not the flag-on-another-event trick. So we
# press the modifier key itself (its own keyDown/keyUp), then the main key.
MOD_KEYCODES = {
    "shift":  0x38,   # left Shift
    "ctrl":   0x3B,   # left Control
    "alt":    0x3A,   # left Option
    "cmd":    0x37,   # left Command
}

# Flag masks set on each posted event while the modifier is held, so the
# system sees a consistent modifier state across the key-down chain.
MOD_FLAGS = {
    "shift":  0x20002,   # NX_DEVICELSHIFTKEYMASK | kCGEventFlagMaskShift
    "ctrl":   0x40001,   # NX_DEVICELCTLKEYMASK   | kCGEventFlagMaskControl
    "alt":    0x80020,   # NX_DEVICELALTKEYMASK   | kCGEventFlagMaskAlternate
    "cmd":   0x100008,   # NX_DEVICELCMDKEYMASK   | kCGEventFlagMaskCommand
}


def _post(keycode: int, down: bool, flags: int) -> None:
    Q = _quartz()
    ev = Q.CGEventCreateKeyboardEvent(None, keycode, down)
    if flags:
        Q.CGEventSetFlags(ev, flags)
    Q.CGEventPost(Q.kCGHIDEventTap, ev)


# Lazy import — pyobjc is heavy and only needed when a press actually
# fires. Cache the module on first use.
_Quartz = None


def _quartz():
    global _Quartz
    if _Quartz is None:
        import Quartz  # type: ignore
        _Quartz = Quartz
    return _Quartz


def key_tap(modifier: Optional[str], key: str) -> bool:
    """Press + immediately release. Use for one-shot keys (Backspace,
    Esc, Return, Ctrl+C).

    With a modifier, presses the real modifier key around the main key:
    mod-down → key-down → key-up → mod-up.

    Returns True on success, False if the key isn't known.
    """
    keycode = KEY_CODES.get(key)
    if keycode is None:
        log.warning("key_tap: unknown key %r", key)
        return False

    mod_keycode = MOD_KEYCODES.get(modifier) if modifier else None
    flags = MOD_FLAGS.get(modifier, 0) if modifier else 0

    if mod_keycode is not None:
        _post(mod_keycode, True, flags)
    _post(keycode, True, flags)
    _post(keycode, False, flags)
    if mod_keycode is not None:
        _post(mod_keycode, False, 0)

    log.info("key_tap %s+%s", modifier or "", key)
    return True


def key_down(modifier: Optional[str], key: str) -> bool:
    """Press but don't release. Use for hold-to-talk Shift+Space.

    Presses the real modifier key first, then the main key, both held.
    Pair with key_up to release. WeChat's hold-to-talk watches for the
    actual left-Shift key-down, so we must press the key itself, not just
    set a flag on the space event.
    """
    keycode = KEY_CODES.get(key)
    if keycode is None:
        log.warning("key_down: unknown key %r", key)
        return False
    mod_keycode = MOD_KEYCODES.get(modifier) if modifier else None
    flags = MOD_FLAGS.get(modifier, 0) if modifier else 0
    if mod_keycode is not None:
        _post(mod_keycode, True, flags)
    _post(keycode, True, flags)
    log.info("key_down %s+%s", modifier or "", key)
    return True


def key_up(modifier: Optional[str], key: str) -> bool:
    """Release a key previously pressed via key_down — main key first,
    then the modifier key, mirroring key_down."""
    keycode = KEY_CODES.get(key)
    if keycode is None:
        log.warning("key_up: unknown key %r", key)
        return False
    mod_keycode = MOD_KEYCODES.get(modifier) if modifier else None
    flags = MOD_FLAGS.get(modifier, 0) if modifier else 0
    _post(keycode, False, flags)
    if mod_keycode is not None:
        _post(mod_keycode, False, 0)
    log.info("key_up %s+%s", modifier or "", key)
    return True
