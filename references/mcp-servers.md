# MCP Server Profiles, Port Registry and Collision Rules (R6/R7)

Reference for the four MCP servers used by this skill. Everything here is
host-tool agnostic: register the servers in whatever MCP-capable AI coding
tool you use. Where a decision belongs to a human, **ask the user before
proceeding**.

Verification status of this document: the download sources (release asset
names), the repository layouts and the game facts were verified live against
the GitHub API / vendor sites while writing this skill; the x64dbg-mcp
install path (download → extract → plugin deploy) was additionally executed
in a sandbox. The four servers themselves were NOT run live at authoring
time — the functional tests below describe what a real pass must look like.
`scripts/verify_mcp.py` enforces exactly this.

---

## Overview

| Server | Upstream source | Host tool requirement | Transport | Default port | Auth | Install id (`setup_toolbox.py`) |
| --- | --- | --- | --- | --- | --- | --- |
| ida-pro-mcp (mrexodia) | https://github.com/mrexodia/ida-pro-mcp | IDA Pro 8.3+/9.x (NOT IDA Free), Python 3.11+ for the server part | IDA plugin serves Streamable HTTP; client connects via stdio wrapper or HTTP | 13337 (`/mcp`) | none (localhost) | `--install ida-mcp` |
| cuttermcp (CutterMCP) | https://github.com/ap425q/cuttermcp | Cutter (rizinorg) with a binary loaded; Python 3 + pip | Bridge (stdio) ↔ Cutter plugin (plain HTTP) | 8000 (**hardcoded**) | none — bind all interfaces | `--install cutter` + `--install cutter-mcp` |
| x64dbg-mcp-server (duty1g) | https://github.com/duty1g/x64dbg-mcp-server | x64dbg/x32dbg (Crazy Taxi 1 → x32dbg) | Streamable HTTP + SSE (no stdio) | 9094 (x64) / 9095 (x32) | Bearer token (mandatory) | `--install x64dbg` + `--install x64dbg-mcp` |
| MCP for Blender (ahujasid) | https://github.com/ahujasid/mcp-for-blender | Blender (World-Labs AI feature needs Blender 5.3+ per upstream README) | Client ↔ server stdio via `uvx`; server ↔ Blender addon JSON over TCP | 9876 | none (keep local) | `--install blender` + `--install blender-mcp` |

Note on names: the Blender project was renamed — the real repository is
`ahujasid/mcp-for-blender` (PyPI package `mcp-for-blender`); older material
may still call it "blender-mcp". There is no user "ahujames" (checked: 404).

---

## 1. ida-pro-mcp (mrexodia)

**What setup automates** (R1, only when IDA Pro is detected): download the
repo into `Toolbox/ida-pro-mcp/`, `pip install` from there, best-effort
`ida-pro-mcp --install` (registers the IDA plugin in `%APPDATA%\Hex-Rays\IDA Pro`).

**Manual completion**: restart IDA and the MCP host completely; open a
binary in IDA. Optional headless mode: activate idalib
(`uv run "<IDA>\idalib\python\py-activate-idalib.py"`) and run `idalib-mcp`.
The exact client config blocks are printed by `ida-pro-mcp --config`.

**Transport**: the IDA plugin serves Streamable HTTP MCP at
`http://127.0.0.1:13337/mcp` (debugger tools via `?ext=dbg`, e.g.
`http://127.0.0.1:13337/mcp?ext=dbg`). If the port is taken, the plugin
auto-increments up to +100 and registers every instance (host, port, pid,
binary, idb path) in a discovery file — multiple IDA instances are supported,
but a client config with a fixed 13337 URL then hits the WRONG instance.
Other ports upstream mentions: 8744 (SSE example), 8745 (idalib-mcp
example), 5173 (MCP Inspector UI).

