#!/usr/bin/env python3
"""Install (or remove) the Quadra knob's Mac service: agent notifications and now playing.

    python3 tools/mac/install.py              install / update
    python3 tools/mac/install.py --dry-run    show what would change
    python3 tools/mac/install.py --uninstall  take it all out again

What it does:
  * copies quadrad.py (the service), quadra_hook.py and the quadra.py CLI to ~/.quadra/, writes
    config.json (once), and builds nowplaying.dylib there (the Now Playing helper; needs the Xcode
    Command Line Tools -- without them the service falls back to AppleScript for Music/Spotify);
  * runs the service as a LaunchAgent (com.quadra.daemon, log ~/Library/Logs/quadrad.log): agent
    approvals + the AGENTS dashboard, and now playing for the MUSIC profile;
  * adds hooks next to whatever is already there, in
      ~/.claude/settings.json   (Claude Code: PermissionRequest, Notification, Stop, ...)
      ~/.codex/hooks.json       (Codex: PermissionRequest, Stop, UserPromptSubmit, PostToolUse)
      ~/.cursor/hooks.json      (Cursor: before/after shell + MCP, stop, beforeSubmitPrompt)
    Each file is backed up first (<file>.quadra-backup-<time>). Only entries whose command runs
    quadra_hook.py are ever touched, so re-running updates them in place and --uninstall removes
    exactly those. Codex's single `notify` setting is left alone.

The daemon needs `hidapi` and `Pillow` for this Python (python3 -m pip install --user hidapi
Pillow), and `cryptography` to reach the knob over WiFi (without it, USB only). The hook
script only needs the standard library.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
HOME = os.path.expanduser("~")
QDIR = os.path.join(HOME, ".quadra")
PLIST = os.path.join(HOME, "Library/LaunchAgents/com.quadra.daemon.plist")
LOG = os.path.join(HOME, "Library/Logs/quadrad.log")
LABEL = "com.quadra.daemon"
# The first release ran as com.quadra.agentd / quadra_agentd.py: taken out on (re)install.
OLD_LABEL, OLD_PLIST = "com.quadra.agentd", os.path.join(HOME, "Library/LaunchAgents/com.quadra.agentd.plist")
MARK = "quadra_hook.py"
PY = sys.executable


def hook_cmd(agent, event):
    return f'"{PY}" "{os.path.join(QDIR, MARK)}" {agent} {event}'


# --- what goes in each file ---
CLAUDE = {
    # matcher, event arg, extra fields
    "PermissionRequest": ("*", "permission", {"timeout": 120}),
    "Notification": (None, "notification", {"async": True, "timeout": 10}),
    "Stop": (None, "stop", {"async": True, "timeout": 10}),
    "UserPromptSubmit": (None, "activity", {"async": True, "timeout": 10}),
    "PostToolUse": ("*", "tool-done", {"async": True, "timeout": 10}),
    "SessionEnd": (None, "end", {"async": True, "timeout": 5}),
    "SessionStart": (None, "start", {"async": True, "timeout": 5}),
}
CODEX = {
    "PermissionRequest": (None, "permission", {"timeout": 120}),
    "Stop": (None, "stop", {"timeout": 10}),
    "UserPromptSubmit": (None, "activity", {"timeout": 10}),
    "PostToolUse": (None, "tool-done", {"timeout": 10}),
    "SessionStart": (None, "start", {"timeout": 5}),
}
CURSOR = {
    "beforeShellExecution": "shell",
    "beforeMCPExecution": "mcp",
    "afterShellExecution": "after",
    "afterMCPExecution": "after",
    "stop": "stop",
    "beforeSubmitPrompt": "activity",
    "sessionStart": "start",
    "sessionEnd": "end",
}


def strip_ours_nested(hooks: dict):
    """Claude / Codex layout: {event: [{matcher?, hooks: [{command}]}]}."""
    for event in list(hooks):
        groups = []
        for g in hooks[event]:
            hs = [h for h in g.get("hooks", []) if MARK not in str(h.get("command", ""))]
            if hs:
                groups.append({**g, "hooks": hs})
        if groups:
            hooks[event] = groups
        else:
            del hooks[event]


def add_nested(hooks: dict, table: dict, agent: str):
    for event, (matcher, arg, extra) in table.items():
        group = {"hooks": [{"type": "command", "command": hook_cmd(agent, arg), **extra}]}
        if matcher is not None:
            group = {"matcher": matcher, **group}
        hooks.setdefault(event, []).append(group)


def strip_ours_flat(hooks: dict):
    """Cursor layout: {event: [{command, ...}]}."""
    for event in list(hooks):
        hs = [h for h in hooks[event] if MARK not in str(h.get("command", ""))]
        if hs:
            hooks[event] = hs
        else:
            del hooks[event]


def load(path, default):
    try:
        with open(path) as f:
            return json.load(f)
    except FileNotFoundError:
        return default


def save(path, data, dry):
    text = json.dumps(data, indent=2) + "\n"
    if dry:
        print(f"--- would write {path} ---\n{text}")
        return
    if os.path.exists(path):  # never overwrite an earlier backup (the first one is the pristine file)
        stamp = time.strftime("%Y%m%d-%H%M%S")
        backup, n = f"{path}.quadra-backup-{stamp}", 1
        while os.path.exists(backup):
            backup, n = f"{path}.quadra-backup-{stamp}-{n}", n + 1
        shutil.copy2(path, backup)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".quadra-tmp"
    with open(tmp, "w") as f:
        f.write(text)
    os.replace(tmp, path)


def configure(uninstall, dry, only):
    if "claude" in only:
        p = os.path.join(HOME, ".claude/settings.json")
        s = load(p, {})
        hooks = s.setdefault("hooks", {})
        strip_ours_nested(hooks)
        if not uninstall:
            add_nested(hooks, CLAUDE, "claude")
        if not hooks:
            del s["hooks"]
        save(p, s, dry)
        print(("removed from" if uninstall else "hooked into") + " Claude Code:", p)
    if "codex" in only and os.path.isdir(os.path.join(HOME, ".codex")):
        p = os.path.join(HOME, ".codex/hooks.json")
        s = load(p, {"hooks": {}})
        hooks = s.setdefault("hooks", {})
        strip_ours_nested(hooks)
        if not uninstall:
            add_nested(hooks, CODEX, "codex")
        save(p, s, dry)
        print(("removed from" if uninstall else "hooked into") + " Codex:", p)
    if "cursor" in only and os.path.isdir(os.path.join(HOME, ".cursor")):
        p = os.path.join(HOME, ".cursor/hooks.json")
        s = load(p, {"version": 1, "hooks": {}})
        s.setdefault("version", 1)
        hooks = s.setdefault("hooks", {})
        strip_ours_flat(hooks)
        if not uninstall:
            for event, arg in CURSOR.items():
                hooks.setdefault(event, []).append({"command": hook_cmd("cursor", arg), "timeout": 5})
        save(p, s, dry)
        print(("removed from" if uninstall else "hooked into") + " Cursor:", p)


PLIST_XML = """<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key><string>{label}</string>
    <key>ProgramArguments</key>
    <array><string>{py}</string><string>{daemon}</string></array>
    <key>RunAtLoad</key><true/>
    <key>KeepAlive</key><true/>
    <key>ThrottleInterval</key><integer>5</integer>
    <key>StandardOutPath</key><string>{log}</string>
    <key>StandardErrorPath</key><string>{log}</string>
