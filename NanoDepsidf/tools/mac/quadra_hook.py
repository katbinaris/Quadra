#!/usr/bin/env python3
"""quadra_hook -- the hook side of the Quadra agent notifications (stdlib only).

Called by Claude Code, Codex, Cursor and VS Code Copilot hooks as `quadra_hook.py <agent>
<event>`, with the hook's JSON on stdin. It forwards a summary to quadra_agentd.py over a Unix socket and, for an
approval, prints the knob's answer in the agent's hook format.

Fail-safe by construction: the only path that prints "allow" is the daemon answering "allow",
which it does only when F1 was held on the knob for that exact request. Anything else -- no
daemon, no knob, a timeout, bad input, an exception -- prints no decision, and the agent asks
in its own UI as usual.
"""
import json
import os
import re
import socket
import sys

SOCK = os.path.join(os.environ.get("QUADRA_HOME") or os.path.expanduser("~/.quadra"), "agentd.sock")
TITLE_MAX, BODY_MAX = 16, 42

SOURCES = {"claude": "claude", "codex": "codex", "cursor": "cursor", "copilot": "copilot"}
# Copilot has no approval event, only PreToolUse before every tool call, and VS Code's own
# prompt waits for the hook: so only these tools (and MCP) go to the knob, and not for long.
COPILOT_ASKS = ("run_in_terminal", "fetch_webpage")
COPILOT_WAIT_S = 20


def ascii_only(s: str) -> str:
    return "".join(c if 0x20 <= ord(c) <= 0x7E else "?" for c in s)


def clip(s: str, n: int) -> str:
    s = ascii_only(" ".join(str(s).split()))
    return s if len(s) <= n else s[: n - 2] + ".."


def summarize(tool: str, inp) -> tuple:
    """(title, body) for an approval: what the agent wants to do, in 16 + 42 characters."""
    inp = inp if isinstance(inp, dict) else {}
    t = tool or "TOOL"
    if t in ("Bash", "shell", "exec", "local_shell", "unified_exec") or "command" in inp and isinstance(inp.get("command"), (str, list)):
        cmd = inp.get("command", "")
        if isinstance(cmd, list):
            cmd = " ".join(str(c) for c in cmd)
        return "RUN", clip(cmd, BODY_MAX)
    if t in ("Edit", "Write", "MultiEdit", "NotebookEdit"):
        path = inp.get("file_path") or inp.get("notebook_path") or ""
        return t.upper(), clip(os.path.basename(path) or path, BODY_MAX)
    if t == "apply_patch":
        patch = inp.get("patch") or inp.get("input") or json.dumps(inp)
        files = re.findall(r"\*\*\* (?:Add|Update|Delete) File: (\S+)", str(patch))
        names = ", ".join(os.path.basename(f) for f in files) or "files"
        return "PATCH", clip(names, BODY_MAX)
    if t == "fetch_webpage":
        urls = inp.get("urls") or [inp.get("url", "")]
        m = re.match(r"https?://([^/]+)", str(urls[0] if isinstance(urls, list) and urls else urls))
        return "FETCH", clip(m.group(1) if m else urls, BODY_MAX)
    if t == "WebFetch":
        m = re.match(r"https?://([^/]+)", str(inp.get("url", "")))
        return "FETCH", clip(m.group(1) if m else inp.get("url", ""), BODY_MAX)
    if t == "WebSearch":
        return "SEARCH", clip(inp.get("query", ""), BODY_MAX)
    if t.startswith("mcp__"):
        parts = t.split("__")
        server, name = (parts[1], parts[-1]) if len(parts) >= 3 else ("mcp", t)
        args = json.dumps(inp, separators=(",", ":"))[:200]
        return clip(name.upper(), TITLE_MAX), clip(f"{server}: {args}", BODY_MAX)
    return clip(t.upper(), TITLE_MAX), clip(json.dumps(inp, separators=(",", ":")), BODY_MAX)


def fingerprint(tool: str, inp) -> str:
    try:
        return f"{tool}:{json.dumps(inp, sort_keys=True)}"[:4000]
    except (TypeError, ValueError):
        return str(tool)


def ask_daemon(req: dict, timeout: float):
    """One request, one JSON reply (or None)."""
    try:
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
            s.settimeout(0.5)
            s.connect(SOCK)
            s.settimeout(timeout)
            s.sendall((json.dumps(req) + "\n").encode())
            data = b""
            while not data.endswith(b"\n"):
                chunk = s.recv(4096)
                if not chunk:
                    break
                data += chunk
            return json.loads(data) if data.strip() else None
    except (OSError, ValueError):
        return None


def tell(req: dict):
    ask_daemon(req, 1.0)


def where(data: dict) -> str:
    roots = data.get("workspace_roots") or []
    return str(data.get("cwd") or (roots[0] if roots else "") or "")


def permission(agent: str, data: dict):
    tool, inp = data.get("tool_name", ""), data.get("tool_input", {})
    title, body = summarize(tool, inp)
    r = ask_daemon({"op": "ask", "source": agent, "session": str(data.get("session_id", "")),
                    "cwd": where(data), "state": "asking",
                    "title": title, "body": body, "fp": fingerprint(tool, inp)}, timeout=900)
    decision = (r or {}).get("decision")
    if decision == "allow":
        out = {"behavior": "allow"}
    elif decision == "deny":
        out = {"behavior": "deny", "message": "Denied on the Quadra knob."}
    else:
        return  # no decision: the agent's own prompt
    print(json.dumps({"hookSpecificOutput": {"hookEventName": "PermissionRequest", "decision": out}}))


