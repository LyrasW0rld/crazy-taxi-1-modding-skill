---
name: crazy-taxi-modder
description: Build, test, and deploy native DLL mods for Crazy Taxi 1 (1999, PC, Win32 x86). Use when the user asks to mod Crazy Taxi on PC - add gameplay features, cheats or trainers, restore Dreamcast-licensed content, replace textures, 3D models or spline routes, debug a broken mod, or reverse-engineer the game. Covers MCP-driven RE tooling (IDA Pro / Cutter, x64dbg, Blender), offset_list.json address lookups, the framework-mod check/download/build flow, C++/Rust sub-mod development, standalone mods, and deployment to mods/<name>/<name>.dll.
---

# Crazy Taxi 1 Modding Skill

## Overview

- **Target game:** Crazy Taxi 1 (1999), native Win32 x86 executable (`Crazy Taxi.exe`). There is no script engine or managed runtime - all modding happens at the native level via DLL mods.
- **Mod form:** Native DLLs loaded through a `dinput8.dll` proxy loader and integrated as sub-mods into Lyra's Framework Mod (in-game menu on Shift+F1). The sub-mod C ABI, build setup, and deployment layout are defined in `SUBMOD_DEVELOPMENT_GUIDE.md` - read it before writing any mod code. Standalone DLLs (no framework integration) are the alternative route - see "Standalone mods" below.
- **Loader prerequisite (framework mod):** sub-mods are loaded by the `dinput8.dll` proxy loader and integrated into Lyra's Framework Mod (`<gameDir>/mods/Framework_MOD/Framework_MOD.dll`, Shift+F1 menu). **Before any sub-mod development, check the framework** with `python scripts/check_framework.py` - see "Framework mod (submod default)" below. If it is missing or invalid, a deployed sub-mod loads but silently never initializes. The framework's complete source ships in this skill at `Examples/Standalone MOD`, and a validated download source plus build flow are documented in `references/framework-mod.md`; `setup_toolbox.py` (R4 step) reports the loader's presence, and `references/modding-workflow.md` re-checks it before every deploy.
- **Fundamentals first:** `references/fundamentals.md` (world-Y sign, VQ texture encoding, 32-bit x86, position authority, 60 Hz frame clock, BAM angles, `__usercall` conventions, PC version history 2012/2014/2016 and why all addresses match across versions) is mandatory reading before every mod and is updated after every RE session - same duty as `offset_list.json`.
- **Parallel RE sessions:** when more than one agent queries the disassembler concurrently - or a whole batch of addresses needs analysis - follow `references/batch-re-queries.md`: file-based request/response queue in the game directory, ONE IDB writer, atomic writes, no MCP request storms, coordinator-only merges into shared files.
- **Knowledge capture (no findings lost):** verified RE findings are routed at the end of every session - addresses into `offset_list.json`, action rules into `references/fundamentals.md`, everything else that is fundamentally verified (two independent methods or dynamic confirmation, tier T2/T3) into the ledger `references/verified_knowledge.json`. Protocol: `references/verified-knowledge.md`.
- **Address source of truth:** `offset_list.json` (workspace root, ~813 KB, 991 curated entries; deduplicated against IDA Pro on 2026-11-14) is the ONLY source for memory addresses, functions, variables, and struct layouts. Never invent or guess an address. Structure and rules: `references/offset-list-schema.md`.
- **Workspace layout:**

