#!/usr/bin/env python3
"""Compose Prefer-ON gate via SailSimToolset MCP (no RunPreferOnGate schema required).

Talks to Unreal editor MCP at http://127.0.0.1:8765/mcp using SailSimToolset tools:
  EnsurePIE → SetCVars (Prefer-ON / DSF2 stick) → poll GetPerfSnapshot until moored==16
  → CapturePlayerView (FramingPreset=midHarborMoored if schema supports it).

Prints JSON: {frameMs_avg,fps,moored,cpvPath,sha,ok,failCode}

Does NOT change MaxBoats / moored deepen. ProfileGPUDump stays parked.
Does NOT kill UnrealEditor.

Env:
  SAILSIM_MCP_URL   default http://127.0.0.1:8765/mcp
  SAILSIM_TIMEOUT_S default 120
  SAILSIM_REPO      optional path for git sha (else cwd / script-relative)
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import time
import urllib.error
import urllib.request
from typing import Any, Dict, List, Optional, Tuple

DEFAULT_URL = "http://127.0.0.1:8765/mcp"
PREFER_ON_CVARS = "r.Lumen.Reflections.Allow=1\nr.Lumen.Reflections.DownsampleFactor=2"
TARGET_MOORED = 16


def git_sha_short(repo: Optional[str]) -> str:
    candidates = []
    if repo:
        candidates.append(repo)
    here = os.path.dirname(os.path.abspath(__file__))
    candidates.append(os.path.dirname(here))
    candidates.append(os.getcwd())
    for root in candidates:
        try:
            out = subprocess.check_output(
                ["git", "-C", root, "rev-parse", "--short", "HEAD"],
                stderr=subprocess.DEVNULL,
                text=True,
            ).strip()
            if out:
                return out
        except Exception:
            continue
    return "unknown"


class McpClient:
    """Minimal streamable-HTTP MCP client with Mcp-Session-Id reuse."""

    def __init__(self, url: str, timeout: float = 120.0) -> None:
        self.url = url
        self.timeout = timeout
        self.session_id: Optional[str] = None
        self._next_id = 0

    def _next(self) -> int:
        self._next_id += 1
        return self._next_id

    def _parse_body(self, content_type: str, raw: bytes) -> Any:
        text = raw.decode("utf-8", errors="replace").strip()
        if not text:
            return None
        if "text/event-stream" in (content_type or "") or text.startswith("event:") or "data:" in text.splitlines()[0]:
            data_lines: List[str] = []
            for line in text.splitlines():
                if line.startswith("data:"):
                    data_lines.append(line[5:].lstrip())
                elif line.strip() == "" and data_lines:
                    # end of one SSE event — prefer last JSON payload
                    pass
            if not data_lines:
                # fall through: try whole body as JSON
                return json.loads(text)
            # Use the last non-empty data payload
            payload = "\n".join(data_lines).strip()
            # multiple events → last JSON object
            last = None
            for chunk in payload.split("\n"):
                chunk = chunk.strip()
                if not chunk:
                    continue
                try:
                    last = json.loads(chunk)
                except json.JSONDecodeError:
                    continue
            if last is None:
                last = json.loads(payload)
            return last
        return json.loads(text)

    def request(self, method: str, params: Optional[Dict[str, Any]] = None, *, notify: bool = False) -> Any:
        payload: Dict[str, Any] = {"jsonrpc": "2.0", "method": method}
        if params is not None:
            payload["params"] = params
        if not notify:
            payload["id"] = self._next()
        data = json.dumps(payload).encode("utf-8")
        headers = {
            "Content-Type": "application/json",
            "Accept": "application/json, text/event-stream",
        }
        if self.session_id:
            headers["Mcp-Session-Id"] = self.session_id
        req = urllib.request.Request(self.url, data=data, headers=headers, method="POST")
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as resp:
                sid = resp.headers.get("Mcp-Session-Id") or resp.headers.get("mcp-session-id")
                if sid:
                    self.session_id = sid
                body = resp.read()
                ctype = resp.headers.get("Content-Type") or ""
                if notify and not body:
                    return None
                return self._parse_body(ctype, body)
        except urllib.error.HTTPError as e:
            err_body = e.read().decode("utf-8", errors="replace")
            raise RuntimeError(f"MCP HTTP {e.code} for {method}: {err_body[:500]}") from e

    def initialize(self) -> None:
        self.request(
            "initialize",
            {
                "protocolVersion": "2024-11-05",
                "capabilities": {},
                "clientInfo": {"name": "ops_run_prefer_on_gate", "version": "1.0"},
            },
        )
        # required follow-up notification (no id)
        try:
            self.request("notifications/initialized", {}, notify=True)
        except Exception:
            # some Unreal MCP builds ignore the notify; continue
            pass

    def list_tools(self) -> List[Dict[str, Any]]:
        result = self.request("tools/list", {})
        if isinstance(result, dict) and "result" in result:
            tools = result["result"].get("tools") or []
        elif isinstance(result, dict) and "tools" in result:
            tools = result["tools"]
        else:
            tools = []
        return list(tools)

    def call_tool(self, name: str, arguments: Optional[Dict[str, Any]] = None) -> Any:
        result = self.request("tools/call", {"name": name, "arguments": arguments or {}})
        if isinstance(result, dict) and "error" in result and result["error"]:
            raise RuntimeError(f"tools/call {name} error: {result['error']}")
        if isinstance(result, dict) and "result" in result:
            return result["result"]
        return result


def resolve_tool_names(tools: List[Dict[str, Any]]) -> Dict[str, str]:
    """Map short names → full MCP tool names (SailSimToolset.* preferred)."""
    by_suffix: Dict[str, str] = {}
    for t in tools:
        name = t.get("name") or ""
        short = name.split(".")[-1].split("_")[-1]
        # Prefer SailSimToolset-prefixed names
        prev = by_suffix.get(short)
        if prev is None or name.startswith("SailSimToolset"):
            by_suffix[short] = name
        # also index full suffixes after last dot
        if "." in name:
            by_suffix[name.split(".")[-1]] = name
    return by_suffix


def tool_has_arg(tools: List[Dict[str, Any]], full_name: str, arg: str) -> bool:
    for t in tools:
        if t.get("name") != full_name:
            continue
        schema = t.get("inputSchema") or t.get("parameters") or {}
        props = schema.get("properties") or {}
        return arg in props
    return False


def extract_text_payload(tool_result: Any) -> str:
    """Unwrap MCP content[] / structuredContent into a string (often JSON)."""
    if tool_result is None:
        return ""
    if isinstance(tool_result, str):
        return tool_result
    if isinstance(tool_result, (int, float, bool)):
        return json.dumps(tool_result)
    if isinstance(tool_result, dict):
        if "structuredContent" in tool_result and tool_result["structuredContent"] is not None:
            sc = tool_result["structuredContent"]
            return sc if isinstance(sc, str) else json.dumps(sc)
        content = tool_result.get("content")
        if isinstance(content, list):
            parts = []
            for c in content:
                if isinstance(c, dict):
                    if c.get("type") == "text" and "text" in c:
                        parts.append(c["text"])
                    elif "json" in c:
                        parts.append(json.dumps(c["json"]))
            if parts:
                return "\n".join(parts)
        # maybe the dict itself is the JSON payload
        return json.dumps(tool_result)
    return str(tool_result)


def parse_jsonish(text: str) -> Dict[str, Any]:
    text = (text or "").strip()
    if not text:
        return {}
    # strip code fences if any
    if text.startswith("```"):
        text = text.strip("`")
        if text.startswith("json"):
            text = text[4:].lstrip()
    try:
        obj = json.loads(text)
        return obj if isinstance(obj, dict) else {"value": obj}
    except json.JSONDecodeError:
        # find first { ... }
        start = text.find("{")
        end = text.rfind("}")
        if start >= 0 and end > start:
            try:
                obj = json.loads(text[start : end + 1])
                return obj if isinstance(obj, dict) else {"value": obj}
            except json.JSONDecodeError:
                pass
        return {"raw": text}


def ensure_pie(client: McpClient, tool: str, timeout_s: float) -> Dict[str, Any]:
    deadline = time.time() + min(timeout_s, 60.0)
    last: Dict[str, Any] = {}
    while time.time() < deadline:
        raw = extract_text_payload(client.call_tool(tool, {"MinWorldSeconds": 0.5}))
        last = parse_jsonish(raw)
        code = str(last.get("code") or "")
        if last.get("ok") and (code in ("running", "") or last.get("AlreadyRunning") or last.get("Settled")):
            if last.get("Settled") or code == "running" or last.get("AlreadyRunning"):
                # if Settled false but running, keep polling briefly
                if last.get("Settled") is True or code == "running" or last.get("IsPIERunning") is True:
                    if last.get("Settled") is not False:
                        return last
        if code in ("requested", "already_requested"):
            time.sleep(1.0)
            continue
        if last.get("ok") is False:
            return last
        time.sleep(0.75)
    return last or {"ok": False, "code": "pie_timeout", "error": "EnsurePIE did not settle"}


def poll_moored(client: McpClient, tool: str, timeout_s: float) -> Tuple[Dict[str, Any], List[float], List[float]]:
    deadline = time.time() + timeout_s
    frames: List[float] = []
    fpss: List[float] = []
    last: Dict[str, Any] = {}
    while time.time() < deadline:
        raw = extract_text_payload(client.call_tool(tool, {}))
        last = parse_jsonish(raw)
        hud = last.get("hud") if isinstance(last.get("hud"), dict) else {}
        moored = hud.get("moored", last.get("moored"))
        try:
            moored_i = int(moored) if moored is not None else -1
        except (TypeError, ValueError):
            moored_i = -1
        frame_ms = float(last.get("frameMs") or last.get("frameMs_avg") or 0.0)
        fps = float(last.get("fps") or 0.0)
        if frame_ms > 0:
            frames.append(frame_ms)
        if fps > 0:
            fpss.append(fps)
        last["_moored"] = moored_i
        if moored_i == TARGET_MOORED:
            return last, frames, fpss
        time.sleep(0.75)
    return last, frames, fpss


def capture_player_view(
    client: McpClient,
    tools: List[Dict[str, Any]],
    names: Dict[str, str],
) -> Tuple[str, str, bool]:
    """Returns (cpvPath_or_note, failCode, ok)."""
    cpv = names.get("CapturePlayerView")
    if not cpv:
        return "", "no_cpv_tool", False
    args: Dict[str, Any] = {"MinWorldSeconds": 0.5}
    used_preset = False
    if tool_has_arg(tools, cpv, "FramingPreset"):
        args["FramingPreset"] = "midHarborMoored"
        used_preset = True
    try:
        result = client.call_tool(cpv, args)
    except Exception as e:
        return "", f"cpv_call_failed:{e}", False

    # Image tools often return path in structured content / text
    text = extract_text_payload(result)
    path = ""
    if isinstance(result, dict):
        for key in ("path", "cpvPath", "filePath", "uri", "localPath"):
            if result.get(key):
                path = str(result[key])
                break
        sc = result.get("structuredContent")
        if isinstance(sc, dict):
            for key in ("path", "cpvPath", "filePath", "uri"):
                if sc.get(key):
                    path = str(sc[key])
                    break
        # ToolsetImage may nest under content image metadata
        content = result.get("content")
        if isinstance(content, list):
            for c in content:
                if isinstance(c, dict):
                    for key in ("path", "uri", "data"):
                        if c.get(key) and key != "data":
                            path = str(c[key])
    obj = parse_jsonish(text)
    if not path:
        path = str(obj.get("cpvPath") or obj.get("path") or obj.get("AbsPath") or "")
    note = ""
    if not used_preset:
        note = "FramingPreset not in schema; CapturePlayerView called without midHarborMoored"
    if path:
        return path if not note else f"{path} ({note})", "", True
    # success without path — still ok if no error
    if obj.get("ok") is False or obj.get("error"):
        return note, str(obj.get("failCode") or obj.get("code") or "cpv_failed"), False
    return note or text[:200], ("cpv_no_path_note" if note else ""), bool(result)


def main() -> int:
    ap = argparse.ArgumentParser(description="Compose Prefer-ON gate via SailSimToolset MCP")
    ap.add_argument("--url", default=os.environ.get("SAILSIM_MCP_URL", DEFAULT_URL))
    ap.add_argument("--timeout", type=float, default=float(os.environ.get("SAILSIM_TIMEOUT_S", "120")))
    ap.add_argument("--repo", default=os.environ.get("SAILSIM_REPO") or "")
    args = ap.parse_args()

    sha = git_sha_short(args.repo or None)
    out: Dict[str, Any] = {
        "frameMs_avg": 0.0,
        "fps": 0.0,
        "moored": -1,
        "cpvPath": "",
        "sha": sha,
        "ok": False,
        "failCode": "",
    }

    client = McpClient(args.url, timeout=max(args.timeout, 30.0))
    try:
        client.initialize()
        tools = client.list_tools()
    except Exception as e:
        out["failCode"] = "mcp_connect"
        out["error"] = str(e)
        print(json.dumps(out, indent=2))
        return 2

    names = resolve_tool_names(tools)

    # Prefer SailSimToolset-qualified names when present
    def pick(*shorts: str) -> Optional[str]:
        for s in shorts:
            if s in names:
                return names[s]
        # fuzzy: any tool ending with the short name
        for t in tools:
            n = t.get("name") or ""
            for s in shorts:
                if n.endswith(s) or n.endswith("." + s) or n.endswith("_" + s):
                    return n
        return None

    ensure = pick("EnsurePIE", "StartPIE")
    set_cvars = pick("SetCVars")
    get_perf = pick("GetPerfSnapshot")
    if not ensure or not set_cvars or not get_perf:
        out["failCode"] = "missing_tools"
        out["error"] = f"need EnsurePIE/SetCVars/GetPerfSnapshot; have={[t.get('name') for t in tools]}"
        print(json.dumps(out, indent=2))
        return 2

    pie = ensure_pie(client, ensure, args.timeout)
    if pie.get("ok") is False and pie.get("code") not in ("running", "requested", "already_requested"):
        out["failCode"] = str(pie.get("code") or "pie_failed")
        out["error"] = pie.get("error") or pie
        print(json.dumps(out, indent=2, default=str))
        return 1

    cvar_raw = extract_text_payload(client.call_tool(set_cvars, {"Assignments": PREFER_ON_CVARS}))
    cvar_obj = parse_jsonish(cvar_raw)
    out["cvars"] = cvar_obj if cvar_obj else cvar_raw

    # settle window after cvars
    settle_budget = max(15.0, min(args.timeout * 0.6, 90.0))
    perf, frames, fpss = poll_moored(client, get_perf, settle_budget)
    moored = int(perf.get("_moored", -1))
    out["moored"] = moored
    if frames:
        out["frameMs_avg"] = sum(frames) / len(frames)
    if fpss:
        out["fps"] = sum(fpss) / len(fpss)
    elif out["frameMs_avg"] > 0.1:
        out["fps"] = 1000.0 / out["frameMs_avg"]

    if moored != TARGET_MOORED:
        out["failCode"] = "moored_count"
        out["error"] = f"expected moored={TARGET_MOORED}, got {moored}"
        print(json.dumps(out, indent=2, default=str))
        return 1

    cpv_path, fail, ok = capture_player_view(client, tools, names)
    out["cpvPath"] = cpv_path
    if not ok:
        out["failCode"] = fail or "cpv_failed"
        out["error"] = fail or "CapturePlayerView failed"
        print(json.dumps(out, indent=2, default=str))
        return 1

    out["ok"] = True
    out["failCode"] = ""
    if cpv_path and "FramingPreset not in schema" in cpv_path:
        out["note"] = "CapturePlayerView schema lacked FramingPreset; left boat where it was"
    print(json.dumps(out, indent=2, default=str))
    return 0


if __name__ == "__main__":
    sys.exit(main())
