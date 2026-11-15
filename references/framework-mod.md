# Framework Mod - check, download, build, repair

Everything about **Lyra's Framework Mod v1.2** (`Framework_MOD.dll`): the host mod that
provides the Shift+F1 in-game menu, the D3D9/ImGui pipeline, and the sub-mod integration
that every default-route mod builds on. Its complete C++ source ships in this skill at
`Examples/Standalone MOD` (25 files) - the folder is both the framework's source of truth
and the standalone-mod coding template.

Companion documents:
- `SUBMOD_DEVELOPMENT_GUIDE.md` - sub-mod C ABI, build setup, deployment, conflict rules
  (sub-mod authoring is defined there, not duplicated here).
- `references/modding-workflow.md` - where the framework check sits in the workflow
  (mandatory gate before any sub-mod development, re-check before every deploy).
- `references/fundamentals.md` - mandatory game fundamentals (32-bit x86, axes, VQ, BAM).
  They apply to every framework-mod change as well.

## 1. Path conventions (no hard-coded paths)

- `gameDir` = the directory containing `Crazy Taxi.exe`. It is persisted in
  `config/environment.json` (field `gameDir`, written by `scripts/find_game.py`).
  **Never hardcode an absolute path** - always resolve from the persisted `gameDir`.
- If `gameDir` is null: run `python scripts/find_game.py`. If it still finds nothing,
  ask the user for the install folder. A missing game path stays null - never guess one.
- Fixed locations (all relative to `<gameDir>`):

| Path | Role |
|---|---|
| `<gameDir>/dinput8.dll` | proxy loader (stage 1), game root |
| `<gameDir>/mods/Framework_MOD/Framework_MOD.dll` | the framework mod (stage 2 host) |
| `<gameDir>/mods/Framework_MOD/crazytaxi_mod_config.json` | framework configuration |
| `<gameDir>/mods/Framework_MOD/mod.log` | framework log incl. crash dumps |
| `<gameDir>/mods/loader.log` | proxy loader log (rewritten each session) |
| `<gameDir>/mods/<modname>/<modname>.dll` | standard deploy target for mods (R18) |
| `<gameDir>/dxvk_optional/d3d9.dll` | optional DXVK (Vulkan) d3d9 replacement |

- **Exception to the generic `<modname>` convention:** the framework's folder and DLL
  names are FIXED as `Framework_MOD`. The proxy loader looks for exactly
  `mods/Framework_MOD/Framework_MOD.dll` (`Examples/Standalone MOD/loader/loader_main.cpp:176`).

## 2. What gets loaded, and when (loader chain)

Two stages, both verified against the real source in `Examples/Standalone MOD`:

**Stage 1 - proxy loader `dinput8.dll`** (`loader/loader_main.cpp`):
- Exports `DirectInput8Create` and forwards it to the system dinput8
  (`loader_main.cpp:49`; `proxy.def`: `LIBRARY "dinput8"` / `EXPORTS DirectInput8Create @1`).
- `DllMain` (`loader_main.cpp:302`) acts only for `Crazy Taxi.exe`; it patches the Steam
  restart check by overwriting the IAT slot at `0x0053432C` with a no-op stub
  (`loader_main.cpp:286-300`), creates `mods/`, then runs `LoadModsFromDirectory`
  (`loader_main.cpp:103`):
  1. DXVK sync: per `"enableDXVK": true` in
     `mods/Framework_MOD/crazytaxi_mod_config.json`, deactivate (`DeleteFileA`) or
     activate (copy from `dxvk_optional/d3d9.dll`) `<gameDir>/d3d9.dll`
     (`loader_main.cpp:117-173`).
  2. Load `mods/Framework_MOD/Framework_MOD.dll` first via `LoadLibraryA`
     (`loader_main.cpp:176-199`; looks up the export `ProcessKeyboardBuffer` via
     `GetProcAddress`, `loader_main.cpp:184`). Failure is logged, not fatal.
  3. Load EVERY `mods/<subfolder>/*.dll` and `*.asi` (`loader_main.cpp:250-271`),
     skipping `dinput8.dll`/`steam_api.dll`; `CT-DC.asi` and `SilentPatchCT.asi` are
     skipped deliberately - their behavior is reimplemented natively inside
     Framework_MOD.dll (`loader_main.cpp:202-207`).
- The loader itself calls NO sub-mod functions. **Loading is not integration.**

