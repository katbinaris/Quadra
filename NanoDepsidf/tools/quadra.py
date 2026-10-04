#!/usr/bin/env python3
"""quadra -- host tool for this fork's firmware extensions (src/ext_proto.h).

    quadra.py hello                   firmware and extension versions
    quadra.py profile [name]          list app profiles, or switch to one (saved)
    quadra.py text HELLO              the idle-screen word ("" = QUADRA)
    quadra.py lights [--color custom --hue 200 --effect breathe ... --save]
    quadra.py notify --ask --nudge    test a notification on the knob
    quadra.py reboot [--serial]       restart; --serial = one boot as USB-Serial-JTAG (flashing)
    quadra.py flash [firmware.bin]    serial reboot -> flash the app -> back to HID, no buttons
    quadra.py loop [SECONDS]          the control loop's health over a window (missed ticks, jitter)
    quadra.py wifi-check [--rekey]    the companion's WiFi link against this knob: crypto, USB-only
                                      commands, a stalling peer (needs: pip install cryptography)
    quadra.py home import [FILE]      HOME's lamps: Xiaomi lights from the token extractor's JSON
                                      (default ~/.quadra/xiaomi-devices.json) to the knob, USB only
    quadra.py home list               the lamps on the knob and what it sees of them

Talks to the vendor HID interface (no macOS Input Monitoring permission needed).
Setup: python3 -m pip install hidapi esptool
"""
import argparse
import getpass
import hashlib
import json
import os
import re
import struct
import subprocess
import sys
import tempfile
import time
import unicodedata
import urllib.request

import hid


def share_hid():
    """macOS hidapi opens devices exclusively by default, which would lock the daemon, this CLI
    and the companion app out of each other. Ask for shared opens (no-op elsewhere)."""
    try:
        import ctypes
        ctypes.CDLL(hid.__file__).hid_darwin_set_open_exclusive(0)
    except (OSError, AttributeError):
        pass


share_hid()

REPORT_SIZE = 64
VENDOR_USAGE_PAGE, VENDOR_USAGE = 0xFF00, 0x01
PRODUCT = "Quadra"
USJ_VID, USJ_PID = 0x303A, 0x1001  # the chip's own USB-Serial-JTAG (serial boot, ROM loader)

# src/host_proto.h
CMD_HELLO, TAG_HELLO = 0x10, 0xB0
# src/ext_proto.h
EXT_HELLO, EXT_REBOOT = 0x20, 0x21
EXT_TAG_HELLO, EXT_TAG_ACK = 0xC0, 0xC1
EXT_REBOOT_NORMAL, EXT_REBOOT_SERIAL = 0, 1
EXT_ST = {0: "OK", 1: "BAD_PARAM", 2: "UNKNOWN", 3: "STORAGE"}

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_FW = os.path.join(HERE, "..", ".pio", "build", "esp32-s3-devkitm-1", "firmware.bin")
APP_OFFSET = "0x10000"  # boards/nano_partitions.csv app0


class Quadra:
    def __init__(self):
        found = [d for d in hid.enumerate()
                 if d.get("usage_page") == VENDOR_USAGE_PAGE and d.get("usage") == VENDOR_USAGE
                 and d.get("product_string") == PRODUCT]
        if not found:
            raise SystemExit("no Quadra found (HID mode, vendor interface)")
        self.dev = hid.device()
        self.dev.open_path(found[0]["path"])

    def close(self):
        self.dev.close()

    def send(self, report: bytes):
        report = report.ljust(REPORT_SIZE, b"\0")
        if self.dev.write(b"\0" + report) < 0:
            raise IOError("HID write failed")

    def request(self, report: bytes, tags, timeout_s=1.0):
        """Send one command, return the first reply whose tag is in `tags` (or None)."""
        self.send(report)
        end = time.monotonic() + timeout_s
        while time.monotonic() < end:
            data = self.dev.read(REPORT_SIZE, 100)
            if data and data[0] in tags:
                return bytes(data)
        return None


def cstr(b: bytes) -> str:
    return b.split(b"\0", 1)[0].decode(errors="replace")


def cmd_hello(_args):
    q = Quadra()
    try:
        h = q.request(bytes([CMD_HELLO]), {TAG_HELLO})
        x = q.request(bytes([EXT_HELLO]), {EXT_TAG_HELLO, EXT_TAG_ACK, 0xA0}, 0.5)
    finally:
        q.close()
    if h:
        print(f"firmware {cstr(h[4:36])} ({cstr(h[36:52])}), protocol {h[1]}, {h[2]} profiles")
    print(f"extensions v{x[1]}" if x and x[0] == EXT_TAG_HELLO else "extensions: none (stock firmware)")


CMD_SET, CMD_SAVE, CMD_PROFILE = 0x12, 0x13, 0x16
TAG_SETTINGS, TAG_PROFILE, TAG_ERROR = 0xB1, 0xB2, 0xBF
SET_HID_TYPE, SET_PROFILE, HID_APP = 7, 9, 3


def profiles(q):
    out, i, count = [], 0, 1
    while i < count:
        r = q.request(bytes([CMD_PROFILE, i]), {TAG_PROFILE, TAG_ERROR})
        if not r or r[0] != TAG_PROFILE:
            break
        count = r[2]
        out.append((cstr(r[4:16]), cstr(r[16:32])))
        i += 1
    return out


def cmd_profile(args):
    """List profiles, or make one the active (and saved) APP profile."""
    q = Quadra()
    try:
        ps = profiles(q)
        if not args.name:
            for i, (pid, name) in enumerate(ps):
                print(f"{i}: {pid:12} {name}")
            return
        want = args.name.lower()
        idx = next((i for i, (pid, name) in enumerate(ps) if want in (pid, name.lower())), None)
        if idx is None:
            raise SystemExit(f"no profile '{args.name}' (have: {', '.join(p for p, _ in ps)})")
        for sid, val in ((SET_HID_TYPE, HID_APP), (SET_PROFILE, idx)):
            r = q.request(bytes([CMD_SET, sid, 0, 0]) + val.to_bytes(4, "little"), {TAG_SETTINGS, TAG_ERROR})
            if not r or r[0] != TAG_SETTINGS:
                raise SystemExit("setting refused")
        q.request(bytes([CMD_SAVE]), {TAG_SETTINGS})
        print(f"active profile: {ps[idx][1]} (saved)")
    finally:
        q.close()


EXT_TEXT, EXT_LIGHTS, EXT_PREFS, EXT_NOTIFY = 0x22, 0x23, 0x24, 0x25
EXT_TAG_PREFS, EXT_TAG_NOTIFY = 0xC2, 0xC3
FX = ["gradient", "solid", "breathe", "spin", "rainbow", "off"]
DECISIONS = {1: "ALLOW", 2: "DENY", 3: "LATER", 4: "DISMISS"}


