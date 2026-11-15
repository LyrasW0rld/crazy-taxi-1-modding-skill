# Modding Workflow - Crazy Taxi 1

Follow the phases in this exact order (R13). Skipping ahead is how mods end up patching the wrong bytes.

```text
(D) Mod type decision: submod (default) or standalone - ask the user FIRST
      |
(0) References + fundamentals.md + offset_list.json search       <- always next
      |
(F) Framework check (gate before ANY submod development; submod route only)
      |      python scripts/check_framework.py -> fix flow: references/framework-mod.md
      |
(1) Static analysis: ida-pro / Cutter MCP server    -> findings go into offset_list.json
      |                                                + fundamentals.md + verified_knowledge.json
      |                                                  (update obligation, references/verified-knowledge.md)
(2) Dynamic analysis: x64dbg MCP server             <- when: broken mod, complex gameplay, 3D
      |                                                (brief the user BEFORE breakpoints)
(3) Assets: Blender MCP server                      <- textures / splines / 3D only
      |
(4) Python glue
      |
Build: C++ (preferred) or Rust, 32-bit x86 DLL        (standalone/framework build: framework-mod.md)
      |
Deploy: <game dir>/mods/<modname>/<modname>.dll -> verify in the real game
      |        (every mod logs per-run: R23, references/mod-logging.md)
      |
Crash? -> R22 triage FIRST: Win32_ReliabilityRecords + newest mod log -> then Step 2
```

## Mod type decision - FIRST, before anything else

**Define the mod type with the user BEFORE any other work** (R15 - this is a
human decision, and everything downstream depends on it):

- **Submod (default):** a DLL with the `GetSubModName` ABI, registered in the
  framework's Shift+F1 menu. **Advantage: the user can keep adding more mods
  later** - sub-mods coexist (each gets its own menu page, enable/disable
  toggle, and SEH safety gate), and this is the route every future mod builds
  on. Requires the framework check (gate F below).
- **Standalone:** a self-contained DLL without the ABI exports.
  **Limitation: the user effectively runs one standalone mod at a time** - a
  standalone mod owns its hooks exclusively (render/WndProc/global hooks
  conflict with a second mod taking the same targets, guide ch. 7), so a
  second standalone mod cannot be added on top the way sub-mods can.
- Ask the user which they want, stating these trade-offs in one or two
  sentences. Default to **submod** when the user has no preference or is
  unreachable - it is the extensible route. The answer decides whether the
  framework check (F) runs at all: submod -> mandatory; standalone -> skip it.

## Step 0 - References, fundamentals and offset_list.json (mandatory, every mod)

1. Read `SUBMOD_DEVELOPMENT_GUIDE.md` completely before EVERY new mod. It defines the sub-mod C ABI (`GetSubModName`, `IsSubModEnabled`, `SetSubModEnabled`, `InitSubMod(ctx, IDirect3DDevice9*)`, `RenderSubModUI`, plus optional metadata exports), the CMake build setup, the deployment layout, and the conflict rules (chapter 7): no second D3D9 EndScene hook, no second WndProc hook, check the in-game state flag before memory writes, `VirtualProtect` before touching `.rdata` constants, disable cleanly. **Caution: the guide's example code (chapter 4 starter example, chapter 7 item 3 "Always Check In-Game State") predates the RE audit - its example addresses contradict `references/fundamentals.md` B2/B5/B6 (it writes the speedometer/camera state block instead of the physics authority). Never copy an example address from the guide without checking it against fundamentals.md first** (see the "Contradiction on record" notes in B2, B5 and B6).
2. Read `references/fundamentals.md` completely (mandatory). It holds the standing game fundamentals and action rules - world/camera Y sign, VQ texture encoding incl. GUI sprites, 32-bit x86 / image base, PC version history 2012/2014/2016 and why all addresses match (A3), position authority, teleport state set, 60 Hz frame clock, BAM angles, `__usercall` conventions, `.rdata` caps - each backed by `offset_list.json` evidence. **Update obligation:** at the end of every RE session, re-verify the rules touched by your findings and extend the file with newly derived rules - same end-of-session duty as `offset_list.json` (R16).
3. Read the matching Examples:
   - New gameplay features -> `Examples/Game Features` (`dc_restoration.cpp`, `game_features.cpp`, `render_hook.cpp`). These files are excerpts of the complete framework source in `Examples/Standalone MOD`, where their headers (`config.h`, `game_features.h`, `render_hook.h`, ...) live. Read them as pattern references, not as drop-in code - the external dependencies (imgui, MinHook, nlohmann/json) are still not in the workspace.
   - Textures / 3D models / splines -> `Examples/Texture,3D,Spline Edit` (what to read there is specified in Step 3).
   - Standalone mods -> `Examples/Standalone MOD` (see "Submod (default) or standalone?" below).