**Stage 2 - framework sub-mod detection** (inside Framework_MOD.dll, `render_hook.cpp`):
- `RefreshSubMods()` (`render_hook.cpp:873-961`) runs at least every 3000 ms, snapshots
  the process modules (`CreateToolhelp32Snapshot`), skips its own modules
  (`Crazy Taxi.exe`, `Framework_MOD.dll`, `LyrasTextureMod.dll`, `dinput8.dll`,
  `steam_api.dll`), and treats every loaded module that exports `GetSubModName()` as a
  sub-mod.
- Sub-mod ABI (authoritative table: `SUBMOD_DEVELOPMENT_GUIDE.md` ch. 2) - required:
  `GetSubModName`, `IsSubModEnabled`, `SetSubModEnabled(bool)`,
  `InitSubMod(void* ImGuiContext, IDirect3DDevice9*)`, `RenderSubModUI(void* ctx)`;
  optional: `GetSubModAuthor`, `GetSubModVersion`, `GetSubModDescription`.
  `InitSubMod` is called ONCE at discovery with the framework's ImGui context and D3D9
  device (`render_hook.cpp:951-954`).
- Every sub-mod call is wrapped in an SEH safety gate; after 3 exceptions the sub-mod is
  quarantined and disabled (`render_hook.cpp:737-810`).

**Consequence:** a sub-mod (exports `GetSubModName`) is useless without a valid framework;
a standalone DLL (no ABI exports) runs from stage 1 alone. That is the submod/standalone
split documented in `references/modding-workflow.md`.

**Framework public API** - the shipped DLL exports exactly these 8 functions (verified
against the downloadable package): `GetFrameworkImGuiContext`,
`GetFrameworkD3D9Device`, `SetFrameworkStatusMessage` (`render_hook.h:10-14`),
`ProcessKeyboardBuffer` (`game_features.h:291`), `GetModConfig` (`config.cpp:11`),
`ModAPI_IsVulkanEnabled`, `ModAPI_SetVulkanEnabled`, `ModAPI_ToggleVulkan`
(`game_features.h:294-298`).

## 3. The check (run before any sub-mod work)

```cmd
python scripts/check_framework.py [--fix] [--json]
```

- Purpose: existence and validity check of `<gameDir>/mods/Framework_MOD/Framework_MOD.dll`
  (resolved relative to the persisted `gameDir` - no hardcoded paths).
- `--fix` lets the script attempt automated remediation instead of only reporting;
  `--json` prints a machine-readable report (keys: `references/environment-setup.md`
  section 8.6). The exact check list and report shape are defined in the script
  itself - read `scripts/check_framework.py`'s docstring before scripting against it.
- Exit codes (kept in sync with the script docstring and
  `references/environment-setup.md` section 8.1; these are check_framework.py's own
  codes, NOT the 0/1/2 convention of the sibling scripts `setup_toolbox.py` /
  `verify_mcp.py`):

| Code | Meaning |
|---|---|
| 0 | framework DLL present and valid - or `--fix` installed it successfully |
| 1 | framework DLL missing (`<gameDir>/mods/Framework_MOD/Framework_MOD.dll` does not exist) |
| 2 | framework DLL present but invalid - read the reasons (structure, not x86, missing exports) - or present but unreadable (locked by the running game / antivirus; the reason names it) |
| 3 | no usable `gameDir` in `config/environment.json` - run `python scripts/find_game.py` first; do NOT enter the fix flow |
| 4 | `--fix` failed (download / validation / install) - the script prints the x86 source-build fallback and points at section 5 |

- **Argparse collision:** a CLI usage error (e.g. an unknown flag) exits with
  argparse's own code **2** - the same number as "framework DLL present but
  invalid". Disambiguate: an argparse exit 2 prints usage text and produces NO JSON
  report; a real verdict always carries the check report (never treat a bare usage
  error as "invalid DLL").

- Operationally: **exit code 0 -> framework valid, proceed. Exit 1 or 2 -> missing
  or invalid -> run the fix flow below (download: consent gate R15). Exit 3 -> no
  game path yet: run `python scripts/find_game.py` first and only ask the user for
  the install folder if it still finds nothing (section 1). Exit 4 -> follow the
  printed source-build fallback or section 5.**
- The proxy loader `<gameDir>/dinput8.dll` is re-checked separately before every deploy
  (`references/modding-workflow.md`, "Deploy & verify" step 0).