**REAL functional test** (R6; `verify_mcp.py --server ida`):
JSON-RPC `initialize` → `notifications/initialized` → `tools/list` →
`tools/call` (preferred: `server_health`; on an idalib supervisor server
also `idb_list`) against `http://127.0.0.1:<port>/mcp`. PASS only if the
answer contains real IDB data — a binary/`.i64`/`.idb` path for the file
actually open in IDA (`server_health.idb_path` / `module`). A bare
"server reachable" answer or a health answer without an open database
(`idb_path` null) is a FAIL. Stronger manual proof: open the game exe in
the IDA GUI, then `decompile`/`list_funcs` must show real functions of that
binary. Tool-name caveats (verified against upstream main): there is NO
`get_metadata` tool (only the `ida://idb/metadata` resource), and
`idb_open`/`idb_list` are supervisor-only idalib tools that do not exist in
the GUI plugin server's tool list.

**Conflicts**: no default-port overlap with the other three servers. Watch
the auto-increment/discovery behavior above; update
`mcpServers.ida.port` in `config/environment.json` if the port moved.

---

## 2. cuttermcp (CutterMCP, ap425q)

**What setup automates** (R1, cutter route): Cutter download
(`Cutter-v<ver>-Windows-x86_64.zip` from rizinorg releases; cutter.re links
there) into `Toolbox/cutter/`; CutterMCP release zip
(`CutterMCP-release-2-0.zip`) into `Toolbox/cuttermcp/`;
`pip install -r requirements.txt` (`mcp==1.5.0`, `requests==2.32.3`);
plugin copy into Cutter's `plugins/python` folder when locatable.

**Manual completion**: Cutter → Edit → Preferences → Plugins shows the
plugin directory → copy `src/CutterMCPPlugin.py` into its `python` subfolder
→ restart Cutter → load the game binary → panel must show
`HTTP Server: Running (Port 8000)`. Client side (stdio):
`python <skill root>\Toolbox\cuttermcp\bridge_mcp_cutter.py`.

**Transport**: bridge ↔ plugin is plain HTTP against
`http://127.0.0.1:8000/` — **port 8000 is hardcoded** in
`src/CutterMCPPlugin.py` (bound to all interfaces, no auth). Endpoints
include `GET /functions`, `GET /decompile?addr=`, `GET /segments`,
`GET /imports`, `GET /xrefsTo?addr=`, `POST /renameFunction` and more.

**REAL functional test** (R6; `verify_mcp.py --server cutter`):
`GET /functions` answers `text/plain`, NOT JSON — the plugin returns
`"\n".join(cutter.cmd("aflq").splitlines())` paginated to 100 lines
(backend is rizin `aflq`, one function name per line). PASS only if the
answer lists REAL function names of the binary loaded in Cutter
(`verify_mcp.py` requires at least 3 non-empty lines). Stronger
manual proof: `GET /decompile?addr=<real function address>` returns real
pseudocode of that binary, or a rename via the MCP becomes visible in the
Cutter UI. Only "port 8000 open" is a FAIL.

**Conflicts**: highest collision risk of the four — 8000 is a very common
dev port (Jupyter, local web apps, proxies) and cannot be changed. The
plugin has a single-instance guard ("Server is already running!"). Because
the plugin binds all interfaces without auth, a Windows firewall prompt may
appear; keep Cutter local.

---

## 3. x64dbg-mcp-server (duty1g)

**What setup automates** (R3): x64dbg snapshot zip (`snapshot_*.zip` from
x64dbg/x64dbg releases) into `Toolbox/x64dbg/`; release zip from
duty1g/x64dbg-mcp-server (current: v1.7, `x64dbg-MCP-Server-v1.7.zip`,
~720 KB, layout `dist/x32/plugins/*.dp32` + `dist/x64/plugins/*.dp64` —
verified by sandbox execution) into `Toolbox/x64dbg-mcp/`; deploy of the
`.dp32`/`.dp64` plugin files into the x64dbg root's `x32/plugins` /
`x64/plugins`.

**Manual completion**: start x32dbg (Crazy Taxi 1 is 32-bit) — the server
runs inside the debugger on a background thread. Read the auto-generated
token: Plugins → x64dbg-MCP Server → Configure MCP Server… (Copy button;
persisted in `mcp_config.json` next to the debugger exe). Store it in
`config/environment.json` (`mcpServers.x64dbg.token`) or rely on the script's
auto-discovery. Same dialog: change bind address to `127.0.0.1` (default
`0.0.0.0`, plain HTTP, must not be LAN-visible). Port changes take effect
immediately.

