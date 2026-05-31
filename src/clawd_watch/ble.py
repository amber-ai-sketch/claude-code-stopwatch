"""BLE client for the M5Stack Stopwatch running clawd-watch firmware.

Maintains a single BLE link: scans for a device whose name starts with
DEVICE_NAME_PREFIX (or advertises NUS), connects, subscribes to TX
notifications, exposes a `send` coroutine for newline-delimited JSON
writes. Auto-reconnects with exponential backoff.

Logic borrowed nearly verbatim from claude-desktop-buddy's BuddyBLE
because the wire-level interaction with a Nordic UART peripheral is
identical regardless of which firmware is on the other end. Differences:
device name prefix, owner_msg/time_sync_msg are imported from THIS
project's protocol module so any future protocol divergence stays
self-contained.
"""
from __future__ import annotations

import asyncio
import json
import logging
from typing import Any, Awaitable, Callable, Optional

from bleak import BleakClient, BleakScanner
from bleak.backends.device import BLEDevice

from . import DEVICE_NAME_PREFIX
from .keyinject import release_all_modifiers
from .protocol import AUDIO_TX, NUS_RX, NUS_TX, owner_msg, time_sync_msg

log = logging.getLogger(__name__)

MessageHandler = Callable[[dict[str, Any]], Awaitable[None]]
ConnectHandler = Callable[[], Awaitable[None]]
# Raw bytes from one BLE notification on the audio characteristic. Sync
# callback (runs in bleak's notify context); keep it fast — just enqueue.
AudioFrameHandler = Callable[[bytes], None]


