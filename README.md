# Crazy Taxi 1 Modding Skill

An AI-agent-ready skill for building, testing, and deploying native DLL mods for **Crazy Taxi 1** (1999, PC, Win32 x86). It drives MCP-connected reverse-engineering tooling (IDA Pro / Cutter, x64dbg, Blender), ships a curated **991-entry address database** (`offset_list.json`, IDA-verified 2026-11-14), covers the framework-mod check/download/build flow, C++/Rust sub-mod development, standalone mods, texture/3D/spline editing, and deployment into the real game.

The skill is a plain folder of English documentation and Python helper scripts — host-tool agnostic: it works with any MCP-capable AI coding tool (ZCode, Claude Code, Codex, Antigravity, ...).

---

## One-line install for AI coding agents

Paste this single line into your AI coding tool:

```text
Clone https://github.com/LyrasW0rld/crazy-taxi-1-modding-skill.git into your skills directory (folder name "CT1 Skill"), then read its SKILL.md completely and follow it exactly, starting with "Environment Setup (run once per machine, in this order)".
```

That one line is the whole setup contract: `SKILL.md` drives everything else —

1. `scripts/setup_toolbox.py` installs the RE/MCP tooling into `<skill>/Toolbox` (IDA Pro or Cutter, x64dbg + MCP servers, Blender only if you opt in; every download is consent-gated).
2. `scripts/verify_mcp.py` proves every MCP server with a REAL, data-bearing call — "port open" is a failure, not a pass.
3. `scripts/find_game.py` locates `Crazy Taxi.exe` (Steam AppID 71230) and writes `config/environment.json`.
4. `scripts/check_framework.py` verifies the `dinput8.dll` proxy loader + `Framework_MOD.dll` host before any sub-mod work (auto-fix via verified download or source build).
5. `references/modding-workflow.md` runs the actual modding loop (research → framework gate → static → dynamic → assets → build → deploy → crash triage), with `offset_list.json` as the ONLY address source and mandatory knowledge capture at the end of every RE session.

If no user is reachable mid-run, the skill's consent gates default to **No** and it leaves a written handoff instead of proceeding silently.

## Manual install

```bash
git clone https://github.com/LyrasW0rld/crazy-taxi-1-modding-skill.git "CT1 Skill"
```

- Host with a native skills directory → place the folder there so the `SKILL.md` frontmatter triggers it (typical triggers: "mod Crazy Taxi", "Crazy Taxi trainer", "Dreamcast restoration", "replace textures/models/splines").
- Host without a skill mechanism → reference `SKILL.md` from your host's instruction file (`AGENTS.md` or equivalent) and have the agent read it on demand.

## Repository layout

| Path | Content |
|---|---|
| `SKILL.md` | entry point — the agent reads this first and it routes everything |
| `SUBMOD_DEVELOPMENT_GUIDE.md` | sub-mod C ABI, CMake build, deployment, conflict rules |
| `references/` | workflow runbook, game fundamentals, framework-mod flow, MCP server registry, offset-list schema, knowledge ledger + schema, batch RE protocol, mod logging, texture upscaling, environment setup |
| `scripts/` | `setup_toolbox.py`, `verify_mcp.py`, `find_game.py`, `check_framework.py` |
| `config/environment.example.json` | shape of the per-machine config (the real `environment.json` is generated, never committed) |
| `offset_list.json` | 991-entry verified address database — the only address source |
| `Examples/` | reference mod sources: Lyra's Framework Mod v1.2 (`Standalone MOD`), feature excerpts (`Game Features`), shipping texture/3D/spline mod + guides (`Texture,3D,Spline Edit`) |
| `sync.bat` | commit-and-push helper used by the optional auto-sync task |

## What is deliberately NOT in this repository

| Path | Why |
|---|---|
| `.zcode/` | local agent-session artifacts (workflow drafts/runs) — owner requirement: never uploaded |
| `Toolbox/` | per-machine RE/MCP tool installs (built by `setup_toolbox.py`, several hundred MB) |
| `config/environment.json` | machine-specific game path, toolchain and MCP facts (may hold a Bearer token) — scripts recreate it |
| `offset_list.*backup*.json` | local rollback snapshots |
| `__pycache__/`, `*.pyc` | Python bytecode |

## After cloning

Run the environment setup exactly as `SKILL.md` says. The first run installs tools into `Toolbox/` and writes `config/environment.json` — both stay local and are git-ignored. Nothing in this repository contains game files, leaked keys, or hardcoded machine paths.

## Auto-sync (optional, per machine)

`sync.bat` commits all non-ignored changes and pushes to `origin/main`; a Windows scheduled task (`CT1Skill AutoSync`, hourly) runs it automatically. Run it manually any time by double-clicking. Remove the task with `schtasks /delete /tn "CT1Skill AutoSync"`.

## Third-party notices

`Examples/Standalone MOD/Game_ref.*`, `CrazyTaxi_FFB_ref.*` (FFB Arcade Plugin, GPL v3+) and `EngineFixes_ref.cpp` are excerpted pattern references from GPL-licensed modding projects — see [`THIRD-PARTY-NOTICES.md`](THIRD-PARTY-NOTICES.md). They are documentation-only and excluded from every build (`references/framework-mod.md` §5.3).

## Scope and ownership

Only mod a copy you own; Crazy Taxi 1 is a single-player offline game; never bypass DRM or anti-cheat. Licensed-content restoration (KFC/Pizza Hut/Fila) is opt-in with full backup and rollback, and consent-gated by the skill.
