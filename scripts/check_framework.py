#!/usr/bin/env python3
"""check_framework.py - verify (and optionally install) Lyra's Framework Mod.

CLI (exact):

    python scripts/check_framework.py [--fix] [--json]

Purpose:
    Sub-mods for Crazy Taxi 1 are hosted by Lyra's Framework Mod
    (``<gameDir>/mods/Framework_MOD/Framework_MOD.dll``). Before any
    sub-mod development starts, that host DLL must exist and be a valid
    32-bit PE - otherwise the sub-mod is loaded by the proxy loader but
    never integrated (silent no-op). This script automates the check and
    offers ``--fix`` to install the official reference build.

    ``gameDir`` is read from ``config/environment.json`` (persisted by
    ``find_game.py``). All paths are resolved relative to this script's
    location and to the config file; nothing is hard-coded.

Exit codes:
    0   framework DLL present and valid (or --fix installed it
        successfully; --fix on an already-valid install is a no-op)
    1   framework DLL missing
    2   framework DLL present but invalid (structure / not x86 / missing
        exports) or present but unreadable (locked by the running game or
        an antivirus - the reasons list names the cause)
    3   no usable gameDir in config/environment.json - run
        ``python scripts/find_game.py`` first
    4   --fix failed (download / validation / install). NOT silent: the
        script then prints the detected C++ toolchain and the exact
        32-bit build command for the complete source in
        ``Examples/Standalone MOD`` and points at
        references/framework-mod.md. The script never compiles anything
        itself.

Output:
    Progress goes to stderr. stdout carries a human summary by default
    and a single-line JSON object with --json (keys documented in
    references/environment-setup.md, section "Framework mod check").

Validation rules (probe-verified against a REAL download on 2026-11-14):
    Official archive "lyras_framwork_mod_48600.zip":
        size   180186115 bytes (byte-exact)
        sha256 ba7c96a6440e002dc612cd6c3f0a558ab389b17dd3c1c9c0c675f7594123cb91
        magic  b"PK\\x03\\x04" (it is a ZIP, NOT a PE - the MZ/PE checks
               apply to the extracted DLL, never to the archive)
        required entries: mods/Framework_MOD/Framework_MOD.dll,
                          dinput8.dll, ReadMe_Installation.txt
    Framework DLL (extracted / installed):
        size 2864128, sha256 7a5d93bb35eb052f1b131a311ba5296a022180993d8ab56a4508924855bb923e,
        MZ -> PE\\x00\\x00, Machine 0x014c (i386; 0x8664 = x64 = invalid for
        this 32-bit game), IMAGE_FILE_DLL flag, OptionalHeader magic 0x10b
        (PE32), non-empty export table, and the 8 public exports:
        GetFrameworkD3D9Device, GetFrameworkImGuiContext, GetModConfig,
        ModAPI_IsVulkanEnabled, ModAPI_SetVulkanEnabled, ModAPI_ToggleVulkan,
        ProcessKeyboardBuffer, SetFrameworkStatusMessage
    Loader dinput8.dll: 153600 bytes, sha256
        f4ff139fdaf5a5399fbc9c6ec2051034594f1e92b4154bc598719ac47b3ee9d9,
        Machine 0x014c, export DirectInput8Create.
    Download URL facts (probe evidence):
        - WORKING form (must be used):
          https://drive.usercontent.google.com/download?id=<ID>&export=download&confirm=t
          (uuid parameter optional)
        - the classic https://drive.google.com/uc?export=download&id=<ID>
          (with or without &confirm=t) always fails: Google's 303 redirect
          strips confirm=t and lands on the 2476-byte "Virus scan warning"
          HTML page; confirm=t is only honoured on the
          drive.usercontent.google.com host.

Design decisions (documented in references/environment-setup.md):
    * For an INSTALLED DLL the reference size/sha256 comparison is
      reported as ``referenceMatch`` info and never a hard fail: a DLL
      built locally from Examples/Standalone MOD is structurally valid
      but has a different hash. The downloaded ARCHIVE itself is
      validated byte-exact (size + sha256) before anything is installed.
    * On write, config/environment.json is upgraded to schemaVersion 2;
      every existing key (including unknown ones) is preserved.
    * The FULL package is installed: EVERY archive entry is extracted
      into the game folder root - the ZIP's own mods/... structure maps
      directly into the game dir, NO extra subfolder is created - with
      forced overwrite of existing files (user requirement). The two
      critical modules are validated in-memory BEFORE extraction and the
      installed framework DLL is re-verified afterwards.
"""

import argparse
import hashlib
import json
import os
import shutil
import struct
import sys
import tempfile
import urllib.error
import urllib.request
import zipfile

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
SKILL_ROOT = os.path.dirname(SCRIPT_DIR)
CONFIG_PATH = os.path.join(SKILL_ROOT, "config", "environment.json")

# ---------------------------------------------------------------------------
# Probe-verified constants (real download test, 2026-11-14; see docstring)
# ---------------------------------------------------------------------------

DOWNLOAD_FILE_ID = "1HQ-6peIb6fPL7cv95ldtKZpWcVD19-M6"
# Working form: confirm=t is honoured only on drive.usercontent.google.com.
DOWNLOAD_URL = ("https://drive.usercontent.google.com/download?id=%s"
                "&export=download&confirm=t" % DOWNLOAD_FILE_ID)
# Documented as NOT working (303 strips confirm=t -> HTML confirmation page):
LEGACY_URL = ("https://drive.google.com/uc?export=download&id="
              + DOWNLOAD_FILE_ID)

EXPECTED_ZIP_SIZE = 180186115
EXPECTED_ZIP_SHA256 = ("ba7c96a6440e002dc612cd6c3f0a558ab389b17dd3c1c9c0c675"
                       "f7594123cb91")
ZIP_MAGIC = b"PK\x03\x04"
ZIP_FILENAME = "lyras_framwork_mod_48600.zip"
REQUIRED_ZIP_ENTRIES = [
    "mods/Framework_MOD/Framework_MOD.dll",
    "dinput8.dll",
    "ReadMe_Installation.txt",
]

EXPECTED_DLL_SIZE = 2864128
EXPECTED_DLL_SHA256 = ("7a5d93bb35eb052f1b131a311ba5296a022180993d8ab56a45089"
                       "24855bb923e")
EXPECTED_LOADER_SIZE = 153600
EXPECTED_LOADER_SHA256 = ("f4ff139fdaf5a5399fbc9c6ec2051034594f1e92b4154bc59"
                          "8719ac47b3ee9d9")