```text
SKILL.md                        this file - start here
references/
  modding-workflow.md           full RE + modding runbook (follow the R13 order)
  fundamentals.md               fundamental game facts - MANDATORY reading before every mod
  framework-mod.md              framework mod: check / download / build / repair flow
  offset-list-schema.md         structure of offset_list.json + maintenance rules
  batch-re-queries.md           parallel RE query protocol (multi-agent sessions)
  verified-knowledge.md         knowledge ledger schema + update protocol
  verified_knowledge.json       ledger of verified RE facts (T2/T3 active; T1 provisional)
  texture-upscaling.md          Real-ESRGAN default pipeline for texture upscaling (R21)
  mod-logging.md                mandatory per-run mod logging + 5-file rotation (R23)
  environment-setup.md          detailed environment setup runbook
  mcp-servers.md                MCP server profiles, ports, collision rules
scripts/
  setup_toolbox.py              installs RE/MCP tooling into <skill>/Toolbox
  verify_mcp.py                 real functional verification of every MCP server
  find_game.py                  locates Crazy Taxi.exe, writes config/environment.json
  check_framework.py            checks <gameDir>/mods/Framework_MOD/Framework_MOD.dll
config/environment.json         persisted game path + environment facts
Toolbox/                        installed tools and MCP servers
offset_list.json                curated address database - the only address source
SUBMOD_DEVELOPMENT_GUIDE.md     sub-mod C ABI, build, deployment, best practices
Examples/                       reference mod sources (see "Examples" below)
```

## Environment Setup (run once per machine, in this order)

**Step 0 - Python.** Check `python --version`. If Python 3 is missing, install it automatically (e.g. `winget install -e --id Python.Python.3` or the python.org installer) and then continue - do not stop to ask.

**Step 1 - Static RE tooling (IDA Pro or Cutter).** Run `python scripts/setup_toolbox.py --check-only --json` to see what is installed.
- IDA Pro installed -> ensure the ida-pro MCP server is available: `python scripts/setup_toolbox.py --install ida-mcp`.
- IDA Pro NOT installed -> use Cutter instead (cutter.re): `python scripts/setup_toolbox.py --install cutter`, then `python scripts/setup_toolbox.py --install cutter-mcp` (the Cutter MCP server, github.com/ap425q/cuttermcp).

Everything installs into `<skill>/Toolbox`.

**Step 2 - Blender (only if the user wants asset work).** Ask the user first, exactly: **"Do you want to edit textures, splines, or 3D objects?"**
- Yes -> `python scripts/setup_toolbox.py --install blender` and `python scripts/setup_toolbox.py --install blender-mcp`.
- No -> skip this step entirely. Never install Blender unasked.
- No user reachable in this run -> treat the answer as No: skip, and record the open question in the report (R15 non-interactive fallback below).

**Step 3 - Debugger (always).** `python scripts/setup_toolbox.py --install x64dbg` and `python scripts/setup_toolbox.py --install x64dbg-mcp` (x64dbg/x32dbg plus the MCP server from github.com/duty1g/x64dbg-mcp-server).

**Step 4 - Locate the game.** Run `python scripts/find_game.py`. It writes the Crazy Taxi.exe path to `config/environment.json` so the game is only searched once. Re-scan later with `python scripts/find_game.py --reindex`. If it cannot find the game, ask the user for the install folder and point the script at it.

**Step 5 - Compiler toolchain.** Detect what is installed: MSVC (`where cl`), GCC/Clang (`gcc --version`, `clang --version`), and/or Rust (`cargo --version`, `rustc --version`). Mods are built in C++ or Rust depending on what is installed; **prefer C++ when both are available**. If no toolchain exists, ask the user before installing one. C++ build commands: `SUBMOD_DEVELOPMENT_GUIDE.md`, chapter 5 (sub-mods) and `references/framework-mod.md` section 5 (framework/standalone).

**Step 6 - Real functional verification (mandatory).** Run `python scripts/verify_mcp.py --server all` (or per server: `--server ida|cutter|x64dbg|blender`). Every MCP server must pass a REAL functional call with a domain-meaningful answer - an open port or a registered-but-unresponsive server is a failure, not a success. Also confirm that no two MCP servers collide (ports, single-instance limits): see `references/mcp-servers.md`.

Details, troubleshooting, and the full runbook: `references/environment-setup.md`.

## Modding Workflow (always in this order)