"Valid" means at minimum (the same bar the download validation in section 4 applies):
PE32 x86 DLL (`MZ` at offset 0, `PE\0\0` signature, Machine `0x014C`, DLL flag
`Characteristics & 0x2000`, OptionalHeader magic `0x10b`), a non-empty export table
containing the framework exports above, and - when the file came from the download
package - the reference SHA256 from section 4.2 step 5.

## 4. Fix flow A - download (verified source and validation rules)

**Consent gate first (R15):** downloading and installing third-party binaries into the
game directory is a consent-gated install - ask the user before proceeding, and tell
them the install extracts the FULL archive into the game folder with existing files
being overwritten (section 4.3). No user reachable in this run -> default No, record
the open question in the report, continue with framework-independent work (analysis,
`offset_list.json`, standalone route).

### 4.1 Download URL

- **Use this URL form** (verified working, 2026-11-14):
  `https://drive.usercontent.google.com/download?id=1HQ-6peIb6fPL7cv95ldtKZpWcVD19-M6&export=download&confirm=t`
  (`confirm=t` is required; a `uuid` query parameter may be appended but is optional).
- **Do NOT use** the canonical form `https://drive.google.com/uc?export=download&id=1HQ-6peIb6fPL7cv95ldtKZpWcVD19-M6`
  or its `&confirm=t` variant: the `drive.google.com` host 303-redirects and strips the
  `confirm` parameter, landing on the HTML virus-scan page (verified; also true with
  `confirm=t` appended to the `drive.google.com` URL - only the `drive.usercontent.google.com`
  host honors it).
- If you receive an HTML page anyway (title `Google Drive - Virus scan warning`, ~2476
  bytes, form action `https://drive.usercontent.google.com/download` with hidden inputs
  `id`, `export=download`, `confirm=t`, `uuid`): switch to the usercontent URL form above
  with `confirm=t`.

### 4.2 Validate before installing (all criteria AND-combined)

Work in a NEW, EMPTY quarantine directory; keep downloads away from workspace and game
files; extract with a zip library - never execute the archive or anything extracted
before validation passes.

1. **HTTP result:** final `200` (`206` only for a ranged request) with
   `Content-Type: application/octet-stream` and
   `Content-Disposition: attachment; filename="lyras_framwork_mod_48600.zip"`
   (the filename typo "framwork" is part of the real name).
2. **Archive identity:** exact size **180186115** bytes (byte-exact, not "at least");
   SHA256 `ba7c96a6440e002dc612cd6c3f0a558ab389b17dd3c1c9c0c675f7594123cb91`.
3. **Container magic:** first 4 bytes are `PK\x03\x04` (a ZIP). **Do not apply an
   MZ/PE check to the archive** - the top-level artifact is a ZIP, not a DLL; an MZ
   check on it would wrongly reject the correct download.
4. **ZIP entries present** (names exactly): `mods/Framework_MOD/Framework_MOD.dll`,
   `dinput8.dll`, `ReadMe_Installation.txt` - these three are the REQUIRED set
   (`check_framework.py` rejects the download without them, `REQUIRED_ZIP_ENTRIES`).
   `dxvk_optional/` (optional DXVK Vulkan replacement) ships in the real archive but
   is NOT enforced as a required entry.
5. **PE check on the EXTRACTED `mods/Framework_MOD/Framework_MOD.dll`** (2,864,128 bytes,
   SHA256 `7a5d93bb35eb052f1b131a311ba5296a022180993d8ab56a4508924855bb923e`):
   first bytes `MZ`; `e_lfanew` (uint32 LE at `0x3C`) points at signature `PE\0\0`;
   Machine (uint16 LE at `e_lfanew+4`) == `0x014C` (x86/32-bit - mandatory, the game is
   a 32-bit process); `Characteristics & 0x2000` (IMAGE_FILE_DLL) set; OptionalHeader
   magic `0x10b` (PE32); export directory (DataDirectory[0]) RVA != 0 and Size != 0
   (the 8 ModAPI exports from section 2).
6. **Proxy loader check:** extracted `dinput8.dll` (153,600 bytes, SHA256
   `f4ff139fdaf5a5399fbc9c6ec2051034594f1e92b4154bc598719ac47b3ee9d9`) is also a PE32
   x86 DLL exporting `DirectInput8Create`.

**Reject - treat the download as invalid, delete it, do not install:**
- any HTML body (`Content-Type: text/html`, virus-scan warning title, `<!DOCTYPE html>`
  prefix, `Content-Length: 2476`) -> `confirm=t` missing; retry with the usercontent URL;