MACHINE_I386 = 0x014C          # only valid value for this 32-bit game
MACHINE_X64 = 0x8664           # explicitly invalid
IMAGE_FILE_DLL = 0x2000
OPT_MAGIC_PE32 = 0x10B

REQUIRED_FRAMEWORK_EXPORTS = [
    "GetFrameworkD3D9Device",
    "GetFrameworkImGuiContext",
    "GetModConfig",
    "ModAPI_IsVulkanEnabled",
    "ModAPI_SetVulkanEnabled",
    "ModAPI_ToggleVulkan",
    "ProcessKeyboardBuffer",
    "SetFrameworkStatusMessage",
]
REQUIRED_LOADER_EXPORTS = ["DirectInput8Create"]

# Sub-mod host layout (standard: <gameDir>/mods/<modname>/<modname>.dll)
MODS_DIR_NAME = "mods"
FRAMEWORK_DIR_NAME = "Framework_MOD"
FRAMEWORK_DLL_NAME = "Framework_MOD.dll"
LOADER_DLL_NAME = "dinput8.dll"

# Source fallback (complete C++ source of Lyra's Framework Mod v1.2;
# MOD_VERSION "1.2" in config.h line 5). The folder has NO build files and
# NO deps - see fallback_build_block() and references/framework-mod.md.
SOURCE_DIR = os.path.join(SKILL_ROOT, "Examples", "Standalone MOD")
FRAMEWORK_DOC = "references/framework-mod.md"

# C++ toolchain install command for the no-toolchain fallback (winget ID
# verified against the winget catalog, 2026-11-14; the VCTools workload ID is
# Microsoft-documented). Printed by print_fallback() and documented in
# references/environment-setup.md (R5).
VS_BUILD_TOOLS_CMD = ("winget install -e --id Microsoft.VisualStudio.2022"
                      ".BuildTools --override \"--quiet --wait --add "
                      "Microsoft.VisualStudio.Workload.VCTools "
                      "--includeRecommended\"")

# Dependencies implied by the source's own #include lines (verified):
#   config.cpp:5                ../deps/nlohmann/json.hpp
#   input_hook.cpp:2            ../deps/imgui/imgui.h
#   render_hook.cpp:10-14       ../deps/imgui/imgui.h, imgui_internal.h
#                               (Dear ImGui 1.90.9 pinned by comment),
#                               backends/imgui_impl_dx9.h,
#                               backends/imgui_impl_win32.h, <MinHook.h>
#   main.cpp:3, game_features.cpp:19  <MinHook.h>
# Expected deps layout: a deps/ folder NEXT TO the (copied) source folder -
# the include lines use ../deps/. NOT present in the workspace; fetch first
# per references/framework-mod.md section 5.2 (exact versions/URLs beyond
# "Dear ImGui 1.90.9" are unknown there - obtain them from the user, never
# guess download URLs).
DEPENDS_NOT_IN_WORKSPACE = [
    "deps/imgui - Dear ImGui 1.90.9 (pinned by render_hook.cpp:11): "
    "imgui.h, imgui_internal.h, imgui.cpp, imgui_draw.cpp, imgui_tables.cpp, "
    "imgui_widgets.cpp",
    "deps/imgui/backends - imgui_impl_dx9.{h,cpp}, imgui_impl_win32.{h,cpp}",
    "deps/nlohmann - json.hpp",
    "deps/MinHook - include/MinHook.h + sources (buffer.c, hook.c, "
    "trampoline.c, HDE/hde.c) or a prebuilt 32-bit MinHook.lib",
]


class ConfigError(Exception):
    """config/environment.json is unreadable or not a JSON object."""


# ---------------------------------------------------------------------------
# Small helpers
# ---------------------------------------------------------------------------

def eprint(msg):
    sys.stderr.write(str(msg) + "\n")
    sys.stderr.flush()


# ---------------------------------------------------------------------------
# Config handling (schema v2; preserves every existing key, like the other
# skill scripts do via setdefault merging)
# ---------------------------------------------------------------------------

CONFIG_TEMPLATE_V2 = {
    "schemaVersion": 2,
    "crazyTaxiExe": None,
    "gameDir": None,
    "frameworkDll": None,
    "frameworkStatus": None,
    "toolboxDir": "Toolbox",
    "toolchain": None,
    "preferredCompiler": None,
    "mcpServers": {},
}


def load_config():
    if not os.path.isfile(CONFIG_PATH):
        return json.loads(json.dumps(CONFIG_TEMPLATE_V2))
    try:
        with open(CONFIG_PATH, "r", encoding="utf-8") as fh:
            cfg = json.load(fh)
    except (OSError, ValueError) as exc:
        raise ConfigError("cannot read/parse %s: %s" % (CONFIG_PATH, exc))
    if not isinstance(cfg, dict):
        raise ConfigError("%s must contain a JSON object" % CONFIG_PATH)
    for key, value in CONFIG_TEMPLATE_V2.items():
        cfg.setdefault(key, json.loads(json.dumps(value)))
    return cfg


def save_config(cfg):
    cfg["schemaVersion"] = 2
    cfg.setdefault("frameworkDll", None)
    cfg.setdefault("frameworkStatus", None)
    os.makedirs(os.path.dirname(CONFIG_PATH), exist_ok=True)
    with open(CONFIG_PATH, "w", encoding="utf-8") as fh:
        json.dump(cfg, fh, indent=2, ensure_ascii=False)
        fh.write("\n")


def usable_game_dir(cfg):
    """Return the gameDir only when it is a non-empty string that exists on
    disk; otherwise None (-> exit code 3)."""
    game_dir = cfg.get("gameDir")
    if not isinstance(game_dir, str) or not game_dir.strip():
        return None
    game_dir = os.path.normpath(game_dir.strip())
    if not os.path.isdir(game_dir):
        return None
    return game_dir


# ---------------------------------------------------------------------------
# PE inspection (stdlib struct only; every access bounds-checked so a
# truncated or corrupt file yields "invalid" instead of a traceback)
# ---------------------------------------------------------------------------

def _u16(data, off):
    if off < 0 or off + 2 > len(data):
        raise ValueError("truncated read (u16 @ 0x%x, len 0x%x)" % (off, len(data)))
    return struct.unpack_from("<H", data, off)[0]


def _u32(data, off):
    if off < 0 or off + 4 > len(data):
        raise ValueError("truncated read (u32 @ 0x%x, len 0x%x)" % (off, len(data)))
    return struct.unpack_from("<I", data, off)[0]