</dict>
</plist>
"""


def launchctl(*args):
    return subprocess.run(["launchctl", *args], capture_output=True, text=True)


def build_nowplaying():
    """The Now Playing helper (nowplaying.m), for whichever arch /usr/bin/perl runs as."""
    out = os.path.join(QDIR, "nowplaying.dylib")
    tmp = out + ".tmp"  # replace, never rewrite in place: a running helper has the old one mapped
    try:
        r = subprocess.run(["xcrun", "clang", "-dynamiclib", "-fobjc-arc", "-O2", "-arch", "arm64", "-arch",
                            "x86_64", "-framework", "Foundation", "-o", tmp, os.path.join(HERE, "nowplaying.m")],
                           capture_output=True, text=True)
    except OSError as e:
        r = subprocess.CompletedProcess([], 1, "", str(e))
    if r.returncode != 0:
        print("now playing helper NOT built (needs the Command Line Tools: xcode-select --install);"
              " Music/Spotify over AppleScript meanwhile\n  " + r.stderr.strip()[-400:])
        return
    os.replace(tmp, out)
    print("now playing helper built:", out)


def daemon(uninstall, dry):
    domain = f"gui/{os.getuid()}"
    if dry:
        print(f"--- would {'remove' if uninstall else 'install and start'} {PLIST} ---")
        return
    launchctl("bootout", f"{domain}/{LABEL}")
    launchctl("bootout", f"{domain}/{OLD_LABEL}")
    for stale in (OLD_PLIST, os.path.join(QDIR, "quadra_agentd.py")):
        if os.path.exists(stale):
            os.remove(stale)
    if uninstall:
        if os.path.exists(PLIST):
            os.remove(PLIST)
        print("daemon stopped and removed")
        return
    os.makedirs(QDIR, mode=0o700, exist_ok=True)
    for src in ("quadrad.py", "quadra_hook.py", "../quadra.py"):
        dst = os.path.join(QDIR, os.path.basename(src))
        shutil.copy2(os.path.join(HERE, src), dst)
        os.chmod(dst, 0o700)
    build_nowplaying()
    cfg = os.path.join(QDIR, "config.json")
    if not os.path.exists(cfg):
        sys.path.insert(0, HERE)
        sys.modules.setdefault("hid", type(sys)("hid"))  # only DEFAULTS is needed here
        from quadrad import DEFAULTS
        with open(cfg, "w") as f:
            json.dump(DEFAULTS, f, indent=2)
    with open(PLIST, "w") as f:
        f.write(PLIST_XML.format(label=LABEL, py=PY, daemon=os.path.join(QDIR, "quadrad.py"), log=LOG))
    r = launchctl("bootstrap", domain, PLIST)
    print("daemon installed:", PLIST, "" if r.returncode == 0 else f"(launchctl: {r.stderr.strip()})")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--uninstall", action="store_true")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--only", default="claude,codex,cursor", help="comma list of agents to (un)hook")
    args = ap.parse_args()
    if not args.uninstall:
        for mod, pkg in (("hid", "hidapi"), ("PIL", "Pillow")):  # Pillow: MUSIC's cover
            try:
                __import__(mod)
            except ImportError:
                raise SystemExit(f"the daemon needs {pkg} for {PY}: {PY} -m pip install --user {pkg}")
        try:
            __import__("cryptography")
        except ImportError:
            print(f"note: without cryptography the daemon reaches the knob over USB only. For WiFi too: "
                  f"{PY} -m pip install --user cryptography")
    only = set(args.only.split(","))
    daemon(args.uninstall, args.dry_run)
    configure(args.uninstall, args.dry_run, only)
    if not args.uninstall and not args.dry_run:
        print("\nCodex asks you to trust new hooks once: open codex, run /hooks, and approve the quadra ones.")
        print("New Claude Code / Cursor sessions pick the hooks up; running ones keep their old set.")


if __name__ == "__main__":
    main()
