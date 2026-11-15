#!/usr/bin/env python3
"""REAL functional tests for the skill's four MCP servers (rules R6 + R7).

This script performs actual calls against each MCP server and requires a
technically meaningful answer:

  ida      ida-pro-mcp (mrexodia): JSON-RPC over Streamable HTTP to
           http://127.0.0.1:<port>/mcp - initialize, tools/list, then a
           tools/call of server_health (idb_list on an idalib supervisor
           server; note: no get_metadata tool exists upstream and
           idb_open/idb_list are supervisor-only). PASS only if the answer
           contains real IDB data - a binary/.i64/.idb path for the file
           actually open in IDA. A bare "server reachable" answer or a
           health answer without an open database (idb_path null) is a FAIL.
  cutter   cuttermcp (ap425q): plain HTTP GET /functions against the Cutter
           plugin. The plugin answers text/plain (joined rizin `aflq`
           lines, one function name per line). PASS only if it lists REAL
           function names of the binary loaded in Cutter (>= 3 non-empty
           lines). Port-open alone is a FAIL.
  x64dbg   x64dbg-mcp-server (duty1g): JSON-RPC over Streamable HTTP with
           Bearer token to http://127.0.0.1:<port>/. PASS only if
           GetDebugState (fallback GetEventLog) reports a real debuggee
           (isDebugging: true, RUNNING/PAUSED/LOCKED status, loaded module,
           or event log lines). A "status: NO_TARGET" answer (transport
           alive, but no binary loaded in the debugger) is a FAIL.
  blender  MCP for Blender (ahujasid): JSON-over-TCP socket to
           127.0.0.1:<port>; sends {"type": "get_scene_info", "params": {}}
           (fallback get_addon_info; the addon routes on the top-level
           "type" field). PASS only if the addon answers with real
           scene/addon data.

Before every test, rule R7 runs: the configured port must be free or owned
by the expected server process, must not collide with another MCP server's
configured port, and collisions are reported together with their resolution.

Output:
  - Human-readable detail goes to stderr.
  - stdout ALWAYS carries a single-line JSON report (the CLI is fixed, so
    there is no --json flag; the JSON output is always present for agents).
  - A server that PASSED its real test gets mcpServers.<name>.tested = true
    written to config/environment.json by this script (never on fail).

CLI (exact):
  python scripts/verify_mcp.py [--server ida|cutter|x64dbg|blender|all]

Exit codes:
  0  all requested servers passed their real test and no collision found
  1  one or more tests failed and/or a port collision was detected
  2  usage or configuration error

Python 3.8+, standard library only, paths relative to the script location.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
SKILL_ROOT = SCRIPT_DIR.parent
CONFIG_PATH = SKILL_ROOT / "config" / "environment.json"

# x64dbg-mcp default: 9095 = the 32-bit x32dbg (Crazy Taxi 1 target);
# the 64-bit x64dbg would be 9094 (upstream config.zig: 64-bit build 9094,
# 32-bit build 9095). Change mcpServers.x64dbg.port if the port moved.
DEFAULT_PORTS = {"ida": 13337, "cutter": 8000, "x64dbg": 9095, "blender": 9876}
SERVER_ORDER = ("ida", "cutter", "x64dbg", "blender")
USER_AGENT = "ct1-skill-verify/1.0 (+local skill; stdlib urllib)"
RPC_TIMEOUT = 20

EXPECTED_IMAGES = {
    "ida": ["ida"],
    # No "python" hint: the CutterMCP plugin serves port 8000 from INSIDE
    # Cutter.exe; a python.exe on 8000 is a foreign process (Jupyter, local
    # web apps) and must be reported as a collision (R7).
    "cutter": ["cutter"],
    "x64dbg": ["x64dbg", "x32dbg"],
    "blender": ["blender"],
}

# Real debugger-state evidence; answer formats verified against upstream
# x64dbg-mcp-server src/mcp/tools.zig (GetDebugState, GetEventLog).
X64DBG_STATE_RE = re.compile(
    r"isDebugging:\s*true|status:\s*(?:RUNNING|PAUSED|LOCKED)|"
    r"currentModule:|Events\s*\(\d+/\d+\)", re.IGNORECASE)

# Keys that only a real MCP-for-Blender answer carries (upstream addon.py:
# get_scene_info -> objects/object_count; get_addon_info ->
# capabilities/addon_version/blender_version).
BLENDER_RESULT_KEYS = ("objects", "object_count", "capabilities",
                       "addon_version", "blender_version")

REMEDY_BY_SERVER = {
    "ida": "start IDA and open a binary; the MCP plugin must show its server "
           "as running. If the port moved (ida-pro-mcp auto-increments when "
           "busy), find the real port in the IDA MCP dialog/discovery file "
           "and store it in config/environment.json (mcpServers.ida.port).",
    "cutter": "start Cutter with the game binary loaded; the MCP plugin "
              "panel must show 'HTTP Server: Running (Port 8000)'. Port 8000 "
              "is hardcoded in the plugin - free it from other programs "
              "(Jupyter, local web apps) if it is occupied.",
    "x64dbg": "start x32dbg/x64dbg (Plugins > x64dbg-MCP Server); get the "
              "Bearer token via 'Configure MCP Server...' and store it in "
              "config/environment.json (mcpServers.x64dbg.token).",
    "blender": "start Blender, enable the 'MCP for Blender' addon and press "
               "Start MCP Server in the N-Panel; only ONE MCP server "
               "instance may talk to a Blender instance (addon README rule).",
}


def eprint(msg):
    sys.stderr.write(str(msg) + "\n")


def load_config():
    if not CONFIG_PATH.exists():
        return {}
    try:
        with open(CONFIG_PATH, "r", encoding="utf-8") as fh:
            cfg = json.load(fh)
        return cfg if isinstance(cfg, dict) else {}
    except (OSError, ValueError) as exc:
        eprint("ERROR: cannot read %s: %s" % (CONFIG_PATH, exc))
        raise SystemExit(2)


def toolbox_dir(cfg):
    raw = cfg.get("toolboxDir") or "Toolbox"
    path = Path(raw)
    if not path.is_absolute():
        path = SKILL_ROOT / path
    return path


# --------------------------------------------------------------------------
# R7 port ownership (netstat/tasklist; stdlib subprocess)
# --------------------------------------------------------------------------

def port_pids(port):
    code, out, _ = run_cmd(["netstat", "-ano", "-p", "TCP"], timeout=60)
    pids = []
    if code == 0:
        suffix = ":%d" % port
        for line in out.splitlines():
            tokens = line.split()
            if len(tokens) >= 5 and tokens[0].upper() == "TCP" and \
                    tokens[1].lower().endswith(suffix):
                pid = tokens[4]
                if pid.isdigit() and int(pid) not in pids:
                    pids.append(int(pid))
    return pids


def run_cmd(cmd, timeout=60):
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True,
                              encoding="utf-8", errors="replace",
                              timeout=timeout)
        return (proc.returncode, (proc.stdout or "").strip(),
                (proc.stderr or "").strip())
    except (OSError, subprocess.SubprocessError) as exc:
        return 1, "", str(exc)


def pid_image_name(pid):
    code, out, _ = run_cmd(["tasklist", "/FI", "PID eq %d" % pid,
                            "/FO", "CSV", "/NH"], timeout=60)
    if code == 0 and out and not out.lower().startswith("info:"):
        parts = [p.strip('"') for p in out.splitlines()[0].split('","')]
        if parts and parts[0]:
            return parts[0]
    return None


def port_owner(port):
    pids = port_pids(port)
    if not pids:
        return False, None, None
    for pid in pids:
        name = pid_image_name(pid)
        if name:
            return True, name, pid
    return True, None, pids[0]


def image_matches(name, hints):
    low = (name or "").lower()
    return any(hint in low for hint in hints)


COLLISION_REMEDY = {
    "ida": "ida-pro-mcp auto-increments to the next free port and registers "
           "each instance in its discovery file - find the real port in the "
           "IDA MCP dialog/discovery registry and store it in "
           "config/environment.json (mcpServers.ida.port), or stop the "
           "foreign process.",
    "cutter": "cuttermcp listens HARDCODED on port 8000 - the port cannot "
              "be changed. Stop the foreign process (e.g. Jupyter, local "
              "web apps) before using the Cutter MCP.",
    "x64dbg": "change the port in x64dbg under Plugins > x64dbg-MCP Server > "
              "Configure MCP Server... (persisted to mcp_config.json), then "
              "store it in config/environment.json (mcpServers.x64dbg.port), "
              "or stop the foreign process.",
    "blender": "change the port in the Blender addon panel AND start the "
               "server with `uvx mcp-for-blender --port N` (both sides must "
               "match), then store it in config/environment.json "
               "(mcpServers.blender.port), or stop the foreign process.",
}


def precheck_port(cfg, name):
    """R7 pre-check. Returns a dict with port state and collision info."""
    entry = (cfg.get("mcpServers") or {}).get(name) or {}
    try:
        port = int(entry.get("port") or DEFAULT_PORTS[name])
    except (TypeError, ValueError):
        port = DEFAULT_PORTS[name]

    result = {"server": name, "port": port, "listening": False,
              "owner": None, "pid": None, "collision": False,
              "problem": None, "remedy": None}

    # Config-level collision: two servers on one port.
    servers = cfg.get("mcpServers") or {}
    peers = []
    for other, other_entry in servers.items():
        if other == name or other not in DEFAULT_PORTS:
            continue
        try:
            other_port = int(other_entry.get("port") or DEFAULT_PORTS[other])
        except (TypeError, ValueError):
            other_port = DEFAULT_PORTS[other]
        if other_port == port:
            peers.append(other)
    if peers:
        result["collision"] = True
        result["problem"] = ("port %d is also configured for MCP server(s): "
                             "%s" % (port, ", ".join(sorted(peers))))
        result["remedy"] = ("each MCP server needs its own port; change one "
                            "of them (see references/mcp-servers.md)")
        return result

    listening, image, pid = port_owner(port)
    result["listening"] = listening
    result["owner"] = image
    result["pid"] = pid
    if listening and not image_matches(image, EXPECTED_IMAGES[name]):
        result["collision"] = True
        result["problem"] = ("port %d is occupied by %s (pid %s), not by the "
                             "%s MCP server" %
                             (port, image or "unknown process", pid, name))
        result["remedy"] = COLLISION_REMEDY[name]
    return result


# --------------------------------------------------------------------------
# generic Streamable-HTTP MCP client (ida-pro-mcp / x64dbg-mcp-server)
# --------------------------------------------------------------------------

def http_post_json(url, payload, headers, timeout=RPC_TIMEOUT):
    request = urllib.request.Request(
        url, data=json.dumps(payload).encode("utf-8"),
        headers=headers, method="POST")
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            return (response.status, dict(response.headers),
                    response.read().decode("utf-8", "replace"))
    except urllib.error.HTTPError as exc:
        body = ""
        try:
            body = exc.read().decode("utf-8", "replace")
        except OSError:
            pass
        return exc.code, dict(exc.headers or {}), body


def header_value(headers, name):
    """Case-insensitive lookup in the plain header dict returned by
    http_post_json (urllib preserves the casing the server sent)."""
    for key, value in (headers or {}).items():
        if key.lower() == name.lower():
            return value
    return None


def parse_rpc_response(body, content_type):
    """JSON body or SSE stream ('data: {...}' lines) -> first JSON object."""
    if "text/event-stream" in (content_type or "").lower():
        for line in body.splitlines():
            line = line.strip()
            if line.startswith("data:"):
                try:
                    return json.loads(line[5:].strip())
                except ValueError:
                    continue
        return None
    try:
        return json.loads(body)
    except ValueError:
        return None


def rpc_result_text(result):
    """Flatten result.content[0..n].text of a tools/call answer."""
    if not isinstance(result, dict):
        return ""
    content = result.get("content")
    if not isinstance(content, list):
        return json.dumps(result)[:2000]
    texts = []
    for item in content:
        if isinstance(item, dict) and isinstance(item.get("text"), str):
            texts.append(item["text"])
        elif isinstance(item, dict) and item.get("type") == "text":
            texts.append(str(item.get("text", "")))
    return "\n".join(texts)[:4000]


def mcp_streamable_sequence(base_url, token, preferred_tools, timeout=RPC_TIMEOUT):
    """initialize -> notifications/initialized -> tools/list -> tools/call.
    Returns a dict describing how far it got and the final call answer."""
    report = {"transport_ok": False, "tools": [], "call": None,
              "call_text": "", "error": None, "auth_error": False}
    headers = {"Content-Type": "application/json",
               "Accept": "application/json, text/event-stream",
               "User-Agent": USER_AGENT}
    if token:
        headers["Authorization"] = "Bearer " + token

    init = {"jsonrpc": "2.0", "id": 1, "method": "initialize",
            "params": {"protocolVersion": "2024-11-05", "capabilities": {},
                       "clientInfo": {"name": "ct1-skill-verify",
                                      "version": "1.0"}}}
    try:
        status, resp_headers, body = http_post_json(base_url, init, headers,
                                                    timeout)
    except (urllib.error.URLError, OSError, socket.timeout) as exc:
        report["error"] = "connection failed: %s" % exc
        return report
    if status in (401, 403):
        report["auth_error"] = True
        report["error"] = "authentication failed (HTTP %s)" % status
        return report
    if status != 200:
        report["error"] = "initialize returned HTTP %s: %s" % (
            status, body[:200])
        return report
    init_resp = parse_rpc_response(body,
                                   header_value(resp_headers, "Content-Type"))
    if not init_resp or "result" not in init_resp:
        report["error"] = "no initialize result (body: %s)" % body[:200]
        return report
    report["transport_ok"] = True
    session = header_value(resp_headers, "Mcp-Session-Id")
    if session:
        headers["Mcp-Session-Id"] = session

    try:
        http_post_json(base_url,
                       {"jsonrpc": "2.0", "method":
                        "notifications/initialized"}, headers, timeout)
    except (urllib.error.URLError, OSError, socket.timeout):
        pass  # notification; answer optional (202 empty is fine)

    status, resp_headers, body = http_post_json(
        base_url, {"jsonrpc": "2.0", "id": 2, "method": "tools/list"},
        headers, timeout)
    tools_resp = parse_rpc_response(body,
                                    header_value(resp_headers, "Content-Type"))
    if tools_resp and isinstance(tools_resp.get("result"), dict):
        report["tools"] = [t.get("name", "?")
                           for t in tools_resp["result"].get("tools", [])
                           if isinstance(t, dict)]
    if not report["tools"]:
        report["error"] = "tools/list returned no tools (body: %s)" % body[:200]
        return report

    chosen = None
    for want in preferred_tools:
        if want in report["tools"]:
            chosen = want
            break
    if not chosen:
        for name in report["tools"]:
            if any(hint in name.lower() for hint in
                   ("debug", "event", "log", "idb", "state", "scene")):
                chosen = name
                break
    if not chosen:
        chosen = report["tools"][0]

    status, resp_headers, body = http_post_json(
        base_url,
        {"jsonrpc": "2.0", "id": 3, "method": "tools/call",
         "params": {"name": chosen, "arguments": {}}},
        headers, timeout)
    call_resp = parse_rpc_response(body,
                                   header_value(resp_headers, "Content-Type"))
    report["call"] = chosen
    if not call_resp:
        report["error"] = "tools/call response unparsable (body: %s)" % \
            body[:200]
        return report
    if isinstance(call_resp.get("error"), dict):
        report["error"] = "tools/call error: %s" % \
            json.dumps(call_resp["error"])[:300]
        return report
    report["call_text"] = rpc_result_text(call_resp.get("result"))
    return report


# --------------------------------------------------------------------------
# per-server REAL tests
# --------------------------------------------------------------------------

def test_ida(cfg):
    entry = (cfg.get("mcpServers") or {}).get("ida") or {}
    try:
        port = int(entry.get("port") or DEFAULT_PORTS["ida"])
    except (TypeError, ValueError):
        port = DEFAULT_PORTS["ida"]
    url = "http://127.0.0.1:%d/mcp" % port
    # Upstream facts (ida-pro-mcp main, verified): there is no get_metadata
    # tool (only the ida://idb/metadata resource); idb_open/idb_list are
    # supervisor-only idalib tools. server_health is the GUI-plugin tool
    # that reports idb_path/module/input_path of the open database.
    seq = mcp_streamable_sequence(
        url, None, preferred_tools=["server_health", "idb_list"])
    evidence = seq["call_text"] or (json.dumps(seq["tools"])[:200])
    # Documented proof rule (R6, references/mcp-servers.md): the answer must
    # carry a real binary/.i64/.idb path for the file actually open in IDA.
    # Any non-empty but path-free answer (e.g. server_health without an open
    # database, idb_path null) is NOT real IDB data.
    real_data = bool(seq["call_text"]) and bool(
        re.search(r"\.(i64|idb|exe)", seq["call_text"], re.IGNORECASE))
    if real_data:
        return True, "tools/call '%s' returned real IDB data: %s" % (
            seq["call"], seq["call_text"][:300])
    return False, ("no real IDB data in answer (error: %s; tools: %s; "
                   "evidence: %s)" % (seq["error"], seq["tools"][:8],
                                      evidence[:200]))


def test_cutter(cfg):
    entry = (cfg.get("mcpServers") or {}).get("cutter") or {}
    try:
        port = int(entry.get("port") or DEFAULT_PORTS["cutter"])
    except (TypeError, ValueError):
        port = DEFAULT_PORTS["cutter"]
    url = "http://127.0.0.1:%d/functions" % port
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    try:
        with urllib.request.urlopen(request, timeout=RPC_TIMEOUT) as response:
            body = response.read().decode("utf-8", "replace")
    except (urllib.error.URLError, OSError, socket.timeout) as exc:
        return False, ("plugin HTTP API not reachable at %s (%s). Cutter must "
                       "be open with the game binary loaded and the plugin "
                       "panel showing 'HTTP Server: Running (Port 8000)'" %
                       (url, exc))
    # The plugin answers text/plain, NOT JSON: handle_functions returns
    # "\n".join(cutter.cmd("aflq").splitlines()) paginated to 100 lines
    # (verified against CutterMCPPlugin.py).
    lines = [line.strip() for line in body.splitlines() if line.strip()]
    if len(lines) >= 3:
        return True, ("plugin returned %d real function names (rizin aflq), "
                      "e.g. %s" % (len(lines), ", ".join(lines[:3])))
    return False, ("HTTP API answered but no real function list (%d non-empty "
                   "line(s); a loaded binary yields many aflq names; "
                   "response: %s)" % (len(lines), body[:200]))


def _search_token(obj):
    """Depth-first search for a value under a key containing 'token'."""
    if isinstance(obj, dict):
        for key, value in obj.items():
            if "token" in str(key).lower() and isinstance(value, str) and \
                    len(value) >= 16:
                return value, str(key)
            found = _search_token(value)
            if found:
                return found
    elif isinstance(obj, list):
        for item in obj:
            found = _search_token(item)
            if found:
                return found
    return None


def find_x64dbg_token(cfg):
    entry = (cfg.get("mcpServers") or {}).get("x64dbg") or {}
    if entry.get("token"):
        return str(entry["token"]), "config/environment.json"
    base = toolbox_dir(cfg) / "x64dbg"
    if base.is_dir():
        for mcp_config in sorted(base.rglob("mcp_config.json")):
            try:
                with open(str(mcp_config), "r", encoding="utf-8") as fh:
                    data = json.load(fh)
            except (OSError, ValueError):
                continue
            found = _search_token(data)
            if found:
                return found[0], "%s (key: %s)" % (mcp_config, found[1])
    return None, None


def test_x64dbg(cfg):
    entry = (cfg.get("mcpServers") or {}).get("x64dbg") or {}
    try:
        port = int(entry.get("port") or DEFAULT_PORTS["x64dbg"])
    except (TypeError, ValueError):
        port = DEFAULT_PORTS["x64dbg"]
    token, token_source = find_x64dbg_token(cfg)
    if not token:
        return False, ("no Bearer token found. Start x32dbg/x64dbg once, open "
                       "Plugins > x64dbg-MCP Server > Configure MCP Server..., "
                       "copy the auto-generated token and store it in "
                       "config/environment.json (mcpServers.x64dbg.token).")
    seq = mcp_streamable_sequence(
        "http://127.0.0.1:%d/" % port, token,
        preferred_tools=["GetDebugState", "GetEventLog"])
    if seq["auth_error"]:
        return False, "Bearer token rejected (%s)" % seq["error"]
    if not seq["transport_ok"]:
        return False, "server not reachable: %s" % seq["error"]
    text = seq["call_text"]
    if text and X64DBG_STATE_RE.search(text):
        return True, ("token from %s; tools/call '%s' returned real debugger "
                      "state: %s" % (token_source, seq["call"], text[:300]))
    if text and ("NO_TARGET" in text or "isDebugging: false" in text):
        # GetDebugState answers key/value text (upstream tools.zig); without
        # a loaded binary it reports status NO_TARGET - transport works, but
        # there is no debuggee to prove real debugger data (R6: no
        # false-positive pass).
        return False, ("MCP transport OK but no debuggee loaded (tools/call "
                       "'%s': %s) - load the game binary in x32dbg "
                       "(LoadBinary or attach), then re-run" %
                       (seq["call"], text[:200]))
    return False, ("MCP transport OK but no real debugger state data "
                   "(error: %s; tools: %s; answer: %s)" %
                   (seq["error"], seq["tools"][:8], text[:200]))


def test_blender(cfg):
    entry = (cfg.get("mcpServers") or {}).get("blender") or {}
    try:
        port = int(entry.get("port") or DEFAULT_PORTS["blender"])
    except (TypeError, ValueError):
        port = DEFAULT_PORTS["blender"]
    # The addon routes on the TOP-LEVEL "type" field (upstream addon.py:
    # cmd_type = command.get("type"); handler = handlers.get(cmd_type));
    # the old {"type": "command", ...} envelope was answered with
    # "Unknown command type: command". Fallback: get_addon_info
    # (get_addon_status does not exist as an addon socket command).
    commands = ["get_scene_info", "get_addon_info"]
    last_error = "no attempt made"
    for command in commands:
        payload = json.dumps({"type": command, "params": {}}) + "\n"
        buffer = b""
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=5) as \
                    sock:
                sock.settimeout(1.5)
                sock.sendall(payload.encode("utf-8"))
                deadline = 6.0
                end = time.time() + deadline
                while time.time() < end:
                    try:
                        chunk = sock.recv(65536)
                    except socket.timeout:
                        continue
                    if not chunk:
                        break
                    buffer += chunk
                    if b"}" in buffer and len(buffer) > 2:
                        # give a short grace period for the rest of the JSON
                        try:
                            time.sleep(0.2)
                            sock.settimeout(0.5)
                            extra = sock.recv(65536)
                            if extra:
                                buffer += extra
                        except (socket.timeout, OSError):
                            pass
                        break
        except (OSError, socket.timeout) as exc:
            last_error = ("addon socket not reachable on 127.0.0.1:%d (%s). "
                          "Start Blender, enable 'MCP for Blender' and press "
                          "Start MCP Server in the N-Panel" % (port, exc))
            continue
        response = _parse_socket_json(buffer)
        if response is None:
            last_error = ("no/invalid JSON answer for %s (raw: %s)" %
                          (command, buffer.decode("utf-8", "replace")[:150]))
            continue
        status = response.get("status")
        result = response.get("result")
        if status == "success" and isinstance(result, dict) and any(
                key in result for key in BLENDER_RESULT_KEYS):
            return True, ("addon answered '%s' with real data: %s" %
                          (command, json.dumps(result)[:300]))
        last_error = ("answer for %s not successful (status=%r, response=%s)"
                      % (command, status, json.dumps(response)[:200]))
    return False, last_error


def _parse_socket_json(buffer):
    text = buffer.decode("utf-8", "replace").strip()
    if not text:
        return None
    try:
        return json.loads(text)
    except ValueError:
        pass
    decoder = json.JSONDecoder()
    for index, char in enumerate(text):
        if char == "{":
            try:
                value, _ = decoder.raw_decode(text[index:])
                return value
            except ValueError:
                continue
    return None


def mark_tested(cfg, names):
    """R6 config update: set mcpServers.<name>.tested = true for every
    server that PASSED its real test (never on fail, never by hand).
    Returns the list of servers whose flag was newly written."""
    if not names:
        return []
    if not CONFIG_PATH.exists():
        eprint("NOTE: config/environment.json does not exist - cannot set "
               "'tested'; run scripts/setup_toolbox.py first")
        return []
    servers = cfg.get("mcpServers")
    if not isinstance(servers, dict):
        servers = {}
        cfg["mcpServers"] = servers
    changed = []
    for name in names:
        entry = servers.get(name)
        if not isinstance(entry, dict):
            entry = {}
            servers[name] = entry
        if entry.get("tested") is not True:
            entry["tested"] = True
            changed.append(name)
    if not changed:
        return []
    try:
        with open(CONFIG_PATH, "w", encoding="utf-8") as fh:
            json.dump(cfg, fh, indent=2, ensure_ascii=False)
            fh.write("\n")
    except OSError as exc:
        eprint("ERROR: cannot write %s: %s" % (CONFIG_PATH, exc))
        return []
    eprint("set tested=true in %s for: %s" %
           (CONFIG_PATH, ", ".join(changed)))
    return changed


TESTS = {"ida": test_ida, "cutter": test_cutter,
         "x64dbg": test_x64dbg, "blender": test_blender}


def verify_server(cfg, name):
    eprint("== %s ==" % name)
    pre = precheck_port(cfg, name)
    result = dict(pre)
    result["test"] = "fail"
    result["evidence"] = None

    if pre["collision"]:
        eprint("  collision: %s" % pre["problem"])
        result["test"] = "fail"
        result["evidence"] = "skipped due to port collision"
        return result

    listening, image, pid = pre["listening"], pre["owner"], pre["pid"]
    if not listening:
        result["evidence"] = "port %d not listening - server not running" % \
            pre["port"]
        result["remedy"] = result["remedy"] or REMEDY_BY_SERVER[name]
        eprint("  port %d free -> %s" % (pre["port"], result["evidence"]))
        return result

    ok, evidence = TESTS[name](cfg)
    result["test"] = "pass" if ok else "fail"
    result["evidence"] = evidence
    if not ok and not result["remedy"]:
        result["remedy"] = REMEDY_BY_SERVER[name]
    eprint("  %s: %s" % (result["test"].upper(), evidence[:400]))
    return result


def main():
    parser = argparse.ArgumentParser(
        description="Run REAL functional tests against the skill's MCP "
                    "servers (R6) and check port collisions (R7).")
    parser.add_argument("--server", choices=list(SERVER_ORDER) + ["all"],
                        default="all",
                        help="which MCP server to verify (default: all)")
    args = parser.parse_args()

    cfg = load_config()
    targets = list(SERVER_ORDER) if args.server == "all" else [args.server]

    results = [verify_server(cfg, name) for name in targets]
    passed = sum(1 for r in results if r["test"] == "pass")
    failed = len(results) - passed
    collisions = sum(1 for r in results if r["collision"])
    code = 0 if (failed == 0 and collisions == 0) else 1
    tested_set = mark_tested(cfg, [r["server"] for r in results
                                   if r["test"] == "pass"])

    print(json.dumps({
        "tool": "verify_mcp.py",
        "arguments": {"server": args.server},
        "results": results,
        "summary": {"passed": passed, "failed": failed,
                    "collisions": collisions},
        "testedSet": tested_set,
        "exitCode": code,
    }))
    return code


if __name__ == "__main__":
    sys.exit(main())