def _read_cstr(data, off, max_len=256):
    if off < 0 or off >= len(data):
        raise ValueError("string offset out of range (0x%x)" % off)
    end = min(len(data), off + max_len)
    nul = data.find(b"\x00", off, end)
    if nul < 0:
        raise ValueError("unterminated string at 0x%x" % off)
    return data[off:nul].decode("latin-1")


def pe_inspect(data):
    """Parse a PE image header. Returns a dict; raises ValueError on any
    structural inconsistency (caller decides how to report it)."""
    info = {
        "mz": data[:2] == b"MZ",
        "peSignature": False,
        "machine": None,
        "machineHex": None,
        "numberOfSections": None,
        "characteristics": None,
        "isDll": False,
        "optMagic": None,
        "isPE32": False,
        "exportTableRva": None,
        "exportTableSize": None,
        "exportTablePresent": False,
        "subsystem": None,
    }
    if not info["mz"]:
        return info
    e_lfanew = _u32(data, 0x3C)
    if e_lfanew <= 0 or e_lfanew + 24 > len(data):
        raise ValueError("e_lfanew 0x%x out of range" % e_lfanew)
    if data[e_lfanew:e_lfanew + 4] != b"PE\x00\x00":
        raise ValueError("no PE\\x00\\x00 signature at e_lfanew 0x%x" % e_lfanew)
    info["peSignature"] = True
    info["machine"] = _u16(data, e_lfanew + 4)
    info["machineHex"] = "0x%04x" % info["machine"]
    info["numberOfSections"] = _u16(data, e_lfanew + 6)
    size_opt = _u16(data, e_lfanew + 20)
    info["characteristics"] = _u16(data, e_lfanew + 22)
    info["isDll"] = bool(info["characteristics"] & IMAGE_FILE_DLL)
    if size_opt < 2:
        raise ValueError("SizeOfOptionalHeader %d too small" % size_opt)
    opt = e_lfanew + 24
    if opt + size_opt > len(data):
        raise ValueError("optional header truncated")
    info["optMagic"] = _u16(data, opt)
    info["isPE32"] = info["optMagic"] == OPT_MAGIC_PE32
    if info["isPE32"] and size_opt >= 96 + 8:
        # PE32: DataDirectory starts at optional header offset 96, i.e.
        # absolute e_lfanew + 120 (export directory = directory[0]).
        info["exportTableRva"] = _u32(data, opt + 96)
        info["exportTableSize"] = _u32(data, opt + 100)
        info["exportTablePresent"] = (info["exportTableRva"] != 0
                                      and info["exportTableSize"] != 0)
        info["subsystem"] = _u16(data, opt + 68)
    return info


def _section_table(data, pe):
    e_lfanew = _u32(data, 0x3C)
    size_opt = _u16(data, e_lfanew + 20)
    num_sections = _u16(data, e_lfanew + 6)
    first = e_lfanew + 24 + size_opt
    sections = []
    for i in range(num_sections):
        base = first + i * 40
        if base + 40 > len(data):
            raise ValueError("section table truncated")
        virtual_size = _u32(data, base + 8)
        virtual_addr = _u32(data, base + 12)
        raw_size = _u32(data, base + 16)
        raw_ptr = _u32(data, base + 20)
        sections.append((virtual_addr, max(virtual_size, raw_size),
                         raw_ptr, raw_size))
    return sections


def _rva_to_offset(rva, sections):
    for virtual_addr, mapped_size, raw_ptr, raw_size in sections:
        if virtual_addr <= rva < virtual_addr + mapped_size:
            delta = rva - virtual_addr
            if delta < raw_size:
                return raw_ptr + delta
    return None


def parse_export_names(data, pe):
    """Parse the export directory's name table. Returns (names, None) on
    success or (None, reason) on any inconsistency (fail-soft)."""
    try:
        if not pe.get("exportTablePresent") or not pe.get("isPE32"):
            return None, "no PE32 export table"
        sections = _section_table(data, pe)
        dir_off = _rva_to_offset(pe["exportTableRva"], sections)
        if dir_off is None:
            return None, "export directory RVA 0x%x not mapped to file" % \
                pe["exportTableRva"]
        num_names = _u32(data, dir_off + 24)
        addr_names = _u32(data, dir_off + 32)
        if num_names > 100000:
            return None, "implausible NumberOfNames %d" % num_names
        names_off = _rva_to_offset(addr_names, sections)
        if names_off is None:
            return None, "AddressOfNames RVA 0x%x not mapped" % addr_names
        names = []
        for i in range(num_names):
            name_rva = _u32(data, names_off + i * 4)
            name_off = _rva_to_offset(name_rva, sections)
            if name_off is None:
                return None, "export name RVA 0x%x not mapped" % name_rva
            names.append(_read_cstr(data, name_off))
        return names, None
    except ValueError as exc:
        return None, str(exc)


def validate_module(data, required_exports):
    """Full validity check for one module image. Returns
    (valid, report_dict)."""
    report = {"valid": False, "reasons": [], "pe": None, "exports": None}
    pe = None
    try:
        pe = pe_inspect(data)
    except ValueError as exc:
        report["reasons"].append("PE structure: %s" % exc)
        return False, report
    report["pe"] = pe
    if not pe["mz"]:
        report["reasons"].append("no MZ signature (not a PE image)")
    if not pe["peSignature"]:
        report["reasons"].append("no PE\\x00\\x00 signature")
    if pe["machine"] is not None and pe["machine"] != MACHINE_I386:
        if pe["machine"] == MACHINE_X64:
            report["reasons"].append(
                "x64 image (Machine 0x8664) - Crazy Taxi 1 is a 32-bit "
                "process; this DLL can never load here")
        else:
            report["reasons"].append(
                "unexpected Machine %s (expected 0x014c i386)" %
                pe["machineHex"])
    if pe["machine"] == MACHINE_I386 and not pe["isDll"]:
        report["reasons"].append("IMAGE_FILE_DLL flag missing")
    if pe["machine"] == MACHINE_I386 and not pe["isPE32"]:
        report["reasons"].append(
            "OptionalHeader magic %s (expected 0x10b PE32)" %
            ("0x%04x" % pe["optMagic"] if pe["optMagic"] is not None else "?"))
    if pe["machine"] == MACHINE_I386 and not pe["exportTablePresent"]:
        report["reasons"].append("export table missing or empty")
    if pe["mz"] and pe["peSignature"] and pe["machine"] == MACHINE_I386:
        names, err = parse_export_names(data, pe)
        if err is not None:
            report["reasons"].append("export table: %s" % err)
            report["exports"] = {"required": required_exports,
                                 "found": [], "missing": required_exports,
                                 "ok": False, "error": err}
        else:
            missing = [n for n in required_exports if n not in names]
            report["exports"] = {"required": required_exports,
                                 "found": names, "missing": missing,
                                 "ok": not missing}
            if missing:
                report["reasons"].append(
                    "missing required exports: %s" % ", ".join(missing))
    report["valid"] = not report["reasons"]
    return report["valid"], report