class WatchBLE:
    def __init__(
        self,
        *,
        name_prefix: str = DEVICE_NAME_PREFIX,
        address: Optional[str] = None,
        owner_name: str = "",
        on_message: Optional[MessageHandler] = None,
        on_connect: Optional[ConnectHandler] = None,
        on_audio_frame: Optional[AudioFrameHandler] = None,
    ) -> None:
        self.name_prefix = name_prefix
        self.address = address
        self.owner_name = owner_name
        self.on_message = on_message
        self.on_connect = on_connect
        self.on_audio_frame = on_audio_frame

        self._client: Optional[BleakClient] = None
        self._device: Optional[BLEDevice] = None
        self._rx_buf = bytearray()
        self._connected = asyncio.Event()
        self._send_lock = asyncio.Lock()
        # Set by the control endpoints to drop the current link and reconnect
        # immediately (skipping backoff). The supervise loop waits on it.
        self._reconnect = asyncio.Event()
        # Serializes on-demand /scan calls so two scanners never run at once.
        self._scan_lock = asyncio.Lock()

    @property
    def connected(self) -> bool:
        return self._client is not None and self._client.is_connected

    @property
    def device_name(self) -> Optional[str]:
        return self._device.name if self._device else None

    async def run_forever(self) -> None:
        backoff = 1.0
        while True:
            self._reconnect.clear()
            try:
                # _connect_once always scans by name and, if an address is
                # pinned, requires it to match — falling back to a plain name
                # scan when the pinned address no longer appears. So there's no
                # separate fail-streak fallback to manage here.
                await self._connect_once()
                backoff = 1.0
                # Supervise the link: wake on a drop OR a reconnect request.
                while self.connected and not self._reconnect.is_set():
                    try:
                        await asyncio.wait_for(self._reconnect.wait(), timeout=1.0)
                    except asyncio.TimeoutError:
                        pass
                if self._reconnect.is_set():
                    log.info("BLE reconnect requested")
                else:
                    log.warning("BLE link dropped; reconnecting")
            except asyncio.CancelledError:
                raise
            except Exception as e:
                log.warning("BLE loop error: %s", e)
            finally:
                self._connected.clear()
                # A half-finished hold-to-talk (key_down sent, link dropped
                # before key_up) would leave Shift stuck down on the Mac. Drop
                # any held modifiers whenever the link goes away.
                release_all_modifiers()
                await self._safe_disconnect()
            # A forced reconnect shouldn't pay the backoff delay.
            if self._reconnect.is_set():
                backoff = 1.0
                continue
            await asyncio.sleep(backoff)
            backoff = min(backoff * 2, 15.0)

    # ─── control surface (called from daemon HTTP handlers) ─────
    # These run on the same event loop as run_forever, so mutating fields and
    # setting the event needs no cross-thread marshaling.

    def request_reconnect(self) -> None:
        """Drop the current link and reconnect now using the current address."""
        self._reconnect.set()

    def use_address(self, address: str) -> None:
        """Pin the link to a specific address and reconnect immediately."""
        if not address or not isinstance(address, str):
            raise ValueError(f"bad address: {address!r}")
        self.address = address
        self._reconnect.set()

    def forget(self) -> None:
        """Forget the pinned address; next connect falls back to name scan."""
        self.address = None
        self._reconnect.set()

    async def scan(self, timeout: float = 6.0) -> list[dict[str, Any]]:
        """One-shot discovery of nearby BLE devices, strongest signal first.

        Safe to call while connected — CoreBluetooth scanning and an active
        GATT link coexist; the live link only sees a brief throughput dip.
        """
        async with self._scan_lock:
            found = await BleakScanner.discover(timeout=timeout, return_adv=True)
        devices = [
            {
                "name": device.name or "",
                "address": device.address,
                "rssi": getattr(adv, "rssi", None),
            }
            for device, adv in found.values()
        ]
        devices.sort(key=lambda d: (d["rssi"] is None, -(d["rssi"] or -999)))
        return devices

    async def send(self, obj: dict[str, Any]) -> None:
        if not self.connected:
            log.debug("send skipped; not connected: %s",
                      obj.get("cmd") or list(obj)[:1])
            return
        line = (json.dumps(obj, separators=(",", ":")) + "\n").encode()
        assert self._client is not None
        async with self._send_lock:
            try:
                # 180-byte chunks fit comfortably in a 247-byte default MTU
                # and a 517-byte negotiated MTU. response=True ensures macOS
                # negotiates encryption on the first write.
                CHUNK = 180
                for i in range(0, len(line), CHUNK):
                    await self._client.write_gatt_char(
                        NUS_RX, line[i: i + CHUNK], response=True
                    )
                log.info(
                    "BLE send ok (%d bytes): %s",
                    len(line),
                    line[:80].decode(errors="replace").strip(),
                )
            except Exception as e:
                log.warning("BLE send failed (%d bytes): %s payload=%r",
                            len(line), e, line[:80])

    async def _connect_once(self) -> None:
        # Always discover via a name-prefix scan, even when we have a pinned
        # address. find_device_by_address / raw BleakClient(address) are both
        # unreliable on macOS (CoreBluetooth won't connect to an address it
        # hasn't freshly discovered), so a scan that returns the BLEDevice
        # object is the only dependable path. When an address is pinned we just
        # additionally require it to match — that keeps us locked to the right
        # watch when several name-matching devices are around.
        if self.address:
            log.info("Reconnecting to watch %s (name scan + address match)",
                     self.address)
        else:
            log.info("Scanning for watch (name prefix %r)...", self.name_prefix)

        want_addr = (self.address or "").lower()

        def _match(d: BLEDevice, ad) -> bool:
            if not (d.name or "").startswith(self.name_prefix):
                return False
            if want_addr:
                return d.address.lower() == want_addr
            return True

        device = await BleakScanner.find_device_by_filter(_match, timeout=20.0)

        # A pinned address that no longer shows up (device renamed, replaced,
        # or address rotated) must not wedge us forever — fall back to a plain
        # name scan once before giving up.
        if not device and self.address:
            log.warning("pinned address %s not found; falling back to name scan",
                        self.address)
            self.address = None
            device = await BleakScanner.find_device_by_filter(
                lambda d, ad: (d.name or "").startswith(self.name_prefix),
                timeout=20.0,
            )

        if not device:
            raise RuntimeError("no watch device found")
        log.info("Found device: %s (%s)", device.name, device.address)
        # Pin the resolved address so on_connect can persist it.
        self.address = device.address

        self._device = device

        client = BleakClient(device)
        await client.connect()
        self._client = client
        log.info("Connected to %s",
                 device.name or device.address)

        try:
            await client.start_notify(NUS_TX, self._notify_handler)
        except Exception as e:
            log.error("start_notify failed: %s", e)
            raise
        await self._subscribe_audio(client)

        # Time-sync first to force macOS encryption negotiation on the very
        # first encrypted-write. Subsequent writes reuse the established link.
        await self.send(time_sync_msg())
        await self.send(owner_msg(self.owner_name))

        self._connected.set()
        if self.on_connect:
            try:
                await self.on_connect()
            except Exception as e:
                log.warning("on_connect handler error: %s", e)

    async def _safe_disconnect(self) -> None:
        if self._client is None:
            return
        try:
            if self._client.is_connected:
                await self._client.disconnect()
        except Exception as e:
            log.debug("disconnect error (ignored): %s", e)
        self._client = None

    async def _subscribe_audio(self, client: BleakClient) -> None:
        """Subscribe to the audio characteristic if the device exposes it.

        Best-effort: a firmware without the audio service (older build) must
        still work for control/status, so a missing characteristic logs and
        returns rather than failing the whole connection.
        """
        if self.on_audio_frame is None:
            return
        try:
            await client.start_notify(AUDIO_TX, self._audio_notify_handler)
            log.info("subscribed to audio TX; negotiated MTU=%s", client.mtu_size)
        except Exception as e:
            log.warning("audio TX subscribe failed (no audio service?): %s", e)

    def _audio_notify_handler(self, _char, data: bytearray) -> None:
        # One BLE notification == one binary audio frame. Hand raw bytes to
        # the consumer; parsing/reassembly lives in audio_receiver.
        if self.on_audio_frame is not None:
            self.on_audio_frame(bytes(data))

    def _notify_handler(self, _char, data: bytearray) -> None:
        log.info("BLE notify %d bytes: %r", len(data), bytes(data)[:80])
        self._rx_buf.extend(data)
        while True:
            nl = self._rx_buf.find(b"\n")
            if nl < 0:
                return
            line = bytes(self._rx_buf[:nl])
            del self._rx_buf[: nl + 1]
            if not line.strip():
                continue
            try:
                msg = json.loads(line.decode())
            except Exception as e:
                log.warning("bad JSON from device: %s (%r)", e, line)
                continue
            log.info("BLE recv JSON: %s", msg)
            if self.on_message is None:
                continue
            asyncio.create_task(self._dispatch(msg))

    async def _dispatch(self, msg: dict[str, Any]) -> None:
        assert self.on_message is not None
        try:
            await self.on_message(msg)
        except Exception as e:
            log.exception("on_message handler error: %s", e)
