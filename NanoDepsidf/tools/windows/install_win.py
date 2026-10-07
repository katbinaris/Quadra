#!/usr/bin/env python3
"""Install (or remove) the Quadra knob's Windows service: agent notifications and now playing.

    py tools\\windows\\install_win.py              install / update
    py tools\\windows\\install_win.py --dry-run    show what would change
    py tools\\windows\\install_win.py --uninstall  take it all out again

What it does (tools/mac/install.py's job, the Windows way):
  * copies quadrad_win.py and quadra_hook_win.py, with the Mac's quadrad.py and quadra_hook.py
    they build on and the quadra.py CLI, to %USERPROFILE%\\.quadra\\, and writes config.json
    (once);
  * runs the service as a scheduled task ("Quadra", at logon, as you, no administrator rights;
    log %LOCALAPPDATA%\\Quadra\\quadrad.log): agent approvals + the AGENTS dashboard, now playing
    for the MUSIC profile, and the CLOCK app's time;
  * adds the same hooks as on the Mac (Claude Code, Codex, Cursor, VS Code Copilot), running
    quadra_hook_win.py; each file is backed up first, and only those entries are ever touched.

The service needs, for this Python: hidapi, Pillow, pycaw, winrt-Windows.Media.Control,
winrt-Windows.Storage.Streams, winrt-Windows.Foundation, tzdata and tzlocal; and cryptography to
reach the knob over WiFi (without it, USB only). The hook script only needs the standard library.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
MAC = os.path.join(HERE, "..", "mac")
sys.path.insert(0, MAC)
import install  # noqa: E402  -- the Mac installer: the hooks' tables and how they're merged

QDIR = install.QDIR
TASK = "Quadra"
LOG = os.path.join(os.environ.get("LOCALAPPDATA") or os.path.expanduser("~"), "Quadra", "quadrad.log")
PY = sys.executable
PYW = os.path.join(os.path.dirname(PY), "pythonw.exe")  # no console window for the service
HOOK = "quadra_hook_win.py"
FILES = [(MAC, "quadrad.py"), (MAC, "quadra_hook.py"), (os.path.join(HERE, ".."), "quadra.py"),
         (HERE, "quadrad_win.py"), (HERE, HOOK)]
NEEDS = [("hid", "hidapi"), ("PIL", "Pillow"), ("pycaw", "pycaw"),
         ("winrt.windows.media.control", "winrt-Windows.Media.Control"),
         ("winrt.windows.storage.streams", "winrt-Windows.Storage.Streams"),
         ("winrt.windows.foundation", "winrt-Windows.Foundation"), ("tzdata", "tzdata"), ("tzlocal", "tzlocal")]


def hook_cmd(agent, event):
    # Forward slashes: the agents run hooks through Git Bash or cmd, and both take them.
    fwd = lambda p: p.replace("\\", "/")  # noqa: E731
    return f'"{fwd(PY)}" "{fwd(os.path.join(QDIR, HOOK))}" {agent} {event}'


# The Mac installer's hook merging, pointed at the Windows hook script.
install.MARK = HOOK
install.hook_cmd = hook_cmd

TASK_XML = """<?xml version="1.0" encoding="UTF-16"?>
<Task version="1.2" xmlns="http://schemas.microsoft.com/windows/2004/02/mit/task">
  <RegistrationInfo><Description>The Quadra knob's service: agent approvals, now playing, the clock.</Description></RegistrationInfo>
  <Triggers><LogonTrigger><Enabled>true</Enabled><UserId>{user}</UserId></LogonTrigger></Triggers>
  <Principals><Principal id="Author"><UserId>{user}</UserId><LogonType>InteractiveToken</LogonType><RunLevel>LeastPrivilege</RunLevel></Principal></Principals>
  <Settings>
    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>
    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>
    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>
    <ExecutionTimeLimit>PT0S</ExecutionTimeLimit>
    <RestartOnFailure><Interval>PT1M</Interval><Count>999</Count></RestartOnFailure>
    <Enabled>true</Enabled>
  </Settings>
  <Actions Context="Author"><Exec><Command>{pyw}</Command><Arguments>"{daemon}"</Arguments><WorkingDirectory>{qdir}</WorkingDirectory></Exec></Actions>
</Task>
"""


def schtasks(*args):
    return subprocess.run(["schtasks", *args], capture_output=True, text=True)


def xml_escape(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;").replace('"', "&quot;")


def daemon(uninstall, dry):
    if dry:
        print(f"--- would {'remove' if uninstall else 'install and start'} the scheduled task {TASK!r} ---")
        return
    schtasks("/End", "/TN", TASK)
    schtasks("/Delete", "/TN", TASK, "/F")
    if uninstall:
        print("service stopped and removed")
        return
    os.makedirs(QDIR, mode=0o700, exist_ok=True)
    os.makedirs(os.path.dirname(LOG), exist_ok=True)
    for src_dir, name in FILES:
        shutil.copy2(os.path.join(src_dir, name), os.path.join(QDIR, name))
    cfg = os.path.join(QDIR, "config.json")
    if not os.path.exists(cfg):
        sys.modules.setdefault("hid", type(sys)("hid"))  # only DEFAULTS is needed here
        from quadrad import DEFAULTS
        with open(cfg, "w") as f:
            json.dump(DEFAULTS, f, indent=2)
    user = f"{os.environ.get('USERDOMAIN', '')}\\{os.environ.get('USERNAME', '')}".lstrip("\\")
    xml = TASK_XML.format(user=xml_escape(user), pyw=xml_escape(PYW), daemon=xml_escape(os.path.join(QDIR, "quadrad_win.py")),
                          qdir=xml_escape(QDIR))
    path = os.path.join(QDIR, "quadra-task.xml")
    with open(path, "w", encoding="utf-16") as f:
        f.write(xml)
    r = schtasks("/Create", "/TN", TASK, "/XML", path, "/F")
    os.remove(path)
    if r.returncode != 0:
        print(f"service NOT installed (schtasks: {(r.stderr or r.stdout).strip()})")
        return
    schtasks("/Run", "/TN", TASK)
    print(f"service installed and started: scheduled task {TASK!r}, log {LOG}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--uninstall", action="store_true")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--only", default="claude,codex,cursor,copilot", help="comma list of agents to (un)hook")
    args = ap.parse_args()
    if sys.platform != "win32":
        raise SystemExit("this is the Windows installer: on a Mac, tools/mac/install.py")
    if not args.uninstall:
        missing = []
        for mod, pkg in NEEDS:
            try:
                __import__(mod)
            except ImportError:
                missing.append(pkg)
        if missing:
            raise SystemExit(f"the service needs these for {PY}: {PY} -m pip install " + " ".join(missing))
        try:
            __import__("cryptography")
        except ImportError:
            print(f"note: without cryptography the service reaches the knob over USB only. For WiFi too: "
                  f"{PY} -m pip install cryptography")
        if not os.path.exists(PYW):
            raise SystemExit(f"no pythonw.exe next to {PY}: install Python from python.org or the Microsoft Store")
    daemon(args.uninstall, args.dry_run)
    install.configure(args.uninstall, args.dry_run, set(args.only.split(",")))
    if not args.uninstall and not args.dry_run:
        print("\nCodex asks you to trust new hooks once: open codex, run /hooks, and approve the quadra ones.")
        print("New Claude Code / Cursor / Copilot sessions pick the hooks up; running ones keep their old set.")
        print("VS Code: leave chat.useClaudeHooks off, or Copilot runs the Claude Code hooks as well.")


if __name__ == "__main__":
    main()