def compact_pe(pe):
    if pe is None:
        return None
    return {
        "machine": pe["machineHex"],
        "machineOk": pe["machine"] == MACHINE_I386,
        "dllFlag": pe["isDll"],
        "pe32": pe["isPE32"],
        "numberOfSections": pe["numberOfSections"],
        "exportTableRva": pe["exportTableRva"],
        "exportTableSize": pe["exportTableSize"],
        "exportTablePresent": pe["exportTablePresent"],
    }


# ---------------------------------------------------------------------------
# Framework / loader checks on disk
# ---------------------------------------------------------------------------

def framework_dll_path(game_dir):
    return os.path.join(game_dir, MODS_DIR_NAME, FRAMEWORK_DIR_NAME,
                        FRAMEWORK_DLL_NAME)


def loader_dll_path(game_dir):
    return os.path.join(game_dir, LOADER_DLL_NAME)


def check_installed_file(path, required_exports, expected_size,
                         expected_sha256):
    """Check one installed module. Returns (exists, valid, report).

    A file that exists but cannot be read (locked by the running game or
    an antivirus) is reported as INVALID with an explicit "unreadable"
    reason instead of raising - the exit-code contract (0-4) must hold
    even while Crazy Taxi is running."""
    if not os.path.isfile(path):
        return False, False, {"exists": False}
    try:
        with open(path, "rb") as fh:
            data = fh.read()
    except OSError as exc:
        return True, False, {
            "exists": True, "size": None, "sha256": None,
            "pe": None, "exports": None, "referenceMatch": None,
            "reasons": ["unreadable (locked by the running game or an "
                        "antivirus?): %s" % exc]}
    valid, report = validate_module(data, required_exports)
    report["exists"] = True
    report["size"] = len(data)
    report["sha256"] = hashlib.sha256(data).hexdigest()
    report["referenceMatch"] = {
        "size": report["size"] == expected_size,
        "sha256": report["sha256"] == expected_sha256,
    }
    return True, valid, report


def check_loader(game_dir):
    """Informational loader check (never affects the exit code): the proxy
    loader lives in the game root and is what actually loads mods/."""
    path = loader_dll_path(game_dir)
    if not os.path.isfile(path):
        return {"path": path, "exists": False, "note":
                "proxy loader missing - nothing under mods/ will ever load; "
                "--fix extracts the full archive with forced overwrite, restoring it"}
    try:
        with open(path, "rb") as fh:
            data = fh.read()
    except OSError as exc:
        return {"path": path, "exists": True, "note": "unreadable: %s" % exc}
    valid, report = validate_module(data, REQUIRED_LOADER_EXPORTS)
    entry = {"path": path, "exists": True, "valid": valid,
             "machine": report["pe"]["machineHex"] if report["pe"] else None,
             "machineOk": bool(report["pe"] and
                               report["pe"]["machine"] == MACHINE_I386),
             "size": len(data)}
    if not valid:
        entry["reasons"] = report["reasons"]
    return entry


# ---------------------------------------------------------------------------
# --fix: download from Google Drive, validate, install
# ---------------------------------------------------------------------------

def looks_like_html(head):
    lowered = head[:512].lower()
    return (b"<!doctype html" in lowered or b"<html" in lowered
            or b"virus scan warning" in lowered)


def download_archive(tmp_dir):
    """Stream the official archive into tmp_dir. Returns (path, evidence).
    Raises FixError with a precise message on any deviation."""
    eprint("[fix] downloading official framework archive...")
    eprint("[fix] url: %s" % DOWNLOAD_URL)
    req = urllib.request.Request(DOWNLOAD_URL, headers={
        # The probe verified this endpoint with a curl-style client; keep
        # the same request shape. The HTML guard below catches any deviation.
        "User-Agent": "curl/8.9.1",
        "Accept": "*/*",
    })
    out_path = os.path.join(tmp_dir, ZIP_FILENAME)
    try:
        resp = urllib.request.urlopen(req, timeout=120)
    except urllib.error.HTTPError as exc:
        raise FixError("HTTP %d from %s (%s)" %
                       (exc.code, DOWNLOAD_URL, exc.reason))
    except (urllib.error.URLError, OSError) as exc:
        raise FixError("network error downloading %s: %s" %
                       (DOWNLOAD_URL, exc))
    with resp:
        ctype = resp.headers.get("Content-Type", "")
        clen = resp.headers.get("Content-Length")
        if clen is not None:
            try:
                if int(clen) != EXPECTED_ZIP_SIZE:
                    raise FixError(
                        "Content-Length %s does not match the verified "
                        "archive size %d (got a different or renamed file?)"
                        % (clen, EXPECTED_ZIP_SIZE))
            except ValueError:
                pass
        if "text/html" in ctype.lower():
            raise FixError(
                "server returned Content-Type %r - this is the Google Drive "
                "virus-scan confirmation HTML page, not the archive. This "
                "script already uses the probe-verified final URL "
                "(drive.usercontent.google.com + confirm=t; uuid optional). "
                "If this persists, download %r manually and install "
                "mods/Framework_MOD/Framework_MOD.dll yourself, or follow %s."
                % (ctype, ZIP_FILENAME, FRAMEWORK_DOC))
        received = 0
        digest = hashlib.sha256()
        with open(out_path, "wb") as fh:
            while True:
                chunk = resp.read(1024 * 1024)
                if not chunk:
                    break
                received += len(chunk)
                if received > EXPECTED_ZIP_SIZE:
                    raise FixError(
                        "download larger than the verified archive size %d "
                        "(%d bytes received) - rejecting" %
                        (EXPECTED_ZIP_SIZE, received))
                fh.write(chunk)
                digest.update(chunk)
                if received % (16 * 1024 * 1024) < 1024 * 1024:
                    eprint("[fix] %d / %d bytes..." %
                           (received, EXPECTED_ZIP_SIZE))
    evidence = {"url": DOWNLOAD_URL, "contentType": ctype,
                "contentLengthHeader": clen, "receivedBytes": received,
                "sha256": digest.hexdigest(), "savedTo": out_path}
    with open(out_path, "rb") as fh:
        head = fh.read(1024)
    if looks_like_html(head):
        raise FixError(
            "downloaded content is the Google Drive virus-scan HTML "
            "confirmation page (not the archive). Expected %s magic. See "
            "URL notes in references/environment-setup.md - only "
            "drive.usercontent.google.com honours confirm=t." %
            ZIP_MAGIC.decode("latin-1"))
    if received != EXPECTED_ZIP_SIZE:
        raise FixError("downloaded %d bytes, expected exactly %d "
                       "(aborted transfer or changed file)" %
                       (received, EXPECTED_ZIP_SIZE))
    if digest.hexdigest() != EXPECTED_ZIP_SHA256:
        raise FixError("sha256 mismatch: got %s, expected %s - corrupt or "
                       "manipulated content; NEVER install it" %
                       (digest.hexdigest(), EXPECTED_ZIP_SHA256))
    if head[:4] != ZIP_MAGIC:
        raise FixError("archive magic mismatch: got %r, expected %r" %
                       (head[:4], ZIP_MAGIC))
    eprint("[fix] archive OK: %d bytes, sha256 verified" % received)
    return out_path, evidence