**Transport**: Streamable HTTP + SSE, JSON-RPC 2.0; x64dbg → port 9094,
x32dbg → port 9095 (upstream default per build width, config.zig); Bearer
token mandatory (401 without). The default in `config/environment.json` and
both scripts for this skill is **9095** — Crazy Taxi 1 is 32-bit and is
debugged with x32dbg; use 9094 only if you deliberately attach via the
64-bit x64dbg.

**REAL functional test** (R6; `verify_mcp.py --server x64dbg`): MCP
initialize → tools/list → `tools/call GetDebugState` (fallback
`GetEventLog`) with the Bearer token. PASS only if the answer carries real
debugger state/log data (`isDebugging: true`, a `RUNNING`/`PAUSED`/`LOCKED`
status, a loaded `currentModule`, or `Events (N/M)` log lines — the exact
answer formats of upstream `GetDebugState`/`GetEventLog`). A
`status: NO_TARGET` answer (`isDebugging: false`) proves only the transport:
no binary is loaded in the debugger yet, which is a FAIL until a target is
loaded (`LoadBinary` or attach the game). Note: `EvalExpression` requires an
`expression` argument and is therefore not used by the verifier. Stronger
manual proof with a target: attach to the
game, `AttachProcess` + `WaitForPause` + `GetAllRegisters` must show real
register values; `SetBreakpoint` + `ListBreakpoints` with hit count > 0.
Only "port open" or "process running" is a FAIL.

**Conflicts**: 9094/9095 are separate per debugger instance — x32dbg and
x64dbg can run side-by-side. One MCP client per port is the sensible limit, and
debugger state is GLOBAL per process: in multi-agent sessions every x64dbg request
serializes through ONE queue owner (`batch-re-queries.md`, tool-specific
constraints; R20) — never run parallel breakpoints from two agents.

---

## 4. MCP for Blender (ahujasid/mcp-for-blender)

**What setup automates** (R2 — ONLY after the user answered YES to
"Do you want to edit textures, splines, or 3D objects?"): Blender
windows-x64 zip from download.blender.org into `Toolbox/blender/`;
`addon.py` (repo root) into `Toolbox/blender-mcp/`; uv/uvx availability
check (`winget install --id=astral-sh.uv -e` if missing).

**Manual completion**: Blender → Edit → Preferences → Add-ons → Install
from Disk… → `Toolbox/blender-mcp/addon.py` → enable "MCP for Blender" →
N-Panel → Start MCP Server. Client side: `uvx mcp-for-blender` (stdio;
add `--python 3.11` / `UV_PYTHON_PREFERENCE=only-managed` for a pinned
managed interpreter). The MCP server part needs uv/uvx.

**Transport**: JSON over TCP socket between server and Blender addon,
default port 9876 on localhost. Configurable per instance: env
`BLENDER_PORT`, CLI `uvx mcp-for-blender --port N` (CLI wins), and the port
field in the Blender addon panel — both sides must match. Remote/bind
address options exist but MUST stay local: the socket has no authentication
or encryption (upstream security warning). `execute_blender_code` runs
arbitrary Python in Blender by design — save your work first.

**REAL functional test** (R6; `verify_mcp.py --server blender`): TCP
connect + `{"type":"get_scene_info","params":{}}` — the addon routes on the
TOP-LEVEL `"type"` field (fallback `get_addon_info`; note `get_addon_status`
does not exist as an addon socket command) — must answer with JSON
`status: success` and real scene/addon data (object names/types of the open
scene, or the addon capability/version handshake). Stronger
manual proof: `execute_blender_code` creating a cube that appears in the
viewport. Only "socket open" is a FAIL.

**Conflicts**: upstream rule — run only ONE MCP server instance against a
Blender instance; multiple Blender instances need one port each. If both
the old `blender-mcp` and the new `mcp-for-blender` namespaces are installed
somewhere, uninstall one of them to avoid confusion.

---

## Port Registry (defaults; all verified against upstream sources)