def show_prefs(r: bytes):
    src = "custom" if r[1] == 1 else "app"
    fx = FX[r[2]] if r[2] < len(FX) else r[2]
    hue, level = int.from_bytes(r[3:5], "little"), int.from_bytes(r[7:9], "little")
    print(f"lights: color={src} hue={hue} sat={r[5]}% effect={fx} speed={r[6]} level={level}%"
          + ("  (unsaved)" if r[9] else ""))
    print(f"idle text: {cstr(r[16:32]) or '(QUADRA)'}")


EXT_NET, EXT_TAG_NET, EXT_TAG_KEY = 0x29, 0xC4, 0xC7
NET_SSID, NET_PASS_A, NET_PASS_B, NET_APPLY, NET_STATUS, NET_KEY = 1, 2, 3, 4, 5, 6
NET_STATE = ["off", "connecting", "connected", "network not found", "wrong password"]


def show_net(r: bytes):
    state = NET_STATE[r[1]] if r[1] < len(NET_STATE) else f"state {r[1]}"
    ssid, host = cstr(r[9:41]), cstr(r[41:64])
    line = f"wifi: {'on' if r[8] else 'off'}, {state}" + (f' ("{ssid}")' if ssid else "")
    if r[1] == 2:
        rssi = r[2] - 256 if r[2] > 127 else r[2]
        line += f", {'.'.join(map(str, r[3:7]))}, {rssi} dBm, {host}.local"
        line += ", clock set" if r[7] else ", clock not set yet"
    print(line)


def cmd_wifi(args):
    """WiFi: show it, or set it up. The password is asked for (never echoed, never stored here)."""
    password = None
    if args.ssid is not None:
        if not 0 < len(args.ssid.encode()) <= 32:
            raise SystemExit("SSID: 1-32 bytes")
        password = getpass.getpass(f'password for "{args.ssid}" (empty for an open network): ')
        if len(password.encode()) > 63:
            raise SystemExit("password: 63 characters at most")
    q = Quadra()
    try:
        status = lambda: q.request(bytes([EXT_NET, NET_STATUS]), {EXT_TAG_NET, EXT_TAG_ACK, 0xA0}, 1.0)
        if args.ssid is None and not (args.on or args.off):
            r = status()
        else:
            if args.ssid is not None:
                pw = password.encode().ljust(64, b"\0")
                q.send(bytes([EXT_NET, NET_SSID]) + args.ssid.encode().ljust(32, b"\0"))
                q.send(bytes([EXT_NET, NET_PASS_A]) + pw[:32])
                q.send(bytes([EXT_NET, NET_PASS_B]) + pw[32:64])
            r = q.request(bytes([EXT_NET, NET_APPLY, 0 if args.off else 1]), {EXT_TAG_NET, EXT_TAG_ACK}, 3.0)
            end = time.monotonic() + 20  # until it's connected, or clearly can't
            while not args.off and r and r[0] == EXT_TAG_NET and r[1] == 1 and time.monotonic() < end:
                time.sleep(0.5)
                r = status() or r
    finally:
        q.close()
    if not r or r[0] != EXT_TAG_NET:
        raise SystemExit("no answer (firmware without WiFi?)" if not r or r[0] != EXT_TAG_ACK
                         else f"refused ({EXT_ST.get(r[2], r[2])})")
    show_net(r)


# --- the CLOCK app (src/clock.h): its time, format and zones ---
EXT_TIME, EXT_CLOCK, EXT_TAG_CLOCK = 0x2A, 0x2B, 0xC5
CLOCK_FORMAT, CLOCK_ZONE, CLOCK_GET = 1, 2, 3
CLOCK_FLAGS = [("24h", 0x01), ("seconds", 0x02), ("date", 0x04), ("led", 0x08)]
CLOCK_SLOTS, CLOCK_TZ_MAX = 5, 45
ZONEINFO = "/usr/share/zoneinfo"
_POSIX_HEAD = re.compile(r"^(<[^>]+>|[A-Za-z]{3,})([+-]?\d{1,3}(?::\d{1,2}){0,2})"
                         r"(?:(<[^>]+>|[A-Za-z]{3,})([+-]?\d{1,3}(?::\d{1,2}){0,2})?)?")


def _secs(hms):
    sign = -1 if hms.startswith("-") else 1
    h, m, s = ([int(x) for x in hms.lstrip("+-").split(":")] + [0, 0])[:3]
    return sign * (h * 3600 + m * 60 + s)


def fixed_rule(off):
    """A POSIX rule for a fixed offset (seconds east): 3600 -> "<+01>-1", -12600 -> "<-0330>3:30"."""
    a = abs(off)
    h, m = a // 3600, a % 3600 // 60
    name = f"<{'+' if off >= 0 else '-'}{h:02d}{f'{m:02d}' if m else ''}>"
    return name + ("-" if off > 0 else "") + (f"{h}:{m:02d}" if m else f"{h}")


def zone_label(name):
    """The knob's name for an IANA zone: its city, upper case, 12 characters at most."""
    return name.rsplit("/", 1)[-1].replace("_", " ").upper()[:12]


def zone_rule(name, now=None):
    """The POSIX TZ rule the knob follows for IANA zone `name`: the zone file's own (its footer)
    when that agrees with zoneinfo over the coming year, else the offset it has now, fixed --
    some zones switch on dates no rule can say (Morocco, around Ramadan) or have a change still
    pending. Whoever sends a fixed one sends it again now and then (quadrad: every 5 minutes)."""
    from datetime import datetime, timedelta, timezone
    from zoneinfo import ZoneInfo
    z, now = ZoneInfo(name), now or datetime.now(timezone.utc)
    try:
        with open(os.path.join(ZONEINFO, name), "rb") as f:
            footer = f.read().rstrip(b"\n").rsplit(b"\n", 1)[-1].decode("ascii")
    except (OSError, UnicodeDecodeError):
        footer = ""
    m = _POSIX_HEAD.match(footer)
    if m and len(footer) <= CLOCK_TZ_MAX:
        std = -_secs(m.group(2))
        dst = (-_secs(m.group(4)) if m.group(4) else std + 3600) if m.group(3) else std
        seen = {int((now + timedelta(hours=12 * i)).astimezone(z).utcoffset().total_seconds()) for i in range(732)}
        if seen == {std, dst}:
            return footer
    return fixed_rule(int(now.astimezone(z).utcoffset().total_seconds()))


def local_zone():
    """(label, rule) for this computer's own zone."""
    try:
        name = os.path.realpath("/etc/localtime").split("zoneinfo/", 1)[1]
        return zone_label(name), zone_rule(name)
    except (IndexError, OSError, ValueError, KeyError):
        return "UTC", "UTC0"


def time_report(label, rule):
    """EXT_CMD_TIME: the time now (UTC, ms) and LOCAL's zone."""
    ms = int(time.time() * 1000)
    return (bytes([EXT_TIME]) + ms.to_bytes(6, "little") + label.encode()[:12].ljust(12, b"\0")
            + rule.encode()[:CLOCK_TZ_MAX].ljust(45, b"\0"))