- size or SHA256 mismatch (corruption, changed file version on Drive, tampering);
- missing or renamed ZIP entries (especially a missing
  `mods/Framework_MOD/Framework_MOD.dll`);
- extracted DLL without `MZ`/PE signature;
- Machine != `0x014C` (especially `0x8664` = x64 - never load an x64 DLL into this
  32-bit game);
- missing DLL flag or empty export table on the extracted Framework DLL.

### 4.3 Install (after every check passed) - FULL extraction, forced overwrite

The install extracts **EVERY archive entry** into the game folder root,
**overwriting existing files by design** (user requirement: forced overwrite is
mandatory during extraction). There is **no extra subfolder** - the ZIP's own
folder structure maps directly into `<gameDir>`:

- `mods/Framework_MOD/*` -> `<gameDir>/mods/Framework_MOD/`
- `dinput8.dll` -> `<gameDir>/dinput8.dll` - an existing proxy loader IS
  replaced (the archive was byte-verified before anything was touched; the
  consent gate for the whole install is at the top of section 4)
- `dxvk_optional/` -> `<gameDir>/dxvk_optional/` (the loader activates DXVK
  only when `"enableDXVK": true`, by copying `dxvk_optional/d3d9.dll` to
  `<gameDir>/d3d9.dll`, and deactivates it by deleting that file)
- `ReadMe_Installation.txt` -> `<gameDir>/ReadMe_Installation.txt` - the
  vendor readme; treat it as secondary documentation, this file and
  `SUBMOD_DEVELOPMENT_GUIDE.md` are authoritative for the skill.

`check_framework.py --fix` implements exactly this: it extracts every entry
into `<gameDir>` (zip-slip-guarded, write-then-replace so a locked file
aborts cleanly as exit 4) and re-verifies the installed framework DLL.
Manual installs follow the same layout.

Then verify: `python scripts/check_framework.py` must pass.

## 5. Fix flow B - build from the shipped source

Fallback when the download is unavailable or fails validation: build
`Framework_MOD.dll` (and, if needed, the proxy loader) from the complete source in
`Examples/Standalone MOD`.

### 5.1 Ground rules

- **C++ is mandatory.** The source is C++/Win32; there is no Rust path for the framework
  itself (R10 prefers C++ anyway).
- **32-bit x86 is mandatory.** Crazy Taxi is a Win32 PE x86 process at image base
  `0x00400000` (`offset_list.json`, `Global & Pointers` / Base Address), and the source
  hard-depends on it: ~130 absolute 32-bit game addresses in `game_features.h` (range
  `0x004xxxxx`-`0x01Exxxxx`), an IAT patch at fixed `0x0053432C`
  (`loader_main.cpp:286-300`), direct pointer dereference of `0x013B9F0C`
  (`dc_restoration.cpp:44-60`), and an explicit x86 register dump (`logger.cpp:185`,
  "Registers (x86)"). An x64 build fails at these points - verify every artifact reports
  Machine `0x014C` before installing.
- **Do not build inside `Examples/`.** Copy `Examples/Standalone MOD` into a fresh build
  work directory and add dependencies and build files there; `Examples/` is read-only
  reference material (R17).

### 5.2 Dependencies (not shipped - obtain first)

The sources include, relative to the mod source folder, dependencies that are NOT in the
workspace:
- `../deps/imgui/imgui.h`, `imgui_internal.h`, `backends/imgui_impl_dx9.h`,
  `backends/imgui_impl_win32.h` (`render_hook.cpp:10-13`, `input_hook.cpp:2`) -
  Dear ImGui, pinned to **1.90.9** by the comment at `render_hook.cpp:11`. The pin
  applies to sub-mod builds too: `InitSubMod` hands the sub-mod the framework's LIVE
  `ImGuiContext*` (`render_hook.cpp:951-954`), and `SUBMOD_DEVELOPMENT_GUIDE.md`
  ch. 5 compiles the ImGui core into the sub-mod DLL - a sub-mod built against a
  different ImGui version operates on a foreign context layout and crashes in-game.
- `<MinHook.h>` (`main.cpp:3`, `render_hook.cpp:14`, `game_features.cpp:19`) - MinHook.
- `../deps/nlohmann/json.hpp` (`config.cpp:5`) - nlohmann/json.

The build tree must therefore look like:

```text
<build-root>/
  Standalone MOD/        <- copied framework source (see 5.3 for the file split)
  deps/
    imgui/               <- Dear ImGui 1.90.9 (core sources + backends)
    MinHook/             <- MinHook (include/ + sources, or a prebuilt 32-bit static lib)
    nlohmann/json.hpp
```

The exact dependency versions/URLs beyond "ImGui 1.90.9" are unknown (TODO) - obtain
them from the user or a source the user names; do not guess download URLs. Without a
user reachable, this is a hard stop for fix flow B (R15): record the open question
and continue with framework-independent work - do not guess URLs or versions and do
not substitute different libraries for these includes (the source hard-depends on
their exact APIs).

### 5.3 What compiles into what

No build files exist in the source tree - create them (the CMake template in
`SUBMOD_DEVELOPMENT_GUIDE.md` ch. 5 is a starting point for a 32-bit DLL). Two artifacts:

1. **`Framework_MOD.dll`** - compile `main.cpp`, `config.cpp`, `logger.cpp`,
   `input_hook.cpp`, `render_hook.cpp`, `game_features.cpp`, `dc_restoration.cpp`
   (includes `fila_textures.h`), `engine_fixes.cpp`, `ffb_arcade.cpp`, plus the
   dependency sources (ImGui core + `imgui_impl_dx9.cpp` + `imgui_impl_win32.cpp`,
   MinHook sources) or link MinHook as a 32-bit static lib. All public exports are
   declared `extern "C" __declspec(dllexport)` in the headers - no .def file needed.
2. **`dinput8.dll`** (proxy loader) - compile `loader/loader_main.cpp` with `proxy.def`
   (`LIBRARY "dinput8"` / `EXPORTS DirectInput8Create @1`).

**EXCLUDE from both builds:** `Game_ref.*`, `CrazyTaxi_FFB_ref.*`, `EngineFixes_ref.cpp`.
They are pattern references from GPL projects with missing includes (`Game.h`,
`../Common Files/Game.h`, `Utils/MemoryMgr.h`, `Utils/Patterns.h`) and do not compile.

### 5.4 Toolchain commands (all must produce a 32-bit x86 DLL)

**MSVC (`cl`) - recommended.** The source uses MSVC structured exceptions
(`__try/__except`, e.g. the sub-mod safety gate in `render_hook.cpp:737-810`), so MSVC
is the safest toolchain. Open the Visual Studio developer environment for the x86
target first (`vcvarsall.bat x86` - `setup_toolbox.py` detects `cl` and prints this
note), then run from the copied source folder:

```cmd
cl /nologo /EHsc /O2 /MD /std:c++17 /LD /D_CRT_SECURE_NO_WARNINGS ^
   /I..\deps\imgui /I..\deps\imgui\backends /I..\deps\MinHook\include /I..\deps\nlohmann ^
   main.cpp config.cpp logger.cpp input_hook.cpp render_hook.cpp game_features.cpp ^
   dc_restoration.cpp engine_fixes.cpp ffb_arcade.cpp ^
   ..\deps\imgui\imgui.cpp ..\deps\imgui\imgui_draw.cpp ..\deps\imgui\imgui_tables.cpp ^
   ..\deps\imgui\imgui_widgets.cpp ..\deps\imgui\backends\imgui_impl_dx9.cpp ^
   ..\deps\imgui\backends\imgui_impl_win32.cpp ^
   ..\deps\MinHook\buffer.c ..\deps\MinHook\hook.c ..\deps\MinHook\trampoline.c ^
   ..\deps\MinHook\HDE\hde.c ^
   /Fe:Framework_MOD.dll /link user32.lib gdi32.lib d3d9.lib dxguid.lib imm32.lib
```

The MinHook sources are compiled in above (they resolve `MH_Initialize` /
`MH_CreateHook` / ...); the link library set is a starting set - add whatever the
linker reports as further unresolved externals. If you have a prebuilt 32-bit
`MinHook.lib`, link it instead of the MinHook sources above. `imm32.lib` is required
by the ImGui Win32 backend (`imgui_impl_win32.cpp`, IME support). Proxy loader:

```cmd
cl /nologo /EHsc /O2 /MD /LD loader\loader_main.cpp ^
   /Fe:dinput8.dll /link /DEF:proxy.def user32.lib kernel32.lib
```

**MinGW-w64 gcc (32-bit):**