**Decide the mod type FIRST (before everything else):** ask the user whether they want a **submod** or a **standalone mod**. Submod (default) = framework integration with the Shift+F1 menu; **advantage: the user can keep adding more mods later** - sub-mods coexist. Standalone = one self-contained DLL; **the user effectively runs one standalone mod at a time** (standalone mods own their hooks exclusively, so a second one cannot be added on top the way sub-mods can). Default to submod when the user has no preference or is unreachable. The answer decides whether the framework check below runs at all (submod -> mandatory; standalone -> skip).

1. **Research first:** read `SUBMOD_DEVELOPMENT_GUIDE.md` (mandatory before every new mod), `references/fundamentals.md` (mandatory before every mod - fundamental game facts incl. the PC version history and why addresses match across versions), and the matching Examples folder; then search `offset_list.json` - it is the first stop for every new mod or feature.
2. **Framework check (submod route only):** if the mod-type decision was submod, verify the framework mod BEFORE any development - `python scripts/check_framework.py [--fix] [--json]`. Valid -> continue. Missing/invalid -> fix flow (download or build): `references/framework-mod.md`. Skip this step entirely on the standalone route.
3. **Static analysis:** the ida-pro MCP server, or the Cutter MCP server if IDA Pro is not installed. Multi-agent sessions and address batches run through the queue protocol `references/batch-re-queries.md` (batch `idat` pass preferred over live MCP). Every verified finding is recorded at session end: into `offset_list.json`, `references/fundamentals.md`, and the ledger `references/verified_knowledge.json` (`references/verified-knowledge.md`).
4. **Dynamic analysis:** the x64dbg MCP server - when a mod misbehaves, for complex gameplay features, and for 3D modding. Before debugging, give the user concrete in-game instructions (where to be, what to trigger) so breakpoints and observations land. On a game or mod crash, ALWAYS triage Windows Reliability Records FIRST (R22) and read the mod's newest run log (R23) before attaching a debugger.
5. **Assets:** the Blender MCP server for textures, splines, and 3D models - only after studying `Examples/Texture,3D,Spline Edit` (the game encodes assets differently than you would expect). Texture upscaling/improvement defaults to Real-ESRGAN (R21): `references/texture-upscaling.md`.
6. **Python** as glue and interface only - the shipped mod is a native DLL.
7. **Build** in C++ (preferred) or Rust as 32-bit x86 DLL -> **deploy** to `<game dir>/mods/<modname>/<modname>.dll` -> verify in the real game. Every mod embeds per-run file logging with 5-file rotation (R23): `references/mod-logging.md`.

Full runbook with every detail: `references/modding-workflow.md`.

## Framework mod (submod default)

- The default route is a **sub-mod** under `<gameDir>/mods/<modname>/<modname>.dll`, integrated into the framework's Shift+F1 menu (C ABI: `SUBMOD_DEVELOPMENT_GUIDE.md`). **Advantage over standalone: extensible** - sub-mods coexist in the menu and the user can keep adding more mods later; a standalone mod effectively means one mod at a time (exclusive hooks). The route (submod vs standalone) is decided with the user FIRST - see "Modding Workflow".
- **Before every sub-mod development**, check the framework mod for existence and validity: `<gameDir>/mods/Framework_MOD/Framework_MOD.dll`, with paths resolved from the persisted `gameDir` in `config/environment.json` - no hardcoded paths. Automated:

```cmd
python scripts/check_framework.py [--fix] [--json]
```

  Exit code 0 -> proceed. Exit 1 or 2 (missing or invalid) -> fix flow, in this order: (a) **download** the checksum-verified package (verified URL and validation rules: `references/framework-mod.md` section 4), or (b) **build** the DLL from the complete source in `Examples/Standalone MOD` (C++ mandatory, 32-bit x86 mandatory, concrete `cl`/`gcc`/`clang` commands: `references/framework-mod.md` section 5). Exit 3 -> no usable `gameDir` in config: run `python scripts/find_game.py` first (not the fix flow). Exit 4 -> the `--fix` run failed: follow the printed x86 source-build fallback (`references/framework-mod.md` section 5). Download and install of third-party binaries are consent-gated (R15) - ask the user before proceeding.