def _zip_entry(namelist, wanted):
    """Exact match first, then case-insensitive fallback. Returns the real
    entry name or None."""
    if wanted in namelist:
        return wanted
    lowered = {n.lower(): n for n in namelist}
    return lowered.get(wanted.lower())


def validate_archive(zip_path):
    """Open the archive read-only and validate its required entries plus the
    PE structure of the two modules. Nothing is executed. Returns a dict of
    validated in-memory payloads."""
    try:
        zf = zipfile.ZipFile(zip_path, "r")
    except zipfile.BadZipFile as exc:
        raise FixError("not a readable ZIP container: %s" % exc)
    with zf:
        namelist = zf.namelist()
        resolved = {}
        for wanted in REQUIRED_ZIP_ENTRIES:
            actual = _zip_entry(namelist, wanted)
            if actual is None:
                raise FixError("archive entry %r missing (required); entries "
                               "present: %d" % (wanted, len(namelist)))
            resolved[wanted] = actual
        payloads = {}
        for wanted, actual in resolved.items():
            try:
                payloads[wanted] = zf.read(actual)
            except (zipfile.BadZipFile, OSError) as exc:
                raise FixError("cannot read archive entry %r: %s" %
                               (actual, exc))
        dll = payloads["mods/Framework_MOD/Framework_MOD.dll"]
        valid, report = validate_module(dll, REQUIRED_FRAMEWORK_EXPORTS)
        if not valid:
            raise FixError("framework DLL inside the archive is INVALID: %s" %
                           "; ".join(report["reasons"]))
        if len(dll) != EXPECTED_DLL_SIZE:
            eprint("[fix] note: framework DLL size %d differs from the "
                   "reference %d (newer official build?)" %
                   (len(dll), EXPECTED_DLL_SIZE))
        dll_sha = hashlib.sha256(dll).hexdigest()
        if dll_sha != EXPECTED_DLL_SHA256:
            eprint("[fix] note: framework DLL sha256 %s differs from the "
                   "reference %s (ZIP hash itself was verified, so this is "
                   "an updated official build)" % (dll_sha,
                                                   EXPECTED_DLL_SHA256))
        loader = payloads["dinput8.dll"]
        l_valid, l_report = validate_module(loader, REQUIRED_LOADER_EXPORTS)
        if not l_valid:
            raise FixError("proxy loader dinput8.dll inside the archive is "
                           "INVALID: %s" % "; ".join(l_report["reasons"]))
        eprint("[fix] archive validated: framework DLL %d bytes x86 PE with "
               "all %d exports, loader %d bytes x86 PE"
               % (len(dll), len(REQUIRED_FRAMEWORK_EXPORTS), len(loader)))
        return {"entries": resolved, "numEntries": len(namelist),
                "frameworkDll": dll, "frameworkDllSha256": dll_sha,
                "loaderDll": loader,
                "loaderDllSha256": hashlib.sha256(loader).hexdigest()}


class FixError(Exception):
    """--fix cannot continue; always reported, never silent."""


def _install_file(tmp_target, target):
    """Replace target with tmp_target. A locked target (running game /
    antivirus) is a FixError, not a traceback - keeps the exit-code
    contract (4 = --fix failed) and leaves no .new file behind."""
    try:
        os.replace(tmp_target, target)
    except OSError as exc:
        try:
            os.remove(tmp_target)
        except OSError:
            pass
        raise FixError("cannot install %s (locked by the running game or an "
                       "antivirus? - close the game first): %s" %
                       (target, exc))


def _safe_target(game_root, name):
    """Resolve an archive entry name inside the game root, rejecting any
    entry that would escape it (zip-slip guard)."""
    target = os.path.abspath(
        os.path.join(game_root, name.replace("/", os.sep)))
    if target != game_root and not target.startswith(game_root + os.sep):
        raise FixError("archive entry escapes the game directory: %r" % name)
    return target


def extract_archive(game_dir, archive_path):
    """Extract EVERY archive entry into the game folder root, preserving the
    ZIP's own folder structure (mods/Framework_MOD/..., dinput8.dll in the
    game root, dxvk_optional/, ReadMe_Installation.txt, ...). No extra
    subfolder is created and existing files are OVERWRITTEN by design
    (forced overwrite, user requirement); the two critical modules were
    validated in-memory before this runs. A locked target (running game /
    antivirus) is a FixError, never a traceback. Returns
    (installed_paths, overwritten_count)."""
    installed, overwritten = [], 0
    game_root = os.path.abspath(game_dir)
    os.makedirs(game_root, exist_ok=True)
    with zipfile.ZipFile(archive_path, "r") as zf:
        for info in zf.infolist():
            name = info.filename
            if name.endswith("/"):
                os.makedirs(_safe_target(game_root, name), exist_ok=True)
                continue
            target = _safe_target(game_root, name)
            parent = os.path.dirname(target)
            if parent:
                os.makedirs(parent, exist_ok=True)
            existed = os.path.isfile(target)
            # Write-then-replace keeps the game directory free of partially
            # written files; os.replace overwrites an existing target.
            tmp_target = target + ".new"
            with open(tmp_target, "wb") as fh:
                fh.write(zf.read(info))
            _install_file(tmp_target, target)
            installed.append(target)
            if existed:
                overwritten += 1
    return installed, overwritten


