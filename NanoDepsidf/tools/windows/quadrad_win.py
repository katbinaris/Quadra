#!/usr/bin/env python3
"""quadrad_win -- the Quadra knob's companion service on Windows.

The Mac service (tools/mac/quadrad.py) does the work here too: the agents' approvals and the
AGENTS dashboard, now playing for the MUSIC profile, the CLOCK app's time, the knob over USB or
WiFi. This file imports it and swaps in Windows for the parts that are the Mac's:

  * Now playing: the system's media controls (SMTC, what the volume flyout shows: Spotify, a
    browser tab, Media Player...) instead of MediaRemote / AppleScript.
  * The volume: the default output device's (Core Audio's endpoint volume) instead of CoreAudio.
  * The time zone: Windows' own, as its IANA name (tzlocal), with the rule read from the tzdata
    package (there's no /usr/share/zoneinfo).
  * The agents' hooks (quadra_hook_win.py) reach it over TCP on 127.0.0.1 instead of a Unix
    socket: a random port and a token, both in ~/.quadra/agentd.json, which only this user can
    read (the profile folder's own permissions). A connection without the token gets nothing.

Needs, for this Python: hidapi, Pillow, pycaw, winrt-Windows.Media.Control,
winrt-Windows.Storage.Streams, winrt-Windows.Foundation, winrt-Windows.Foundation.Collections,
tzdata, tzlocal; and cryptography for
WiFi. install_win.py installs it as a scheduled task that starts at logon.

Log: stdout, or %LOCALAPPDATA%\\Quadra\\quadrad.log when it has none (pythonw, the task).
"""
import asyncio
import hashlib
import hmac
import json
import os
import secrets
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
# Installed: quadrad.py, quadra_hook.py and quadra.py sit next to this file (~/.quadra). In the
# repo: tools/mac/ and tools/.
sys.path[:0] = [HERE, os.path.join(HERE, "..", "mac"), os.path.join(HERE, "..")]

LOG_DIR = os.path.join(os.environ.get("LOCALAPPDATA") or os.path.expanduser("~"), "Quadra")
if sys.stdout is None:  # pythonw: no console, so the log goes to a file
    os.makedirs(LOG_DIR, exist_ok=True)
    sys.stdout = sys.stderr = open(os.path.join(LOG_DIR, "quadrad.log"), "a", buffering=1, encoding="utf-8")

sys.coinit_flags = 0  # COM multithreaded: the volume callback comes on Windows' own thread
import quadra  # noqa: E402
import quadrad  # noqa: E402
from quadrad import log  # noqa: E402

AGENTD = os.path.join(quadrad.HOME, "agentd.json")


# ── the knob on USB ────────────────────────────────────────────────────────────

def is_knob(d):
    """The knob's vendor interface. Windows names it by its interface ("Quadra Data"), not
    "Quadra" as the Mac does: any name that starts so, as the companion app takes it."""
    return d.get("usage_page") == quadrad.VENDOR_USAGE_PAGE and (d.get("product_string") or "").startswith(quadrad.PRODUCT)


def usb_present():
    return not quadrad.NO_USB and any(is_knob(d) for d in quadrad.hid.enumerate())


class WinDevice(quadrad.Device):
    def _open(self):
        if quadrad.NO_USB:
            return None
        for d in quadrad.hid.enumerate():
            if is_knob(d):
                dev = quadrad.hid.device()
                dev.open_path(d["path"])
                return quadrad.UsbLink(dev)
        return None


# ── the time zone ──────────────────────────────────────────────────────────────

def use_tzdata():
    """quadra.zone_rule reads the zone file's footer (its POSIX rule): from the tzdata package."""
    try:
        import tzdata
        quadra.ZONEINFO = os.path.join(os.path.dirname(tzdata.__file__), "zoneinfo")
    except ImportError:
        log("no tzdata package: the CLOCK app's LOCAL zone keeps today's offset, without DST")


def local_zone():
    """(label, rule) for this PC's zone (Settings > Time & language), as quadra.local_zone."""
    try:
        import tzlocal
        name = tzlocal.get_localzone_name()
        return quadra.zone_label(name), quadra.zone_rule(name)
    except Exception:
        return "UTC", "UTC0"


# ── the volume ─────────────────────────────────────────────────────────────────

class WinAudio:
    """quadrad.CoreAudio on Windows: the default output device's volume, and word when it
    changes. The device is looked up again on every read, so a switch (headphones in) follows."""

    def __init__(self):
        from pycaw.callbacks import AudioEndpointVolumeCallback

        class Changed(AudioEndpointVolumeCallback):
            def __init__(cb, owner):
                super().__init__()
                cb.owner = owner

            def on_notify(cb, *_):
                cb.owner.on_change()

        self._Changed = Changed
        self.on_change = lambda: None
        self._id = None
        self._ev = None
        self._cb = None

    @staticmethod
    def _speakers():
        try:
            from pycaw.utils import AudioUtilities
            return AudioUtilities.GetSpeakers()
        except Exception:
            return None

    def watch(self, on_change):
        """on_change() -- from a Windows thread -- whenever the output volume changes."""
        self.on_change = on_change
        self.follow()

    def follow(self):
        """Listen to the current output device (again after the device changed)."""
        dev = self._speakers()
        did = getattr(dev, "id", None)
        if did == self._id:
            return
        if self._ev is not None and self._cb is not None:
            try:
                self._ev.UnregisterControlChangeNotify(self._cb)
            except Exception:
                pass
        self._id, self._ev, self._cb = did, None, None
        if dev is not None:
            try:
                self._ev = dev.EndpointVolume
                self._cb = self._Changed(self)
                self._ev.RegisterControlChangeNotify(self._cb)
            except Exception:
                self._ev = self._cb = None

    def volume(self):
        """0..100, or -1 = unknown"""
        self.follow()
        try:
            return round(self._ev.GetMasterVolumeLevelScalar() * 100)
        except Exception:
            return -1