- `--fix` lets the script attempt automated remediation; the manual flows always work (the download flow, framework-mod.md section 4, needs nothing extra - it extracts the FULL archive into the game folder, **no extra subfolder, with forced overwrite of existing files**; the source-build flow additionally needs the build dependencies - section 5.2: the MinHook / nlohmann/json sources are not in this workspace and their download sources are deliberately undocumented - obtain them from the user; with no user reachable, fix flow B stops at dependency acquisition and records the open question). Re-verified before every deploy: `references/modding-workflow.md`, "Deploy & verify" step 0.

## Standalone mods

- A **standalone mod** is a self-contained DLL - own `DllMain`, own hooks, own config and logging - deployed to the same `<gameDir>/mods/<modname>/<modname>.dll` path but WITHOUT the sub-mod ABI, so it never appears in the Shift+F1 menu and does not require `Framework_MOD.dll` (the proxy loader loads every `mods/<subfolder>/*.dll` on its own).
- **Limitation - one standalone mod at a time:** a standalone mod owns its hooks exclusively; a second standalone mod hooking the same targets (EndScene/WndProc/global hooks) conflicts with the first and cannot be added on top the way sub-mods can. If the user expects to keep adding mods over time, recommend the submod route.
- **Choose standalone instead of a submod when:** no in-game UI is needed; the mod must hook what the framework already owns (a second EndScene/WndProc hook conflicts with it); or the target machine runs without the framework.
- **Coding example / template: `Examples/Standalone MOD`** - the complete C++ source of Lyra's Framework Mod v1.2, i.e. a full standalone mod DLL. Copy it to a build work directory and adapt: entry point with process guard, crash handler, ordered subsystem init, clean detach shutdown (`main.cpp`); DLL-relative config persistence with JSON (`config.cpp`/`config.h`); logging + x86 crash dump (`logger.h`/`logger.cpp`); MinHook gameplay hooks (`game_features.cpp`); D3D9 render hook + ImGui menu (`render_hook.cpp`). It needs external dependencies (Dear ImGui 1.90.9, MinHook, nlohmann/json - not in the workspace) and 32-bit x86 build commands: `references/framework-mod.md` section 5.
- The rules in `references/fundamentals.md` apply to standalone mods equally.

## Core Rules