def install_archive(game_dir, archive_path):
    """Install the full package: extract every archive entry into the game
    folder root (forced overwrite), then re-verify the installed framework
    DLL (trust nothing). Returns (installed, overwritten)."""
    installed, overwritten = extract_archive(game_dir, archive_path)
    eprint("[fix] extracted %d files into the game dir (%d overwritten)"
           % (len(installed), overwritten))
    fw_dir = os.path.join(game_dir, MODS_DIR_NAME, FRAMEWORK_DIR_NAME)
    fw_target = os.path.join(fw_dir, FRAMEWORK_DLL_NAME)
    if not os.path.isfile(fw_target):
        raise FixError("archive did not install %s" % fw_target)
    # Read back and re-verify what was written (trust nothing).
    exists, valid, check = check_installed_file(
        fw_target, REQUIRED_FRAMEWORK_EXPORTS, EXPECTED_DLL_SIZE,
        EXPECTED_DLL_SHA256)
    if not valid:
        try:
            os.remove(fw_target)
        except OSError:
            pass
        raise FixError("installed framework DLL failed re-verification: %s" %
                       "; ".join(check.get("reasons", ["unknown"])))
    eprint("[fix] installed: %s" % fw_target)
    return installed, overwritten


# ---------------------------------------------------------------------------
# Fallback: source build instructions (script NEVER compiles)
# ---------------------------------------------------------------------------

def detect_toolchain(cfg):
    """Toolchain facts for the fallback message. Uses the toolchain dict
    persisted by setup_toolbox.py when present; falls back to PATH lookup
    of cl/gcc/clang."""
    tc = cfg.get("toolchain") if isinstance(cfg.get("toolchain"), dict) else {}
    cpp = tc.get("cpp") if isinstance(tc.get("cpp"), dict) else {}
    cl = cpp.get("cl")
    gcc = cpp.get("gcc")
    clang = cpp.get("clang")
    source = "config"
    if not (cl or gcc or clang):
        source = "fallback"
        cl = shutil.which("cl")
        gcc = shutil.which("gcc")
        clang = shutil.which("clang")
    return {"source": source, "cl": cl, "clNote": cpp.get("clNote"),
            "gcc": gcc, "clang": clang,
            "any": bool(cl or gcc or clang)}


def fallback_build_block(cfg):
    """Build the exact fallback instructions (data for JSON + human text).

    Mirrors the maintained recipe in references/framework-mod.md sections
    5.2-5.4: the source is 32-bit only by construction (~130 absolute game
    addresses in game_features.h, IAT patch at 0x0053432C, x86 naked inline
    asm) - an x64 build cannot work; every command below targets x86.
    Examples/ is read-only reference material (R17), so the build happens in
    a copied work directory, never inside Examples/.
    """
    tc = detect_toolchain(cfg)
    result = {"triggered": True, "toolchain": tc, "sourceDir": SOURCE_DIR,
              "sources": [], "depsMissing": DEPENDS_NOT_IN_WORKSPACE,
              "commands": [], "docs":
              ["references/framework-mod.md (sections 5.2-5.4: deps layout, "
               "file split, toolchain commands)",
               "SUBMOD_DEVELOPMENT_GUIDE.md chapter 5 (CMake template)",
               "references/environment-setup.md (Framework mod check)"]}
    if not os.path.isdir(SOURCE_DIR):
        result["commands"].append(
            "ERROR: source folder not found: %s" % SOURCE_DIR)
        return result
    sources = sorted(name for name in os.listdir(SOURCE_DIR)
                     if name.lower().endswith(".cpp")
                     and not name.lower().endswith("_ref.cpp"))
    result["sources"] = sources
    result["commands"].append(
        'mkdir "<build-root>\\Standalone MOD" && xcopy /E /I /H "%s" '
        '"<build-root>\\Standalone MOD\\"  (build-root = fresh directory '
        'outside Examples/ - never build inside Examples/, R17)' % SOURCE_DIR)
    result["commands"].append(
        "fill <build-root>\\deps\\ (next to the copied source folder) with "
        "the dependencies listed above: imgui + imgui\\backends + MinHook + "
        "nlohmann")
    # setup_toolbox.py stores either the vswhere installationPath (a
    # DIRECTORY -> vcvarsall.bat lives under it) or a full cl.exe path
    # (cl already on PATH - no vcvars line makes sense then).
    cl_dir = tc["cl"] if (tc["cl"] and
                          not str(tc["cl"]).lower().endswith(".exe")) else None
    if cl_dir and tc["clNote"]:
        result["commands"].append(
            'call "%s\\VC\\Auxiliary\\Build\\vcvarsall.bat" x86' %
            str(cl_dir).rstrip("\\"))
    else:
        result["commands"].append(
            "start an x86 Native Tools Command Prompt (Visual Studio Build "
            "Tools, C++ workload) - or run vcvarsall.bat x86 first")
    result["commands"].append('cd /d "<build-root>\\Standalone MOD"')
    # Canonical MSVC command - identical to references/framework-mod.md 5.4.
    result["commands"].append(
        "cl /nologo /EHsc /O2 /MD /std:c++17 /LD /D_CRT_SECURE_NO_WARNINGS "
        "/I..\\deps\\imgui /I..\\deps\\imgui\\backends "
        "/I..\\deps\\MinHook\\include /I..\\deps\\nlohmann " +
        " ".join(sources) +
        " ..\\deps\\imgui\\imgui.cpp ..\\deps\\imgui\\imgui_draw.cpp "
        "..\\deps\\imgui\\imgui_tables.cpp ..\\deps\\imgui\\imgui_widgets.cpp "
        "..\\deps\\imgui\\backends\\imgui_impl_dx9.cpp "
        "..\\deps\\imgui\\backends\\imgui_impl_win32.cpp "
        "..\\deps\\MinHook\\buffer.c ..\\deps\\MinHook\\hook.c "
        "..\\deps\\MinHook\\trampoline.c ..\\deps\\MinHook\\HDE\\hde.c "
        "/Fe:Framework_MOD.dll /link user32.lib gdi32.lib d3d9.lib dxguid.lib "
        "imm32.lib")
    result["commands"].append(
        "note: MinHook sources are compiled in above (they resolve MH_*); "
        "with a prebuilt 32-bit MinHook.lib link it instead, and extend "
        "/link with whatever the linker reports as unresolved "
        "(references/framework-mod.md 5.3/5.4)")
    if tc["gcc"] or tc["clang"]:
        result["commands"].append(
            "alternative (MinGW-w64 32-bit; MSVC structured exceptions are "
            "NOT portable to GCC on 32-bit Windows - if GCC rejects the SEH "
            "blocks in render_hook.cpp, build with MSVC, see "
            "references/framework-mod.md 5.4): i686-w64-mingw32-g++ -m32 "
            "-std=c++17 -O2 -shared -static-libgcc -static-libstdc++ "
            "-I../deps/imgui -I../deps/imgui/backends "
            "-I../deps/MinHook/include -I../deps/nlohmann "
            "-o Framework_MOD.dll " + " ".join(sources) +
            " ../deps/imgui/imgui.cpp ../deps/imgui/imgui_draw.cpp "
            "../deps/imgui/imgui_tables.cpp ../deps/imgui/imgui_widgets.cpp "
            "../deps/imgui/backends/imgui_impl_dx9.cpp "
            "../deps/imgui/backends/imgui_impl_win32.cpp "
            "../deps/MinHook/buffer.c ../deps/MinHook/hook.c "
            "../deps/MinHook/trampoline.c ../deps/MinHook/HDE/hde.c "
            "-luser32 -lgdi32 -ld3d9 -luuid -limm32")
    result["commands"].append(
        "CMake alternative: adapt the chapter 5 template of "
        "SUBMOD_DEVELOPMENT_GUIDE.md to the sources above and configure "
        "with -A Win32 (32-bit toolset is mandatory)")
    return result