```bash
i686-w64-mingw32-g++ -m32 -std=c++17 -O2 -shared -static-libgcc -static-libstdc++ \
  -I../deps/imgui -I../deps/imgui/backends -I../deps/MinHook/include -I../deps/nlohmann \
  -o Framework_MOD.dll \
  main.cpp config.cpp logger.cpp input_hook.cpp render_hook.cpp game_features.cpp \
  dc_restoration.cpp engine_fixes.cpp ffb_arcade.cpp \
  ../deps/imgui/imgui.cpp ../deps/imgui/imgui_draw.cpp ../deps/imgui/imgui_tables.cpp \
  ../deps/imgui/imgui_widgets.cpp ../deps/imgui/backends/imgui_impl_dx9.cpp \
  ../deps/imgui/backends/imgui_impl_win32.cpp \
  ../deps/MinHook/buffer.c ../deps/MinHook/hook.c ../deps/MinHook/trampoline.c \
  ../deps/MinHook/HDE/hde.c \
  -luser32 -lgdi32 -ld3d9 -luuid -limm32
```

Caveat: MSVC structured exceptions (`__try/__except`) are not a portable GCC feature on
32-bit Windows - if GCC rejects the SEH blocks in `render_hook.cpp`, build with MSVC
instead. Proxy loader with gcc (the .def export is stdcall, so name decoration must be
stripped with `--kill-at`):

```bash
i686-w64-mingw32-g++ -m32 -O2 -shared -o dinput8.dll loader/loader_main.cpp proxy.def -Wl,--kill-at
```

**clang:** use the MSVC-compatible driver with the exact same flags and file list as
the `cl` command above (including the MinHook sources and `imm32.lib`):
`clang-cl /nologo /EHsc /O2 /MD /std:c++17 /LD ... /Fe:Framework_MOD.dll /link user32.lib gdi32.lib d3d9.lib dxguid.lib imm32.lib`
- or the GNU driver with the gcc command above (same SEH caveat).

**Verify the architecture of every artifact before installing** (an x64 DLL cannot load
into the 32-bit game): `dumpbin /headers Framework_MOD.dll` must report
`14C machine (x86)`; without dumpbin, use the same Python PE check as in section 4.2
step 5.

### 5.5 Install and verify

1. Copy `Framework_MOD.dll` to `<gameDir>/mods/Framework_MOD/Framework_MOD.dll`
   (folder and DLL name fixed - section 1).
2. If the proxy loader was also (re)built: `dinput8.dll` -> `<gameDir>/dinput8.dll`
   (consent gate R15 if one already exists).
3. Run `python scripts/check_framework.py` - it must pass.
4. Launch the game and confirm the Shift+F1 menu opens.

## 6. Repair flow (the framework mod misbehaves)

Symptoms: Shift+F1 menu does not open; sub-mods load but never appear in the sidebar;
crash at game start; `[ERROR] Failed to load Framework_MOD` in the loader log.

1. **Read the logs:** `<gameDir>/mods/loader.log` (rewritten every session,
   `loader_main.cpp:105-113`) and `<gameDir>/mods/Framework_MOD/mod.log` (framework log
   with crash dumps, default path of `Logger::Initialize`).
2. **Check the config first:** `mods/Framework_MOD/crazytaxi_mod_config.json` -
   `"enableDXVK"` drives d3d9.dll activation/deactivation (`loader_main.cpp:117-173`);
   a stale `<gameDir>/d3d9.dll` breaks the framework's D3D9 hooks.
3. **Inspect the source** at `Examples/Standalone MOD` to locate the faulty module:
   framework entry point `main.cpp:13` (process guard, Logger + crash handler,
   `MH_Initialize`, `LoadConfig`, feature/fixes/FFB init, render hook init, clean
   detach shutdown), loader entry point `loader_main.cpp:302`, sub-mod integration
   `render_hook.cpp:737-810` (SEH gate) and `render_hook.cpp:873-961` (detection).
4. **Fix in the build work directory** (copy the source there first - never edit
   `Examples/`): adjust the C++ code.
5. **Rebuild as DLL - C++ mandatory, 32-bit x86 mandatory** (section 5.1 and 5.4).
   Never byte-patch the installed DLL: changes must go through the source so they are
   reproducible and re-checkable.
6. **Reinstall** (section 5.5), run `python scripts/check_framework.py`, and verify the
   behavior in the real game (Shift+F1 menu, sub-mod sidebar, affected features).