- **offset_list.json first (R8, R16):** search it before any analysis; never invent addresses; correct and extend it at the end of every RE session so no knowledge is lost. Schema and maintenance rules: `references/offset-list-schema.md`. Apply the same end-of-session update duty to `references/fundamentals.md` and to the ledger `references/verified_knowledge.json` (`references/verified-knowledge.md`: only tier T2/T3 findings count as fundamentally verified; T1 findings are recorded as `provisional`).
- **Parallel RE discipline (R20):** more than one agent at the disassembler always goes through `references/batch-re-queries.md` - one IDB writer, atomic per-agent files, batched queries instead of per-agent MCP calls, coordinator as the single writer of all shared skill files.
- **Texture upscaling default (R21):** for texture upscaling or texture improvement the default tool is Real-ESRGAN (https://github.com/xinntao/real-esrgan) whenever the user names no other tool - but only after the fit check passes (`references/texture-upscaling.md`).
- **Crash triage (R22):** when the game or a mod crashes, ALWAYS run `Get-CimInstance -ClassName Win32_ReliabilityRecords` FIRST (Windows stores crash records: faulting module, exception code) before any other diagnosis, then read the newest mod run log (R23), then escalate to x64dbg.
- **Mod logging (R23):** every mod embeds file logging - ONE new log file per game run, automatic rotation keeping at most 5 files (oldest deleted), no log flooding (throttled messages, no per-frame lines). Requirement + pattern: `references/mod-logging.md`.
- **Relative paths only (R14):** no hardcoded absolute paths inside mods - resolve file access from the DLL's own directory or the game root.
- **Ask the user (R15):** actively ask when anything is unclear or when a decision is better made by a human (feature scope, destructive changes, toolchain installs, licensed content).
  - **Non-interactive fallback (no user reachable in this run):** never silently assume YES. Consent gates for installs/downloads (Blender R2, toolchain install, third-party loader/framework components) default to **No** - skip the branch, record the open question in the report, and continue with work that needs nothing gated. Presence gates for in-game work (breakpoint briefing, anything touching the running game or user data) **stop at the gate**: leave a written handoff (what was planned, what the user must do, where to resume) instead of proceeding. A missing game path stays null in config - never guess one.
- **Keep the workspace tidy (R17):** one folder per mod, meaningful names, no throwaway scripts or temp files left behind.
- **Deploy target (R18):** finished mods go to `<game dir>/mods/<modname>/<modname>.dll` - folder name must match the DLL name.
- **Portability (R19):** all skill files are English; MCP servers are addressed generically ("the ida-pro MCP server", "the x64dbg MCP server", "the Blender MCP server") so the skill works on any host.

## Examples (read the real thing before coding)

- **Before EVERY new mod:** `SUBMOD_DEVELOPMENT_GUIDE.md` - completely, including chapter 7 (conflict prevention). Caution: the guide's chapter 4 starter example and chapter 7 item 3 use example addresses that contradict references/fundamentals.md B2/B5/B6 - never copy a guide example address without checking fundamentals.md first (same warning: modding-workflow.md Step 0).
- **New gameplay features (R12):** `Examples/Game Features` - `dc_restoration.cpp`, `game_features.cpp`, `render_hook.cpp`. These files are excerpts of the complete framework source in `Examples/Standalone MOD` (where their headers - `config.h`, `game_features.h`, `render_hook.h`, ... - live); they demonstrate hook styles (MinHook, naked hooks with inline asm, `.rdata` byte patches), patch patterns, and framework integration. Read as pattern references, not drop-in code - the external dependencies (imgui, MinHook, nlohmann/json) are not in the workspace.
- **Textures, 3D models, splines (R11):** `Examples/Texture,3D,Spline Edit` - read `REPLACE_3D_AND_SPLINES_GUIDE.md`, `FEATURE_LIST.md`, and `Crazy Taxi Texture Modding Guide.pdf` first. This folder contains the complete, shipping LyrasTextureMod (a standalone mod) and documents the engine's real asset rules.
- **Framework mod source & standalone template:** `Examples/Standalone MOD` - the complete source of Lyra's Framework Mod v1.2 (the host mod) plus the `loader/` proxy-loader source and `proxy.def`. Doubles as the standalone-mod coding template (see "Standalone mods" above); check/download/build/repair flow: `references/framework-mod.md`.

## Host Notes (Claude / ZCode / Codex / Antigravity)

- This skill is a plain folder: `SKILL.md` + `references/` + `scripts/` + `config/`. It contains no host-specific tool calls; if user input is needed, ask the user in plain language.
- **Loading:** place the folder in the host's skills directory so the frontmatter `description` triggers it (typical triggers: "mod Crazy Taxi", "Crazy Taxi trainer", "Dreamcast restoration", "replace textures/models/splines"). If the host has no native skill mechanism, reference `SKILL.md` from the host's instruction file (AGENTS.md or equivalent) and read it on demand.
- **MCP registration:** register the installed MCP servers in whichever MCP configuration the host uses, pointing at the servers under `<skill>/Toolbox` (see `references/mcp-servers.md`).
- **On any host:** read `SKILL.md` first, then pull `references/*.md` only when the relevant section applies; run the `scripts/*.py` CLIs with `python`.