def print_fallback(result):
    eprint("")
    eprint("=" * 72)
    eprint("FIX FAILED - fallback: build Framework_MOD.dll from source (x86!)")
    eprint("=" * 72)
    tc = result["toolchain"]
    if tc["any"]:
        eprint("Toolchain (%s):" % tc["source"])
        for key in ("cl", "gcc", "clang"):
            if tc.get(key):
                eprint("  %s: %s" % (key, tc[key]))
        if tc.get("clNote"):
            eprint("  note: %s" % tc["clNote"])
    else:
        eprint("Toolchain: NONE detected (config + PATH). Install Visual "
               "Studio Build Tools with the C++ workload - a C++ compiler "
               "is MANDATORY for framework changes (32-bit game process):")
        eprint("  %s" % VS_BUILD_TOOLS_CMD)
    eprint("")
    eprint("Complete C++ source of Lyra's Framework Mod v1.2:")
    eprint("  %s" % result["sourceDir"])
    if result["sources"]:
        eprint("Framework build sources (root *.cpp, *_ref files excluded):")
        for name in result["sources"]:
            eprint("  %s" % name)
    eprint("")
    eprint("The source needs dependencies that are NOT in this workspace")
    eprint("(fetch first - see references/framework-mod.md):")
    for dep in result["depsMissing"]:
        eprint("  - %s" % dep)
    eprint("")
    eprint("Build commands (x86/32-bit MANDATORY - Crazy Taxi 1 is a 32-bit")
    eprint("process; the source uses ~130 absolute 32-bit addresses, an IAT")
    eprint("patch at a fixed x86 address and x86 naked inline asm):")
    for cmd in result["commands"]:
        eprint("  %s" % cmd)
    eprint("")
    eprint("Follow %s for the complete framework mod flow (deps setup," % FRAMEWORK_DOC)
    eprint("maintained build recipe, loader, sub-mod ABI).")
    eprint("=" * 72)


# ---------------------------------------------------------------------------
# Main flow
# ---------------------------------------------------------------------------

EXIT_VALID = 0
EXIT_MISSING = 1
EXIT_INVALID = 2
EXIT_NO_GAMEDIR = 3
EXIT_FIX_FAILED = 4


def summarize_human(result):
    print("Framework mod check (%s)" % FRAMEWORK_DLL_NAME)
    game_dir = result["gameDir"]
    print("  gameDir : %s" % (game_dir or "(none - run find_game.py first)"))
    if game_dir is None:
        return
    print("  DLL     : %s" % result["dllPath"])
    print("  status  : %s" % result["frameworkStatus"])
    if result["exists"]:
        if result["size"] is not None:
            print("  size    : %d bytes" % result["size"])
            print("  sha256  : %s" % result["sha256"])
        ref = result["referenceMatch"]
        if ref and (not ref["size"] or not ref["sha256"]):
            print("  NOTE    : differs from the reference release build "
                  "(v1.2). Fine if you built it yourself from "
                  "Examples/Standalone MOD; otherwise re-run with --fix "
                  "to install the verified reference build.")
        for reason in result["reasons"]:
            print("  INVALID : %s" % reason)
    loader = result["loader"] or {}
    if loader.get("exists") and not loader.get("valid", True):
        print("  loader  : %s exists but looks INVALID: %s" %
              (loader.get("path"), "; ".join(loader.get("reasons", []))))
    elif not loader.get("exists"):
        print("  loader  : %s missing (framework mod would never load)" %
              loader.get("path"))
    fix_info = result["fix"] or {}
    if fix_info.get("attempted"):
        if fix_info.get("ok"):
            print("  --fix   : SUCCESS")
            for item in fix_info.get("installed", []):
                print("            installed %s" % item)
            for item in fix_info.get("skipped", []):
                print("            skipped  %s (%s)" %
                      (item["path"], item["reason"]))
        else:
            print("  --fix   : FAILED - see stderr for the source-build "
                  "fallback (x86) and %s" % FRAMEWORK_DOC)


