# Environment Setup Runbook (R0–R5 + R7)

Detailed setup instructions for the Crazy Taxi 1 modding skill. Everything in
this skill is written for AI agents and is **host-tool agnostic**: it works
with any MCP-capable AI coding tool. Where a decision belongs to a human, the
agent must **ask the user before proceeding** instead of deciding silently.

The one-entry setup script is `scripts/setup_toolbox.py`. This runbook
explains what it does, what it cannot automate, and what the agent must ask
or do manually.

---

## 0. Conventions

| Term | Meaning |
| --- | --- |
| Skill root | The directory containing this skill (`scripts/`, `config/`, `references/`, `Toolbox/`). All scripts resolve every path relative to their own location — nothing is hard-coded. |
| Toolbox | `<skill root>/Toolbox/` (name configurable via `toolboxDir` in `config/environment.json`). Every downloaded tool lands here, self-contained. |
| Config | `<skill root>/config/environment.json`. Single source of truth for the game path, toolchain facts and MCP server state. See "Config schema" below. |

Toolbox layout after a complete setup:

```
Toolbox/
  _downloads/          downloaded zip archives (kept as installer cache)
  ida-pro-mcp/         ida-pro-mcp repo copy, pip-installed from here
  cutter/              portable Cutter (only if no IDA Pro is present)
  cuttermcp/           CutterMCP bridge + plugin (bridge_mcp_cutter.py, src/)
  x64dbg/              portable x64dbg snapshot (release/x96dbg.exe, x32/, x64/)
  x64dbg-mcp/          x64dbg-mcp-server release (plugin files deployed from here)
  blender/             portable Blender (only after the R2 question was answered YES)
  blender-mcp/         MCP-for-Blender addon.py
```

`scripts/setup_toolbox.py` CLI (exact):

```
python scripts/setup_toolbox.py [--check-only] [--json]
    [--install ida-mcp|cutter|cutter-mcp|blender|blender-mcp|x64dbg|x64dbg-mcp]
```

- Default run (no flags): R0 + R1 (IDA-or-Cutter) + R3 (x64dbg) + R4 (game)
  + R5 (toolchain) + R7 (port report). **R2/Blender is never installed by the
  default run** — it is status-only; installation requires an explicit
  `--install blender` / `--install blender-mcp` and a prior YES from the user.
- `--check-only`: read-only status report; no downloads, no installs, no
  config writes. Always run this first and tell the user what a full run
  would download (Cutter ~140 MB, Blender ~300 MB, x64dbg ~32 MB).
- `--install <branch>`: runs only that installation branch (plus the Python
  check and the R7 port report).
- Output: progress goes to stderr; stdout carries a human summary by default
  and a single-line JSON report with `--json`.

Exit codes (both `setup_toolbox.py` and `verify_mcp.py`):

| Code | Meaning |
| --- | --- |
| 0 | success / everything present |
| 1 | one or more steps failed or are missing (read the step details). Note: in a default run, R2/Blender being absent does NOT fail the setup — it is ask-first by design; with `--check-only` any missing step yields 1 |
| 2 | usage or configuration error (bad arguments, unparsable config) |

---

## 1. R0 — Python prerequisite

Python 3.8+ is required to run the skill's scripts (ida-pro-mcp additionally
requires Python 3.11+ for its own server component).

Check:

```
python --version
py -3 --version      (Windows launcher, if present)
```

**If Python is missing**, the setup script cannot run either — the agent
installs it automatically and then continues (no user decision needed; inform
the user what is happening):

1. Preferred (winget, id verified against the winget catalog):
   ```
   winget install -e --id Python.Python.3.12
   ```
2. Fallback (python.org installer, URL pattern verified; silent install):
   ```
   curl -L -o python-installer.exe https://www.python.org/ftp/python/3.12.9/python-3.12.9-amd64.exe
   python-installer.exe /quiet InstallAllUsers=1 PrependPath=1
   ```
3. Open a NEW shell afterwards (PATH refresh), re-run `python --version`,
   then continue with the setup below. If pip is missing in the chosen
   interpreter, run `python -m ensurepip` first.

`setup_toolbox.py` performs this check as its first step and reports the
interpreter path, the version, and whether pip works.

---

## 2. R1 — Static analysis: IDA Pro first, Cutter otherwise