def show_clock_slot(r: bytes):
    off = int.from_bytes(r[4:6], "little", signed=True)
    label, rule = cstr(r[6:18]), cstr(r[18:64])
    sign, a = "+" if off >= 0 else "-", abs(off)
    where = "LOCAL" if r[3] == 0 else f"zone {r[3]}"
    print(f"{where}: {label or '-'}" + (f"  UTC{sign}{a // 60}{f':{a % 60:02d}' if a % 60 else ''}  {rule}" if label else ""))


def cmd_clock(args):
    """The CLOCK app: show it, or change its format / zones. --sync sends the time (as quadrad does)."""
    q = Quadra()
    try:
        get = lambda slot: q.request(bytes([EXT_CLOCK, CLOCK_GET, slot]), {EXT_TAG_CLOCK, EXT_TAG_ACK, 0xA0}, 1.0)
        r = get(0)
        if not r or r[0] != EXT_TAG_CLOCK:
            raise SystemExit("no answer (firmware without the CLOCK app?)")
        if args.sync:
            q.send(time_report(*local_zone()))
        flags = r[1]
        for name, bit in CLOCK_FLAGS:
            v = getattr(args, name.replace("24h", "h24"))
            if v is not None:
                flags = flags | bit if v else flags & ~bit
        if flags != r[1]:
            q.request(bytes([EXT_CLOCK, CLOCK_FORMAT, flags]), {EXT_TAG_CLOCK}, 1.0)
        for slot, name in args.zone or []:
            slot = int(slot)
            if not 1 <= slot < CLOCK_SLOTS:
                raise SystemExit("zones 1-4 (LOCAL is this computer's, from the Mac service)")
            label, rule = "", ""
            if name.lower() != "none":
                label, rule = (args.label or zone_label(name)).upper()[:12], zone_rule(name)
            z = q.request(bytes([EXT_CLOCK, CLOCK_ZONE, slot]) + label.encode().ljust(12, b"\0")
                          + rule.encode().ljust(46, b"\0"), {EXT_TAG_CLOCK, EXT_TAG_ACK}, 1.0)
            if not z or z[0] != EXT_TAG_CLOCK:
                raise SystemExit(f"refused: {rule}")
        time.sleep(0.05)
        r = get(0)
        print("time: " + ("set" if r[2] else "not set yet (WiFi, or the Mac service)") + "; format: "
              + ", ".join(n for n, b in CLOCK_FLAGS if r[1] & b))
        for slot in range(CLOCK_SLOTS):
            show_clock_slot(get(slot))
    finally:
        q.close()


def cmd_text(args):
    t = args.text.upper() if args.upper else args.text
    if len(t) > 12 or any(not (0x20 <= ord(c) <= 0x7E) for c in t):
        raise SystemExit("up to 12 plain ASCII characters")
    q = Quadra()
    try:
        r = q.request(bytes([EXT_TEXT, 0]) + t.encode().ljust(16, b"\0"), {EXT_TAG_ACK, 0xA0}, 2.0)
    finally:
        q.close()
    if not r or r[0] != EXT_TAG_ACK or r[2] != 0:
        raise SystemExit("refused" if r else "no answer (firmware without the extension?)")
    print(f"idle text: {t or '(QUADRA)'}")


def cmd_lights(args):
    q = Quadra()
    try:
        if not any(v is not None for v in (args.color, args.hue, args.sat, args.effect, args.speed, args.level)) and not args.save:
            r = q.request(bytes([EXT_PREFS]), {EXT_TAG_PREFS}, 1.0)
        else:
            def b8(v):
                return 0xFF if v is None else v
            def b16(v):
                return (0xFFFF if v is None else v).to_bytes(2, "little")
            src = None if args.color is None else (1 if args.color == "custom" else 0)
            fx = None if args.effect is None else FX.index(args.effect)
            r = q.request(bytes([EXT_LIGHTS, 1 if args.save else 0, b8(src), b8(fx)]) + b16(args.hue)
                          + bytes([b8(args.sat), b8(args.speed)]) + b16(args.level), {EXT_TAG_PREFS}, 2.0)
    finally:
        q.close()
    if not r:
        raise SystemExit("no answer (firmware without the extension?)")
    show_prefs(r)


def cmd_notify(args):
    """Post one notification straight to the knob (no daemon) and wait for its answer."""
    src = {"claude": 0, "codex": 1, "cursor": 2, "other": 3}[args.agent]
    kind = (0 if args.ask else 1) | (0x80 if args.nudge else 0)
    color = bytes.fromhex(args.color.lstrip("#")) if args.color else b"\0\0\0"
    q = Quadra()
    try:
        q.send(bytes([EXT_NOTIFY, 1, 0x34, 0x12, src, kind]) + args.title.encode()[:16].ljust(16, b"\0")
               + args.body.encode()[:39].ljust(39, b"\0") + color)
        print(f"posted: {args.agent} {'approval' if args.ask else 'info'} -- answer on the knob (Ctrl+C to clear)")
        try:
            while True:
                data = q.dev.read(REPORT_SIZE, 200)
                if data and data[0] == EXT_TAG_NOTIFY and data[2] | data[3] << 8 == 0x1234:
                    print(f"knob: {DECISIONS.get(data[1], data[1])}")
                    return
        except KeyboardInterrupt:
            q.send(bytes([EXT_NOTIFY, 2, 0x34, 0x12]))
            print("cleared")
    finally:
        q.close()


EXT_COVER, EXT_TRACK = 0x26, 0x27


