#!/usr/bin/env python3
"""Locate the Crazy Taxi 1 (PC) executable and persist the path (skill rule R4).

Search order:
  1. The path already stored in config/environment.json (unless --reindex).
  2. Steam libraries (libraryfolders.vdf + appmanifest for AppID 71230).
  3. Windows registry Uninstall keys (DisplayName matching "Crazy Taxi").
  4. A bounded scan of typical install locations (Program Files, Games folders).

Only *confirmed* matches are persisted. Confirmed = the executable name is
in KNOWN_EXE_NAMES **and** the file is a 32-bit PE (machine i386) - the
skill targets the Win32 x86 build. Loose matches (folder looks like Crazy
Taxi but the exe name is unknown) and known-name executables that fail the
32-bit PE check are reported as candidates, never saved. NOTE: even a
confirmed match proves the location, not the game VERSION - the CD port and
the 2011 Dreamcast Collection release use the same exe names and are also
32-bit, but have different addresses than offset_list.json (see
references/environment-setup.md section 5).

Output:
  - Human-readable progress goes to stderr.
  - A single-line JSON object goes to stdout (machine/agent consumable):
      {"found": true, "crazyTaxiExe": "...", "gameDir": "...",
       "source": "config|steam|registry|filesystem", "candidates": [...],
       "configUpdated": false, "message": "..."}

CLI (exact):
  python scripts/find_game.py [--reindex]

Exit codes:
  0  game executable found (and persisted, unless only candidates were found)
  1  not found - ask the user for the path and store it manually in
     config/environment.json ("crazyTaxiExe" + "gameDir"), then re-run
  2  configuration or environment error (unparsable config, etc.)

Python 3.8+, standard library only, Windows-oriented (registry steps are
skipped on other platforms). All paths are resolved relative to the script
location; no hard-coded machine paths.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
from pathlib import Path

try:
    import winreg  # Windows only
except ImportError:  # non-Windows host
    winreg = None

SCRIPT_DIR = Path(__file__).resolve().parent
SKILL_ROOT = SCRIPT_DIR.parent
CONFIG_PATH = SKILL_ROOT / "config" / "environment.json"

CONFIG_TEMPLATE = {
    "schemaVersion": 1,
    "crazyTaxiExe": None,
    "gameDir": None,
    "toolboxDir": "Toolbox",
    "toolchain": None,
    "preferredCompiler": None,
    "mcpServers": {},
}

# Verified in this skill's research: the Steam release ("Crazy Taxi",
# AppID 71230, delisted 2024-12-06 but playable if already owned) installs to
# <library>\steamapps\common\Crazy Taxi and its executable is "Crazy Taxi.exe"
# (referenced by the Steam Community analog-fix instructions and PCGamingWiki).
STEAM_APP_ID = "71230"
KNOWN_EXE_NAMES = {"crazy taxi.exe", "crazytaxi.exe"}

# PE machine type for 32-bit x86 executables (the only build this skill
# targets; offsets in offset_list.json are invalid for other architectures).
IMAGE_FILE_MACHINE_I386 = 0x014C


def is_32bit_pe(path):
    """True if path is a PE executable with machine == i386 (32-bit x86).
    Read-only header check (stdlib only): DOS 'MZ' magic, e_lfanew at 0x3C,
    'PE\\0\\0' signature, machine field at PE+4. False for non-PE files,
    64-bit exes, and unreadable files."""
    try:
        with open(path, "rb") as fh:
            dos = fh.read(0x40)
            if len(dos) < 0x40 or dos[:2] != b"MZ":
                return False
            e_lfanew = int.from_bytes(dos[0x3C:0x40], "little")
            if not 0 < e_lfanew < 0x1000000:
                return False
            fh.seek(e_lfanew)
            pe = fh.read(6)
    except OSError:
        return False
    return len(pe) == 6 and pe[:4] == b"PE\x00\x00" and \
        int.from_bytes(pe[4:6], "little") == IMAGE_FILE_MACHINE_I386


def pick_confirmed(candidates):
    """First candidate that is a confirmed match: exe name in
    KNOWN_EXE_NAMES and a 32-bit PE header. Non-PE / 64-bit executables
    with a known name are reported (stderr) but never picked."""
    for candidate in candidates:
        if os.path.basename(candidate["path"]).lower() not in KNOWN_EXE_NAMES:
            continue
        if is_32bit_pe(candidate["path"]):
            return candidate
        eprint("rejected known-name candidate (not a 32-bit PE): %s" %
               candidate["path"])
    return None

# Folder names that look like the game (used for the loose filesystem scan).
DIR_MATCH = re.compile(r"crazy[\s_.\-]*taxi", re.IGNORECASE)
STEAM_INSTALLDIR_MATCH = re.compile(r'"installdir"\s*"([^"]+)"', re.IGNORECASE)
VDF_PATH_MATCH = re.compile(r'"path"\s*"([^"]+)"')

MAX_WALK_DEPTH = 5
SKIP_DIRS = {
    "windows", "winsxs", "$recycle.bin", "system volume information",
    "appdata", "node_modules", ".git", "steamapps", "common",
}


def eprint(msg):
    """Human-readable progress goes to stderr; stdout stays JSON-only."""
    sys.stderr.write(str(msg) + "\n")


def load_config():
    """Load config/environment.json, filling in template defaults for
    missing keys. Raises SystemExit(2) on unparsable JSON."""
    if not CONFIG_PATH.exists():
        return dict(CONFIG_TEMPLATE)
    try:
        with open(CONFIG_PATH, "r", encoding="utf-8") as fh:
            cfg = json.load(fh)
    except (OSError, ValueError) as exc:
        eprint("ERROR: cannot read/parse %s: %s" % (CONFIG_PATH, exc))
        raise SystemExit(2)
    if not isinstance(cfg, dict):
        eprint("ERROR: %s must contain a JSON object" % CONFIG_PATH)
        raise SystemExit(2)
    for key, value in CONFIG_TEMPLATE.items():
        cfg.setdefault(key, value)
    return cfg


def save_config(cfg):
    """Persist config back to disk (preserves unknown/extra keys)."""
    CONFIG_PATH.parent.mkdir(parents=True, exist_ok=True)
    with open(CONFIG_PATH, "w", encoding="utf-8") as fh:
        json.dump(cfg, fh, indent=2, ensure_ascii=False)
        fh.write("\n")


def confirmed_exe_in_dir(directory):
    """Return the path of a known Crazy Taxi executable inside directory,
    or None. Case-insensitive name comparison (Windows-style)."""
    try:
        for entry in os.listdir(directory):
            if entry.lower() in KNOWN_EXE_NAMES:
                candidate = os.path.join(directory, entry)
                if os.path.isfile(candidate):
                    return candidate
    except OSError:
        pass
    return None


def loose_exes_in_dir(directory, limit=5):
    """Any .exe inside a Crazy-Taxi-like folder whose own name mentions
    crazy/taxi - reported as a candidate, never auto-persisted."""
    found = []
    try:
        for entry in os.listdir(directory):
            low = entry.lower()
            if low.endswith(".exe") and "crazy" in low and "taxi" in low:
                full = os.path.join(directory, entry)
                if os.path.isfile(full):
                    found.append(full)
                    if len(found) >= limit:
                        break
    except OSError:
        pass
    return found


def steam_library_roots():
    """Steam library roots from the registry (HKCU\\Software\\Valve\\Steam
    "SteamPath"), the default install dir and libraryfolders.vdf files."""
    roots = []

    def add_root(path):
        if path:
            path = os.path.normpath(path.replace("\\\\", "\\").strip())
            if os.path.isdir(path) and path not in roots:
                roots.append(path)

    # Steam writes its main path to HKCU\Software\Valve\Steam\SteamPath.
    if winreg is not None:
        for hive, view in ((winreg.HKEY_CURRENT_USER, 0),
                           (winreg.HKEY_CURRENT_USER, winreg.KEY_WOW64_32KEY)):
            try:
                with winreg.OpenKey(hive, r"Software\Valve\Steam", 0,
                                    winreg.KEY_READ | view) as key:
                    value, _ = winreg.QueryValueEx(key, "SteamPath")
                add_root(value)
            except OSError:
                continue

    add_root(os.path.join(os.environ.get("ProgramFiles(x86)", ""), "Steam"))
    add_root(os.path.join(os.environ.get("ProgramFiles", ""), "Steam"))

    # libraryfolders.vdf lists every additional library (other drives).
    for root in list(roots):
        for vdf_rel in (os.path.join("steamapps", "libraryfolders.vdf"),
                        os.path.join("config", "libraryfolders.vdf")):
            vdf = os.path.join(root, vdf_rel)
            if os.path.isfile(vdf):
                try:
                    with open(vdf, "r", encoding="utf-8", errors="replace") as fh:
                        content = fh.read()
                except OSError:
                    continue
                for match in VDF_PATH_MATCH.finditer(content):
                    add_root(match.group(1))
    return roots


def scan_steam(candidates):
    """Check every Steam library for AppID 71230 (manifest) and for
    Crazy-Taxi-like folders in steamapps/common. Adds confirmed hits to
    `candidates` as (source, exe_path)."""
    for root in steam_library_roots():
        common = os.path.join(root, "steamapps", "common")
        if not os.path.isdir(common):
            continue

        # Preferred: the appmanifest for AppID 71230 names the install dir.
        manifest = os.path.join(root, "steamapps",
                                "appmanifest_%s.acf" % STEAM_APP_ID)
        if os.path.isfile(manifest):
            try:
                with open(manifest, "r", encoding="utf-8", errors="replace") as fh:
                    content = fh.read()
                match = STEAM_INSTALLDIR_MATCH.search(content)
                if match:
                    game_dir = os.path.join(common, match.group(1))
                    exe = confirmed_exe_in_dir(game_dir)
                    if exe:
                        candidates.append(("steam", exe))
            except OSError:
                pass

        # Fallback: any folder that looks like Crazy Taxi in any library.
        try:
            for entry in os.listdir(common):
                if DIR_MATCH.search(entry):
                    game_dir = os.path.join(common, entry)
                    if not os.path.isdir(game_dir):
                        continue
                    exe = confirmed_exe_in_dir(game_dir)
                    if exe:
                        candidates.append(("steam", exe))
        except OSError:
            pass


def scan_registry(candidates):
    """Search the Windows Uninstall keys for a Crazy Taxi entry and derive
    the install directory from InstallLocation / DisplayIcon."""
    if winreg is None:
        eprint("registry scan skipped (non-Windows host)")
        return
    key_paths = [
        (winreg.HKEY_LOCAL_MACHINE,
         r"Software\Microsoft\Windows\CurrentVersion\Uninstall"),
        (winreg.HKEY_LOCAL_MACHINE,
         r"Software\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall"),
        (winreg.HKEY_CURRENT_USER,
         r"Software\Microsoft\Windows\CurrentVersion\Uninstall"),
    ]
    for hive, key_path in key_paths:
        try:
            root = winreg.OpenKey(hive, key_path, 0, winreg.KEY_READ)
        except OSError:
            continue
        with root:
            index = 0
            while True:
                try:
                    subkey_name = winreg.EnumKey(root, index)
                except OSError:
                    break
                index += 1
                try:
                    with winreg.OpenKey(root, subkey_name, 0,
                                        winreg.KEY_READ) as subkey:
                        values = {}
                        for value_name in ("DisplayName", "InstallLocation",
                                           "DisplayIcon"):
                            try:
                                values[value_name] = str(
                                    winreg.QueryValueEx(subkey, value_name)[0])
                            except OSError:
                                pass
                except OSError:
                    continue
                display = values.get("DisplayName", "")
                if "crazy taxi" not in display.lower():
                    continue
                # InstallLocation is the most direct hint.
                location = values.get("InstallLocation", "").strip()
                if location and os.path.isdir(location):
                    exe = confirmed_exe_in_dir(location)
                    if exe:
                        candidates.append(("registry", exe))
                        continue
                # DisplayIcon often points at the exe (possibly "path,0").
                icon = values.get("DisplayIcon", "")
                if icon.lower().endswith(".exe") and os.path.isfile(icon):
                    candidates.append(("registry", icon))
                elif ", " in icon or "," in icon:
                    icon_exe = icon.split(",")[0].strip()
                    if icon_exe.lower().endswith(".exe") and \
                            os.path.isfile(icon_exe):
                        candidates.append(("registry", icon_exe))


def scan_filesystem(candidates):
    r"""Bounded scan of typical install locations for Crazy-Taxi-like folders.
    Deliberately NOT a full-disk scan: depth-limited walk over Program Files,
    ProgramData and <drive>:\Games-style roots only."""
    roots = []
    for var in ("ProgramFiles", "ProgramFiles(x86)", "ProgramData"):
        value = os.environ.get(var)
        if value and os.path.isdir(value):
            roots.append(value)
    for letter in "CDEFGH":
        drive = "%s:\\" % letter
        if os.path.isdir(drive):
            for sub in ("Games", "GOG Games"):
                path = os.path.join(drive, sub)
                if os.path.isdir(path):
                    roots.append(path)

    seen_dirs = set()
    for start in roots:
        start_depth = start.rstrip("\\/").count(os.sep)
        for current, dirs, _files in os.walk(start):
            depth = current.rstrip("\\/").count(os.sep) - start_depth
            dirs[:] = [d for d in dirs
                       if d.lower() not in SKIP_DIRS and not d.startswith(".")]
            if depth >= MAX_WALK_DEPTH:
                dirs[:] = []
            if current.lower() in seen_dirs:
                continue
            seen_dirs.add(current.lower())
            if DIR_MATCH.search(os.path.basename(current)) and \
                    os.path.isdir(current):
                exe = confirmed_exe_in_dir(current)
                if exe:
                    candidates.append(("filesystem", exe))


def dedupe_candidates(candidates):
    """Keep first occurrence per (source, normalized path)."""
    seen = set()
    result = []
    for source, path in candidates:
        key = (source, os.path.normcase(os.path.normpath(path)))
        if key not in seen:
            seen.add(key)
            result.append({"source": source, "path": path})
    return result


def search_all():
    """Run the full search chain; returns the deduplicated candidate list."""
    candidates = []
    eprint("[1/3] scanning Steam libraries (AppID %s + name match)..." %
           STEAM_APP_ID)
    scan_steam(candidates)
    eprint("[2/3] scanning registry Uninstall keys...")
    scan_registry(candidates)
    eprint("[3/3] scanning typical install folders...")
    scan_filesystem(candidates)
    return dedupe_candidates(candidates)


def main():
    parser = argparse.ArgumentParser(
        description="Find the Crazy Taxi 1 (PC) executable (R4) and persist "
                    "the path to config/environment.json.")
    parser.add_argument("--reindex", action="store_true",
                        help="ignore the stored path and rescan everything")
    args = parser.parse_args()

    cfg = load_config()
    stored = cfg.get("crazyTaxiExe")

    # 1) Trust the stored path unless --reindex was given.
    if stored and not args.reindex:
        if os.path.isfile(stored):
            result = {
                "found": True,
                "crazyTaxiExe": stored,
                "gameDir": os.path.dirname(stored),
                "source": "config",
                "candidates": [],
                "configUpdated": False,
                "message": "path from config/environment.json is valid",
            }
            print(json.dumps(result))
            return 0
        eprint("WARNING: stored path no longer exists: %s" % stored)
        eprint("rescanning automatically...")
        candidates = search_all()
        found = pick_confirmed(candidates)
        if found:
            cfg["crazyTaxiExe"] = found["path"]
            cfg["gameDir"] = os.path.dirname(found["path"])
            save_config(cfg)
            print(json.dumps({
                "found": True,
                "crazyTaxiExe": found["path"],
                "gameDir": os.path.dirname(found["path"]),
                "source": found["source"],
                "candidates": candidates,
                "configUpdated": True,
                "message": "stored path was stale; updated from rescan",
            }))
            return 0
        print(json.dumps({
            "found": False,
            "crazyTaxiExe": None,
            "gameDir": None,
            "source": None,
            "candidates": candidates,
            "configUpdated": False,
            "message": "stored path is stale (%s) and rescan found nothing; "
                       "ask the user for the path and store it in "
                       "config/environment.json, or run with --reindex" % stored,
        }))
        return 1

    # 2) Full search (also when --reindex or when nothing is stored).
    candidates = search_all()
    pick = pick_confirmed(candidates)
    if pick:
        cfg["crazyTaxiExe"] = pick["path"]
        cfg["gameDir"] = os.path.dirname(pick["path"])
        save_config(cfg)
        print(json.dumps({
            "found": True,
            "crazyTaxiExe": pick["path"],
            "gameDir": os.path.dirname(pick["path"]),
            "source": pick["source"],
            "candidates": candidates,
            "configUpdated": True,
            "message": "found and persisted to config/environment.json",
        }))
        return 0

    # 3) Nothing confirmed - be honest, list loose candidates if any.
    print(json.dumps({
        "found": False,
        "crazyTaxiExe": None,
        "gameDir": None,
        "source": None,
        "candidates": candidates,
        "configUpdated": False,
        "message": "Crazy Taxi.exe not found. Ask the user for the install "
                   "path; store it in config/environment.json as "
                   "\"crazyTaxiExe\" (full exe path) and \"gameDir\" "
                   "(folder), then re-run to validate.",
    }))
    return 1


if __name__ == "__main__":
    sys.exit(main())