| Server / tool | Port(s) | Bind | Changeable? | Notes |
| --- | --- | --- | --- | --- |
| ida-pro-mcp plugin | 13337 | 127.0.0.1 | Yes (IDA dialog; auto-increments +1..+100 when busy, discovery registry) | SSE example 8744; idalib-mcp example 8745; Inspector UI 5173 |
| cuttermcp plugin | 8000 | 0.0.0.0 (`''`) | **NO — hardcoded** | most common dev port; single-instance guard |
| x64dbg-mcp (x64dbg) | 9094 | 0.0.0.0 (→ set 127.0.0.1) | Yes (plugin dialog / mcp_config.json) | Bearer token mandatory |
| x64dbg-mcp (x32dbg) | 9095 | 0.0.0.0 (→ set 127.0.0.1) | Yes (same dialog) | Crazy Taxi 1 → x32dbg |
| MCP for Blender addon | 9876 | localhost | Yes (env/CLI/addon panel, both sides must match) | one server instance per Blender instance |

Companion-skill ports that may already be in use on machines where other
modding skills are installed (checked for cross-collisions, none of them
overlap with the four above): Cheat-Engine-MCP bridge 5556/5557, ImHex
31337, an alternate x64dbg REST bridge 27042. Treat 8000, 9094, 9095, 9876
and 13337 as reserved for this skill.

---

## Collision rules (R7)

1. **Before every start/test**, run the pre-check: the port must be free or
   owned by the expected server, and no two servers may share a port.
   Automated in `scripts/verify_mcp.py` (before every test) and
   `scripts/setup_toolbox.py` (R7 step). A collision fails with exit code 1
   and prints the owning process (pid) plus the resolution.
2. **Resolution per server**:
   - ida: find the moved port (IDA MCP dialog / discovery registry), store
     it in `config/environment.json` (`mcpServers.ida.port`), or stop the
     foreign process.
   - cutter: port 8000 is not changeable — stop the foreign process.
   - x64dbg: change the port in the plugin dialog, store it in
     `config/environment.json` (`mcpServers.x64dbg.port`), or stop the
     foreign process.
   - blender: change the port on BOTH sides (addon panel + `--port`), store
     it in `config/environment.json` (`mcpServers.blender.port`).
3. **Single-instance limits**: cuttermcp plugin (hard guard), x64dbg MCP
   (one per debugger instance), blender MCP (upstream rule: one server
   instance per Blender instance). ida-pro-mcp is multi-instance by design
   (port hopping + discovery) — re-check which instance a fixed URL hits.
4. **Local-only rule**: x64dbg-mcp and cuttermcp bind to all interfaces by
   default and have no/weak auth — switch to 127.0.0.1 where possible and
   never expose them beyond the local machine.
5. **Never mark a server "tested"** because its port was reachable (R6);
   only a real, data-bearing call may set `tested: true` — and
   `verify_mcp.py` writes `mcpServers.<name>.tested = true` into
   `config/environment.json` itself when (and only when) a server passes
   its real test. Never set the flag by hand.

---

## Generic client registration (host-agnostic snippets)

Register in your MCP host's server configuration; restart the host after
changes. Replace paths/ports/tokens with the values from
`config/environment.json`.

```jsonc
// ida-pro-mcp — after `pip install Toolbox/ida-pro-mcp`:
{"mcpServers": {"ida-pro-mcp": {"command": "ida-pro-mcp",
                                "args": []}}}
// (exact stdio wrapper args: see `ida-pro-mcp --config`;
//  or point a Streamable-HTTP client at http://127.0.0.1:13337/mcp)

// cuttermcp — bridge (stdio):
{"mcpServers": {"cutter": {"command": "python",
                           "args": ["<skill root>\\Toolbox\\cuttermcp\\bridge_mcp_cutter.py"]}}}

// x64dbg-mcp-server — Streamable HTTP with Bearer token (9095 = x32dbg,
// the default for this skill; 9094 = 64-bit x64dbg):
{"mcpServers": {"x64dbg": {"type": "http",
                           "url": "http://127.0.0.1:9095/",
                           "headers": {"Authorization": "Bearer <TOKEN>"}}}}
// legacy SSE clients: "type": "sse", url "http://127.0.0.1:9095/sse"

// MCP for Blender — via uvx (stdio):
{"mcpServers": {"blender": {"command": "uvx",
                            "args": ["mcp-for-blender"]}}}
```