Decision tree (automated by the default run and by `--install ida-mcp` /
`--install cutter` + `--install cutter-mcp`):

```
Is IDA Pro installed?                      (setup detects IDA*/ida.exe in Program Files)
├── YES → install/complete ida-pro-mcp     (Toolbox/ida-pro-mcp + pip + IDA plugin)
└── NO  → is Cutter installed?
         ├── YES → install/complete cuttermcp (bridge + plugin)
         └── NO  → download Cutter from cutter.re (GitHub releases) into
                   Toolbox, then install cuttermcp
```

Ask the user before installing anything heavy in an interactive session, and
tell them which route was chosen and why. If the user owns IDA Pro but it is
not detected (custom install location), ask for the install path.

### 2a. IDA route — ida-pro-mcp (mrexodia)

Requirements: IDA Pro 8.3+ (9.x recommended; IDA Free is NOT supported),
Python 3.11+ for the MCP server part.

`--install ida-mcp` (or the default run when IDA is detected) automates:
1. Download `https://github.com/mrexodia/ida-pro-mcp/archive/refs/heads/main.zip`
   into `Toolbox/_downloads/` and extract to `Toolbox/ida-pro-mcp/`.
2. `python -m pip install <skill root>\Toolbox\ida-pro-mcp`
3. Best-effort `ida-pro-mcp --install` (registers the IDA plugin in the
   IDA user directory, `%APPDATA%\Hex-Rays\IDA Pro`).

Manual completion (agent must do/instruct):
- Restart IDA **completely** (and the MCP host tool) after installation.
- Inside IDA the plugin serves Streamable HTTP MCP at
  `http://127.0.0.1:13337/mcp` while a database is open.
- Optional (headless mode, no IDA GUI): activate idalib once via
  `uv run "<IDA install>\idalib\python\py-activate-idalib.py"` and start
  `idalib-mcp` instead. On Windows, upstream notes that enabling Developer
  Mode (symlink support) helps if plugin registration uses symlinks.
- Inspect the exact client configuration blocks with `ida-pro-mcp --config`.

Functional test: `python scripts/verify_mcp.py --server ida` (must return
real IDB data — see references/mcp-servers.md).

### 2b. Cutter route — Cutter + CutterMCP (cuttermcp)

`--install cutter` automates: resolve the newest
`Cutter-v<version>-Windows-x86_64.zip` from the `rizinorg/cutter` GitHub
releases (cutter.re links there), download into `Toolbox/_downloads/`,
extract to `Toolbox/cutter/`. (~140 MB.)

`--install cutter-mcp` automates: download the CutterMCP release zip
(`CutterMCP-release-2-0.zip` from `ap425q/CutterMCP` releases, fallback:
main-branch archive) into `Toolbox/cuttermcp/`, run
`pip install -r Toolbox/cuttermcp/requirements.txt` (pins `mcp==1.5.0`,
`requests==2.32.3`), and copy `src/CutterMCPPlugin.py` into Cutter's python
plugin folder when it can be located automatically.

Manual completion (exact UI path): open Cutter → **Edit → Preferences →
Plugins** — the dialog shows the plugin directory. Copy
`Toolbox/cuttermcp/src/CutterMCPPlugin.py` into that directory's `python`
subfolder, then restart Cutter. Load the game binary, open the plugin panel
(**Windows → Plugins**), and confirm the status label shows
`HTTP Server: Running (Port 8000)`.

Bridge for the MCP host (stdio): `python <skill root>\Toolbox\cuttermcp\bridge_mcp_cutter.py`

Functional test: `python scripts/verify_mcp.py --server cutter`.

---

## 3. R2 — Blender / Blender-MCP (ASK FIRST)

**Never install Blender or its MCP without asking the user first.** Before
running `--install blender` or `--install blender-mcp`, ask the user exactly
this question (and wait for the answer):

> **"Do you want to edit textures, splines, or 3D objects?"**

Only on YES: run `python scripts/setup_toolbox.py --install blender` and/or
`--install blender-mcp`. On NO: skip R2 entirely and note the decision.
Both install branches print this reminder themselves as a safeguard.

- `--install blender` resolves the newest `blender-X.Y.Z-windows-x64.zip`
  from `https://download.blender.org/release/` and extracts it to
  `Toolbox/blender/` (~300 MB).