4. Search `offset_list.json` FIRST (R8). Structure, address notation, and search recipes: `references/offset-list-schema.md`. Search by keyword across `identifier`, `functions`, `search_tags`, and filter by `category`. All addresses in it are absolute virtual addresses for the 32-bit process with image base `0x00400000` - except the non-canonical shapes documented in the schema (one FILE offset, tuples/ranges, struct-field annotations).
5. Grep the knowledge ledger `references/verified_knowledge.json` for your topic/tags BEFORE starting RE work: T2/T3 entries are verified building blocks you can build on directly; a `provisional` (T1) entry requires re-verification before you build on it (`references/verified-knowledge.md`). This avoids re-deriving already-verified facts.
6. If the address you need is NOT in the list, it does not exist yet. Do not guess, and do not copy an address from an unverified source. Obtain it in Step 1 / Step 2, then record it (R16).

## Framework check - mandatory gate before any submod development

The default mod route is a sub-mod: `<game dir>/mods/<modname>/<modname>.dll`, integrated into the framework's Shift+F1 menu (`SUBMOD_DEVELOPMENT_GUIDE.md`). A sub-mod is loaded by the proxy loader (stage 1) but only integrated by `Framework_MOD.dll` (stage 2, detection via the `GetSubModName` export) - without a valid framework it is loaded but never initialized, silently. So check the framework BEFORE writing any sub-mod code:

```cmd
python scripts/check_framework.py [--fix] [--json]
```

- Checks `<game dir>/mods/Framework_MOD/Framework_MOD.dll` for existence and validity. Paths resolve from the persisted `gameDir` in `config/environment.json` - no hardcoded paths.
- Exit code 0 -> framework valid, continue with Step 1. Exit 1 or 2 (missing or invalid) -> fix flow, in this order:
  1. **Download** the checksum-verified package (verified URL, validation rules, install steps; consent gate R15): `references/framework-mod.md` section 4.
  2. **Or build it** from the complete source in `Examples/Standalone MOD` (C++ mandatory, 32-bit x86 mandatory, concrete cl/gcc/clang commands): `references/framework-mod.md` section 5.
- Exit code 3 -> no usable `gameDir` in config: run `python scripts/find_game.py` first - do NOT enter the download/build fix flow (full exit-code table: `references/environment-setup.md` section 8.1).
- Exit code 4 -> the `--fix` run failed: follow the printed x86 source-build fallback (`references/framework-mod.md` section 5).
- No user reachable in this run (R15 non-interactive fallback): consent-gated downloads default to No - record the open question and continue with work that does not depend on the framework (analysis, `offset_list.json`, standalone route).
- Re-check before every deploy: "Deploy & verify" step 0.

## Submod (default) or standalone? Choosing the mod type

Decided FIRST with the user (see "Mod type decision - FIRST" above). The decision and its trade-offs are defined ONCE there; this section only adds the per-route implementation pointers:

- **Submod implementation path:** ABI, starter code, and build setup: `SUBMOD_DEVELOPMENT_GUIDE.md`; the framework check (gate F) is mandatory on this route.
- **Standalone implementation path:** a self-contained DLL with its own `DllMain`, hooks, config, and logging; same deploy path `<gameDir>/mods/<name>/<name>.dll` (the proxy loader loads every `mods/<subfolder>/*.dll`, `loader_main.cpp:250-271`), but WITHOUT the `GetSubModName` export, so the framework never registers it. Runs without Framework_MOD.dll. Precedent: LyrasTextureMod (`Examples/Texture,3D,Spline Edit`) ships as a standalone mod - the framework detection deliberately skips it (`render_hook.cpp:891-895`).
- **Coding example / template: `Examples/Standalone MOD`.** The complete source of Lyra's Framework Mod v1.2 - itself a full standalone mod DLL. Copy-and-adapt points: entry point with process guard, crash handler, ordered subsystem init, clean detach shutdown (`main.cpp:13`); DLL-relative config persistence with JSON (`config.cpp`/`config.h`); logging + x86 crash dump (`logger.h`/`logger.cpp`); MinHook gameplay hooks with whole-state writes (`game_features.cpp`); D3D9 render hook + ImGui menu (`render_hook.cpp`). Dependency list (imgui 1.90.9, MinHook, nlohmann/json - not in the workspace) and build commands: `references/framework-mod.md` section 5.
- Fundamentals (`references/fundamentals.md`) apply to both routes equally.

## Step 1 - Static analysis (ida-pro MCP server / Cutter MCP server)

Use whichever stack Environment Setup Step 1 installed: the **ida-pro MCP server** when IDA Pro is present, otherwise the **Cutter MCP server**. If NEITHER stack is installed or verified yet, run `references/environment-setup.md` first (Environment Setup Steps 0-6) - do not improvise an unverified tool setup here.

**Multi-agent rule (R20):** when more than one agent needs disassembler data at the same time - or a batch of addresses must be checked - do NOT let every agent call the MCP server directly. Follow `references/batch-re-queries.md`: a headless `idat` batch pass over a file-based request queue (preferred), or one queue-owner serializing live MCP calls. This avoids request storms, single-instance collisions (Cutter port 8000, ida-pro-mcp port hopping) and concurrent IDB writes.

Do at minimum:
- Load `Crazy Taxi.exe` (path from `config/environment.json`).
- For every candidate address taken from `offset_list.json`: confirm function boundaries, calling convention, and what it actually reads/writes. Check callers via cross-references before hooking.
- For new features: locate the relevant functions, variables, and globals. Work from strings and cross-references to code - not the other way around - and dynamically confirm a candidate (Step 2) before building on it.
- For struct/field work: document the layout with real, verified offsets.

Deliverable of Step 1 (R16): at the END of the session, write every verified finding into `offset_list.json` - corrections in place, new findings as full 9-field entries (schema: `references/offset-list-schema.md`) - and route the rest per `references/verified-knowledge.md`: action rules into `references/fundamentals.md`, other fundamentally verified facts (tier T2/T3) into `references/verified_knowledge.json`. Findings that are not written down are lost.

## Step 2 - Dynamic analysis (x64dbg MCP server)

Use the **x64dbg MCP server** when any of these hold:
- A mod misbehaves at runtime (crash, wrong values, hooks not firing) and static analysis is not enough.
- The feature is complex gameplay logic (physics, streaming, state machines, economy) where runtime data flow matters.
- 3D / asset modding: confirming injection points, chunk budgets, or texture paths at runtime.

Crazy Taxi is a 32-bit process - the debugging side is x32dbg; the x64dbg MCP server controls both.

**Parallel debugging (R20):** debugger state is global - when more than one agent works, ALL x64dbg requests serialize through ONE queue owner (`references/batch-re-queries.md`, tool-specific constraints); never run parallel breakpoints from two agents.

**MANDATORY before any breakpoint (R15): brief the user in concrete in-game terms FIRST.** Tell the user exactly what to do so the breakpoint hits and the observation is meaningful, for example:
- which menu or game mode to enter,
- where to drive, which action to perform, and at what moment,
- what must be visible on screen while you watch.

Wait for the user to confirm they are in position, then attach and set breakpoints. Typical loop: breakpoint on a verified address from `offset_list.json` -> read registers/memory at the hit -> confirm or discard the hypothesis -> adjust. Confirm every assumption dynamically before writing a mod around it.

No user reachable in this run (R15 non-interactive fallback, SKILL.md)? Do not set breakpoints blind: stop at this gate and leave the briefing as a written handoff (planned breakpoint addresses, the in-game state that must be reached, where work resumes) instead of attaching to the running game.

If the same approach fails three times in a row: stop, record what you learned in `offset_list.json`, and switch approach.

## Step 3 - Assets: Blender MCP server (textures, splines, 3D models)

