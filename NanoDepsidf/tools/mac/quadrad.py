#!/usr/bin/env python3
"""quadrad -- the Quadra knob's companion service on the Mac.

Agents. Hooks in Claude Code, Codex and Cursor (quadra_hook.py) talk to this service over a
Unix socket (~/.quadra/agentd.sock, owner-only). Approvals show on the knob (vendor HID,
src/ext_proto.h EXT_CMD_NOTIFY) and wait for its answer: hold F1 = allow, F3 = deny, F2 / F4 =
answer in the app. Nothing here approves on its own: without a knob, after `approval_wait_s`,
or when the hook goes away, the answer is "no decision" and the agent asks in its own UI. The
same events keep the AGENTS profile's dashboard (EXT_CMD_AGENTS) current.

Music. The Mac's Now Playing -- what Control Center shows, from any player: Kaset, Music,
Spotify, a browser tab -- read from MediaRemote by a small helper that /usr/bin/perl loads
(nowplaying.m; no Full Disk Access or Automation prompts), goes to the MUSIC profile: the cover
as a 240x240 JPEG (EXT_CMD_COVER), the title, artist, the cover's colours, the system volume and
the play state (EXT_CMD_TRACK). Without the helper: Music and Spotify over AppleScript.

The knob over USB when it's plugged in, else over WiFi (src/net_link.h): whenever it's on USB
this service keeps its WiFi address and pairing key (~/.quadra/wifi.json, owner-only), and
without a cable reaches it on the network -- the same reports, encrypted. WiFi needs the
`cryptography` package; without it, USB only.

Config: ~/.quadra/config.json (see DEFAULTS). Log: stdout (the LaunchAgent sends it to
~/Library/Logs/quadrad.log).
"""
import asyncio
import base64
import colorsys
import ctypes
import hashlib
import hmac
import io
import json
import os
import queue
import select
import socket
import ssl
import struct
import subprocess
import sys
import threading
import time
import unicodedata
import urllib.parse
import urllib.request
import zlib

import hid

try:  # WiFi only (NetLink)
    from cryptography.hazmat.primitives.ciphers.aead import AESGCM
except ImportError:
    AESGCM = None

