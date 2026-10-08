#!/usr/bin/env python3
"""quadra_hook_win -- quadra_hook.py on Windows (stdlib only).

The hooks' logic is the Mac's (tools/mac/quadra_hook.py: what each agent's event means, and its
answer in the agent's format); only the way to the service differs. quadrad_win.py listens on
127.0.0.1, on the port in ~/.quadra/agentd.json, and only answers a connection that starts with
the token kept there.

Fail-safe as on the Mac: no service, no file, a timeout or anything else -- no decision, and the
agent asks in its own UI.
"""
import json
import os
import socket
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path[:0] = [HERE, os.path.join(HERE, "..", "mac")]  # installed: next to it; in the repo: tools/mac/
import quadra_hook  # noqa: E402

AGENTD = os.path.join(os.environ.get("QUADRA_HOME") or os.path.expanduser("~/.quadra"), "agentd.json")


def ask_daemon(req: dict, timeout: float):
    """One request, one JSON reply (or None)."""
    try:
        with open(AGENTD) as f:
            a = json.load(f)
        with socket.create_connection(("127.0.0.1", int(a["port"])), timeout=0.5) as s:
            s.settimeout(timeout)
            s.sendall((str(a["token"]) + "\n" + json.dumps(req) + "\n").encode())
            data = b""
            while not data.endswith(b"\n"):
                chunk = s.recv(4096)
                if not chunk:
                    break
                data += chunk
            return json.loads(data) if data.strip() else None
    except (OSError, ValueError, KeyError, TypeError):
        return None


quadra_hook.ask_daemon = ask_daemon

if __name__ == "__main__":
    try:
        # The agents send UTF-8; a Windows pipe would be read as the ANSI code page.
        sys.stdin.reconfigure(encoding="utf-8", errors="replace")
        sys.stdout.reconfigure(encoding="utf-8")
        quadra_hook.main()
    except Exception:
        # As quadra_hook.py: never break the agent; Cursor always gets its neutral answer.
        if sys.argv[1:2] == ["cursor"]:
            print(json.dumps({"continue": True}) if sys.argv[2:3] == ["activity"] else "{}")