**Warning (R11): read `Examples/Texture,3D,Spline Edit` BEFORE touching assets.** The game encodes assets differently than you would expect - strict power-of-two texture sizes and aspect rules, GBIX/CRC-based slot discipline in the Sega containers, byte-budgeted mesh chunks, Y-up right-handed coordinate conventions, and injection into archive files. The folder documents the real pipeline; do not freelance. Read, in this order:

1. `REPLACE_3D_AND_SPLINES_GUIDE.md` - authoritative 3D-model and spline-route replacement guide (coordinate conventions, capacity = bytes, chunk budgets, waypoint structures, topology rules).
2. `Crazy Taxi Texture Modding Guide.pdf` - hard texture engine limits, VRAM budget, naming conventions.
3. `FEATURE_LIST.md` - how the shipping LyrasTextureMod solves replacement (hooks, cache formats, hotkeys).
4. Source references: `TextureEngine.h/.cpp`, `ModelEngine.h/.cpp`, `AssetExtractor.h/.cpp`, `ModelTextureViewer.cpp`. (The folder is header-complete but expects external dependencies - MinHook, imgui - per the development guide.)

Use the **Blender MCP server** for authoring: build or adjust meshes, UVs, and spline geometry in Blender, export (OBJ or scripted output), then follow the compile/inject conventions from the guides above (auto-rescale onto the bounding sphere, axis conversion, chunk byte budget with zero padding).

