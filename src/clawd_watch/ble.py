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
from .protocol import NUS_RX, NUS_SERVICE, NUS_TX, owner_msg, time_sync_msg

log = logging.getLogger(__name__)

MessageHandler = Callable[[dict[str, Any]], Awaitable[None]]
ConnectHandler = Callable[[], Awaitable[None]]


class WatchBLE:
    def __init__(
        self,
        *,
        name_prefix: str = DEVICE_NAME_PREFIX,
        address: Optional[str] = None,
        owner_name: str = "",
        on_message: Optional[MessageHandler] = None,
        on_connect: Optional[ConnectHandler] = None,
    ) -> None:
        self.name_prefix = name_prefix
        self.address = address
        self.owner_name = owner_name
        self.on_message = on_message
        self.on_connect = on_connect

        self._client: Optional[BleakClient] = None
        self._device: Optional[BLEDevice] = None
        self._rx_buf = bytearray()
        self._connected = asyncio.Event()
        self._send_lock = asyncio.Lock()

    @property
    def connected(self) -> bool:
        return self._client is not None and self._client.is_connected

    @property
    def device_name(self) -> Optional[str]:
        return self._device.name if self._device else None

    async def run_forever(self) -> None:
        backoff = 1.0
        while True:
            try:
                await self._connect_once()
                backoff = 1.0
                while self.connected:
                    await asyncio.sleep(1.0)
                log.warning("BLE link dropped; reconnecting")
            except asyncio.CancelledError:
                raise
            except Exception as e:
                log.warning("BLE loop error: %s", e)
            finally:
                self._connected.clear()
                await self._safe_disconnect()
            await asyncio.sleep(backoff)
            backoff = min(backoff * 2, 15.0)

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
        if self.address:
            log.info("Connecting to watch by address %s", self.address)
            try:
                device = await BleakScanner.find_device_by_address(
                    self.address, timeout=15.0
                )
            except Exception as e:
                log.warning("find_device_by_address failed (%s); trying raw connect", e)
                device = None
            if device is None:
                client = BleakClient(self.address)
                await client.connect()
                self._client = client
                self._device = None
                log.info("Connected (by address) to %s", self.address)
                await client.start_notify(NUS_TX, self._notify_handler)
                await self.send(time_sync_msg())
                await self.send(owner_msg(self.owner_name))
                self._connected.set()
                if self.on_connect:
                    try:
                        await self.on_connect()
                    except Exception as e:
                        log.warning("on_connect handler error: %s", e)
                return
        else:
            log.info("Scanning for watch (name prefix %r or NUS service)...",
                     self.name_prefix)

            def _match(d: BLEDevice, ad) -> bool:
                if (d.name or "").startswith(self.name_prefix):
                    return True
                try:
                    uuids = getattr(ad, "service_uuids", None) or []
                    return NUS_SERVICE.lower() in [u.lower() for u in uuids]
                except Exception:
                    return False

            device = await BleakScanner.find_device_by_filter(_match, timeout=20.0)

            if not device:
                raise RuntimeError("no watch device found")
            log.info("Found device: %s (%s)", device.name, device.address)

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
        except Exception:
            pass
        self._client = None

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