def cmd_cover(args):
    """Show an image as the MUSIC profile's now-playing cover (the service does this for real
    players; this is for testing, or anything it doesn't know)."""
    import io
    import zlib
    from PIL import Image
    img = Image.open(args.image).convert("RGB")
    w, h = img.size
    s = min(w, h)
    img = img.crop(((w - s) // 2, (h - s) // 2, (w + s) // 2, (h + s) // 2)).resize((240, 240), Image.LANCZOS)
    buf = io.BytesIO()
    img.save(buf, "JPEG", quality=88, optimize=True, progressive=False)
    data = buf.getvalue()
    q = Quadra()
    try:
        r = q.request(bytes([EXT_COVER, 1, 0, 0]) + len(data).to_bytes(4, "little")
                      + (zlib.crc32(data) & 0xFFFFFFFF).to_bytes(4, "little"), {EXT_TAG_ACK}, 2.0)
        if not r or r[1] != EXT_COVER or r[2] != 0:
            raise SystemExit("the knob refused the cover (firmware without now playing?)")
        t0 = time.monotonic()
        for off in range(0, len(data), 58):
            piece = data[off:off + 58]
            q.send(bytes([EXT_COVER, 2]) + off.to_bytes(3, "little") + bytes([len(piece)]) + piece)
        r = q.request(bytes([EXT_COVER, 3]), {EXT_TAG_ACK}, 5.0)
        if not r or r[2] != 0:
            raise SystemExit("cover rejected (transfer damaged)")
        color = bytes.fromhex(args.color.lstrip("#")) if args.color else img.resize((1, 1)).getpixel((0, 0))
        pal = bytes(color) * 3
        q.send(bytes([EXT_TRACK, 1, 0xFF]) + pal + args.title.encode()[:24].ljust(24, b"\0")
               + args.artist.encode()[:24].ljust(24, b"\0"))
        print(f"cover: {len(data)} B in {time.monotonic() - t0:.2f} s -- switch the knob to MUSIC to see it")
    finally:
        q.close()


def reboot(serial: bool) -> bool:
    """True if the device acknowledged; False if its firmware has no extensions."""
    q = Quadra()
    try:
        r = q.request(bytes([EXT_REBOOT, EXT_REBOOT_SERIAL if serial else EXT_REBOOT_NORMAL]),
                      {EXT_TAG_ACK, 0xA0}, 1.0)
    finally:
        q.close()
    if not r or r[0] != EXT_TAG_ACK or r[1] != EXT_REBOOT:
        return False
    if r[2] != 0:
        raise SystemExit(f"reboot refused: {EXT_ST.get(r[2], r[2])}")
    return True


EXT_PD, EXT_TAG_PD = 0x2F, 0xC8
PD_RESULT = {0: "ok", 1: "already 5 V 3 A only: nothing written", 2: "busy (the boot read): try again in a few seconds",
             3: "no PD chip found", 4: "couldn't read the NVM",
             5: "the NVM doesn't match the chip's working copy: nothing written",
             6: "written but the read-back differs (twice): don't unplug, and tell the maintainer"}


def cmd_pd(args):
    """The USB-PD chip's NVM (STUSB4500, src/pd_status.h): check it, or with --write-5v make it ask
    for 5 V 3 A only, for good. Saves the NVM as read to ~/.quadra/ first."""
    q = Quadra()
    try:
        r = q.request(bytes([EXT_PD, 1 if args.write_5v else 0]), (EXT_TAG_PD, EXT_TAG_ACK), 5.0)
    finally:
        q.close()
    if not r or r[0] != EXT_TAG_PD:
        raise SystemExit("no answer (firmware before extensions v9, or not over USB)")
    nvm = r[4:44]
    if any(nvm):
        os.makedirs(os.path.expanduser("~/.quadra"), exist_ok=True)
        base = os.path.expanduser(time.strftime("~/.quadra/stusb4500-nvm-%Y%m%d-%H%M%S"))
        path, n = base + ".bin", 1
        while os.path.exists(path):  # never over an earlier copy (it may be the only original)
            path, n = f"{base}-{n}.bin", n + 1
        with open(path, "wb") as f:
            f.write(nvm)
        print("NVM as read:", " ".join(nvm[i:i + 8].hex() for i in range(0, 40, 8)), f"(saved to {path})")
    print(f"sink PDOs in the NVM: {r[2]}" + (f" -> {r[3]}" if args.write_5v and r[1] == 0 else ""))
    if r[1] == 0 and not args.write_5v:
        print("check ok: --write-5v makes it 5 V 3 A only (PDO1), from the next power-up")
    elif r[1] == 0:
        print("written and verified: 5 V 3 A only from the next power-up (unplug and plug in again)")
    elif r[1] == 1:
        print(PD_RESULT[1])
    else:
        raise SystemExit(PD_RESULT.get(r[1], f"result {r[1]}"))


# --- HOME (src/home.h): Xiaomi lamps on the network, run from the knob ---
EXT_HOME, EXT_TAG_HOME = 0x30, 0xCA
HOME_BEGIN, HOME_LAMP, HOME_COMMIT, HOME_STATUS = 1, 2, 3, 4
HOME_MAX_LAMPS, HOME_NAME_LEN = 12, 20
CAP_BRIGHT, CAP_TEMP, CAP_COLOR = 1, 2, 4
KIND_BULB, KIND_DESK, KIND_DESK_ARM, KIND_STRIP = 0, 1, 2, 3  # src/home.h HOME_KIND_*: the icon
PROTO_MIOT, PROTO_LEGACY = 0, 1
MIOT_SPEC = "https://miot-spec.org/miot-spec-v2"
SPEC_CACHE = os.path.expanduser("~/.quadra/miot-spec")
EXTRACTOR_JSON = os.path.expanduser("~/.quadra/xiaomi-devices.json")


def miot_spec(model):
    """A model's MIoT spec (miot-spec.org), cached in ~/.quadra/miot-spec/."""
    os.makedirs(SPEC_CACHE, exist_ok=True)
    path = os.path.join(SPEC_CACHE, model + ".json")
    if os.path.exists(path):
        with open(path) as f:
            return json.load(f)
    with urllib.request.urlopen(f"{MIOT_SPEC}/instances?status=all", timeout=30) as r:
        instances = json.load(r)["instances"]
    found = [i for i in instances if i["model"] == model]
    if not found:
        return None
    best = max(found, key=lambda i: (i.get("status") == "released", i.get("version", 0)))
    with urllib.request.urlopen(f"{MIOT_SPEC}/instance?type={best['type']}", timeout=30) as r:
        spec = json.load(r)
    with open(path, "w") as f:
        json.dump(spec, f)
    return spec


def light_props(spec):
    """From the spec's light service: (siid, piid) of on, brightness, colour temperature, colour, the
    capabilities, and the colour temperature's range."""
    siid, piid, caps, ct = [0] * 4, [0] * 4, 0, (0, 0)
    for svc in spec.get("services", []):
        if svc["type"].split(":")[3] != "light":
            continue
        for p in svc.get("properties", []):
            if "write" not in p.get("access", []):
                continue
            name = p["type"].split(":")[3]
            slot = {"on": 0, "brightness": 1, "color-temperature": 2, "color": 3}.get(name)
            if slot is None or siid[slot]:
                continue
            siid[slot], piid[slot] = svc["iid"], p["iid"]
            if slot == 1:
                caps |= CAP_BRIGHT
            elif slot == 2:
                caps |= CAP_TEMP
                lo, hi = p.get("value-range", [2700, 6500])[:2]
                ct = (int(lo), int(hi))
            elif slot == 3:
                caps |= CAP_COLOR
        break
    return siid, piid, caps, ct


class Miio:
    """Just enough miIO to ask a lamp which protocol it answers (src/home.c does the rest)."""

    def __init__(self, ip, token):
        from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
        import socket
        self.ip, self.token = ip, token
        self.key = hashlib.md5(token).digest()
        self.iv = hashlib.md5(self.key + token).digest()
        self.cipher = lambda: Cipher(algorithms.AES(self.key), modes.CBC(self.iv))
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.settimeout(1.0)
        self.did = self.stamp = None

    def hello(self):
        self.sock.sendto(bytes.fromhex("21310020" + "ff" * 28), (self.ip, 54321))
        try:
            data, _ = self.sock.recvfrom(1024)
        except OSError:
            return False
        self.did, self.stamp = struct.unpack(">II", data[8:16])
        self.t0 = time.monotonic()
        return True

    def call(self, method, params, mid):
        body = json.dumps({"id": mid, "method": method, "params": params}).encode()
        pad = 16 - len(body) % 16
        e = self.cipher().encryptor()
        enc = e.update(body + bytes([pad]) * pad) + e.finalize()
        hdr = struct.pack(">HHIII", 0x2131, 32 + len(enc), 0, self.did, self.stamp + int(time.monotonic() - self.t0) + 1)
        self.sock.sendto(hdr + hashlib.md5(hdr + self.token + enc).digest() + enc, (self.ip, 54321))
        try:
            data, _ = self.sock.recvfrom(4096)
        except OSError:
            return None
        d = self.cipher().decryptor()
        dec = d.update(data[32:]) + d.finalize()
        return json.loads(dec[:-dec[-1]].rstrip(b"\0"))


def probe_proto(ip, token, did, siid, piid):
    """Which protocol a lamp answers: MIoT, the older one, or None (no answer: offline)."""
    try:
        m = Miio(ip, token)
    except ImportError:
        return None
    if not m.hello():
        return None
    props = [{"did": str(did), "siid": siid[i], "piid": piid[i]} for i in range(4) if siid[i]][:1]
    if props and m.call("get_properties", props, 101):
        return PROTO_MIOT
    if m.call("get_prop", ["power"], 102):
        return PROTO_LEGACY
    return None


def find_lamps(ips):
    """Where each miIO device answers now ({did: ip}): a hello to every address of the subnets the
    lamps were last seen on (a router hands out new addresses; the extractor's are from the cloud)."""
    import socket
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(0.05)
    hello = bytes.fromhex("21310020" + "ff" * 28)
    for net in sorted({ip.rsplit(".", 1)[0] for ip in ips}):
        for i in range(1, 255):
            s.sendto(hello, (f"{net}.{i}", 54321))
    seen, end = {}, time.monotonic() + 2.0
    while time.monotonic() < end:
        try:
            data, (ip, _) = s.recvfrom(1024)
        except OSError:
            continue
        if len(data) == 32:
            seen[struct.unpack(">I", data[8:12])[0]] = ip
    s.close()
    return seen


def lamp_kind(model):
    """Which icon the knob draws, from the model name: a strip, a desk lamp (the 1S-style ones
    with a slim arm: lamp1, lamp4), another desk lamp, or a bulb (anything else)."""
    m = model.split(".")[-1]
    if "strip" in m:
        return KIND_STRIP
    if m in ("lamp1", "lamp4"):
        return KIND_DESK_ARM
    if m.startswith("lamp"):
        return KIND_DESK
    return KIND_BULB


def knob_name(name):
    """The knob's font is ASCII upper case: accents dropped, a leading brand word too (every lamp
    here is one), cut to fit."""
    name = name.translate({ord("Ł"): "L", ord("ł"): "l", ord("Ø"): "O", ord("ø"): "o", ord("ß"): "ss"})  # no NFKD form
    n = unicodedata.normalize("NFKD", name).encode("ascii", "ignore").decode().upper().strip()
    n = re.sub(r"^(XIAOMI|MIJIA|MI|YEELIGHT)\s+(?=\S)", "", n)
    return n[:HOME_NAME_LEN - 1].strip() or "LAMP"


def cmd_home_import(args):
    """Every Xiaomi light in the token extractor's output, with its MIoT spec, to the knob."""
    with open(args.file) as f:
        data = json.load(f)
    lamps = []
    devices = [d for server in data for home in server.get("homes", []) for d in home.get("devices", [])]
    where = {} if args.no_probe else find_lamps([d["localip"] for d in devices if d.get("localip")])
    for d in devices:
        model = d.get("model", "")
        if ".light." not in model or not d.get("token") or not d.get("localip"):
            continue
        spec = miot_spec(model)
        if spec is None:
            print(f"  {d.get('name')}: {model} has no MIoT spec, skipped")
            continue
        siid, piid, caps, ct = light_props(spec)
        if not siid[0]:
            print(f"  {d.get('name')}: {model} has no light service, skipped")
            continue
        token = bytes.fromhex(d["token"])
        ip = where.get(int(d["did"]), d["localip"])
        if ip != d["localip"]:
            print(f"  {d.get('name')}: moved from {d['localip']} to {ip}")
        proto = None if args.no_probe else probe_proto(ip, token, int(d["did"]), siid, piid)
        lamps.append(dict(name=knob_name(d.get("name") or model), model=model, did=int(d["did"]),
                          ip=ip, token=token, siid=siid, piid=piid, caps=caps, ct=ct, kind=lamp_kind(model),
                          proto=PROTO_MIOT if proto is None else proto, seen=proto is not None))
    if not lamps:
        raise SystemExit("no Xiaomi lights with tokens in " + args.file)
    if len(lamps) > HOME_MAX_LAMPS:
        print(f"{len(lamps)} lights: the knob takes {HOME_MAX_LAMPS}, the rest are left out")
        lamps = lamps[:HOME_MAX_LAMPS]
    q = Quadra()
    try:
        def ok(r, what):
            if not r or r[0] != EXT_TAG_ACK or r[2] != 0:
                st = EXT_ST.get(r[2], r[2]) if r and r[0] == EXT_TAG_ACK else "no answer"
                raise SystemExit(f"{what}: {st} (firmware before extensions v11, or not over USB?)")
        ok(q.request(bytes([EXT_HOME, HOME_BEGIN]), {EXT_TAG_ACK}), "begin")
        for i, l in enumerate(lamps):
            rep = bytes([EXT_HOME, HOME_LAMP, i]) + struct.pack("<I", l["did"]) + bytes(map(int, l["ip"].split(".")))
            rep += l["token"] + bytes([l["proto"], l["caps"]]) + struct.pack("<HH", *l["ct"])
            rep += bytes(l["siid"]) + bytes(l["piid"]) + l["name"].encode().ljust(HOME_NAME_LEN, b"\0")[:HOME_NAME_LEN]
            rep += bytes([l["kind"]])
            assert len(rep) <= REPORT_SIZE
            ok(q.request(rep, {EXT_TAG_ACK}), f"lamp {i}")
        ok(q.request(bytes([EXT_HOME, HOME_COMMIT, len(lamps)]), {EXT_TAG_ACK}, 3.0), "store")
    finally:
        q.close()
    names = {CAP_BRIGHT: "brightness", CAP_TEMP: "white", CAP_COLOR: "colour"}
    for l in lamps:
        what = ", ".join(v for k, v in names.items() if l["caps"] & k)
        how = ("older protocol" if l["proto"] == PROTO_LEGACY else "MIoT") if l["seen"] else "offline now, MIoT assumed"
        print(f"  {l['name']:<20} {l['model']:<24} {l['ip']:<15} {how}; {what}")
    print(f"{len(lamps)} lamps on the knob: pick HOME in PROFILES")


def cmd_home_list(_args):
    q = Quadra()
    try:
        slot, rows = 0, []
        while slot < HOME_MAX_LAMPS:
            r = q.request(bytes([EXT_HOME, HOME_STATUS, slot]), {EXT_TAG_HOME, EXT_TAG_ACK})
            if not r or r[0] != EXT_TAG_HOME:
                raise SystemExit("no answer (firmware before extensions v11)")
            if slot >= r[1]:
                break
            rows.append(r)
            slot += 1
    finally:
        q.close()
    if not rows:
        print("no lamps: quadra.py home import")
    for r in rows:
        f = r[7]
        state = "offline" if not f & 1 else "no reply" if not f & 2 else "on" if f & 4 else "off"
        if f & 1 and f & 2 and f & 4:
            state += f" {r[8]}%"
            if r[14] & CAP_TEMP:
                state += f" {struct.unpack('<H', r[9:11])[0]} K"
        if f & 8:
            state += " (last change got no reply)"
        print(f"  {cstr(r[15:35]):<20} {state}   #{r[11]:02x}{r[12]:02x}{r[13]:02x}")
    print("(states are read while HOME is on screen)")


def cmd_reboot(args):
    if not reboot(args.serial):
        raise SystemExit("this firmware has no reboot command (stock?)")
    print("restarting" + (" into serial mode" if args.serial else ""))


def usj_port():
    from serial.tools import list_ports
    for p in list_ports.comports():
        if p.vid == USJ_VID and p.pid == USJ_PID:
            return p.device
    return None


def wait_for(fn, timeout_s, what):
    end = time.monotonic() + timeout_s
    while time.monotonic() < end:
        v = fn()
        if v:
            return v
        time.sleep(0.25)
    raise SystemExit(f"timed out waiting for {what}")


def quadra_present():
    return any(d.get("usage_page") == VENDOR_USAGE_PAGE and d.get("product_string") == PRODUCT
               for d in hid.enumerate())


def partition_table(path):
    """{label: (offset, size)} from a partition table image (partitions.bin)."""
    with open(path, "rb") as f:
        data = f.read()
    out = {}
    for i in range(0, len(data) - 31, 32):
        e = data[i:i + 32]
        if e[:2] != b"\xaa\x50":  # the end of the entries
            break
        off, size = struct.unpack_from("<II", e, 4)
        out[e[12:28].split(b"\0")[0].decode()] = (off, size)
    return out


def cmd_flash(args):
    fw = os.path.abspath(args.firmware)
    if not os.path.isfile(fw):
        raise SystemExit(f"no firmware at {fw} (build first: pio run)")
    images = [APP_OFFSET, fw]
    if args.partitions:
        # A new layout: the table at 0x8000, and the region it gives the profile store blanked
        # so that formats cleanly on the first boot. NVS (settings, WiFi) is left alone.
        table = partition_table(args.partitions)
        if table.get("app0", (None,))[0] != int(APP_OFFSET, 16) or "spiffs" not in table:
            raise SystemExit(f"{args.partitions}: not a table with app0 at {APP_OFFSET} and a spiffs partition")
        off, size = table["spiffs"]
        with tempfile.NamedTemporaryFile(delete=False, suffix=".bin") as blank:
            blank.write(b"\xff" * size)
        images = ["0x8000", os.path.abspath(args.partitions), hex(off), blank.name] + images
        print(f"new partition table; the profile store ({size // 1024} KiB at {hex(off)}) starts empty")
    port = usj_port()
    if port is None:
        if quadra_present() and reboot(True):
            print("serial reboot requested")
            port = wait_for(usj_port, 20, "the serial port after the reboot "
                            "(if the device went dark: tap EN, or hold F3+F4 while plugging in)")
        else:
            print("This firmware can't reboot itself into serial mode (first flash of this fork?).\n"
                  "Hold F3+F4 and tap EN (or plug in while holding them); keep holding ~3 s.")
            port = wait_for(usj_port, 600, "the serial port (F3+F4 boot)")
    print(f"flashing {os.path.basename(fw)} via {port}")
    rc = subprocess.call([sys.executable, "-m", "esptool", "--chip", "esp32s3", "--port", port,
                          "--baud", "921600", "--before", "default-reset", "--after", "hard-reset",
                          "write-flash", *images])
    if rc != 0:
        raise SystemExit(f"esptool failed ({rc})")
    wait_for(quadra_present, 30, "the device to come back in HID mode")
    time.sleep(0.5)
    cmd_hello(args)


# --- checks ---

TAG_SYS_B, CMD_STREAM, CMD_RESET_PEAKS = 0xB7, 0x15, 0x18


def cmd_loop(args):
    """SYS INFO's loop numbers over a window: peaks reset, then read (host_proto.h SYS_B)."""
    q = Quadra()

    def sys_b(timeout_s):
        end = time.monotonic() + timeout_s
        while time.monotonic() < end:
            d = q.dev.read(REPORT_SIZE, 100)
            if d and d[0] == TAG_SYS_B:
                return bytes(d)
        return None

    first = sys_b(1.5)
    ours = first is None  # nobody streams (the companion does while it's open): ask, and stop after
    if ours:
        q.send(bytes([CMD_STREAM, 2]))
        first = sys_b(3)
    if first is None:
        raise SystemExit("no SYS report from the knob")
    q.send(bytes([CMD_RESET_PEAKS]))
    time.sleep(args.seconds)
    last = sys_b(3)
    if ours:
        q.send(bytes([CMD_STREAM, 0]))
    q.close()
    if last is None:
        raise SystemExit("no SYS report from the knob")
    _khz, _avg, wmax, jitter = struct.unpack_from("<ffff", last, 8)
    missed, spikes = struct.unpack_from("<If", last, 24)
    free, low = struct.unpack_from("<II", last, 32)
    print(f"{args.seconds:g} s: work max {wmax:.1f} us, jitter max {jitter:.1f} us, "
          f"missed ticks +{missed - struct.unpack_from('<I', first, 24)[0]} (total {missed}), "
          f"spikes {spikes:.2f}/s, heap {free} B free (low {low})")


def cmd_wifi_check(args):
    """The WiFi link (src/net_link.h) end to end. The key comes over USB and is never shown."""
    import hashlib
    import hmac
    import socket
    import threading
    try:
        from cryptography.hazmat.primitives.ciphers.aead import AESGCM
    except ImportError:
        raise SystemExit("needs the cryptography package: python3 -m pip install cryptography")

    q = Quadra()
    hello = q.request(bytes([CMD_HELLO]), (TAG_HELLO,))
    ext = q.request(bytes([EXT_HELLO]), (EXT_TAG_HELLO,))
    if not hello or not ext or ext[1] < 7:
        raise SystemExit("this firmware has no WiFi link (extensions v7)")
    net = q.request(bytes([EXT_NET, NET_STATUS]), (EXT_TAG_NET,))
    if not net or net[1] != 2:
        raise SystemExit("the knob isn't on WiFi (quadra wifi)")
    ip = ".".join(map(str, net[3:7]))
    k = q.request(bytes([EXT_NET, NET_KEY, 0]), (EXT_TAG_KEY,))
    if not k:
        raise SystemExit("no key from the knob")
    key, port = k[1:33], struct.unpack_from("<H", k, 33)[0]
    print(f"{cstr(net[41:64])}.local {ip}:{port}")
    failed = []

    def check(name, ok, detail=""):
        print(f"  {'ok  ' if ok else 'FAIL'} {name}{': ' + detail if detail else ''}")
        if not ok:
            failed.append(name)

    def nonce(d, seq):
        return bytes([d]) + struct.pack("<Q", seq) + b"\0\0\0"

    class Link:
        def __init__(self, key, bad_proof=False):
            self.s = socket.create_connection((ip, port), timeout=3)
            cn = os.urandom(16)
            self.s.sendall(b"QDR1" + cn)
            r = self.recv(36)
            kk = hmac.new(key, b"quadra session" + cn + r[4:20], hashlib.sha256).digest()
            if r[:4] != b"QDR1" or not hmac.compare_digest(hmac.new(kk, b"knob", hashlib.sha256).digest()[:16], r[20:]):
                raise ConnectionError("the knob's proof doesn't match this key")
            proof = hmac.new(kk, b"client", hashlib.sha256).digest()[:16]
            self.s.sendall(bytes([proof[0] ^ 1]) + proof[1:] if bad_proof else proof)
            self.g, self.tx, self.rx = AESGCM(kk), 0, 0

        def recv(self, n):
            b = b""
            while len(b) < n:
                c = self.s.recv(n - len(b))
                if not c:
                    raise ConnectionError("closed by the knob")
                b += c
            return b

        def send(self, report, tamper=False):
            f = self.g.encrypt(nonce(ord("C"), self.tx), bytes(report).ljust(REPORT_SIZE, b"\0"), None)
            self.tx += 1
            self.s.sendall(bytes([f[0] ^ 1]) + f[1:] if tamper else f)

        def read(self, timeout=3):
            self.s.settimeout(timeout)
            p = self.g.decrypt(nonce(ord("K"), self.rx), self.recv(REPORT_SIZE + 16), None)
            self.rx += 1
            return p

        def ask(self, report, tags, timeout=3):
            tags = tags if isinstance(tags, tuple) else (tags,)
            t0 = time.monotonic()
            self.send(report)
            while True:
                p = self.read(timeout)
                if p[0] in tags and (p[0] != EXT_TAG_ACK or p[1] == report[0]):
                    return p, time.monotonic() - t0

        def closed_by_knob(self, timeout=4):
            try:
                while True:
                    self.read(timeout)
            except ConnectionError:
                return True
            except (socket.timeout, OSError):
                return False

    live = Link(key)
    p, _ = live.ask([EXT_HELLO], EXT_TAG_HELLO)
    check("handshake, encrypted round trip", p[1] >= 7)

    # Two clients at once (the companion app and the Mac service), each answered on its own
    # session; a third takes the slot of the one that's been quiet the longest.
    second = Link(key)
    p1, _ = live.ask([EXT_HELLO], EXT_TAG_HELLO)
    p2, _ = second.ask([EXT_HELLO], EXT_TAG_HELLO)
    check("two clients at once, both answered", p1[0] == p2[0] == EXT_TAG_HELLO)
    time.sleep(0.3)
    live.ask([EXT_HELLO], EXT_TAG_HELLO)  # `second` is the quieter one now
    third = Link(key)
    p3, _ = third.ask([EXT_HELLO], EXT_TAG_HELLO)
    check("a third client takes the quietest one's slot", p3[0] == EXT_TAG_HELLO and second.closed_by_knob())
    p1, _ = live.ask([EXT_HELLO], EXT_TAG_HELLO)
    check("...and the other client carries on", p1[0] == EXT_TAG_HELLO)
    third.s.close()
    second.s.close()
    time.sleep(0.3)
    count = hello[2]
    rtt = [live.ask([0x16, i % count], 0xB2)[1] * 1000 for i in range(20)]
    check("round trips", max(rtt) < 250, f"{min(rtt):.0f}/{sum(rtt) / len(rtt):.0f}/{max(rtt):.0f} ms min/avg/max")
    for name, report in (("WiFi setup", [EXT_NET, 1]), ("the key", [EXT_NET, NET_KEY, 1]),
                         ("serial boot", [EXT_REBOOT, EXT_REBOOT_SERIAL])):
        p, _ = live.ask(report, EXT_TAG_ACK)
        check(f"{name}: USB only", p[2] == 4)
    # SET BOOT, to HID: harmless even if a broken build took it (refused is the point)
    p, _ = live.ask([0x12, 10, 0, 0, 1, 0, 0, 0], (0xBF, 0xB1))
    check("boot mode: USB only", p[0] == 0xBF and p[1] == 0x12)

    for name, kw in (("a wrong key", {"key": os.urandom(32)}), ("a bad proof", {"key": key, "bad_proof": True})):
        try:
            Link(**kw).ask([EXT_HELLO], EXT_TAG_HELLO, timeout=2)
            check(f"{name} refused", False)
        except (ConnectionError, OSError):
            check(f"{name} refused", True)

    drip = []

    def dripper():  # a peer that never finishes its hello, a byte at a time
        s = socket.create_connection((ip, port), timeout=5)
        t0 = time.monotonic()
        try:
            for b in b"QDR1" + bytes(16):
                s.sendall(bytes([b]))
                time.sleep(0.4)
            drip.append(None if s.recv(1) else time.monotonic() - t0)
        except OSError:
            drip.append(time.monotonic() - t0)

    t = threading.Thread(target=dripper)
    t.start()
    time.sleep(0.2)
    during = [live.ask([0x16, i % count], 0xB2)[1] * 1000 for i in range(30)]
    t.join()
    check("a stalling peer is cut off", drip[0] is not None and drip[0] < 3, f"after {drip[0] or 0:.1f} s")
    check("...and the live session doesn't wait for it", max(during) < 250, f"max {max(during):.0f} ms")

    # A flood of half-open connections from this address, all held open: the knob keeps one slot
    # per address (each new attempt replaces that address's last), the live session keeps
    # answering, and a handshake from here still goes through. (Other addresses keep their own
    # slots: tools/net_pend_test.)
    stop, held = threading.Event(), []

    def flooder():
        while not stop.is_set() and len(held) < 60:
            try:
                s = socket.create_connection((ip, port), timeout=2)
                s.sendall(b"QDR1"[: len(held) % 5])  # nothing, or part of a hello
                held.append(s)
            except OSError:
                time.sleep(0.01)

    def still_open(s):
        s.setblocking(False)
        try:
            return s.recv(1, socket.MSG_PEEK) != b""
        except BlockingIOError:
            return True
        except OSError:
            return False

    t = threading.Thread(target=flooder)
    t.start()
    time.sleep(0.3)
    during = [live.ask([0x16, i % count], 0xB2)[1] * 1000 for i in range(30)]
    stop.set()
    t.join()
    time.sleep(0.2)
    kept = sum(still_open(s) for s in held)
    check(f"a flood of {len(held)} half-open connections held open: the live session keeps answering",
          max(during) < 250, f"max {max(during):.0f} ms")
    check("...the knob keeps one of them (one slot per address)", kept <= 1, f"{kept} still open")
    try:
        live = Link(key)  # the free slot (the session before keeps its own)
        p, _ = live.ask([EXT_HELLO], EXT_TAG_HELLO)
        check("...and a handshake from the same address goes through", p[0] == EXT_TAG_HELLO)
    except (ConnectionError, OSError) as e:
        check("...and a handshake from the same address goes through", False, str(e))
    for s in held:
        s.close()

    live.send([EXT_HELLO], tamper=True)
    check("a tampered frame ends the session", live.closed_by_knob())

    if args.rekey:
        live = Link(key)
        k2 = q.request(bytes([EXT_NET, NET_KEY, 1]), (EXT_TAG_KEY,))
        check("a new key ends the old session", live.closed_by_knob())
        try:
            Link(key)
            check("the old key is refused", False)
        except (ConnectionError, OSError):
            check("the old key is refused", True)
        p, _ = Link(k2[1:33]).ask([EXT_HELLO], EXT_TAG_HELLO)
        check("the new key works", p[0] == EXT_TAG_HELLO)
        print("  (every paired companion has to pair again: DEVICE > WIFI > PAIR AGAIN)")
    q.close()
    if failed:
        raise SystemExit(f"{len(failed)} failed")
    print("all good")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("hello").set_defaults(fn=cmd_hello)
    p = sub.add_parser("profile", help="list profiles, or switch to one (saved)")
    p.add_argument("name", nargs="?")
    p.set_defaults(fn=cmd_profile)
    p = sub.add_parser("text", help="the idle-screen word ('' = QUADRA), stored on the knob")
    p.add_argument("text")
    p.add_argument("--upper", action="store_true", help="uppercase it")
    p.set_defaults(fn=cmd_text)
    p = sub.add_parser("lights", help="show or change the LED look (live; --save to keep it)")
    p.add_argument("--color", choices=["app", "custom"])
    p.add_argument("--hue", type=lambda v: int(v) % 360)
    p.add_argument("--sat", type=lambda v: max(0, min(100, int(v))))
    p.add_argument("--effect", choices=FX)
    p.add_argument("--speed", type=lambda v: max(1, min(10, int(v))))
    p.add_argument("--level", type=lambda v: max(10, min(200, int(v))), help="%% of the stock brightness")
    p.add_argument("--save", action="store_true")
    p.set_defaults(fn=cmd_lights)
    p = sub.add_parser("cover", help="show an image as MUSIC's now-playing cover")
    p.add_argument("image")
    p.add_argument("--title", default="TEST COVER")
    p.add_argument("--artist", default="QUADRA")
    p.add_argument("--color", help="#RRGGBB for the ring (default: the image's average)")
    p.set_defaults(fn=cmd_cover)
    p = sub.add_parser("notify", help="test: show a notification and print how it was answered")
    p.add_argument("--agent", choices=["claude", "codex", "cursor", "other"], default="claude")
    p.add_argument("--ask", action="store_true", help="an approval (hold F1 / F3) instead of an attention item")
    p.add_argument("--nudge", action="store_true", help="tap gently every few seconds")
    p.add_argument("--color", help="#RRGGBB (default: the agent's)")
    p.add_argument("--title", default="RUN")
    p.add_argument("--body", default="echo hello from the knob")
    p.set_defaults(fn=cmd_notify)
    p = sub.add_parser("wifi", help="show WiFi; --ssid NAME sets it up (asks for the password), --off / --on")
    p.add_argument("--ssid")
    g = p.add_mutually_exclusive_group()
    g.add_argument("--off", action="store_true", help="switch it off (the setup is kept)")
    g.add_argument("--on", action="store_true", help="switch it back on with the stored setup")
    p.set_defaults(fn=cmd_wifi)
    p = sub.add_parser("clock", help="the CLOCK app: show it; --24h/--12h, --[no-]seconds, --[no-]date, "
                                     "--[no-]led, --zone N IANA/Zone|none [--label X], --sync")
    p.add_argument("--24h", dest="h24", action="store_true", default=None)
    p.add_argument("--12h", dest="h24", action="store_false")
    for name in ("seconds", "date", "led"):
        p.add_argument(f"--{name}", dest=name, action="store_true", default=None)
        p.add_argument(f"--no-{name}", dest=name, action="store_false")
    p.add_argument("--zone", nargs=2, action="append", metavar=("N", "ZONE"),
                   help="zone N (1-4): an IANA name (Europe/London), or none; repeatable")
    p.add_argument("--label", help="its name on the knob (default: the city)")
    p.add_argument("--sync", action="store_true", help="send the time and this computer's zone now")
    p.set_defaults(fn=cmd_clock)
    p = sub.add_parser("pd", help="the USB-PD chip's NVM: check it; --write-5v: 5 V 3 A only, for good")
    p.add_argument("--write-5v", action="store_true")
    p.set_defaults(fn=cmd_pd)
    p = sub.add_parser("home", help="HOME's lamps: import them from the token extractor, or list them")
    hs = p.add_subparsers(dest="home_cmd", required=True)
    h = hs.add_parser("import", help="Xiaomi lights from the token extractor's JSON to the knob (USB)")
    h.add_argument("file", nargs="?", default=EXTRACTOR_JSON)
    h.add_argument("--no-probe", action="store_true", help="don't ask each lamp which protocol it answers")
    h.set_defaults(fn=cmd_home_import)
    hs.add_parser("list", help="the lamps on the knob and what it sees of them").set_defaults(fn=cmd_home_list)
    p = sub.add_parser("reboot")
    p.add_argument("--serial", action="store_true", help="one boot as USB-Serial-JTAG (for flashing)")
    p.set_defaults(fn=cmd_reboot)
    p = sub.add_parser("flash")
    p.add_argument("firmware", nargs="?", default=DEFAULT_FW)
    p.add_argument("--partitions", metavar="PARTITIONS_BIN",
                   help="also write this partition table (a new layout: the profile store is cleared)")
    p.set_defaults(fn=cmd_flash)
    p = sub.add_parser("loop", help="the control loop's health over a window (missed ticks, jitter)")
    p.add_argument("seconds", nargs="?", type=float, default=10)
    p.set_defaults(fn=cmd_loop)
    p = sub.add_parser("wifi-check", help="check the companion's WiFi link against this knob")
    p.add_argument("--rekey", action="store_true",
                   help="also check that a new key ends the old session (every companion pairs again)")
    p.set_defaults(fn=cmd_wifi_check)
    args = ap.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()
