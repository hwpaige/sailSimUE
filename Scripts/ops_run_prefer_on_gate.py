#!/usr/bin/env python3
"""Compose Prefer-ON gate via SailSimToolset MCP (tool-search + direct schemas).

Talks to Unreal editor MCP at http://127.0.0.1:8765/mcp.

Routing:
  1) Port guard on TCP 8765 — UnrealEditor only; CRC → mcp_port_stolen; empty → mcp_down.
  2) Initialize MCP session (Mcp-Session-Id).
  3) If tools/list is meta-only (list_toolsets / describe_toolset / call_tool):
       wait/retry for SailSimToolset via list_toolsets + describe_toolset,
       then prefer call_tool(SailSimToolset.SailSimToolset, RunPreferOnGate),
       else compose EnsurePIE → SetCVars → poll GetPerfSnapshot → CapturePlayerView.
  4) If tools are advertised flat, call them directly (legacy / tool-search off).

Prints JSON: {frameMs_avg,fps,moored,mooringSceneryBudget,mooringSceneryFloor,heroesMaxBoats,heroesNearFullCap,cpvPath,sha,ok,failCode}

Does NOT raise MaxBoats (hero cap stays 1; MaxNearFullBoats stays 1). Harbor fill is
MooringSceneryInstanceCount (floor ~64 / soft ~96). ProfileGPUDump stays parked.
Does NOT kill UnrealEditor.

Env:
  SAILSIM_MCP_URL          default http://127.0.0.1:8765/mcp
  SAILSIM_TIMEOUT_S        default 120 (gate settle / PIE)
  SAILSIM_TOOLSET_WAIT_S   default 90 (wait for SailSimToolset after editor boot / CRC clear)
  SAILSIM_MCP_PORT         default 8765
  SAILSIM_REPO             optional path for git sha
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import time
import urllib.error
import urllib.request
from typing import Any, Dict, List, Optional, Tuple

DEFAULT_URL = "http://127.0.0.1:8765/mcp"
DEFAULT_PORT = 8765
PREFER_ON_CVARS = "r.Lumen.Reflections.Allow=1\nr.Lumen.Reflections.DownsampleFactor=2"
# Harbor fill is HISM scenery only (MooringSceneryInstanceCount default 96).
# Pass when moored >= floor; soft target is the scenery count. Heroes are reported, not the bar.
TARGET_MOORED_MIN = 64  # scenery floor
TARGET_MOORED = 96  # soft scenery target
HERO_MAX_BOATS = 1
HERO_NEAR_FULL = 1
GATE_REPORT_KEYS = (
    "frameMs_avg",
    "fps",
    "moored",
    "mooringSceneryBudget",
    "mooringSceneryFloor",
    "mooredSlots",
    "heroesMaxBoats",
    "heroesNearFullCap",
    "heroesNear",
    "cpvPath",
    "ok",
    "failCode",
    "error",
    "note",
    "sha",
)
META_TOOL_NAMES = frozenset({"list_toolsets", "describe_toolset", "call_tool"})
SAILSIM_TOOLSET_CANDIDATES = (
    "SailSimToolset.SailSimToolset",
    "SailSimToolset",
)
MISSING_TOOLSET_MSG = (
    "SailSimToolset not registered yet; kill CrashReportClient on :8765 "
    "and ensure Unreal MCP bound."
)


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


def _run_capture(cmd: List[str]) -> str:
    try:
        return subprocess.check_output(cmd, stderr=subprocess.DEVNULL, text=True, timeout=10)
    except Exception:
        return ""


def _listener_body(raw: str) -> str:
    """Drop command headers so empty LISTEN tables look empty."""
    lines = []
    for line in (raw or "").splitlines():
        s = line.strip()
        if not s:
            continue
        low = s.lower()
        if low.startswith("command") and "pid" in low:
            continue
        if low.startswith("user") and "command" in low:
            continue
        if low.startswith("proto") and "local" in low:
            continue
        if low.startswith("state") and "recv-q" in low:
            continue
        if low.startswith("active internet"):
            continue
        lines.append(line)
    return "\n".join(lines).strip()


def check_mcp_port(port: int) -> Tuple[str, str]:
    """Return (failCode_or_empty, detail). Empty failCode means UnrealEditor owns the port."""
    text = _listener_body(_run_capture(["lsof", "-nP", f"-iTCP:{port}", "-sTCP:LISTEN"]))
    if not text:
        # FreeBSD / some Macs
        text = _listener_body(_run_capture(["sockstat", "-l", "-p", str(port)]))
    if not text:
        # Linux fallback
        text = _listener_body(_run_capture(["ss", "-ltnp", f"sport = :{port}"]))
    if not text:
        # last resort: any line mentioning the port as local listen
        raw_ss = _run_capture(["ss", "-ltnp"])
        hits = []
        for line in raw_ss.splitlines():
            if f":{port} " in line or line.rstrip().endswith(f":{port}"):
                if "LISTEN" in line.upper() or "LISTEN" in raw_ss.upper():
                    hits.append(line)
        text = "\n".join(hits).strip()
    if not text:
        return (
            "mcp_down",
            f"nothing listening on TCP {port}; start Unreal MCP "
            f"(ModelContextProtocol.StartServer) or enable bAutoStartServer",
        )

    lower = text.lower()
    # Prefer COMMAND / process name tokens
    cmds: List[str] = []
    for line in text.splitlines()[1:] if "\n" in text else [text]:
        parts = line.split()
        if not parts:
            continue
        # lsof: COMMAND PID USER FD TYPE ...
        if parts[0].lower() not in ("command", "user", "proto", "listen"):
            cmds.append(parts[0])
        # ss users:(("UnrealEditor",pid=...))
        for m in re.finditer(r'"([^"]+)"', line):
            cmds.append(m.group(1))
        for m in re.finditer(r"\(([^,)]+)", line):
            token = m.group(1).strip()
            if token and not token.isdigit() and "=" not in token:
                cmds.append(token)

    joined = " ".join(cmds) + " " + text
    joined_l = joined.lower()

    if "crashreportclient" in joined_l or "crashreport" in joined_l:
        return (
            "mcp_port_stolen",
            f"CrashReportClient (or CRC) is listening on TCP {port} — Ops: kill CRC "
            f"before MCP gate so UnrealEditor can bind. Detail: {text.strip()[:300]}",
        )

    unreal_markers = (
        "unrealeditor",
        "unreal editor",
        "ue-editor",
        "ue5editor",
        "ue4editor",
    )
    if any(m in joined_l for m in unreal_markers):
        return "", text.strip()[:300]

    # Something else owns the port
    return (
        "mcp_port_stolen",
        f"TCP {port} listener is not UnrealEditor (got: {', '.join(cmds) or 'unknown'}). "
        f"Ops: kill the process on :{port} (often CrashReportClient) and ensure Unreal MCP bound. "
        f"Detail: {text.strip()[:300]}",
    )


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
        if "text/event-stream" in (content_type or "") or text.startswith("event:") or (
            text.splitlines() and "data:" in text.splitlines()[0]
        ):
            data_lines: List[str] = []
            for line in text.splitlines():
                if line.startswith("data:"):
                    data_lines.append(line[5:].lstrip())
            if not data_lines:
                return json.loads(text)
            payload = "\n".join(data_lines).strip()
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
                "clientInfo": {"name": "ops_run_prefer_on_gate", "version": "1.1"},
            },
        )
        try:
            self.request("notifications/initialized", {}, notify=True)
        except Exception:
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


def tool_names(tools: List[Dict[str, Any]]) -> List[str]:
    return [str(t.get("name") or "") for t in tools]


def is_meta_only(tools: List[Dict[str, Any]]) -> bool:
    names = {n.split(".")[-1] for n in tool_names(tools) if n}
    if not names:
        return False
    # Meta mode when call_tool is present and no SailSim direct tools are advertised
    has_call = "call_tool" in names
    sailsim_direct = any(
        n.endswith("EnsurePIE")
        or n.endswith("SetCVars")
        or n.endswith("GetPerfSnapshot")
        or n.endswith("RunPreferOnGate")
        or n.endswith("CapturePlayerView")
        for n in tool_names(tools)
    )
    return has_call and not sailsim_direct


def resolve_tool_names(tools: List[Dict[str, Any]]) -> Dict[str, str]:
    """Map short names → full MCP tool names (SailSimToolset.* preferred)."""
    by_suffix: Dict[str, str] = {}
    for t in tools:
        name = t.get("name") or ""
        short = name.split(".")[-1].split("_")[-1]
        prev = by_suffix.get(short)
        if prev is None or name.startswith("SailSimToolset"):
            by_suffix[short] = name
        if "." in name:
            by_suffix[name.split(".")[-1]] = name
    return by_suffix


def tool_has_arg(tools: List[Dict[str, Any]], full_or_short: str, arg: str) -> bool:
    target_short = full_or_short.split(".")[-1]
    for t in tools:
        name = str(t.get("name") or "")
        if name != full_or_short and name.split(".")[-1] != target_short:
            continue
        schema = t.get("inputSchema") or t.get("parameters") or t.get("input_schema") or {}
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
        return json.dumps(tool_result)
    return str(tool_result)


def parse_jsonish(text: str) -> Dict[str, Any]:
    text = (text or "").strip()
    if not text:
        return {}
    if text.startswith("```"):
        text = text.strip("`")
        if text.startswith("json"):
            text = text[4:].lstrip()
    try:
        obj = json.loads(text)
        return obj if isinstance(obj, dict) else {"value": obj}
    except json.JSONDecodeError:
        start = text.find("{")
        end = text.rfind("}")
        if start >= 0 and end > start:
            try:
                obj = json.loads(text[start : end + 1])
                return obj if isinstance(obj, dict) else {"value": obj}
            except json.JSONDecodeError:
                pass
        return {"raw": text}


def _unwrap_listish(obj: Any) -> List[Any]:
    if isinstance(obj, list):
        return obj
    if isinstance(obj, dict):
        for key in ("toolsets", "tools", "result", "value", "data"):
            if key in obj:
                inner = obj[key]
                if isinstance(inner, list):
                    return inner
                if isinstance(inner, dict):
                    for k2 in ("toolsets", "tools"):
                        if isinstance(inner.get(k2), list):
                            return inner[k2]
        # maybe dict of name→desc
        if all(isinstance(v, (str, dict)) for v in obj.values()):
            return [{"name": k, **(v if isinstance(v, dict) else {"description": v})} for k, v in obj.items()]
    return []


def parse_toolset_names(payload: Any) -> List[str]:
    text = extract_text_payload(payload)
    obj: Any = parse_jsonish(text) if text else payload
    if not isinstance(obj, (dict, list)):
        obj = payload
    items = _unwrap_listish(obj)
    names: List[str] = []
    for it in items:
        if isinstance(it, str):
            names.append(it)
        elif isinstance(it, dict):
            n = it.get("name") or it.get("toolset") or it.get("toolset_name") or it.get("id")
            if n:
                names.append(str(n))
    # also scan raw text for SailSimToolset.*
    blob = text or (json.dumps(payload) if payload is not None else "")
    for m in re.finditer(r"SailSimToolset(?:\.\w+)?", blob):
        names.append(m.group(0))
    # dedupe preserve order
    seen = set()
    out: List[str] = []
    for n in names:
        if n not in seen:
            seen.add(n)
            out.append(n)
    return out


def parse_described_tools(payload: Any) -> List[Dict[str, Any]]:
    text = extract_text_payload(payload)
    obj: Any = parse_jsonish(text) if text else payload
    if not isinstance(obj, (dict, list)):
        obj = payload
    items = _unwrap_listish(obj)
    tools: List[Dict[str, Any]] = []
    for it in items:
        if isinstance(it, str):
            tools.append({"name": it})
        elif isinstance(it, dict):
            # already a tool schema
            if "name" in it or "inputSchema" in it or "parameters" in it:
                tools.append(it)
            else:
                # nested tools map
                for k, v in it.items():
                    if isinstance(v, dict) and ("inputSchema" in v or "parameters" in v or "description" in v):
                        tools.append({"name": k, **v})
                    elif k == "name":
                        tools.append(it)
                        break
    if not tools and isinstance(obj, dict):
        # describe may return {tools: {RunPreferOnGate: {...}}}
        nested = obj.get("tools")
        if isinstance(nested, dict):
            for k, v in nested.items():
                entry = {"name": k}
                if isinstance(v, dict):
                    entry.update(v)
                tools.append(entry)
    # short-name normalize
    for t in tools:
        if "name" in t and isinstance(t["name"], str):
            t["name"] = t["name"].split(".")[-1]
    return tools


def pick_sailsim_toolset(listed: List[str]) -> Optional[str]:
    lower_map = {n.lower(): n for n in listed}
    for cand in SAILSIM_TOOLSET_CANDIDATES:
        if cand.lower() in lower_map:
            return lower_map[cand.lower()]
    for n in listed:
        if "sailsimtoolset" in n.lower():
            return n
    return None


def meta_name(tools: List[Dict[str, Any]], short: str) -> str:
    for n in tool_names(tools):
        if n == short or n.endswith("." + short) or n.split(".")[-1] == short:
            return n
    return short


class ToolRouter:
    """Invoke SailSim tools either flat or via Unreal tool-search call_tool."""

    def __init__(
        self,
        client: McpClient,
        *,
        mode: str,
        tools: List[Dict[str, Any]],
        toolset_name: str = "",
        direct_names: Optional[Dict[str, str]] = None,
        sail_tools: Optional[List[Dict[str, Any]]] = None,
    ) -> None:
        self.client = client
        self.mode = mode  # "meta" | "direct"
        self.tools = tools  # tools/list (meta or flat)
        self.toolset_name = toolset_name
        self.direct_names = direct_names or {}
        self.sail_tools = sail_tools or []  # describe_toolset schemas (meta mode)
        self._meta_call = meta_name(tools, "call_tool") if mode == "meta" else ""

    def has(self, short: str) -> bool:
        if self.mode == "meta":
            if self.sail_tools:
                return any((t.get("name") or "").split(".")[-1] == short for t in self.sail_tools)
            # describe empty → allow known SailSim surface
            return short in {
                "RunPreferOnGate",
                "EnsurePIE",
                "StartPIE",
                "SetCVars",
                "GetPerfSnapshot",
                "CapturePlayerView",
                "ExecuteConsole",
            }
        return short in self.direct_names or any(
            (t.get("name") or "").endswith(short) for t in self.tools
        )

    def invoke(self, short: str, arguments: Optional[Dict[str, Any]] = None) -> Any:
        args = arguments or {}
        if self.mode == "direct":
            full = self.direct_names.get(short)
            if not full:
                for t in self.tools:
                    n = t.get("name") or ""
                    if n.endswith(short) or n.endswith("." + short) or n.endswith("_" + short):
                        full = n
                        break
            if not full:
                raise RuntimeError(f"direct tool missing: {short}")
            return self.client.call_tool(full, args)
        # meta: call_tool(toolset_name, tool_name, arguments)
        return self.client.call_tool(
            self._meta_call,
            {
                "toolset_name": self.toolset_name,
                "tool_name": short,
                "arguments": args,
            },
        )


def discover_router(
    client: McpClient,
    listed_tools: List[Dict[str, Any]],
    wait_s: float,
) -> Tuple[Optional[ToolRouter], str]:
    """Wait/retry until SailSimToolset is usable. Returns (router, error)."""
    deadline = time.time() + max(0.0, wait_s)
    backoff = 2.0
    last_err = MISSING_TOOLSET_MSG
    attempt = 0

    while True:
        attempt += 1
        tools = listed_tools if attempt == 1 else None
        try:
            if tools is None:
                # re-init is unnecessary; refresh list
                tools = client.list_tools()
        except Exception as e:
            last_err = f"tools/list failed while waiting: {e}"
            tools = []

        if is_meta_only(tools):
            try:
                list_name = meta_name(tools, "list_toolsets")
                desc_name = meta_name(tools, "describe_toolset")
                raw_list = client.call_tool(list_name, {})
                names = parse_toolset_names(raw_list)
                ts = pick_sailsim_toolset(names)
                if not ts:
                    last_err = (
                        f"{MISSING_TOOLSET_MSG} "
                        f"(list_toolsets attempt={attempt} saw={names[:20]})"
                    )
                else:
                    # describe with a few arg key variants
                    described = None
                    desc_err = None
                    for args in (
                        {"toolset_name": ts},
                        {"name": ts},
                        {"toolset": ts},
                    ):
                        try:
                            described = client.call_tool(desc_name, args)
                            break
                        except Exception as e:
                            desc_err = e
                            continue
                    if described is None:
                        last_err = f"describe_toolset failed for {ts}: {desc_err}"
                    else:
                        sail_tools = parse_described_tools(described)
                        # If describe returned empty but toolset exists, still allow known tools
                        if not sail_tools:
                            sail_tools = [
                                {"name": "RunPreferOnGate"},
                                {"name": "EnsurePIE"},
                                {"name": "SetCVars"},
                                {"name": "GetPerfSnapshot"},
                                {"name": "CapturePlayerView", "inputSchema": {"properties": {"FramingPreset": {}, "MinWorldSeconds": {}}}},
                            ]
                        return (
                            ToolRouter(
                                client,
                                mode="meta",
                                tools=tools,  # keep meta names for call_tool
                                toolset_name=ts,
                                sail_tools=sail_tools,
                            ),
                            "",
                        )
            except Exception as e:
                last_err = f"meta discovery failed: {e}"
        else:
            # direct / flat catalog
            names = resolve_tool_names(tools)
            if any(
                k in names
                for k in ("EnsurePIE", "SetCVars", "GetPerfSnapshot", "RunPreferOnGate", "StartPIE")
            ):
                return (
                    ToolRouter(client, mode="direct", tools=tools, direct_names=names),
                    "",
                )
            # Maybe meta tools mixed oddly — try list_toolsets if present
            short_names = {n.split(".")[-1] for n in tool_names(tools)}
            if "list_toolsets" in short_names and "call_tool" in short_names:
                # force meta path next loop by treating as meta
                listed_tools = tools
                # fall through to wait
                last_err = f"{MISSING_TOOLSET_MSG} (flat list had no SailSim tools: {tool_names(tools)[:12]})"
            else:
                last_err = (
                    f"{MISSING_TOOLSET_MSG} "
                    f"(have={tool_names(tools)[:20]})"
                )

        if time.time() >= deadline:
            break
        time.sleep(backoff)
        backoff = min(backoff * 1.5, 15.0)
        listed_tools = []  # force refresh

    return None, last_err


def schema_tools(router: ToolRouter) -> List[Dict[str, Any]]:
    return router.sail_tools or router.tools


def ensure_pie(router: ToolRouter, timeout_s: float) -> Dict[str, Any]:
    deadline = time.time() + min(timeout_s, 60.0)
    last: Dict[str, Any] = {}
    tool = "EnsurePIE" if router.has("EnsurePIE") else "StartPIE"
    while time.time() < deadline:
        raw = extract_text_payload(router.invoke(tool, {"MinWorldSeconds": 0.5}))
        last = parse_jsonish(raw)
        code = str(last.get("code") or "")
        if last.get("ok") and (code in ("running", "") or last.get("AlreadyRunning") or last.get("Settled")):
            if last.get("Settled") or code == "running" or last.get("AlreadyRunning"):
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


def poll_moored(router: ToolRouter, timeout_s: float) -> Tuple[Dict[str, Any], List[float], List[float]]:
    deadline = time.time() + timeout_s
    frames: List[float] = []
    fpss: List[float] = []
    last: Dict[str, Any] = {}
    while time.time() < deadline:
        raw = extract_text_payload(router.invoke("GetPerfSnapshot", {}))
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
        if moored_i >= TARGET_MOORED_MIN:
            return last, frames, fpss
        time.sleep(0.75)
    return last, frames, fpss


def capture_player_view(router: ToolRouter) -> Tuple[str, str, bool]:
    """Returns (cpvPath_or_note, failCode, ok)."""
    if router.mode == "direct" and not router.has("CapturePlayerView"):
        return "", "no_cpv_tool", False
    args: Dict[str, Any] = {"MinWorldSeconds": 0.5}
    used_preset = False
    schemas = schema_tools(router)
    if tool_has_arg(schemas, "CapturePlayerView", "FramingPreset"):
        args["FramingPreset"] = "midHarborMoored"
        used_preset = True
    else:
        cpv = next(
            (t for t in schemas if (t.get("name") or "").split(".")[-1] == "CapturePlayerView"),
            None,
        )
        # Meta + no explicit CapturePlayerView schema, or properties absent → known C++ API has FramingPreset
        if router.mode == "meta" and (
            cpv is None
            or (cpv.get("inputSchema") or cpv.get("parameters") or {}).get("properties") is None
        ):
            args["FramingPreset"] = "midHarborMoored"
            used_preset = True

    try:
        result = router.invoke("CapturePlayerView", args)
    except Exception as e:
        return "", f"cpv_call_failed:{e}", False

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
        content = result.get("content")
        if isinstance(content, list):
            for c in content:
                if isinstance(c, dict):
                    for key in ("path", "uri"):
                        if c.get(key):
                            path = str(c[key])
    obj = parse_jsonish(text)
    if not path:
        path = str(obj.get("cpvPath") or obj.get("path") or obj.get("AbsPath") or "")
    note = ""
    if not used_preset:
        note = "FramingPreset not in schema; CapturePlayerView called without midHarborMoored"
    if path:
        return path if not note else f"{path} ({note})", "", True
    if obj.get("ok") is False or obj.get("error"):
        return note, str(obj.get("failCode") or obj.get("code") or "cpv_failed"), False
    return note or text[:200], ("cpv_no_path_note" if note else ""), bool(result)



def run_via_run_prefer_on_gate(router: ToolRouter) -> Optional[Dict[str, Any]]:
    """If describe shows RunPreferOnGate, call it and map JSON. None = not available."""
    if not router.has("RunPreferOnGate"):
        return None
    try:
        raw = extract_text_payload(router.invoke("RunPreferOnGate", {}))
    except Exception:
        return None
    obj = parse_jsonish(raw)
    if not obj:
        return None
    # Normalize keys
    if "ok" not in obj and "failCode" not in obj and "moored" not in obj:
        return None
    return obj


def compose_gate(router: ToolRouter, timeout_s: float) -> Dict[str, Any]:
    out: Dict[str, Any] = {
        "frameMs_avg": 0.0,
        "fps": 0.0,
        "moored": -1,
        "cpvPath": "",
        "ok": False,
        "failCode": "",
    }
    need = []
    for s in ("EnsurePIE", "StartPIE"):
        if router.has(s):
            break
    else:
        need.append("EnsurePIE")
    for s in ("SetCVars", "GetPerfSnapshot"):
        if not router.has(s):
            # meta mode: assume known API if describe was synthetic/empty
            if router.mode != "meta":
                need.append(s)
    if need:
        out["failCode"] = "missing_tools"
        out["error"] = f"need {need}; {MISSING_TOOLSET_MSG}"
        return out

    pie = ensure_pie(router, timeout_s)
    if pie.get("ok") is False and pie.get("code") not in ("running", "requested", "already_requested"):
        out["failCode"] = str(pie.get("code") or "pie_failed")
        out["error"] = pie.get("error") or pie
        return out

    cvar_raw = extract_text_payload(router.invoke("SetCVars", {"Assignments": PREFER_ON_CVARS}))
    cvar_obj = parse_jsonish(cvar_raw)
    out["cvars"] = cvar_obj if cvar_obj else cvar_raw

    settle_budget = max(15.0, min(timeout_s * 0.6, 90.0))
    perf, frames, fpss = poll_moored(router, settle_budget)
    moored = int(perf.get("_moored", -1))
    out["moored"] = moored
    if frames:
        out["frameMs_avg"] = sum(frames) / len(frames)
    if fpss:
        out["fps"] = sum(fpss) / len(fpss)
    elif out["frameMs_avg"] > 0.1:
        out["fps"] = 1000.0 / out["frameMs_avg"]

    out["mooringSceneryBudget"] = TARGET_MOORED
    out["mooringSceneryFloor"] = TARGET_MOORED_MIN
    out["heroesMaxBoats"] = HERO_MAX_BOATS
    out["heroesNearFullCap"] = HERO_NEAR_FULL
    if moored < TARGET_MOORED_MIN:
        out["failCode"] = "moored_count"
        out["error"] = (
            f"expected mid-harbor scenery moored>={TARGET_MOORED_MIN} "
            f"(floor {TARGET_MOORED_MIN}, soft scenery ~{TARGET_MOORED}); "
            f"heroes MaxBoats={HERO_MAX_BOATS} NearFull≤{HERO_NEAR_FULL}; got moored={moored}"
        )
        return out

    cpv_path, fail, ok = capture_player_view(router)
    out["cpvPath"] = cpv_path
    if not ok:
        out["failCode"] = fail or "cpv_failed"
        out["error"] = fail or "CapturePlayerView failed"
        return out

    out["ok"] = True
    out["failCode"] = ""
    if cpv_path and "FramingPreset not in schema" in cpv_path:
        out["note"] = "CapturePlayerView schema lacked FramingPreset; left boat where it was"
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description="Compose Prefer-ON gate via SailSimToolset MCP")
    ap.add_argument("--url", default=os.environ.get("SAILSIM_MCP_URL", DEFAULT_URL))
    ap.add_argument("--timeout", type=float, default=float(os.environ.get("SAILSIM_TIMEOUT_S", "120")))
    ap.add_argument(
        "--toolset-wait",
        type=float,
        default=float(os.environ.get("SAILSIM_TOOLSET_WAIT_S", "90")),
        help="seconds to wait/retry for SailSimToolset after editor boot / CRC clear (default 90)",
    )
    ap.add_argument("--port", type=int, default=int(os.environ.get("SAILSIM_MCP_PORT", str(DEFAULT_PORT))))
    ap.add_argument("--repo", default=os.environ.get("SAILSIM_REPO") or "")
    ap.add_argument("--skip-port-check", action="store_true", help="skip TCP listener guard (debug only)")
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

    if not args.skip_port_check:
        fail, detail = check_mcp_port(args.port)
        if fail:
            out["failCode"] = fail
            out["error"] = detail
            print(json.dumps(out, indent=2))
            return 2

    client = McpClient(args.url, timeout=max(args.timeout, 30.0))
    try:
        client.initialize()
        tools = client.list_tools()
    except Exception as e:
        out["failCode"] = "mcp_connect"
        out["error"] = str(e)
        print(json.dumps(out, indent=2))
        return 2

    router, err = discover_router(client, tools, args.toolset_wait)
    if router is None:
        out["failCode"] = "missing_tools"
        out["error"] = err or MISSING_TOOLSET_MSG
        print(json.dumps(out, indent=2))
        return 2

    out["route"] = router.mode
    if router.toolset_name:
        out["toolset"] = router.toolset_name

    # Prefer one-shot RunPreferOnGate when describe / catalog shows it
    one = run_via_run_prefer_on_gate(router)
    if one is not None:
        for k in GATE_REPORT_KEYS:
            if k in one and one[k] is not None:
                out[k] = one[k]
        out["sha"] = out.get("sha") or sha
        out["via"] = "RunPreferOnGate"
        print(json.dumps(out, indent=2, default=str))
        return 0 if out.get("ok") else 1

    composed = compose_gate(router, args.timeout)
    out.update(composed)
    out["sha"] = sha
    out["via"] = "compose"
    print(json.dumps(out, indent=2, default=str))
    if out.get("failCode") == "missing_tools":
        return 2
    return 0 if out.get("ok") else 1


if __name__ == "__main__":
    sys.exit(main())