# ── now playing ────────────────────────────────────────────────────────────────

class SmtcNowPlaying(threading.Thread):
    """quadrad.NowPlaying on Windows: the system media controls. `latest` = (state, artwork bytes
    or None), None while nothing has a session; state has the keys nowplaying.m sends:
    {"bundle", "playing", "title", "artist", "album", "art_seq"}."""

    POLL_S = 0.5
    STALE_S = 2.0  # a player often shows the last track's picture a moment into the next one

    def __init__(self):
        super().__init__(name="nowplaying", daemon=True)
        self.latest, self.usable = None, True

    def run(self):
        asyncio.run(self._run())

    @staticmethod
    def _pick(mgr, playing):
        """The session to show: one that's playing, else the one Windows calls current."""
        try:
            for s in mgr.get_sessions():
                if s.get_playback_info().playback_status == playing:
                    return s
        except Exception:
            pass
        return mgr.get_current_session()

    @staticmethod
    async def _read(ref):
        from winrt.windows.storage.streams import Buffer, InputStreamOptions
        stream = await ref.open_read_async()
        size = min(int(stream.size), quadrad.MAX_ART)
        if size <= 0:
            return None
        buf = await stream.read_async(Buffer(size), size, InputStreamOptions.READ_AHEAD)
        data = bytes(buf)
        return data or None

    async def _run(self):
        try:
            from winrt.windows.media.control import (
                GlobalSystemMediaTransportControlsSessionManager as Manager,
                GlobalSystemMediaTransportControlsSessionPlaybackStatus as Status,
            )
            mgr = await Manager.request_async()
        except Exception as e:
            log(f"now playing: no system media controls here ({e})")
            self.usable = False
            return
        key, art, art_hash, prev_hash, seq, since, next_art, why = None, None, None, None, 0, 0.0, 0.0, None
        while True:
            try:
                s = self._pick(mgr, Status.PLAYING)
                if s is None:
                    self.latest, key = None, None
                else:
                    p = await s.try_get_media_properties_async()
                    playing = s.get_playback_info().playback_status == Status.PLAYING
                    app = s.source_app_user_model_id or "?"
                    now = time.monotonic()
                    k = (app, p.title or "", p.artist or "")
                    if k != key:  # another track: its picture still to come
                        key, prev_hash, art, since, next_art = k, art_hash, None, now, 0.0
                    if p.thumbnail is not None and now >= next_art:
                        data = await self._read(p.thumbnail)
                        h = hashlib.sha1(data).digest() if data else None
                        stale = h == prev_hash and now - since < self.STALE_S
                        if h and not stale and (h != art_hash or art is None):
                            art, art_hash, seq = data, h, seq + 1
                        next_art = now + (1.0 if art is None else 5.0)
                    self.latest = ({"bundle": app, "playing": playing, "title": p.title or "",
                                    "artist": p.artist or "", "album": p.album_title or "", "art_seq": seq}, art)
                why = None
            except Exception as e:
                if str(e) != why:
                    log(f"now playing: {e}")
                    why = str(e)
                self.latest = None
            await asyncio.sleep(self.POLL_S)


class WinMusic(quadrad.Music):
    """quadrad.Music, with no AppleScript to fall back on."""

    def scripted(self):
        return None

    def status(self):
        return {**super().status(), "source": "SMTC" if self.np.usable else "none"}


# ── the agents' hooks: TCP on this computer only, with a token ─────────────────

def read_agentd():
    try:
        with open(AGENTD) as f:
            a = json.load(f)
        return int(a["port"]), str(a["token"])
    except (OSError, ValueError, KeyError, TypeError):
        return None


def write_agentd(port, token):
    tmp = AGENTD + ".tmp"
    fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w") as f:
        json.dump({"port": port, "token": token}, f)
    os.replace(tmp, AGENTD)


async def main():
    os.makedirs(quadrad.HOME, mode=0o700, exist_ok=True)
    known = read_agentd()
    if known:
        try:  # another instance answering? then leave it be
            _, w = await asyncio.wait_for(asyncio.open_connection("127.0.0.1", known[0]), 1)
            w.close()
            log("already running")
            return
        except (OSError, asyncio.TimeoutError):
            pass
    use_tzdata()
    quadra.local_zone = local_zone
    quadrad.CoreAudio, quadrad.NowPlaying, quadrad.Music = WinAudio, SmtcNowPlaying, WinMusic
    quadrad.Device, quadrad.usb_present = WinDevice, usb_present
    loop = asyncio.get_running_loop()
    d = quadrad.Daemon(loop)
    token = secrets.token_hex(16)

    async def handle(reader, writer):
        # The token first, on a line of its own; then the hook's request, as on the Mac.
        try:
            line = await asyncio.wait_for(reader.readline(), 5)
        except (asyncio.TimeoutError, ConnectionError, ValueError):
            line = b""
        if not hmac.compare_digest(line.strip(), token.encode()):
            writer.close()
            return
        await d.handle(reader, writer)

    server = await asyncio.start_server(handle, "127.0.0.1", 0)
    port = server.sockets[0].getsockname()[1]
    write_agentd(port, token)
    d.dev.start()
    log(f"listening on 127.0.0.1:{port} ({AGENTD}), config {d.cfg}")
    log("now playing: Windows' media controls (SMTC: players that show in the volume flyout)")
    loop.create_task(d.janitor())
    loop.create_task(d.music.run())
    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    asyncio.run(main())