**Texture upscaling / texture improvement (R21):** the DEFAULT tool is **Real-ESRGAN** (https://github.com/xinntao/real-esrgan) whenever the user names no other tool - but only after the fit check (GUI sprites/masks/hard byte budgets do NOT fit) and always through the engine-ordered pipeline (VQ/PVR decode first, power-of-two + budget re-encode, one-sample style check with the user before batch jobs): `references/texture-upscaling.md`.

Scope note: the "Y-up" conventions above are the guides' model/spline authoring domain. Memory-level world/camera coordinates follow `references/fundamentals.md` A1 (up = negative Y, verify the sign per coordinate block) - do not mix the two when writing position math.

Note: some artifacts referenced by `REPLACE_3D_AND_SPLINES_GUIDE.md` (e.g. `spline_tool.py`, `replace_3d_model.py`, `ida_findings/*.md`) are NOT in this workspace. If you need their behavior, reimplement it or ask the user - do not assume the files exist.

## Step 4 - Python as support / interface

Python is glue, not the product:
- searching and updating `offset_list.json`,
- asset conversion, manifest, and cache tooling; quick calculations,
- small automation around the MCP servers and build steps.

The shipped mod itself is always a native DLL. Per the project convention in `Examples/Texture,3D,Spline Edit` (native extraction and backup tooling), runtime mod features avoid Python dependencies entirely - pure C++/Win32.

## Build

- **Prefer C++** (R5, R10). Target: 32-bit x86 DLL. Toolchain: MSVC + CMake; the exact `vcvars32` / CMake commands and a ready-made `CMakeLists.txt` are in `SUBMOD_DEVELOPMENT_GUIDE.md`, chapter 5.
- **Sub-mods must build against the framework's pinned Dear ImGui 1.90.9** (`references/framework-mod.md` section 5.2): `InitSubMod` hands the sub-mod the framework's live `ImGuiContext*` (`render_hook.cpp:951-954`) and the guide's CMake template compiles the ImGui core into the sub-mod DLL - a sub-mod built against a different ImGui version operates on a foreign context layout and crashes in-game, only visible after deploy + game start.
- **Rust** is acceptable if that is the installed toolchain - still produce a 32-bit DLL exporting the same sub-mod C ABI. No ready-made Rust template exists in this workspace (TODO) - minimum setup: `Cargo.toml` with `crate-type = ["cdylib"]`; 32-bit target `i686-pc-windows-msvc` (`rustup target add i686-pc-windows-msvc`, then `cargo build --release --target i686-pc-windows-msvc` - the default host triple emits an x64 DLL that only fails in-game at LoadLibrary, `loader_main.cpp:222`; with MinGW tooling use `i686-pc-windows-gnu`); exports as `#[no_mangle] pub extern "C" fn` - the ABI is cdecl with undecorated names (the framework looks each export up by exact name via `GetProcAddress`, `render_hook.cpp:917-949`; the loader typedefs at `render_hook.cpp:911-915` are plain cdecl), signatures per the ABI table in `SUBMOD_DEVELOPMENT_GUIDE.md` ch. 2. Verify the artifact per the pre-check bullet below before deploying.
- **Framework or standalone builds:** concrete `cl` / `gcc` / `clang` commands, dependency layout (imgui 1.90.9, MinHook, nlohmann/json), and the x86 architecture verification are in `references/framework-mod.md` section 5. C++ is mandatory there.
- **Verify the built sub-mod DLL itself before deploy** - `check_framework.py` covers only the framework host and the proxy loader, never your own mod. The artifact must be a PE32 x86 image (`dumpbin /headers <modname>.dll` reports `14C machine (x86)`; without dumpbin use the Python PE check from `references/framework-mod.md` section 4.2 step 5 - same procedure as for framework/standalone builds, section 5.4), and on the submod route it must export `GetSubModName` (ABI: `SUBMOD_DEVELOPMENT_GUIDE.md` chapter 2). An accidentally x64-built DLL is only rejected in-game at LoadLibrary time (`loader_main.cpp:222` logs "[ERROR] Failed to load plugin"), i.e. after build + deploy + game start. No offline load test without the installed game is documented in this workspace (TODO) - while `gameDir` is still null after the R15 fallback, the static PE/export check is the only documented verification of the artifact.
- Respect chapter 7 of the guide (no second EndScene/WndProc hook, game-state check, `VirtualProtect` for `.rdata`, clean disable). Reuse the `Logger.*` and `IniConfig.*` patterns from `Examples/Texture,3D,Spline Edit` instead of inventing new plumbing.
- **Embed per-run logging from the start (R23):** one new `logs/run_YYYYMMDD_HHMMSS.log` per game run, max-5 rotation, throttled messages, crash-safe writes - requirement and the exact pattern: `references/mod-logging.md`. A mod without this logging cannot be debugged via the crash-triage flow (R22).

## Deploy & verify (R18)

0. **Check the loader chain first - submod route:** `<game dir>/dinput8.dll` (proxy loader, game root) and `<game dir>/mods/Framework_MOD/Framework_MOD.dll` (host mod) must both exist and be valid - run `python scripts/check_framework.py` (see the gate before Step 1). Without them the deployed sub-mod is silently never loaded: no Shift+F1 menu, no sub-mod in the sidebar, and you would debug the wrong cause. Note: the exit code reflects only the framework DLL - the proxy loader is checked informationally and never changes it - so also read the loader verdict from the report (`loader` key with `--json`, or the `loader ...` line on stderr) and require `exists` AND `valid` before deploying; an exit-code-only agent would deploy past a broken loader. `--fix` extracts the FULL archive into the game folder with forced overwrite (existing files incl. `dinput8.dll` are replaced - framework-mod.md section 4.3); the whole install is consent-gated (R15). If the check fails, run the fix flow in `references/framework-mod.md` (download - section 4, or build from source - section 5; consent gate R15, non-interactive fallback if nobody is reachable).
   **Standalone route instead:** `Framework_MOD.dll` is deliberately NOT required - check only that `<game dir>/dinput8.dll` exists (informational, so the proxy loader will load your DLL) and deploy; do NOT enter the framework fix flow for a component the route was chosen to avoid.
1. Install the finished DLL as `<game dir>/mods/<modname>/<modname>.dll` - folder name and DLL name must match (the framework loads `mods/<name>/<name>.dll`). The game directory is in `config/environment.json`.
2. Launch `Crazy Taxi.exe`. Open the Mod Menu with **Shift+F1**; the sub-mod appears in the left sidebar. If the menu does not open, re-check step 0 (missing loader) before suspecting your DLL.
3. Verify the feature in the REAL game, not only in logs: trigger the feature, observe the effect, and capture proof (screenshot or concrete description). Check the mod's newest run log for errors (R23). If the game or the mod CRASHES during verification, run crash triage (R22, section below) immediately - do not start guessing.
4. Mods must use **relative paths only** (R14): resolve any file access from the DLL's own directory or the game root - never a hardcoded `C:\...` path.
5. Every mod embeds **per-run file logging** (R23): one new `logs/run_YYYYMMDD_HHMMSS.log` per game run, automatic rotation keeping at most 5 files (oldest deleted), no log flooding. Requirement, pattern, and the debugging flow: `references/mod-logging.md`. Build it in from the first prototype - a mod without logging is undebuggable in the field.

## Crash triage - ALWAYS first when the game or a mod crashes (R22)

Windows records every application crash (faulting module, exception code) in the
reliability database. When the game or a mod crashes - hard crash, CTD, mod DLL
fault - run this check BEFORE any other diagnosis:

```powershell
Get-CimInstance -ClassName Win32_ReliabilityRecords |
  Where-Object { $_.Message -match 'Crazy Taxi' -or $_.ProductName -match 'Crazy Taxi' } |
  Sort-Object TimeGenerated -Descending |
  Select-Object -First 10 TimeGenerated, SourceName, ProductName, Message |
  Format-List
```

Read from the records:
- **Faulting module** - `Crazy Taxi.exe` itself, `mods\<name>\<name>.dll` (your mod),
  a hook dependency (d3d9.dll/dinput8.dll), or something else entirely. This alone
  decides whether the crash is a mod bug, a hook conflict, or engine instability.
- **Exception code** - `0xC0000005` access violation (bad pointer/offset),
  `0xC0000094` divide by zero, `0x80000003` breakpoint, stack overflow, etc.
- **Offset within the module** - match it against `offset_list.json` entries of that
  module's address region (image base `0x00400000`) to identify the failed function.

Then, in order:
1. Read the **newest mod run log** (R23, `references/mod-logging.md`): which init or
   runtime step was the last to complete? The first ERROR line localizes the failure.
2. Correlate module + exception code + last log step into ONE hypothesis.
3. Only then escalate to x64dbg (Step 2 above) - the user briefing gate (R15) still
   applies; the log facts make the breakpoint plan concrete.

If ReliabilityRecords shows nothing (some silent exits never reach WER), fall back
to the Application event log (`Get-WinEvent -FilterHashtable @{LogName='Application'; Id=1000,1001}`) - but the ReliabilityRecords check itself is never skipped.

## Rules that apply throughout

- **Ask the user (R15)** when requirements are unclear or a decision belongs to a human: feature scope, destructive changes (saves, original game files), toolchain or tool installs, anything touching licensed branding (see how `Examples/Game Features/dc_restoration.cpp` structures opt-in restoration with full backup and rollback).
- **Keep the project tidy (R17):** one folder per mod, meaningful file names, no throwaway scripts, no temp files left behind; properly integrate or delete experiments.
- **offset_list.json after every RE activity (R16):** corrections and additions at the end of each session - that is how this skill accumulates knowledge.
- **fundamentals.md after every RE activity:** the same update duty applies to `references/fundamentals.md` (Step 0, item 2) - re-verify affected rules, extend with newly derived rules, each with evidence. Fundamentals hold action rules; addresses stay in `offset_list.json`.
- **verified_knowledge.json after every RE activity:** non-address findings that are fundamentally verified (two independent methods = T2, or dynamic confirmation = T3; protocol: `references/verified-knowledge.md`) go into the ledger - tool quirks, binary behavior, version facts, mislabeling verdicts. T1 single-method findings are recorded as `provisional`. Search before adding; contradictions are recorded, never deleted.
- **Parallel RE discipline (R20):** more than one agent at the disassembler always runs through `references/batch-re-queries.md` - one IDB writer, atomic per-agent files, batched queries, coordinator-only writes to shared skill files.
- **Texture upscaling default (R21):** Real-ESRGAN is the default for texture upscaling/improvement when the user names no tool - after the fit check (`references/texture-upscaling.md`).
- **Crash triage (R22):** game/mod crash -> `Get-CimInstance -ClassName Win32_ReliabilityRecords` FIRST (section above), then the newest mod run log (R23), then x64dbg.
- **Mod logging (R23):** per-run log files (`logs/run_*.log`), max-5 rotation, no flooding; requirement and debugging flow: `references/mod-logging.md`.
- **Ownership and scope:** only mod a copy you own; this is a single-player offline game; never bypass DRM or anti-cheat.