- `--install blender-mcp` downloads `addon.py` from
  `https://github.com/ahujasid/mcp-for-blender` (repo root) into
  `Toolbox/blender-mcp/` and checks for uv/uvx (the MCP server part runs via
  `uvx mcp-for-blender`; if missing: `winget install --id=astral-sh.uv -e`).
- Manual completion: Blender → **Edit → Preferences → Add-ons → Install from
  Disk…** → select `Toolbox/blender-mcp/addon.py` → enable "MCP for Blender"
  → start the server in the N-Panel ("MCP for Blender" tab). Only ONE MCP
  server instance may talk to one Blender instance (upstream README rule);
  multiple Blender instances need one port per instance.

Before doing any texture/3D/spline work, read the existing examples first
(rule R11): `Examples/Texture,3D,Spline Edit/` — especially
`REPLACE_3D_AND_SPLINES_GUIDE.md` and the `Crazy Taxi Texture Modding Guide.pdf`.
The engine's texture rules are not what one would assume.

Functional test: `python scripts/verify_mcp.py --server blender` (real
`get_scene_info` call over the addon's TCP socket).

---

## 4. R3 — x64dbg/x32dbg + x64dbg-mcp-server (automatic)

The default run (and `--install x64dbg`) automates:
1. Resolve the newest `snapshot_*.zip` from `x64dbg/x64dbg` GitHub releases,
   download to `Toolbox/_downloads/`, extract to `Toolbox/x64dbg/`.
2. Download the `x64dbg-mcp-server` release zip from
   `duty1g/x64dbg-mcp-server` releases into `Toolbox/x64dbg-mcp/`.
3. Deploy the plugin files: the release's `.dp32` → `<x64dbg root>\x32\plugins\`,
   `.dp64` → `<x64dbg root>\x64\plugins\` (verified release layout:
   `dist/x32/plugins/…`, `dist/x64/plugins/…`).

Crazy Taxi 1 is a 32-bit process — the debugger to use is **x32dbg**
(launcher: `Toolbox/x64dbg/release/x96dbg.exe`). The MCP server runs inside
the debugger process on port 9094 (x64dbg) / 9095 (x32dbg).

Manual completion:
- Start x32dbg once; the plugin auto-generates a Bearer token. Read it via
  **Plugins → x64dbg-MCP Server → Configure MCP Server…** (Copy button).
- Store the token in `config/environment.json` under
  `mcpServers.x64dbg.token` so `verify_mcp.py` can authenticate (it also
  auto-discovers `mcp_config.json` next to the debugger exe).
- Recommendation (security): switch the bind address from `0.0.0.0` to
  `127.0.0.1` in the same dialog — the server has no encryption and must not
  be reachable from the network.

Functional test: `python scripts/verify_mcp.py --server x64dbg` (real
`GetDebugState`/`GetEventLog` call with the Bearer token).

---

## 5. R4 — Locate Crazy Taxi.exe

Run:

```
python scripts/find_game.py [--reindex]
```

Search order: stored config path → Steam libraries
(`libraryfolders.vdf` + `appmanifest_71230.acf`) → Windows registry
Uninstall keys → bounded scan of typical install folders. Known executable
names: `Crazy Taxi.exe`, `CrazyTaxi.exe` (case-insensitive).

Facts verified during skill research:
- Steam release: AppID **71230**, install folder
  `steamapps\common\Crazy Taxi`, executable `Crazy Taxi.exe`; delisted from
  Steam purchase on 2024-12-06 but playable if already owned.
- Only confirmed matches are persisted. **Confirmed** = the exe name is one
  of the known names (`Crazy Taxi.exe`, `CrazyTaxi.exe`) **and** the file
  carries a 32-bit PE header (machine i386 — read-only check in
  `find_game.py`; a 64-bit impostor with the right name is reported but
  never saved). Loose candidates (folder looks like Crazy Taxi but unknown
  exe) are reported, never saved.
- This check proves the *location*, not the game *version*: the CD port and
  the Dreamcast Collection release use the same exe names and are also
  32-bit. A persisted path is therefore not a version guarantee — verify at
  least one known `offset_list.json` address against the real binary
  (static analysis, workflow Step 1) before building any hook on the
  address database.
- If nothing is found, exit code is 1 — **ask the user for the install
  path**, then store it manually in `config/environment.json`
  (`"crazyTaxiExe"` = full exe path, `"gameDir"` = folder). Re-run the
  script afterwards to validate; it never re-searches once a valid path is
  stored. `--reindex` forces a full rescan. If no user is reachable in this
  run, keep `crazyTaxiExe`/`gameDir` null, report the open question, and
  continue only with work that needs no game files — never guess a path
  (R15 non-interactive fallback, SKILL.md).

Output format: progress on stderr; a single-line JSON object on stdout with
keys `found`, `crazyTaxiExe`, `gameDir`, `source`, `candidates`,
`configUpdated`, `message`.

Version warning: the skill's `offset_list.json` addresses target the
**32-bit Steam build** (see `SUBMOD_DEVELOPMENT_GUIDE.md`). The original CD
port and the 2011 Dreamcast Collection release have different addresses —
do not reuse offsets across versions.

---

## 6. R5 — C++ / Rust toolchain detection

The default run (and every full setup) detects:

- `cl.exe` via PATH or vswhere (`C:\Program Files (x86)\Microsoft Visual
  Studio\Installer\vswhere.exe`, requiring the VC tools workload),
- `gcc`, `clang` via PATH,
- `cargo`, `rustc` via PATH.

Results are stored in `config/environment.json` as `toolchain` (paths) and
`preferredCompiler` (`"cpp"` if any C++ compiler exists, else `"rust"`, else
`null` — rule R10 prefers C++). Mods are built in C++ or Rust depending on
what is installed. Note that `cl.exe` requires the Visual Studio environment
(vcvars) before invocation; build commands for the sub-mod DLL are in
`SUBMOD_DEVELOPMENT_GUIDE.md` chapter 5. The vcvars path printed there is
one edition's example — resolve the real path from the Visual Studio
installation that `setup_toolbox.py` stores (`toolchain.cpp.cl` /
`clNote` come from vswhere's `installationPath`; then run
`<installationPath>\VC\Auxiliary\Build\vcvarsall.bat x86` for 32-bit DLL
builds).

If no toolchain is found, the step reports `missing` and the agent should
ask the user whether to install Visual Studio Build Tools (C++ workload,
needed for 32-bit DLLs) or Rust (https://rustup.rs) — a human decision.
On YES for the C++ Build Tools, install them with (winget ID verified against
the winget catalog on 2026-11-14; the `VCTools` workload ID is
Microsoft-documented):

```cmd
winget install -e --id Microsoft.VisualStudio.2022.BuildTools --override "--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
```

Afterwards re-run `python scripts/setup_toolbox.py --check-only --json` so
vswhere re-detects `cl.exe` and `config/environment.json` is updated.
If no user is reachable in this run, treat the answer as No: do not install
anything, record the open question, and stop at the Build step with a clear
report (R15 non-interactive fallback, SKILL.md).

---

## 7. R7 — Port / collision checks (mandatory before every start)

Before relying on any MCP server (and again inside `verify_mcp.py` before
every test), the scripts check:

1. The configured port is free **or** owned by the expected server process
   (owner identified via `netstat -ano` + `tasklist`).
2. No two MCP servers are configured on the same port.
3. A foreign process on a server's port is reported as a **collision** with
   a concrete resolution (see references/mcp-servers.md for the registry).

Run the check any time:

```
python scripts/setup_toolbox.py --check-only --json   # full status incl. ports
python scripts/verify_mcp.py --server all             # tests + collision checks
```

A hard collision fails the run (exit code 1) until resolved.

---

## 8. Framework mod check (run before any sub-mod development)

Sub-mods are hosted by **Lyra's Framework Mod**: the game loads the proxy
loader `<gameDir>/dinput8.dll`, which loads
`<gameDir>/mods/Framework_MOD/Framework_MOD.dll`, which detects every
sub-mod DLL that exports `GetSubModName()` and hosts it in the Shift+F1
menu (architecture: references/framework-mod.md; deploy-time checks:
references/modding-workflow.md, "Deploy & verify" step 0). The standard mod
target is therefore a sub-mod under `<gameDir>/mods/<modname>/` — and
before any sub-mod development starts, the host DLL must be proven present
and valid. A missing or broken framework mod makes a sub-mod a silent
no-op: the loader still loads the DLL into the process, but nothing ever
calls its exports.

### 8.1 CLI (exact)

```
python scripts/check_framework.py [--fix] [--json]
```

- Reads `gameDir` from `config/environment.json` (persisted by
  `find_game.py`, section 5). All paths are resolved relative to the script
  location and to the config — nothing is hard-coded.
- Without `--fix`: check only. With `--fix`: check, and when the framework
  DLL is missing or invalid, download + validate + install (8.3 / 8.4) —
  consent gate R15 first, see 8.4.
- Output: progress on stderr; on stdout a human summary by default, with
  `--json` a single-line JSON report (keys in 8.6).

Exit codes:

| Code | Meaning |
| --- | --- |
| 0 | framework DLL present and valid — or `--fix` installed it successfully (`--fix` on an already-valid install is a no-op: no download) |
| 1 | framework DLL missing (`<gameDir>/mods/Framework_MOD/Framework_MOD.dll` does not exist) |
| 2 | framework DLL present but invalid — read the reasons (structure, not x86, missing exports) — or present but unreadable (locked by the running game or an antivirus; the reason names it) |
| 3 | no usable `gameDir` in `config/environment.json` — run `python scripts/find_game.py` first |
| 4 | `--fix` failed (download / validation / install). NOT silent: the script prints the detected C++ toolchain and the exact 32-bit build commands for the source in `Examples/Standalone MOD` and points at `references/framework-mod.md`. The script itself never compiles anything. |

(Argparse usage errors — e.g. an unknown flag — print help and exit with
the standard argparse code **2**, which collides with the "framework DLL present
but invalid" verdict in the table above. Disambiguate: argparse exit 2 prints
usage text and produces NO JSON report; a real verdict always carries the check
report.)

### 8.2 Validation rules (probe-verified, 2026-11-14)

All thresholds below were measured against a real download of the official
archive — nothing is guessed:

| Object | Expected facts |
| --- | --- |
| Official archive `lyras_framwork_mod_48600.zip` | exactly **180186115 bytes**; sha256 `ba7c96a6440e002dc612cd6c3f0a558ab389b17dd3c1c9c0c675f7594123cb91`; first 4 bytes `PK\x03\x04` (**a ZIP, not a PE** — the MZ/PE checks apply to the extracted DLL, never to the archive); entries `mods/Framework_MOD/Framework_MOD.dll`, `dinput8.dll`, `ReadMe_Installation.txt` |
| Framework DLL (extracted / installed) | **2864128 bytes**; sha256 `7a5d93bb35eb052f1b131a311ba5296a022180993d8ab56a4508924855bb923e`; `MZ` → `PE\0\0`; Machine **`0x014c`** (i386); IMAGE_FILE_DLL flag; OptionalHeader magic `0x10b` (PE32); non-empty export table with the 8 public exports `GetFrameworkD3D9Device`, `GetFrameworkImGuiContext`, `GetModConfig`, `ModAPI_IsVulkanEnabled`, `ModAPI_SetVulkanEnabled`, `ModAPI_ToggleVulkan`, `ProcessKeyboardBuffer`, `SetFrameworkStatusMessage` |
| Proxy loader `dinput8.dll` | **153600 bytes**; sha256 `f4ff139fdaf5a5399fbc9c6ec2051034594f1e92b4154bc598719ac47b3ee9d9`; Machine `0x014c`; export `DirectInput8Create` |

- Machine rule: **only `0x014c` (x86/32-bit) is valid.** `0x8664` (x64) or
  any other value is invalid — Crazy Taxi 1 is a 32-bit process; an x64
  DLL can never load.
- For the **downloaded archive**, size and sha256 are hard rules: any
  deviation (corrupted or manipulated content) is rejected and never
  installed. For an **installed DLL**, the same comparison is reported as
  `referenceMatch` info and is NOT a hard fail: a DLL built locally from
  `Examples/Standalone MOD` is structurally valid but has a different
  hash. The `valid`/`invalid` verdict comes from structure + exports only.
- The loader is checked informationally (the exit codes are
  framework-scoped), but remember: without `<gameDir>/dinput8.dll` nothing
  under `mods/` loads at all.

### 8.3 Download facts (probe evidence)

- Working URL (what the script uses):
  `https://drive.usercontent.google.com/download?id=1HQ-6peIb6fPL7cv95ldtKZpWcVD19-M6&export=download&confirm=t`
  (the `uuid` parameter is optional).
- The classic `https://drive.google.com/uc?export=download&id=…` does NOT
  work, with or without `&confirm=t`: Google's 303 redirect strips
  `confirm=t` and always lands on the 2476-byte "Virus scan warning" HTML
  page. `confirm=t` is honoured only on the `drive.usercontent.google.com`
  host. The script guards against receiving that HTML page and reports a
  fix failure instead of installing garbage.

### 8.4 `--fix` flow

**Consent gate first (R15):** `--fix` downloads and installs third-party
binaries (the official framework archive) into the game directory — the same
consent-gated install that `references/framework-mod.md` section 4 and
`SKILL.md` define. **Ask the user before running `--fix`.** No user reachable
in this run → default **No**: do not run `--fix`, record the open question in
the report, and continue with framework-independent work (analysis,
`offset_list.json`, standalone route).

1. Check the installed DLL first: valid → no-op (idempotent, no download).
2. Download the archive (streamed, ~172 MB) into a fresh temp directory and
   validate byte-exact: size, sha256, ZIP magic, HTML-page guard.
3. Validate the archive contents read-only (nothing is executed): required
   entries, PE structure of both modules, export tables.
4. Install: **EVERY archive entry is extracted into the game folder root**
   (`<gameDir>`) — the ZIP's own `mods/...` structure maps directly into the
   game dir, **no extra subfolder is created** — and existing files are
   **overwritten (forced overwrite is mandatory)**: `mods/Framework_MOD/`
   receives `Framework_MOD.dll`, the proxy loader replaces
   `<gameDir>/dinput8.dll`, plus `ReadMe_Installation.txt` and
   `dxvk_optional/`. Entries are zip-slip-guarded and installed
   write-then-replace, so a locked file (running game / antivirus) aborts
   cleanly as exit 4 instead of a traceback.
5. Read the installed DLL back and re-verify; persist `frameworkStatus` /
   `frameworkDll` to `config/environment.json`; exit 0.

On ANY fix failure the script does not stay silent: it prints the detected
toolchain (the `toolchain` config field written by `setup_toolbox.py`; if
empty, a `cl` / `gcc` / `clang` PATH lookup), the complete source folder
and the exact build commands — always **x86/32-bit**, because the game is a
32-bit process (the source uses ~130 absolute 32-bit addresses, an IAT
patch at a fixed x86 address and x86 naked inline assembler) — and points
at `references/framework-mod.md`. Exit code 4.

### 8.5 Fallback chain: download → source build

The complete C++ source of Lyra's Framework Mod v1.2 lives in
`Examples/Standalone MOD` (folder name exact, with a space). It contains no
build system and no third-party dependencies. Building it yourself:

- Fetch the dependencies first — they are referenced by the source's own
  include lines but are not part of the workspace: Dear ImGui **1.90.9**
  (pinned by a comment in render_hook.cpp), MinHook, nlohmann/json.
  Expected layout: `<build-root>/deps/{imgui, imgui/backends, MinHook/include,
  nlohmann}`, NEXT TO the copied source folder (`<build-root>/Standalone
  MOD/`) — dependencies never go inside `Examples/` (read-only reference
  material, R17). Layout and maintained recipe:
  `references/framework-mod.md` sections 5.1-5.2.
- **C++ is mandatory** for framework changes: never port the framework to
  Rust or another language — the sub-mod ABI, the game hooks and the x86
  inline asm are all C++/Win32.
- Build a 32-bit DLL: with MSVC open an x86 Native Tools Command Prompt
  (or run `vcvarsall.bat x86`; `setup_toolbox.py` stores the installation
  path in `toolchain.cpp.cl` / `clNote`), with CMake configure `-A Win32`
  (template: SUBMOD_DEVELOPMENT_GUIDE.md chapter 5), with MinGW/clang use
  `-m32`.
- The script prints the exact per-file command for the current workspace
  state: it lists the actual root `*.cpp` files, excluding the `*_ref`
  reference excerpts (they are not part of the build — missing includes).

### 8.6 `--json` report keys

Single-line JSON on stdout with: `tool`, `arguments`, `gameDir`, `dllPath`,
`frameworkStatus` (`"valid"` | `"missing"` | `"invalid"` | `null`),
`configUpdated`, `exists`, `size`, `sha256`, `reasons`, `pe`
(machine / dllFlag / pe32 / export-table facts), `exports`
(required / found / missing), `referenceMatch` (`size`, `sha256` booleans),
`loader` (informational), `reference` (the probe-verified expected values
from 8.2, incl. the download URL and the not-usable legacy URL), `fix`
(attempted / performed / ok / download evidence / installed / overwritten /
skipped / errors), `fallback` (toolchain, sourceDir, sources, depsMissing, commands,
docs — only when `--fix` failed), `nextAction`, `exitCode`.

---

## 9. Config schema (`config/environment.json`)

```jsonc
{
  "schemaVersion": 2,
  "crazyTaxiExe": null,        // full path to Crazy Taxi.exe (R4; string when set)
  "gameDir": null,             // folder containing the exe (R4)
  "frameworkDll": null,        // absolute path of the framework DLL that was
                               // last checked (check_framework.py); null when
                               // no usable gameDir exists
  "frameworkStatus": null,     // "valid" | "missing" | "invalid" - verdict of
                               // the last check_framework.py run (section 8)
  "toolboxDir": "Toolbox",     // Toolbox folder name, relative to skill root
  "toolchain": null,           // {"cpp": {"cl","clNote","gcc","clang"},
                               //  "rust": {"cargo","rustc"}} (R5)
  "preferredCompiler": null,   // "cpp" | "rust" | null (R10: prefer cpp)
  "mcpServers": {
    // one entry per server, created by the scripts; example state:
    // "ida":     {"port": 13337, "installed": true, "tested": true},
    // "cutter":  {"port": 8000,  "installed": true, "tested": false},
    // "x64dbg":  {"port": 9095,  "installed": true, "tested": false,
    //             "token": "…"},          // optional, for verify_mcp.py
    //             (9095 = x32dbg default - Crazy Taxi 1 is 32-bit; the
    //              64-bit x64dbg would use 9094)
    // "blender": {"port": 9876,  "installed": false, "tested": false}
  }
}
```

Field rules:
- `port` — integer; default per server, change here when a server was moved.
- `installed` — set by `setup_toolbox.py` only (never hand-edit to true).
- `tested` — set to `true` by `verify_mcp.py` itself, only for a server
  that PASSED its real test (the script writes this file on a pass; never
  write it by hand).
- `token` (x64dbg only) — Bearer token, optional but recommended.
- `frameworkDll` / `frameworkStatus` — written by `check_framework.py`
  only, after every completed run (see section 8). Both are reset to null
  when no usable `gameDir` exists. Never hand-edit; re-run the script.
- `schemaVersion` — `2` since the framework check was added. Older v1
  files are upgraded automatically on the next write by
  `check_framework.py`; no manual migration is needed.
- Unknown/extra keys are preserved by the scripts; do not remove the file —
  it is the skill's memory across sessions.

---

## 10. Recommended full-run order

1. `python scripts/setup_toolbox.py --check-only --json` — read-only overview; report to the user what would be installed/downloaded.
2. Ask the user the R2 Blender question **before any Blender install**; note the answer.
3. `python scripts/setup_toolbox.py --json` — full default setup (R0, R1, R3, R4, R5, R7; R2 status-only).
4. Targeted branches as agreed with the user: `--install ida-mcp` / `--install cutter` + `--install cutter-mcp` / `--install blender` + `--install blender-mcp` / `--install x64dbg` / `--install x64dbg-mcp`.
5. Complete the manual steps printed in `actions` (IDA restart, Cutter plugin copy, x32dbg token, Blender addon install).
6. Start the relevant tools, then prove everything: `python scripts/verify_mcp.py --server all` — every server the workflow will use must show `test: "pass"` with real data. Port-open or process-running alone is NOT a pass (R6).
7. `python scripts/check_framework.py --json` — the framework mod host must be present and valid before any sub-mod work (section 8). With `--fix` the script installs the verified reference build; if the download fails it prints the x86 source-build fallback.
8. Only now start modding work (see references/modding-workflow.md).