# quadra.py (the CLI) sits next to this file once installed (tools/ in the repo): the device
# protocol's helpers -- time zones for the CLOCK app.
sys.path[:0] = [os.path.dirname(os.path.abspath(__file__)), os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")]
import quadra  # noqa: E402


def share_hid():
    """macOS hidapi opens devices exclusively by default, which would lock this service, the
    CLI and the companion app out of each other. Ask for shared opens (no-op elsewhere)."""
    try:
        ctypes.CDLL(hid.__file__).hid_darwin_set_open_exclusive(0)
    except (OSError, AttributeError):
        pass


share_hid()

HOME = os.environ.get("QUADRA_HOME") or os.path.expanduser("~/.quadra")
SOCK = os.path.join(HOME, "agentd.sock")
CONFIG = os.path.join(HOME, "config.json")
WIFI = os.path.join(HOME, "wifi.json")  # the knob's WiFi pairing, from USB (owner-only)
DEFAULTS = {
    "approval_wait_s": 30,  # how long an approval waits for the knob before the app asks
    "your_turn": True,      # blink when an agent finishes its turn and waits for you
    "turn_delay_s": 4,      # ...unless you're already typing back within this
    "run_delay_s": 1.5,     # Cursor: only show "RUN?" if the command is still waiting by then
    "info_ttl_s": 1800,     # attention items clear themselves after this
    # Each agent's colour on the knob (ring, badge); "#000000" = the knob's own default.
    "colors": {"claude": "#E8825F", "codex": "#4F9DFF", "cursor": "#B98CFF"},
    # Which items tap the knob gently every few seconds: approvals, "needs input", an approval
    # that moved to the app, "your turn", Cursor's "run?".
    "nudge": {"ask": True, "attention": True, "inapp": True, "turn": False, "run": False},
    "music": True,          # now playing on the MUSIC profile
}

# --- device (src/ext_proto.h, src/notify.h, src/media.h, src/agent_board.h) ---
REPORT = 64
VENDOR_USAGE_PAGE, PRODUCT = 0xFF00, "Quadra"
EXT_CMD_NOTIFY, EXT_CMD_COVER, EXT_CMD_TRACK, EXT_CMD_AGENTS = 0x25, 0x26, 0x27, 0x28
EXT_TAG_ACK, EXT_TAG_NOTIFY = 0xC1, 0xC3
EXT_CMD_NET, EXT_TAG_NET, EXT_TAG_KEY = 0x29, 0xC4, 0xC7
NET_STATUS, NET_KEY, NET_CONNECTED = 5, 6, 2
NET_KEEPALIVE_S = 2   # the knob gives a third client the slot of the quietest: never be quiet
USB_STATUS_S = 30     # on USB: the knob's WiFi address again (DHCP can move it)
USB_WATCH_S = 3       # on WiFi: a cable plugged in meanwhile -> back to USB
NET_RETRY_S = 5
EXT_NOTIFY_NUDGE = 0x80
POST, CLEAR, CLEAR_ALL = 1, 2, 3
COVER_BEGIN, COVER_DATA, COVER_END, COVER_CHUNK = 1, 2, 3, 58
TRACK_PLAYING, TRACK_NONE = 0x01, 0x80
ASK, INFO = 0, 1
SOURCE_ID = {"claude": 0, "codex": 1, "cursor": 2}
ALLOW, DENY, LATER, DISMISS = 1, 2, 3, 4
SHOWN_MAX = 4   # NOTIFY_MAX
BOARD_MAX = 4   # AGENT_BOARD_MAX
IDLE, WORKING, YOUR_TURN, ASKING = 0, 1, 2, 3
BOARD_STATE = {"working": WORKING, "turn": YOUR_TURN, "asking": ASKING, "idle": IDLE}
SESSION_TTL_S = 1800


def log(*a):
    print(time.strftime("%H:%M:%S"), *a, flush=True)


def load_config():
    cfg = json.loads(json.dumps(DEFAULTS))
    try:
        user = json.load(open(CONFIG))
    except (OSError, ValueError):
        user = {}
    for k, v in user.items():
        if isinstance(v, dict) and isinstance(cfg.get(k), dict):
            cfg[k].update(v)
        else:
            cfg[k] = v
    return cfg


def rgb(spec) -> bytes:
    try:
        v = int(str(spec).lstrip("#"), 16)
        return bytes([(v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF])
    except ValueError:
        return b"\0\0\0"


ASCII_PUNCT = str.maketrans({"‘": "'", "’": "'", "“": '"', "”": '"', "–": "-", "—": "-", "…": "..."})


def cstr(s: str, n: int) -> bytes:
    """The knob's font is ASCII: accents come off (Semberé -> Sembere), anything else is '?'."""
    s = unicodedata.normalize("NFKD", str(s).translate(ASCII_PUNCT))
    s = "".join(c if 0x20 <= ord(c) <= 0x7E else "?" for c in s if not unicodedata.combining(c))
    return s.encode("ascii")[:n].ljust(n, b"\0")


def load_pairing():
    try:
        p = json.load(open(WIFI))
        return p if len(bytes.fromhex(p["key"])) == 32 and p["port"] > 0 and (p["host"] or p["ip"]) else None
    except (OSError, ValueError, KeyError, TypeError):
        return None


def save_pairing(p):
    fd = os.open(WIFI + ".tmp", os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w") as f:
        json.dump(p, f)
    os.replace(WIFI + ".tmp", WIFI)


# QUADRA_NO_USB=1: no knob on USB, so the service takes WiFi with the cable in (the cable powers it).
NO_USB = bool(os.environ.get("QUADRA_NO_USB"))


def usb_present():
    return not NO_USB and any(d.get("usage_page") == VENDOR_USAGE_PAGE and d.get("product_string") == PRODUCT for d in hid.enumerate())


class UsbLink:
    via = "USB"

    def __init__(self, dev):
        self.dev = dev

    def write(self, report: bytes):
        if self.dev.write(b"\0" + report) < 0:
            raise OSError("write failed")

    def read(self, ms):
        data = self.dev.read(REPORT, ms)
        return bytes(data) if data else None

    def close(self):
        self.dev.close()


class NetLink:
    """The knob over WiFi (src/net_link.h): TCP, a handshake that proves both ends hold the
    pairing key, then every report AES-256-GCM with a per-direction counter."""
    via = "WiFi"

    def __init__(self, p):
        key, last = bytes.fromhex(p["key"]), None
        for h in [f"{p['host']}.local" if p["host"] else None, p["ip"] or None]:
            if not h:
                continue
            try:  # IPv4 only: asked for any family, macOS waits seconds for a .local IPv6 answer
                addr = socket.getaddrinfo(h, p["port"], socket.AF_INET, socket.SOCK_STREAM)[0][4]
                self.s = socket.create_connection(addr, timeout=3)
                break
            except OSError as e:
                last = e
        else:
            raise last or OSError("no address")
        self.s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        cn = os.urandom(16)
        self.s.sendall(b"QDR1" + cn)
        r = self._recv(36)
        k = hmac.new(key, b"quadra session" + cn + r[4:20], hashlib.sha256).digest()
        if r[:4] != b"QDR1" or not hmac.compare_digest(hmac.new(k, b"knob", hashlib.sha256).digest()[:16], r[20:]):
            raise ConnectionError("the knob holds another key: plug it in once to pair again")
        self.s.sendall(hmac.new(k, b"client", hashlib.sha256).digest()[:16])
        self.g, self.tx, self.rx, self.buf = AESGCM(k), 0, 0, b""
        self.s.settimeout(3)

    def _recv(self, n):
        b = b""
        while len(b) < n:
            c = self.s.recv(n - len(b))
            if not c:
                raise ConnectionError("closed by the knob")
            b += c
        return b

    @staticmethod
    def _nonce(d, seq):
        return bytes([d]) + struct.pack("<Q", seq) + b"\0\0\0"

    def write(self, report: bytes):
        self.s.sendall(self.g.encrypt(self._nonce(ord("C"), self.tx), report, None))
        self.tx += 1

    def read(self, ms):
        if len(self.buf) < REPORT + 16:
            if select.select([self.s], [], [], ms / 1000)[0]:
                c = self.s.recv(4096)
                if not c:
                    raise ConnectionError("closed by the knob")
                self.buf += c
            if len(self.buf) < REPORT + 16:
                return None
        frame, self.buf = self.buf[:REPORT + 16], self.buf[REPORT + 16:]
        try:
            p = self.g.decrypt(self._nonce(ord("K"), self.rx), frame, None)
        except Exception:
            raise ConnectionError("a frame from the knob didn't verify")
        self.rx += 1
        return p

    def close(self):
        self.s.close()


class Device(threading.Thread):
    """Owns the link to the knob -- USB when it's plugged in, else WiFi once paired: reconnects
    forever, writes queued reports, reads what comes back."""

    def __init__(self, loop, on_decision, on_ack, on_link):
        super().__init__(daemon=True)
        self.loop, self.on_decision, self.on_ack, self.on_link = loop, on_decision, on_ack, on_link
        self.out = queue.Queue()
        self.connected = False
        self.via = None
        self.net_why = None  # why WiFi last failed: logged once, not every retry
        self.pending_host = None  # on USB: the WiFi status that came, waiting for its key

    def send(self, report: bytes):
        if self.connected:
            self.out.put(report.ljust(REPORT, b"\0"))

    def _open(self):
        if NO_USB:
            return None
        for d in hid.enumerate():
            if d.get("usage_page") == VENDOR_USAGE_PAGE and d.get("product_string") == PRODUCT:
                dev = hid.device()
                dev.open_path(d["path"])
                return UsbLink(dev)
        return None

    def _open_net(self):
        p = load_pairing()
        if p is None or AESGCM is None:
            return None
        try:
            link = NetLink(p)
            self.net_why = None
            return link
        except (OSError, ConnectionError) as e:
            if str(e) != self.net_why:
                log(f"knob over WiFi: {e}")
                self.net_why = str(e)
            return None

    def run(self):
        while True:
            try:
                link = self._open()
            except OSError:
                link = None
            if link is None:
                link = self._open_net()
            if link is None:
                time.sleep(1 if load_pairing() is None or AESGCM is None else NET_RETRY_S)
                continue
            self._serve(link)
            time.sleep(0.5)

    def _on_net(self, data, link):
        """On USB: the knob's WiFi status, then its key -- kept so this service can reach it
        without a cable (the current key: never a new one, which would unpair every companion)."""
        if data[0] == EXT_TAG_NET and data[1] == NET_CONNECTED:
            host, ip = data[41:64].split(b"\0", 1)[0].decode(errors="replace"), ".".join(map(str, data[3:7]))
            if link.via == "USB":
                self.pending_host = (host, ip)
                link.write(bytes([EXT_CMD_NET, NET_KEY, 0]).ljust(REPORT, b"\0"))
            else:
                p = load_pairing()
                if p and p["ip"] != ip:
                    save_pairing({**p, "ip": ip})  # DHCP moved it: the fallback follows
        elif data[0] == EXT_TAG_KEY and link.via == "USB" and self.pending_host:
            host, ip = self.pending_host
            self.pending_host = None
            p = {"host": host, "ip": ip, "port": data[33] | data[34] << 8, "key": data[1:33].hex()}
            if p != load_pairing():
                save_pairing(p)
                log(f"WiFi pairing kept: {host}.local ({ip})")

    def _serve(self, link):
        while not self.out.empty():  # anything queued was for the previous connection
            self.out.get_nowait()
        self.connected, self.via = True, link.via
        log(f"knob connected ({link.via})")
        self.loop.call_soon_threadsafe(self.on_link, True)
        status = bytes([EXT_CMD_NET, NET_STATUS]).ljust(REPORT, b"\0")
        next_status = 0.0
        next_watch = time.monotonic() + USB_WATCH_S
        try:
            while True:
                now = time.monotonic()
                if now >= next_status:  # USB: keep the pairing current; WiFi: the keepalive
                    link.write(status)
                    next_status = now + (USB_STATUS_S if link.via == "USB" else NET_KEEPALIVE_S)
                if link.via == "WiFi" and now >= next_watch:
                    next_watch = now + USB_WATCH_S
                    if usb_present():
                        log("knob plugged in: over to USB")
                        break
                while not self.out.empty():
                    link.write(self.out.get_nowait())
                data = link.read(20)
                if not data:
                    continue
                if data[0] == EXT_TAG_NOTIFY:
                    self.loop.call_soon_threadsafe(self.on_decision, data[2] | data[3] << 8, data[1])
                elif data[0] == EXT_TAG_ACK:
                    self.loop.call_soon_threadsafe(self.on_ack, data[1], data[2])
                elif data[0] in (EXT_TAG_NET, EXT_TAG_KEY):
                    self._on_net(data, link)
        except (OSError, ValueError, ConnectionError) as e:
            log(f"knob gone ({link.via}):", e)
        finally:
            self.connected, self.via = False, None
            try:
                link.close()
            except Exception:
                pass
            self.loop.call_soon_threadsafe(self.on_link, False)


class Item:
    def __init__(self, iid, kind, source, session, key, title, body, fp=""):
        self.id, self.kind, self.source, self.session, self.key = iid, kind, source, session, key
        self.title, self.body, self.fp = title, body, fp
        self.created = time.monotonic()
        self.expires = None
        self.waiter = None  # asyncio.Future for an approval

    def report(self, cfg) -> bytes:
        nudge = cfg["nudge"].get("ask" if self.kind == ASK else self.key, False)
        return (bytes([EXT_CMD_NOTIFY, POST]) + self.id.to_bytes(2, "little")
                + bytes([SOURCE_ID.get(self.source, 3), self.kind | (EXT_NOTIFY_NUDGE if nudge else 0)])
                + cstr(self.title, 16) + cstr(self.body, 39) + rgb(cfg["colors"].get(self.source, "#000000")))


# ── agents ─────────────────────────────────────────────────────────────────────

class Daemon:
    def __init__(self, loop):
        self.loop = loop
        self.cfg = load_config()
        self.items: dict[int, Item] = {}
        self.shown: list[int] = []  # what the knob holds, in its order
        self.timers: dict[tuple, asyncio.TimerHandle] = {}
        self.next_id = 1
        self.sessions: dict[tuple, dict] = {}  # (source, session) -> {name, state, t}
        self.board_sent = None
        self.acks: dict[int, asyncio.Future] = {}
        self.dev = Device(loop, self.on_decision, self.on_ack, self.on_link)
        self.music = Music(self)
        self.time_sent = 0.0  # the CLOCK app's time, last sent

    # --- the knob's queue: approvals first (oldest first), then attention items (newest first)
    def sync(self):
        if not self.dev.connected:
            self.shown = []
            return
        asks = sorted((i for i in self.items.values() if i.kind == ASK), key=lambda i: i.created)
        infos = sorted((i for i in self.items.values() if i.kind == INFO), key=lambda i: -i.created)
        want = [i.id for i in (asks + infos)[:SHOWN_MAX]]
        keep = 0
        while keep < min(len(want), len(self.shown)) and want[keep] == self.shown[keep]:
            keep += 1
        for iid in self.shown[keep:]:  # everything after the common head comes off...
            self.dev.send(bytes([EXT_CMD_NOTIFY, CLEAR]) + iid.to_bytes(2, "little"))
        for iid in want[keep:]:        # ...and goes back on in the right order
            self.dev.send(self.items[iid].report(self.cfg))
        self.shown = want

    def repost(self, item: Item):
        if item.id in self.shown:
            self.dev.send(item.report(self.cfg))  # same id: the knob updates it in place

    def new_id(self) -> int:
        while True:
            iid, self.next_id = self.next_id, self.next_id % 65535 + 1
            if iid not in self.items:
                return iid

    def remove(self, iid: int, answer=None):
        item = self.items.pop(iid, None)
        if item and item.waiter and not item.waiter.done():
            item.waiter.set_result(answer)
        self.sync()

    # --- the AGENTS dashboard ---
    def touch(self, req):
        source, session = req.get("source", ""), req.get("session", "")
        state = req.get("state")
        if not source or not session or not state:
            return
        key = (source, session)
        if state == "end":
            self.sessions.pop(key, None)
        else:
            name = os.path.basename(str(req.get("cwd") or "").rstrip("/")) or source
            s = self.sessions.setdefault(key, {"name": name})
            s.update(state=BOARD_STATE.get(state, IDLE), t=time.monotonic())
            if req.get("cwd"):
                s["name"] = name
        self.push_board()

    def push_board(self, force=False):
        order = {ASKING: 0, YOUR_TURN: 1, WORKING: 2, IDLE: 3}
        rows = sorted(self.sessions.items(), key=lambda kv: (order[kv[1]["state"]], -kv[1]["t"]))[:BOARD_MAX]
        payload = bytes([EXT_CMD_AGENTS, len(rows)])
        for (source, _), s in rows:
            payload += bytes([SOURCE_ID.get(source, 3), s["state"]]) + cstr(s["name"], 12)
        if force or payload != self.board_sent:
            self.board_sent = payload
            self.dev.send(payload)

    # --- device callbacks (event loop thread) ---
    def on_link(self, up: bool):
        if up:
            self.dev.send(bytes([EXT_CMD_NOTIFY, CLEAR_ALL, 0, 0]))
            self.shown = []
            self.sync()
            self.push_board(force=True)
            self.music.resend()
            self.send_time()
        else:
            self.shown = []
            for item in list(self.items.values()):  # nobody can answer on the knob now
                if item.kind == ASK:
                    self.remove(item.id, None)
            for f in self.acks.values():
                if not f.done():
                    f.set_result(None)

    def send_time(self):
        """The CLOCK app's time and LOCAL zone (this Mac's): on connecting, then every 5 minutes."""
        self.time_sent = time.monotonic()
        if self.dev.connected:
            self.dev.send(quadra.time_report(*quadra.local_zone()))

    def on_decision(self, iid: int, decision: int):
        if iid in self.shown:
            self.shown.remove(iid)  # the knob already dropped it
        item = self.items.get(iid)
        if item is None:
            return
        log(f"knob: {item.source} #{iid} {item.title!r} -> {({ALLOW: 'ALLOW', DENY: 'DENY', LATER: 'LATER', DISMISS: 'DISMISS'}).get(decision, decision)}")
        if item.kind == ASK and (item.source, item.session) in self.sessions:
            self.touch({"source": item.source, "session": item.session, "state": "working"})
        self.remove(iid, decision)

    def on_ack(self, cmd: int, status: int):
        f = self.acks.pop(cmd, None)
        if f and not f.done():
            f.set_result(status)

    async def request_ack(self, report: bytes, timeout=2.0):
        """Send one command that the knob acknowledges; its status, or None."""
        cmd = report[0]
        f = self.loop.create_future()
        self.acks[cmd] = f
        self.dev.send(report)
        try:
            return await asyncio.wait_for(f, timeout)
        except asyncio.TimeoutError:
            self.acks.pop(cmd, None)
            return None

    # --- hook requests ---
    async def ask(self, req, reader) -> dict:
        if not self.dev.connected:
            return {"decision": "none"}
        item = Item(self.new_id(), ASK, req.get("source", ""), req.get("session", ""), "ask",
                    req.get("title", "APPROVE?"), req.get("body", ""), req.get("fp", ""))
        item.waiter = self.loop.create_future()
        self.items[item.id] = item
        # A pending "approve in app" reminder for this session is superseded by the real thing.
        self.clear(item.source, item.session, "inapp")
        self.sync()
        log(f"ask: {item.source} #{item.id} {item.title!r} {item.body!r}")
        gone = asyncio.ensure_future(reader.read(1))  # EOF: the agent dropped the hook
        done, _ = await asyncio.wait({item.waiter, gone}, timeout=float(self.cfg["approval_wait_s"]),
                                     return_when=asyncio.FIRST_COMPLETED)
        gone.cancel()
        if item.waiter.done():
            answer = item.waiter.result()
            return {"decision": "allow" if answer == ALLOW else "deny" if answer == DENY else "none"}
        if gone in done:
            log(f"ask #{item.id}: hook went away")
            self.remove(item.id)
            return {"decision": "none"}
        # Timed out: the app asks now. Leave a reminder that the session moving on clears.
        item.waiter.set_result(None)
        item.waiter, item.kind, item.key = None, INFO, "inapp"
        item.title, item.body = "APPROVE IN APP", item.title + ": " + item.body
        item.expires = time.monotonic() + float(self.cfg["info_ttl_s"])
        self.repost(item)
        self.sync()
        log(f"ask #{item.id}: no answer on the knob, the app asks")
        return {"decision": "none"}

    def info(self, req):
        source, session, key = req.get("source", ""), req.get("session", ""), req.get("key", "info")
        if key == "turn" and not self.cfg["your_turn"]:
            return
        if req.get("unless_ask") and any(i.kind == ASK and i.source == source and i.session == session
                                         for i in self.items.values()):
            return
        delay = {"turn": self.cfg["turn_delay_s"], "run": self.cfg["run_delay_s"]}.get(req.get("delay"), 0)
        tk = (source, session, key)
        if tk in self.timers:
            self.timers.pop(tk).cancel()

        def post():
            self.timers.pop(tk, None)
            existing = next((i for i in self.items.values()
                             if i.kind == INFO and (i.source, i.session, i.key) == tk), None)
            if existing:
                existing.title, existing.body = req.get("title", ""), req.get("body", "")
                existing.expires = time.monotonic() + float(self.cfg["info_ttl_s"])
                self.repost(existing)
                return
            item = Item(self.new_id(), INFO, source, session, key, req.get("title", ""), req.get("body", ""))
            item.expires = time.monotonic() + float(self.cfg["info_ttl_s"])
            self.items[item.id] = item
            log(f"info: {source} #{item.id} {item.title!r} {item.body!r}")
            self.sync()

        if delay:
            self.timers[tk] = self.loop.call_later(float(delay), post)
        else:
            post()

    def clear(self, source, session, what="all", fp=""):
        for tk in [k for k in self.timers if k[0] == source and k[1] == session
                   and (what == "all" or k[2] == what or (what == "asks" and k[2] == "inapp"))]:
            self.timers.pop(tk).cancel()
        for item in list(self.items.values()):
            if item.source != source or item.session != session:
                continue
            hit = (what == "all"
                   or (what == "asks" and (item.kind == ASK or item.key == "inapp"))
                   or (what == "fp" and fp and item.fp == fp)
                   or (what not in ("all", "asks", "fp") and item.key == what))
            if hit:
                self.remove(item.id, None)

    async def handle(self, reader, writer):
        try:
            line = await asyncio.wait_for(reader.readline(), 5)
            req = json.loads(line or b"{}")
            self.touch(req)
            op = req.get("op")
            if op == "ask":
                reply = await self.ask(req, reader)
            elif op == "info":
                self.info(req)
                reply = {"ok": True}
            elif op == "clear":
                self.clear(req.get("source", ""), req.get("session", ""), req.get("what", "all"), req.get("fp", ""))
                reply = {"ok": True}
            elif op == "seen":
                reply = {"ok": True}
            elif op == "status":
                reply = {"knob": self.dev.connected,
                         "items": [{"id": i.id, "kind": "ask" if i.kind == ASK else "info", "source": i.source,
                                    "title": i.title, "body": i.body} for i in self.items.values()],
                         "agents": [{"source": k[0], "name": s["name"], "state": s["state"]} for k, s in self.sessions.items()],
                         "music": self.music.status()}
            else:
                reply = {"error": "unknown op"}
            writer.write((json.dumps(reply) + "\n").encode())
            await writer.drain()
        except (asyncio.TimeoutError, ValueError, ConnectionError):
            pass
        finally:
            writer.close()

    async def janitor(self):
        while True:
            await asyncio.sleep(10)
            now = time.monotonic()
            if now - self.time_sent >= 300:
                self.send_time()
            for item in list(self.items.values()):
                if item.expires and now > item.expires:
                    self.remove(item.id)
            stale = [k for k, s in self.sessions.items() if now - s["t"] > SESSION_TTL_S]
            for k in stale:
                self.sessions.pop(k)
            if stale:
                self.push_board()


# ── music ──────────────────────────────────────────────────────────────────────

try:
    import certifi
    SSL_CTX = ssl.create_default_context(cafile=certifi.where())
except Exception:
    SSL_CTX = ssl.create_default_context()

MAX_ART = 8 * 1024 * 1024
NP_LIB = os.path.join(HOME, "nowplaying.dylib")
# mediaremoted (the Mac's Now Playing) only answers Apple-signed processes since macOS 15.4.
# /usr/bin/perl is one: it loads the helper (tools/mac/nowplaying.m) and runs it for us.
NP_PERL = ('use DynaLoader; my $h = DynaLoader::dl_load_file($ARGV[0], 0) or die DynaLoader::dl_error();'
           ' my $f = DynaLoader::dl_find_symbol($h, "quadra_np_stream") or die DynaLoader::dl_error();'
           ' &{DynaLoader::dl_install_xsub("main::np", $f)}();')
NP_NO_API = 3  # the helper's exit status when this macOS has no MediaRemote Now Playing


class CoreAudio:
    """The system output volume, straight from CoreAudio (cheap), and word when it changes."""

    class Addr(ctypes.Structure):
        _fields_ = [("sel", ctypes.c_uint32), ("scope", ctypes.c_uint32), ("elem", ctypes.c_uint32)]

    LISTENER = ctypes.CFUNCTYPE(ctypes.c_int32, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p, ctypes.c_void_p)

    def __init__(self):
        self.ca = ctypes.cdll.LoadLibrary("/System/Library/Frameworks/CoreAudio.framework/CoreAudio")
        for fn in (self.ca.AudioObjectAddPropertyListener, self.ca.AudioObjectRemovePropertyListener):
            fn.argtypes = [ctypes.c_uint32, ctypes.POINTER(self.Addr), self.LISTENER, ctypes.c_void_p]
        self.on_change = lambda: None
        self._listener = self.LISTENER(self._changed)  # kept: CoreAudio holds a raw pointer
        self._dev = 0

    def _changed(self, *_):
        self.on_change()
        return 0  # an OSStatus, whatever on_change returns (call_soon_threadsafe: a Handle)

    def _listen(self, obj, sel, scope, on):
        a = self.Addr(self._fcc(sel), self._fcc(scope), 0)
        fn = self.ca.AudioObjectAddPropertyListener if on else self.ca.AudioObjectRemovePropertyListener
        fn(obj, ctypes.byref(a), self._listener, None)

    def watch(self, on_change):
        """on_change() -- from a CoreAudio thread -- whenever the output device or its volume changes."""
        self.on_change = on_change
        self._listen(1, "dOut", "glob", True)
        self.follow()

    def follow(self):
        """Listen to the current output device's volume (again after the device changed)."""
        dev = self._get(1, "dOut", "glob", ctypes.c_uint32) or 0
        if dev != self._dev:
            if self._dev:
                self._listen(self._dev, "vmvc", "outp", False)
            self._dev = dev
            if dev:
                self._listen(dev, "vmvc", "outp", True)

    @staticmethod
    def _fcc(s):
        return struct.unpack(">I", s.encode())[0]

    def _get(self, obj, sel, scope, ctype):
        a = self.Addr(self._fcc(sel), self._fcc(scope), 0)
        size, v = ctypes.c_uint32(ctypes.sizeof(ctype)), ctype()
        err = self.ca.AudioObjectGetPropertyData(ctypes.c_uint32(obj), ctypes.byref(a), 0, None,
                                                 ctypes.byref(size), ctypes.byref(v))
        return None if err else v.value

    def volume(self):
        """0..100, or -1 = unknown"""
        dev = self._get(1, "dOut", "glob", ctypes.c_uint32)
        vol = self._get(dev, "vmvc", "outp", ctypes.c_float) if dev else None
        return round(vol * 100) if vol is not None else -1


class NowPlaying(threading.Thread):
    """The Mac's Now Playing, from the helper. `latest` = (state, artwork bytes or None), None until
    the helper speaks; `usable` is False without a working helper (then AppleScript stands in).
    state: {"bundle", "playing", "title", "artist", "album", "art_seq"} (see nowplaying.m)."""

    def __init__(self):
        super().__init__(name="nowplaying", daemon=True)
        self.latest, self.usable = None, os.path.exists(NP_LIB)

    def run(self):
        fails = 0
        while self.usable:
            started, art = time.monotonic(), None
            try:
                p = subprocess.Popen(["/usr/bin/perl", "-e", NP_PERL, NP_LIB],
                                     stdin=subprocess.DEVNULL, stdout=subprocess.PIPE)
            except OSError as e:
                log(f"now playing: no /usr/bin/perl ({e}): AppleScript instead")
                self.usable = False
                break
            for line in p.stdout:
                try:
                    state = json.loads(line)
                    if "art" in state:
                        a = state.pop("art")
                        art = base64.b64decode(a) if a else None
                except (ValueError, TypeError, AttributeError):
                    continue
                self.latest = (state, art)
            self.latest = None
            code = p.wait()
            quick = time.monotonic() - started < 10
            fails = fails + 1 if quick else 0
            if code == NP_NO_API or fails >= 3:
                log(f"now playing: the MediaRemote helper doesn't work here ({code}): AppleScript instead")
                self.usable = False
            else:
                log(f"now playing helper stopped ({code}), restarting")
                time.sleep(10 if quick else 2)


def osa(script, timeout=3):
    try:
        r = subprocess.run(["osascript", "-e", script], capture_output=True, text=True, timeout=timeout)
        return r.stdout.strip() if r.returncode == 0 else None
    except (subprocess.TimeoutExpired, OSError):
        return None


def running(app):
    return subprocess.run(["pgrep", "-x", app], capture_output=True).returncode == 0


def fetch(url):
    """An image's bytes, or None. Only image/* responses, size-capped, TLS verified."""
    if not url or not url.startswith("https://"):
        return None
    try:
        req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
        with urllib.request.urlopen(req, timeout=8, context=SSL_CTX) as r:
            if not r.headers.get_content_type().startswith("image/"):
                return None
            data = r.read(MAX_ART + 1)
        return data if 100 < len(data) <= MAX_ART else None
    except Exception:
        return None


def loose(s):
    """A name for comparing: no case, accents or punctuation."""
    s = unicodedata.normalize("NFKD", s if isinstance(s, str) else "").casefold()
    return " ".join("".join(c if c.isalnum() else " " for c in s if not unicodedata.combining(c)).split())


def itunes_cover(title, artist):
    """The iTunes Store's cover for the song -- only when its result really is this song."""
    want_t, want_a = loose(title), loose(artist)
    if not (want_t and want_a):
        return None
    try:
        term = urllib.parse.quote(f"{artist} {title}")
        with urllib.request.urlopen(f"https://itunes.apple.com/search?term={term}&entity=song&limit=5",
                                    timeout=5, context=SSL_CTX) as r:
            results = json.loads(r.read(256 * 1024)).get("results") or []
    except Exception:
        return None
    for s in results:
        t, a = loose(s.get("trackName")), loose(s.get("artistName"))
        if t and a and (want_t in t or t in want_t) and (want_a in a or a in want_a):
            return fetch(s.get("artworkUrl100", "").replace("100x100bb", "600x600bb"))
    return None


def placeholder(title, artist):
    """A cover for a track that has none anywhere (so the last track's never stays up): a record
    on a gradient in the track's own colours, the same ones every time. PNG bytes."""
    from PIL import Image, ImageDraw
    h = zlib.crc32(f"{artist}\t{title}".encode()) % 360 / 360
    top = tuple(round(v * 255) for v in colorsys.hsv_to_rgb(h, 0.65, 0.85))
    bottom = tuple(round(v * 255) for v in colorsys.hsv_to_rgb((h + 0.08) % 1, 0.8, 0.35))
    img = Image.composite(Image.new("RGB", (240, 240), bottom), Image.new("RGB", (240, 240), top),
                          Image.linear_gradient("L").resize((240, 240)))
    d, cx, cy, ink = ImageDraw.Draw(img), 120, 104, (16, 16, 20)
    d.ellipse((cx - 84, cy - 84, cx + 84, cy + 84), fill=ink)
    for r in range(78, 32, -7):  # the grooves
        d.ellipse((cx - r, cy - r, cx + r, cy + r), outline=(42, 42, 48))
    d.ellipse((cx - 28, cy - 28, cx + 28, cy + 28), fill=top)  # the label
    d.ellipse((cx - 4, cy - 4, cx + 4, cy + 4), fill=ink)
    buf = io.BytesIO()
    img.save(buf, "PNG")
    return buf.getvalue()


class Music:
    POLL_S = 0.5
    GONE_S = 15     # nothing playing for this long: the MUSIC screen goes back to normal
    ART_WAIT_S = 3  # a track's artwork often comes a moment after its title

    def __init__(self, daemon):
        self.d = daemon
        self.audio = CoreAudio()
        self.np = NowPlaying()
        self.track = None       # {"id", "title", "artist", "player", "playing"}
        self.since = 0.0        # when this track came on
        self.art_seq = None     # the helper's artwork last looked at
        self.cover_of = None    # the id of the track the knob's cover belongs to
        self.cover = None       # the knob's cover: JPEG bytes
        self.palette = [0, 0, 0]
        self.sent = None        # the last EXT_CMD_TRACK report
        self.playing = False
        self.need_cover = False
        self.last_seen = 0.0

    def status(self):
        t = self.track or {}
        return {"source": "MediaRemote" if self.np.usable else "AppleScript", "player": t.get("player"),
                "title": t.get("title"), "artist": t.get("artist"), "cover_bytes": len(self.cover or b"")}

    def resend(self):
        self.sent = None
        self.need_cover = self.cover is not None

    @staticmethod
    def from_np(s):
        """A track from the helper's state, None when nothing is playing. The album isn't part of
        which track it is: Kaset's stand-in card (paused, loading) has none, its WebKit card has."""
        title, artist, player = s.get("title") or "", s.get("artist") or "", s.get("bundle") or "?"
        if not title:
            return None
        return {"id": f"{player}\t{title}\t{artist}", "title": title, "artist": artist, "player": player,
                "playing": bool(s.get("playing"))}

    # --- without the helper: Music and Spotify over AppleScript (blocking: runs in the executor) ---
    def scriptable(self, app):
        if not running(app):  # never `tell` an app that isn't running: that would launch it
            return None
        out = osa(f'''tell application "{app}"
            if player state is playing or player state is paused then
              set t to current track
              return (name of t) & "\t" & (artist of t) & "\t" & (album of t) & "\t" & (player state as text)
            end if
          end tell''')
        parts = (out or "").split("\t")
        if len(parts) < 4:
            return None
        return {"id": "\t".join([app] + parts[:3]), "title": parts[0], "artist": parts[1], "player": app,
                "playing": parts[3] == "playing"}

    def scripted(self):
        """The Music or Spotify track to show: a playing one wins over a paused one."""
        found = [t for t in map(self.scriptable, ("Music", "Spotify")) if t]
        return next((t for t in found if t["playing"]), found[0] if found else None)

    def scripted_art(self, t):
        """The cover's bytes for track `t` from its player, else the iTunes Store's."""
        if t["player"] == "Music":
            tmp = os.path.join(HOME, "cover.tmp")
            ok = osa(f'''tell application "Music" to set ad to data of artwork 1 of current track
                set f to open for access POSIX file "{tmp}" with write permission
                set eof f to 0
                write ad to f
                close access f
                return "ok"''')
            if ok == "ok":
                try:
                    with open(tmp, "rb") as fh:
                        return fh.read(MAX_ART)
                except OSError:
                    pass
        elif t["player"] == "Spotify":
            data = fetch(osa('tell application "Spotify" to get artwork url of current track') or "")
            if data:
                return data
        return itunes_cover(t["title"], t["artist"])

    @staticmethod
    def render(art):
        """(JPEG bytes for the knob, its 3 colours) from any image's bytes; None if unreadable."""
        from PIL import Image
        try:
            img = Image.open(io.BytesIO(art)).convert("RGB")
        except Exception:
            return None
        w, h = img.size
        s = min(w, h)
        img = img.crop(((w - s) // 2, (h - s) // 2, (w + s) // 2, (h + s) // 2)).resize((240, 240), Image.LANCZOS)
        for q in (88, 80, 70, 60):  # baseline JPEG (the knob's decoder doesn't do progressive)
            buf = io.BytesIO()
            img.save(buf, "JPEG", quality=q, optimize=True, progressive=False)
            if buf.tell() <= 60 * 1024:
                break
        # The cover's colours for the ring: weighted by how much of the cover they cover and how
        # vivid they are, each clearly different from the ones already picked (three shades of
        # one orange make a flat ring). Near-black and near-white make a dead ring: skipped.
        quant = img.quantize(colors=12, method=Image.Quantize.FASTOCTREE)
        pal, cands = quant.getpalette(), []
        for count, idx in quant.getcolors() or []:
            r, g, b = pal[idx * 3:idx * 3 + 3]
            mx, mn = max(r, g, b), min(r, g, b)
            if mx < 50 or (mx > 225 and mx - mn < 30):
                continue
            cands.append((count * (mx - mn + 20), (r, g, b)))
        picked = []
        for _, c in sorted(cands, reverse=True):
            if all(sum((a - b) ** 2 for a, b in zip(c, p)) > 90 ** 2 for p in picked):
                picked.append(c)
            if len(picked) == 3:
                break
        vivid = [(r << 16) | (g << 8) | b for r, g, b in picked] or [0xFFB030]
        while len(vivid) < 3:
            vivid.append(vivid[len(vivid) % len(picked)] if picked else vivid[-1])
        return buf.getvalue(), vivid

    async def set_cover(self, art):
        """Make `art` (any image's bytes) the knob's cover; False if it isn't a readable image."""
        out = await asyncio.get_running_loop().run_in_executor(None, self.render, art)
        if not out:
            return False
        if out[0] != self.cover:  # players re-send the same picture (Kaset on every play/pause)
            self.cover, self.palette = out
            self.need_cover = True
        return True

    async def show_cover(self, t, art, where):
        """`art` as track `t`'s cover; a placeholder when there's none."""
        if art and await self.set_cover(art):
            log(f"  cover from {where}")
        else:
            await self.set_cover(placeholder(t["title"], t["artist"]))
            log("  no cover anywhere: placeholder")
        self.cover_of = t["id"]

    # --- to the knob ---
    async def upload_cover(self):
        data = self.cover
        if not data or not self.d.dev.connected:
            return False
        begin = bytes([EXT_CMD_COVER, COVER_BEGIN, 0, 0]) + len(data).to_bytes(4, "little") \
            + (zlib.crc32(data) & 0xFFFFFFFF).to_bytes(4, "little")
        if await self.d.request_ack(begin) != 0:
            return False
        for off in range(0, len(data), COVER_CHUNK):
            piece = data[off:off + COVER_CHUNK]
            self.d.dev.send(bytes([EXT_CMD_COVER, COVER_DATA]) + off.to_bytes(3, "little") + bytes([len(piece)]) + piece)
        return await self.d.request_ack(bytes([EXT_CMD_COVER, COVER_END]), timeout=5.0) == 0

    def track_report(self, vol, playing):
        t = self.track
        if t is None:
            return bytes([EXT_CMD_TRACK, TRACK_NONE])
        flags = TRACK_PLAYING if playing else 0
        pal = b"".join(c.to_bytes(3, "big") for c in self.palette)
        return (bytes([EXT_CMD_TRACK, flags, vol if 0 <= vol <= 100 else 0xFF]) + pal
                + cstr(t["title"], 24) + cstr(t["artist"], 24))

    def send_track(self):
        report = self.track_report(self.audio.volume(), self.playing)
        if report != self.sent:
            self.sent = report
            self.d.dev.send(report)

    async def follow_volume(self, wake):
        """The knob's volume ring follows the Mac the moment its volume changes, not on the next poll."""
        while True:
            await wake.wait()
            wake.clear()
            self.audio.follow()
            if self.d.cfg.get("music", True) and self.d.dev.connected:
                self.send_track()

    async def run(self):
        self.np.start()
        log("now playing: " + ("MediaRemote (system Now Playing)" if self.np.usable else
                               f"AppleScript (Music, Spotify) -- no {NP_LIB}: run tools/mac/install.py"))
        loop = asyncio.get_running_loop()
        wake = asyncio.Event()
        self.audio.watch(lambda: loop.call_soon_threadsafe(wake.set))
        loop.create_task(self.follow_volume(wake))
        while True:
            await asyncio.sleep(self.POLL_S)
            if not self.d.cfg.get("music", True) or not self.d.dev.connected:
                continue
            np = self.np.latest if self.np.usable else None
            if self.np.usable:
                t = self.from_np(np[0]) if np else None
            else:
                t = await loop.run_in_executor(None, self.scripted)
            now = time.monotonic()
            if t:
                self.last_seen = now
                if self.track is None or t["id"] != self.track["id"]:
                    log(f"now playing ({t['player']}): {t['title']} -- {t['artist']}")
                    self.since = now
                    if np and np[1]:  # this track's artwork came along with it
                        self.cover_of = t["id"]
                    elif not np:      # AppleScript: one look per track
                        await self.show_cover(t, await loop.run_in_executor(None, self.scripted_art, t), t["player"])
                self.track = t
            elif self.track is not None and now - self.last_seen > self.GONE_S:
                log("nothing playing")
                self.track = None
            if np and np[0].get("art_seq") != self.art_seq:  # the player's artwork changed
                self.art_seq = np[0].get("art_seq")
                if np[1] and self.track and await self.set_cover(np[1]):
                    self.cover_of = self.track["id"]
            cur = self.track
            if np and cur and self.cover_of != cur["id"] and now - self.since >= self.ART_WAIT_S:
                # No artwork from the player for this track (Kaset's stand-in card has none).
                art = await loop.run_in_executor(None, itunes_cover, cur["title"], cur["artist"])
                await self.show_cover(cur, art, "the iTunes Store")
            if self.need_cover:
                self.need_cover = False
                t0 = time.monotonic()
                ok = await self.upload_cover()
                log(f"  cover {'sent' if ok else 'FAILED'}: {len(self.cover or b'')} B in {time.monotonic() - t0:.2f} s")
                if not ok:
                    self.need_cover = True
            self.playing = bool(t and t["playing"])
            self.send_track()


async def main():
    os.makedirs(HOME, mode=0o700, exist_ok=True)
    if os.path.exists(SOCK):
        try:  # another instance answering? then leave it be
            r, w = await asyncio.open_unix_connection(SOCK)
            w.close()
            log("already running")
            return
        except OSError:
            os.unlink(SOCK)
    loop = asyncio.get_running_loop()
    d = Daemon(loop)
    old = os.umask(0o177)  # the socket is owner-only
    server = await asyncio.start_unix_server(d.handle, SOCK)
    os.umask(old)
    d.dev.start()
    log(f"listening on {SOCK}, config {d.cfg}")
    loop.create_task(d.janitor())
    loop.create_task(d.music.run())
    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    asyncio.run(main())