def claude_or_codex(agent: str, event: str, data: dict):
    session = str(data.get("session_id", ""))
    base = {"source": agent, "session": session, "cwd": where(data)}
    if event == "permission":
        permission(agent, data)
    elif event == "start":
        tell({"op": "seen", **base, "state": "idle"})
    elif event == "notification":
        kind = data.get("notification_type", "")
        msg = data.get("message", "")
        if kind == "idle_prompt":
            tell({"op": "info", **base, "state": "turn", "key": "attention",
                  "title": "YOUR TURN", "body": clip(msg or "waiting for your input", BODY_MAX)})
        elif kind in ("elicitation_dialog", "elicitation_url_dialog", "agent_needs_input"):
            tell({"op": "info", **base, "state": "asking", "key": "attention",
                  "title": "NEEDS INPUT", "body": clip(msg or "answer in the app", BODY_MAX)})
        elif kind == "permission_prompt":
            tell({"op": "info", **base, "state": "asking", "key": "inapp", "unless_ask": True,
                  "title": "APPROVE IN APP", "body": clip(msg or "a prompt is waiting", BODY_MAX)})
    elif event == "stop":
        if data.get("stop_hook_active") or data.get("background_tasks"):
            return  # not really idle yet
        tell({"op": "clear", **base, "what": "asks"})
        tell({"op": "info", **base, "state": "turn", "key": "turn", "delay": "turn",
              "title": "YOUR TURN", "body": clip(data.get("last_assistant_message", "") or "done", BODY_MAX)})
    elif event == "tool-done":
        tell({"op": "clear", **base, "state": "working", "what": "fp",
              "fp": fingerprint(data.get("tool_name", ""), data.get("tool_input", {}))})
    elif event == "activity":
        tell({"op": "clear", **base, "state": "working", "what": "all"})
    elif event == "end":
        tell({"op": "clear", **base, "state": "end", "what": "all"})


def copilot(event: str, data: dict):
    if event != "pretool":
        claude_or_codex("copilot", event, data)
        return
    tool, inp = data.get("tool_name", ""), data.get("tool_input", {})
    if tool not in COPILOT_ASKS and not tool.startswith("mcp_"):
        return
    title, body = summarize(tool, inp)
    r = ask_daemon({"op": "ask", "source": "copilot", "session": str(data.get("session_id", "")),
                    "cwd": where(data), "state": "asking", "wait": COPILOT_WAIT_S,
                    "title": title, "body": body, "fp": fingerprint(tool, inp)}, timeout=COPILOT_WAIT_S + 5)
    decision = (r or {}).get("decision")
    if decision not in ("allow", "deny"):
        return  # no decision: VS Code's own prompt (or its auto-approve)
    print(json.dumps({"hookSpecificOutput": {
        "hookEventName": "PreToolUse", "permissionDecision": decision,
        "permissionDecisionReason": "Allowed on the Quadra knob." if decision == "allow" else "Denied on the Quadra knob."}}))


def cursor(event: str, data: dict):
    base = {"source": "cursor", "session": str(data.get("conversation_id", "")), "cwd": where(data)}
    if event in ("shell", "mcp"):
        if event == "shell":
            title, body = "RUN?", clip(data.get("command", ""), BODY_MAX)
        else:
            title = clip((data.get("tool_name") or "MCP").upper(), TITLE_MAX)
            body = clip(f"{data.get('mcp_server_name', 'mcp')}: {data.get('tool_input', '')}", BODY_MAX)
        tell({"op": "info", **base, "state": "working", "key": "run", "delay": "run",
              "title": title, "body": body})
        print("{}")  # no opinion: Cursor's own Run card decides
    elif event == "after":
        tell({"op": "clear", **base, "state": "working", "what": "run"})
        print("{}")
    elif event == "stop":
        tell({"op": "clear", **base, "what": "run"})
        if data.get("status", "completed") == "completed":
            tell({"op": "info", **base, "state": "turn", "key": "turn", "delay": "turn",
                  "title": "YOUR TURN", "body": "Cursor finished"})
        print("{}")
    elif event == "start":
        tell({"op": "seen", **base, "state": "idle"})
        print("{}")
    elif event == "end":
        tell({"op": "clear", **base, "state": "end", "what": "all"})
        print("{}")
    elif event == "activity":
        tell({"op": "clear", **base, "state": "working", "what": "all"})
        print(json.dumps({"continue": True}))


def main():
    if len(sys.argv) < 3 or sys.argv[1] not in SOURCES:
        return
    agent, event = sys.argv[1], sys.argv[2]
    try:
        data = json.loads(sys.stdin.read(1 << 20) or "{}")
    except ValueError:
        data = {}
    if agent == "cursor":
        cursor(event, data)
    elif agent == "copilot":
        copilot(event, data)
    else:
        claude_or_codex(agent, event, data)


if __name__ == "__main__":
    try:
        main()
    except Exception:
        # Never break the agent. Claude Code / Codex / Copilot: no output = no decision. Cursor reads
        # empty output as invalid and blocks, so it always gets its neutral answer.
        if sys.argv[1:2] == ["cursor"]:
            print(json.dumps({"continue": True}) if sys.argv[2:3] == ["activity"] else "{}")