def main():
    parser = argparse.ArgumentParser(
        description="Check (and optionally install) Lyra's Framework Mod "
                    "(<gameDir>/mods/Framework_MOD/Framework_MOD.dll) "
                    "before sub-mod development. gameDir comes from "
                    "config/environment.json (see find_game.py).")
    parser.add_argument("--fix", action="store_true",
                        help="download the probe-verified official archive, "
                             "validate it byte-exact and install the "
                             "framework DLL (and the proxy loader when "
                             "absent); on failure print the exact x86 "
                             "source-build fallback instead of failing "
                             "silently")
    parser.add_argument("--json", action="store_true",
                        help="print a single-line JSON report on stdout")
    args = parser.parse_args()

    result = {
        "tool": "check_framework.py",
        "arguments": {"fix": bool(args.fix)},
        "gameDir": None,
        "dllPath": None,
        "frameworkStatus": None,
        "configUpdated": False,
        "exists": None,
        "size": None,
        "sha256": None,
        "reasons": [],
        "pe": None,
        "exports": None,
        "referenceMatch": None,
        "loader": None,
        "reference": {
            "archive": {"filename": ZIP_FILENAME, "url": DOWNLOAD_URL,
                        "legacyUrlNotUsable": LEGACY_URL,
                        "size": EXPECTED_ZIP_SIZE,
                        "sha256": EXPECTED_ZIP_SHA256,
                        "magic": "PK\\x03\\x04",
                        "requiredEntries": REQUIRED_ZIP_ENTRIES},
            "frameworkDll": {"size": EXPECTED_DLL_SIZE,
                             "sha256": EXPECTED_DLL_SHA256,
                             "machine": "0x014c",
                             "requiredExports": REQUIRED_FRAMEWORK_EXPORTS},
            "loaderDll": {"size": EXPECTED_LOADER_SIZE,
                          "sha256": EXPECTED_LOADER_SHA256,
                          "machine": "0x014c",
                          "requiredExports": REQUIRED_LOADER_EXPORTS},
        },
        "fix": None,
        "fallback": None,
        "nextAction": None,
        "exitCode": None,
    }

    def finish(code):
        result["exitCode"] = code
        if args.json:
            print(json.dumps(result))
        else:
            summarize_human(result)
        return code

    # --- config / gameDir gate --------------------------------------------
    try:
        cfg = load_config()
    except ConfigError as exc:
        eprint("ERROR: %s" % exc)
        eprint("Fix or delete %s, then run: python scripts/find_game.py" %
               CONFIG_PATH)
        result["nextAction"] = ("repair config/environment.json, then run "
                                "python scripts/find_game.py")
        return finish(EXIT_NO_GAMEDIR)

    game_dir = usable_game_dir(cfg)
    if game_dir is None:
        stored = cfg.get("gameDir")
        eprint("ERROR: no usable gameDir in %s%s" %
               (CONFIG_PATH, "" if not stored else
                (" (stored value %r does not exist on disk)" % stored)))
        eprint("Run first: python scripts/find_game.py")
        result["nextAction"] = "run python scripts/find_game.py first"
        # Reset stale framework state - it cannot refer to any game dir.
        if cfg.get("frameworkDll") is not None or \
                cfg.get("frameworkStatus") is not None:
            cfg["frameworkDll"] = None
            cfg["frameworkStatus"] = None
            save_config(cfg)
            result["configUpdated"] = True
        result["frameworkStatus"] = None
        return finish(EXIT_NO_GAMEDIR)

    result["gameDir"] = game_dir
    dll_path = framework_dll_path(game_dir)
    result["dllPath"] = dll_path

    loader = check_loader(game_dir)
    result["loader"] = loader

    # --- installed-DLL check -----------------------------------------------
    exists, valid, report = check_installed_file(
        dll_path, REQUIRED_FRAMEWORK_EXPORTS, EXPECTED_DLL_SIZE,
        EXPECTED_DLL_SHA256)
    result["exists"] = exists
    result["size"] = report.get("size")
    result["sha256"] = report.get("sha256")
    result["reasons"] = report.get("reasons", [])
    result["pe"] = compact_pe(report.get("pe"))
    result["exports"] = report.get("exports")
    result["referenceMatch"] = report.get("referenceMatch")
    status = "missing" if not exists else ("valid" if valid else "invalid")
    if exists:
        eprint("checked %s" % dll_path)
        if report.get("size") is not None:
            eprint("  %d bytes, sha256 %s" % (report["size"], report["sha256"]))
        for reason in report["reasons"]:
            eprint("  INVALID: %s" % reason)
        ref = report["referenceMatch"]
        if valid and (not ref["size"] or not ref["sha256"]):
            eprint("  INFO: valid x86 DLL but differs from the reference "
                   "release (size match=%s, sha256 match=%s) - typical for "
                   "a locally built DLL" %
                   (ref["size"], ref["sha256"]))
    else:
        eprint("%s does not exist" % dll_path)
    result["frameworkStatus"] = status
    cfg["frameworkDll"] = dll_path
    cfg["frameworkStatus"] = status

    # --- fix state for output (may stay untouched) --------------------------
    fix_info = {"attempted": False, "performed": False, "ok": False,
                "download": None, "installed": [], "overwritten": None,
                "skipped": [], "errors": []}
    result["fix"] = fix_info

    def persist():
        save_config(cfg)
        result["configUpdated"] = True

    # --- decide the exit path ------------------------------------------------
    if status == "valid":
        if args.fix:
            eprint("--fix: framework DLL already present and valid; nothing "
                   "to do (idempotent)")
            fix_info["attempted"] = True
            fix_info["ok"] = True
        persist()
        result["nextAction"] = None
        return finish(EXIT_VALID)

    # status is "missing" or "invalid" here
    if not args.fix:
        persist()
        action = ("run python scripts/check_framework.py --fix to download "
                  "and install the verified reference build, or follow %s "
                  "to build from Examples/Standalone MOD (x86)" %
                  FRAMEWORK_DOC)
        eprint("framework mod %s; %s" % (status, action))
        result["nextAction"] = action
        return finish(EXIT_MISSING if status == "missing" else EXIT_INVALID)

    # --- --fix on missing/invalid --------------------------------------------
    fix_info["attempted"] = True
    tmp_dir = tempfile.mkdtemp(prefix="ct1-framework-")
    try:
        archive_path, evidence = download_archive(tmp_dir)
        fix_info["download"] = evidence
        payloads = validate_archive(archive_path)
        installed, overwritten = install_archive(game_dir, archive_path)
        fix_info["installed"] = installed
        fix_info["overwritten"] = overwritten
        fix_info["skipped"] = []
        fix_info["performed"] = True
        fix_info["ok"] = True
        _, fw_report = validate_module(payloads["frameworkDll"],
                                       REQUIRED_FRAMEWORK_EXPORTS)
        result["exists"] = True
        result["size"] = len(payloads["frameworkDll"])
        result["sha256"] = payloads["frameworkDllSha256"]
        result["reasons"] = []
        result["pe"] = compact_pe(fw_report["pe"])
        result["exports"] = fw_report["exports"]
        result["referenceMatch"] = {
            "size": result["size"] == EXPECTED_DLL_SIZE,
            "sha256": result["sha256"] == EXPECTED_DLL_SHA256,
        }
        result["frameworkStatus"] = "valid"
        cfg["frameworkDll"] = dll_path
        cfg["frameworkStatus"] = "valid"
        # Refresh the loader info: --fix may have just installed it.
        result["loader"] = check_loader(game_dir)
        persist()
        result["nextAction"] = None
        return finish(EXIT_VALID)
    except FixError as exc:
        fix_info["errors"].append(str(exc))
        persist()
        eprint("ERROR (--fix): %s" % exc)
        fallback = fallback_build_block(cfg)
        result["fallback"] = fallback
        print_fallback(fallback)
        result["nextAction"] = (
            "follow %s: build Framework_MOD.dll from Examples/Standalone MOD "
            "with the printed 32-bit commands, or fix the download problem "
            "and re-run python scripts/check_framework.py --fix" %
            FRAMEWORK_DOC)
        return finish(EXIT_FIX_FAILED)
    finally:
        shutil.rmtree(tmp_dir, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
