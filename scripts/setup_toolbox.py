#!/usr/bin/env python3
"""One-entry toolbox setup for the Crazy Taxi 1 modding skill (rules R0-R5).

What it does, in order:
  R0  Verify Python (>= 3.8). If Python is missing entirely, this script
      cannot run - the setup runbook (references/environment-setup.md) then
      tells the agent to install Python via winget or the python.org
      installer and continue afterwards.
  R1  Detect IDA Pro. Found -> install the ida-pro-mcp integration into
      <skill>/Toolbox. Not found -> detect Cutter; if missing, download it
      from cutter.re (GitHub releases) into Toolbox, then install the
      CutterMCP bridge/plugin into Toolbox.
  R2  Blender / Blender-MCP: STATUS ONLY in the default run. Installation
      happens exclusively via `--install blender` / `--install blender-mcp`
      and may only be run AFTER the agent asked the user the R2 question
      (printed by those branches):
          "Do you want to edit textures, splines, or 3D objects?"
  R3  Download x64dbg/x32dbg (portable snapshot) into Toolbox and install
      the x64dbg-mcp-server plugin (github.com/duty1g/x64dbg-mcp-server).
  R4  Locate Crazy Taxi.exe (delegates to scripts/find_game.py) and persist
      the path to config/environment.json.
  R5  Detect C++ (cl/gcc/clang via PATH + vswhere) and Rust (cargo/rustc)
      toolchains; store them in config/environment.json. C++ is preferred.
  R7  Report port status/collisions for the four MCP servers (see
      references/mcp-servers.md). Hard collisions fail the setup.

CLI (exact):
  python scripts/setup_toolbox.py [--check-only] [--json]
      [--install ida-mcp|cutter|cutter-mcp|blender|blender-mcp|x64dbg|x64dbg-mcp]

Output:
  - Progress/human detail always goes to stderr.
  - stdout: a human summary by default, a single-line JSON report with
    --json (agent consumable).

Exit codes:
  0  setup completed without errors (or, with --check-only, everything
     required is present)
  1  one or more steps failed / are missing (see step details)
  2  usage or configuration error

Python 3.8+, standard library only. Downloads go into <skill>/Toolbox via
urllib. All paths resolve relative to this script; no hard-coded machine
paths. Host-tool agnostic: register resulting MCP servers in whatever MCP
host you use ("ask the user before proceeding" when unsure).
"""

from __future__ import annotations

import argparse
import importlib.util
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import sysconfig
import tempfile
import urllib.request
import zipfile
from pathlib import Path

try:
    import winreg  # Windows only; optional
except ImportError:
    winreg = None

SCRIPT_DIR = Path(__file__).resolve().parent
SKILL_ROOT = SCRIPT_DIR.parent
CONFIG_PATH = SKILL_ROOT / "config" / "environment.json"

CONFIG_TEMPLATE = {
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

# Default MCP ports (verified against the upstream sources; see
# references/mcp-servers.md for the full registry and collision rules).
# x64dbg-mcp: the 64-bit debugger defaults to 9094, the 32-bit x32dbg to
# 9095 (upstream config.zig). This skill targets Crazy Taxi 1 (32-bit) ->
# x32dbg -> 9095.
DEFAULT_PORTS = {"ida": 13337, "cutter": 8000, "x64dbg": 9095, "blender": 9876}
SERVER_ORDER = ("ida", "cutter", "x64dbg", "blender")

DOWNLOAD_TIMEOUT = 120          # seconds per connect/read
GITHUB_API = "https://api.github.com"
USER_AGENT = "ct1-skill-setup/1.0 (+local skill toolbox; stdlib urllib)"

R2_QUESTION = "Do you want to edit textures, splines, or 3D objects?"

STEPS = []  # accumulated step reports


# --------------------------------------------------------------------------
# small helpers
# --------------------------------------------------------------------------

def eprint(msg):
    sys.stderr.write(str(msg) + "\n")


def step(step_id, name, status, detail="", actions=None):
    entry = {"id": step_id, "name": name, "status": status,
             "detail": detail, "actions": actions or []}
    STEPS.append(entry)
    eprint("[%s] %s -> %s%s" % (step_id, name, status,
                                (" - " + detail) if detail else ""))
    return entry


def load_config():
    if CONFIG_PATH.exists():
        try:
            with open(CONFIG_PATH, "r", encoding="utf-8") as fh:
                cfg = json.load(fh)
            if not isinstance(cfg, dict):
                raise ValueError("config root must be a JSON object")
        except (OSError, ValueError) as exc:
            eprint("ERROR: cannot read %s: %s" % (CONFIG_PATH, exc))
            raise SystemExit(2)
    else:
        cfg = json.loads(json.dumps(CONFIG_TEMPLATE))
    for key, value in CONFIG_TEMPLATE.items():
        cfg.setdefault(key, json.loads(json.dumps(value)))
    return cfg


def save_config(cfg):
    CONFIG_PATH.parent.mkdir(parents=True, exist_ok=True)
    with open(CONFIG_PATH, "w", encoding="utf-8") as fh:
        json.dump(cfg, fh, indent=2, ensure_ascii=False)
        fh.write("\n")


def toolbox_dir(cfg):
    raw = cfg.get("toolboxDir") or "Toolbox"
    path = Path(raw)
    if not path.is_absolute():
        path = SKILL_ROOT / path
    return path


def mcp_entry(cfg, name, port):
    servers = cfg.setdefault("mcpServers", {})
    entry = servers.setdefault(name, {})
    entry.setdefault("port", port)
    entry.setdefault("installed", False)
    entry.setdefault("tested", False)
    return entry


# --------------------------------------------------------------------------
# network helpers (stdlib urllib only)
# --------------------------------------------------------------------------

def http_get(url, timeout=DOWNLOAD_TIMEOUT):
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return response.read()


def github_json(path):
    data = http_get(GITHUB_API + path)
    return json.loads(data.decode("utf-8"))


def github_latest_asset(repo, predicate):
    """Return the download URL of the newest release asset matching
    `predicate(name)` or None. Uses the public GitHub API."""
    release = github_json("/repos/%s/releases/latest" % repo)
    for asset in release.get("assets", []):
        name = asset.get("name", "")
        if predicate(name):
            return asset.get("browser_download_url")
    return None


def download_file(url, dest, min_bytes=1):
    """Stream a URL to dest (via .part + atomic replace). Idempotent:
    an existing, big-enough file short-circuits the download."""
    if dest.exists() and dest.stat().st_size >= min_bytes:
        eprint("  already downloaded: %s" % dest)
        return dest
    dest.parent.mkdir(parents=True, exist_ok=True)
    eprint("  downloading %s" % url)
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(request, timeout=DOWNLOAD_TIMEOUT) as resp, \
            open(str(dest) + ".part", "wb") as out:
        total = 0
        while True:
            chunk = resp.read(256 * 1024)
            if not chunk:
                break
            out.write(chunk)
            total += len(chunk)
    if total < min_bytes:
        raise OSError("download too small (%d bytes): %s" % (total, url))
    os.replace(str(dest) + ".part", str(dest))
    eprint("  saved %s (%d bytes)" % (dest, total))
    return dest


def extract_and_place(zip_path, target_dir):
    """Extract a zip into target_dir, moving every top-level entry of the
    archive into target_dir (no flattening - release layouts differ between
    projects, so consumers locate their files by recursive search).
    Idempotent: a non-empty target_dir is left untouched."""
    target_dir = Path(target_dir)
    if target_dir.exists() and any(target_dir.iterdir()):
        eprint("  already extracted: %s" % target_dir)
        return target_dir
    target_dir.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(
            prefix="ct1skill-", dir=str(target_dir.parent)) as tmp:
        tmp_path = Path(tmp)
        with zipfile.ZipFile(zip_path) as archive:
            for member in archive.infolist():
                dest = (tmp_path / member.filename).resolve()
                if not str(dest).startswith(str(tmp_path.resolve())):
                    raise OSError("unsafe zip entry: %s" % member.filename)
            archive.extractall(tmp_path)
        target_dir.mkdir(parents=True, exist_ok=True)
        for entry in list(tmp_path.iterdir()):
            shutil.move(str(entry), str(target_dir / entry.name))
    eprint("  extracted to %s" % target_dir)
    return target_dir


def run_cmd(cmd, timeout=300):
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True,
                              encoding="utf-8", errors="replace",
                              timeout=timeout)
        return (proc.returncode, (proc.stdout or "").strip(),
                (proc.stderr or "").strip())
    except (OSError, subprocess.SubprocessError) as exc:
        return 1, "", str(exc)


def pip_install(pip_args):
    """Run pip with the running interpreter. Returns (ok, output)."""
    code, out, err = run_cmd([sys.executable, "-m", "pip", "install"]
                             + pip_args, timeout=900)
    return code == 0, (out or err)


# --------------------------------------------------------------------------
# port / collision helpers (R7)
# --------------------------------------------------------------------------

def port_pids(port):
    """PIDs of processes with a socket on the local port (any state),
    via netstat -ano. Empty list when the port is unused."""
    code, out, _ = run_cmd(["netstat", "-ano", "-p", "TCP"], timeout=60)
    pids = []
    if code == 0:
        suffix = ":%d" % port
        for line in out.splitlines():
            tokens = line.split()
            # Windows netstat -ano TCP row: TCP  <local>  <foreign>  <state>  <pid>
            if len(tokens) >= 5 and tokens[0].upper() == "TCP" and \
                    tokens[1].lower().endswith(suffix):
                pid = tokens[4]
                if pid.isdigit() and int(pid) not in pids:
                    pids.append(int(pid))
    return pids


def pid_image_name(pid):
    code, out, _ = run_cmd(["tasklist", "/FI", "PID eq %d" % pid,
                            "/FO", "CSV", "/NH"], timeout=60)
    if code == 0 and out and not out.startswith("INFO:"):
        first = out.splitlines()[0].strip()
        parts = [p.strip('"') for p in first.split('","')]
        if parts and parts[0]:
            return parts[0]
    return None


def port_owner(port):
    """Returns (listening: bool, owner_name or None, pid or None).
    Prefers a LISTENING owner; falls back to any process on the port."""
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


EXPECTED_IMAGES = {
    "ida": ["ida"],
    # No "python" hint: the CutterMCP plugin serves port 8000 from INSIDE
    # Cutter.exe; a python.exe on 8000 is a foreign process (Jupyter, local
    # web apps) and must be reported as a collision (R7).
    "cutter": ["cutter"],
    "x64dbg": ["x64dbg", "x32dbg"],
    "blender": ["blender"],
}

REMEDY_BY_SERVER = {
    "ida": "ida-pro-mcp auto-increments to the next free port and registers "
           "each instance in its discovery file. Find the real port in the "
           "IDA dialog (MCP server config) or the discovery registry, then "
           "store it in config/environment.json under mcpServers.ida.port, "
           "or stop the foreign process.",
    "cutter": "cuttermcp listens HARDCODED on port 8000 - it cannot be "
              "changed. Stop the foreign process occupying 8000 (e.g. "
              "Jupyter/local webapps) before using the Cutter MCP.",
    "x64dbg": "Change the port in x64dbg under Plugins > x64dbg-MCP Server > "
              "Configure MCP Server... (persisted to mcp_config.json next to "
              "the debugger exe), then store it in config/environment.json "
              "under mcpServers.x64dbg.port, or stop the foreign process.",
    "blender": "Change the port in the Blender addon panel and/or start the "
               "MCP server with `uvx mcp-for-blender --port N` (the addon "
               "panel port must match), then store it in "
               "config/environment.json under mcpServers.blender.port.",
}


def check_ports(cfg):
    """R7: verify the configured ports of the four MCP servers are free or
    owned by the expected server, and that no two servers share a port."""
    servers = cfg.get("mcpServers") or {}
    ports = {}
    for name in SERVER_ORDER:
        entry = servers.get(name) or {}
        ports[name] = int(entry.get("port") or DEFAULT_PORTS[name])

    duplicates = {}
    for name, port in ports.items():
        duplicates.setdefault(port, []).append(name)
    dup_ports = {p: names for p, names in duplicates.items() if len(names) > 1}

    results = []
    for name in SERVER_ORDER:
        port = ports[name]
        listening, image, pid = port_owner(port)
        entry = {"server": name, "port": port, "listening": listening,
                 "owner": image, "pid": pid, "collision": False}
        if name in dup_ports.get(port, []) and len(dup_ports.get(port, [])) > 1:
            entry["collision"] = True
            entry["problem"] = ("configured ports collide: %s" %
                                ", ".join(dup_ports[port]))
            entry["remedy"] = ("each MCP server needs its own port; change "
                               "one of them as described in "
                               "references/mcp-servers.md")
        elif listening and not image_matches(image, EXPECTED_IMAGES[name]):
            entry["collision"] = True
            entry["problem"] = ("port %d is occupied by %s (pid %s), not by "
                                "the %s MCP server" %
                                (port, image or "unknown process", pid, name))
            entry["remedy"] = REMEDY_BY_SERVER[name]
        elif listening:
            entry["problem"] = None
        else:
            entry["problem"] = None
        results.append(entry)
    return results


# --------------------------------------------------------------------------
# detection helpers
# --------------------------------------------------------------------------

def find_ida_dir():
    """IDA Pro install directory (contains ida.exe/ida64.exe/ida32.exe),
    or None. Scans Program Files for IDA* directories. A Hex-Rays user dir
    under %APPDATA% alone is NOT treated as an IDA installation (see
    step_r1, which reports it as a hint)."""
    for var in ("ProgramFiles", "ProgramFiles(x86)"):
        base = os.environ.get(var)
        if not base or not os.path.isdir(base):
            continue
        try:
            entries = os.listdir(base)
        except OSError:
            continue
        for entry in entries:
            if entry.lower().startswith("ida"):
                directory = os.path.join(base, entry)
                if not os.path.isdir(directory):
                    continue
                for exe in ("ida.exe", "ida64.exe", "ida32.exe"):
                    if os.path.isfile(os.path.join(directory, exe)):
                        return directory
    return None


def exe_under(root, exe_name, max_depth=3):
    """Find exe_name under root (recursive, depth-limited). Returns the
    path as str or None."""
    root = Path(root)
    if not root.is_dir():
        return None
    root_str = os.path.normpath(str(root))
    for current, dirs, files in os.walk(root):
        rel = os.path.relpath(os.path.normpath(current), root_str)
        depth = 0 if rel == "." else len(rel.split(os.sep))
        if depth >= max_depth:
            dirs[:] = []
        if exe_name.lower() in (f.lower() for f in files):
            return str(Path(current) / exe_name)
    return None


def find_cutter_exe(toolbox):
    for candidate in (os.path.join(os.environ.get("ProgramFiles", ""),
                                   "Cutter", "Cutter.exe"),
                      os.path.join(os.environ.get("ProgramFiles(x86)", ""),
                                   "Cutter", "Cutter.exe"),
                      os.path.join(os.environ.get("LOCALAPPDATA", ""),
                                   "Programs", "Cutter", "Cutter.exe")):
        if candidate and os.path.isfile(candidate):
            return candidate
    return exe_under(toolbox / "cutter", "Cutter.exe")


def find_blender_exe(toolbox):
    base = os.environ.get("ProgramFiles", "")
    if base and os.path.isdir(base):
        foundation = os.path.join(base, "Blender Foundation")
        if os.path.isdir(foundation):
            for entry in sorted(os.listdir(foundation), reverse=True):
                exe = os.path.join(foundation, entry, "blender.exe")
                if os.path.isfile(exe):
                    return exe
    return exe_under(toolbox / "blender", "blender.exe")


def find_x96dbg(toolbox):
    """x96dbg.exe launcher path: the Toolbox copy first, then an existing
    system-wide installation (Program Files / LOCALAPPDATA Programs).
    R3 only downloads the ~32 MB snapshot when neither exists; the MCP
    plugin is then deployed into whichever installation was found."""
    exe = exe_under(toolbox / "x64dbg", "x96dbg.exe", max_depth=4)
    if exe:
        return exe
    for var, sub in (("ProgramFiles", None), ("ProgramFiles(x86)", None),
                     ("LOCALAPPDATA", "Programs")):
        base = os.environ.get(var)
        if sub:
            base = os.path.join(base, sub) if base else None
        if not base or not os.path.isdir(base):
            continue
        try:
            entries = os.listdir(base)
        except OSError:
            continue
        for entry in entries:
            if "x64dbg" not in entry.lower():
                continue
            directory = os.path.join(base, entry)
            if not os.path.isdir(directory):
                continue
            found = exe_under(directory, "x96dbg.exe", max_depth=4)
            if found:
                return found
    return None


# --------------------------------------------------------------------------
# R0 - Python
# --------------------------------------------------------------------------

def step_python(check_only):
    version = "%d.%d.%d" % sys.version_info[:3]
    if sys.version_info < (3, 8):
        return step("R0", "Python", "error",
                    "Python %s is too old; this skill needs Python 3.8+ "
                    "(ida-pro-mcp additionally needs 3.11+). Install a newer "
                    "Python (winget: `winget install -e --id "
                    "Python.Python.3.12`, or https://www.python.org/downloads/) "
                    "and re-run." % version)
    detail = "Python %s at %s" % (version, sys.executable)
    if sys.version_info < (3, 11):
        detail += " (note: ida-pro-mcp requires Python 3.11+; the Cutter and " \
                  "x64dbg branches work on 3.8+)"
    code, out, _ = run_cmd([sys.executable, "-m", "pip", "--version"],
                           timeout=60)
    if code != 0:
        detail += "; pip NOT available - package installs will fail"
        return step("R0", "Python", "partial", detail,
                    ["enable pip for this interpreter (e.g. "
                     "`python -m ensurepip`) and re-run"])
    return step("R0", "Python", "ok", detail + "; pip available")


# --------------------------------------------------------------------------
# R1 - IDA Pro vs Cutter (+ their MCPs)
# --------------------------------------------------------------------------

IDA_MCP_ZIP = ("https://github.com/mrexodia/ida-pro-mcp/"
               "archive/refs/heads/main.zip")


def install_ida_mcp(cfg, check_only, toolbox):
    ida_dir = find_ida_dir()
    if not ida_dir:
        return step("R1", "ida-pro-mcp", "error",
                    "IDA Pro not detected (no IDA* directory with "
                    "ida.exe/ida64.exe/ida32.exe in Program Files). Install "
                    "IDA Pro 8.3+/9.x first, or use the Cutter route instead "
                    "(`python scripts/setup_toolbox.py --install cutter` "
                    "then `--install cutter-mcp`).",
                    ["ask the user whether they own IDA Pro before "
                     "installing anything"])
    detail = "IDA Pro detected at %s" % ida_dir
    actions = []

    repo = toolbox / "ida-pro-mcp"
    marker = (repo / "pyproject.toml").exists() or \
        (repo / "setup.py").exists() or (repo / "src").is_dir()
    if not marker:
        if check_only:
            return step("R1", "ida-pro-mcp", "missing",
                        detail + "; Toolbox copy missing (would be installed "
                        "by `--install ida-mcp` or a default run)")
        zip_path = toolbox / "_downloads" / "ida-pro-mcp-main.zip"
        try:
            download_file(IDA_MCP_ZIP, zip_path, min_bytes=10000)
            extract_and_place(zip_path, repo)
        except (OSError, zipfile.BadZipFile) as exc:
            return step("R1", "ida-pro-mcp", "error",
                        "download/extract failed: %s" % exc)
        actions.append("downloaded ida-pro-mcp into Toolbox")
    else:
        detail += "; Toolbox copy present"

    if importlib.util.find_spec("ida_pro_mcp") is None:
        if check_only:
            return step("R1", "ida-pro-mcp", "missing",
                        detail + "; Python package not installed")
        ok, output = pip_install([str(repo)])
        if not ok:
            return step("R1", "ida-pro-mcp", "error",
                        "pip install failed: %s" % output)
        actions.append("pip-installed ida-pro-mcp from Toolbox")
        detail += "; Python package installed"
    else:
        detail += "; Python package installed"

    # Register the IDA plugin (upstream: `ida-pro-mcp --install`). Best
    # effort; the plugin dialog inside IDA can do this manually too.
    entry_point = shutil.which("ida-pro-mcp")
    if not entry_point:
        candidate = os.path.join(sysconfig.get_path("scripts"),
                                 "ida-pro-mcp.exe")
        if os.path.isfile(candidate):
            entry_point = candidate
    if entry_point and not check_only:
        code, out, err = run_cmd([entry_point, "--install"], timeout=120)
        if code == 0:
            actions.append("registered the IDA plugin (ida-pro-mcp --install)")
        else:
            actions.append("ida-pro-mcp --install failed (%s); run it "
                           "manually inside the IDA Python environment" %
                           (err or out))
    elif not entry_point:
        actions.append("entry point ida-pro-mcp.exe not on PATH; run "
                       "`ida-pro-mcp --install` manually after pip install")
    actions.append("restart IDA completely; the plugin serves "
                   "http://127.0.0.1:13337/mcp while IDA is open")

    if not check_only:
        entry = mcp_entry(cfg, "ida", DEFAULT_PORTS["ida"])
        entry["installed"] = True
    return step("R1", "ida-pro-mcp", "installed", detail, actions)


CUTTER_ZIP_PREDICATE = lambda name: (name.startswith("Cutter-v") and
                                     "Windows" in name and "x86_64" in name
                                     and name.endswith(".zip"))


def ensure_cutter(cfg, check_only, toolbox):
    exe = find_cutter_exe(toolbox)
    if exe:
        return step("R1", "Cutter", "ok", "Cutter found at %s" % exe)
    if check_only:
        return step("R1", "Cutter", "missing",
                    "Cutter not detected (would be downloaded from "
                    "cutter.re / GitHub releases into Toolbox, ~140 MB)")
    url = github_latest_asset("rizinorg/cutter", CUTTER_ZIP_PREDICATE)
    if not url:
        return step("R1", "Cutter", "error",
                    "could not resolve a Windows-x86_64 zip from the "
                    "rizinorg/cutter latest release (check network/username "
                    "of cutter.re redirects)")
    zip_path = toolbox / "_downloads" / url.rsplit("/", 1)[-1]
    try:
        download_file(url, zip_path, min_bytes=1000000)
        extract_and_place(zip_path, toolbox / "cutter")
    except (OSError, zipfile.BadZipFile) as exc:
        return step("R1", "Cutter", "error", "download/extract failed: %s" % exc)
    exe = find_cutter_exe(toolbox)
    if not exe:
        return step("R1", "Cutter", "partial",
                    "downloaded but Cutter.exe not found under %s" %
                    (toolbox / "cutter"))
    return step("R1", "Cutter", "installed", "portable Cutter at %s" % exe,
                ["open the binary in Cutter before using the Cutter MCP"])


CUTTERMCP_ZIP_PREDICATE = lambda name: name.endswith(".zip")


def ensure_cuttermcp(cfg, check_only, toolbox):
    if not find_cutter_exe(toolbox):
        return step("R1", "CutterMCP", "error",
                    "Cutter is not installed; run "
                    "`python scripts/setup_toolbox.py --install cutter` "
                    "first (R1 order: Cutter, then cuttermcp).")
    detail, actions = "CutterMCP bridge/plugin for Cutter", []
    bridge = toolbox / "cuttermcp" / "bridge_mcp_cutter.py"
    plugin_src = toolbox / "cuttermcp" / "src" / "CutterMCPPlugin.py"

    if not bridge.exists() or not plugin_src.exists():
        if check_only:
            return step("R1", "CutterMCP", "missing",
                        "Toolbox/cuttermcp incomplete (would be installed by "
                        "`--install cutter-mcp` or a default run)")
        url = github_latest_asset("ap425q/CutterMCP", CUTTERMCP_ZIP_PREDICATE)
        zip_path = toolbox / "_downloads" / (
            url.rsplit("/", 1)[-1] if url else "cuttermcp-main.zip")
        try:
            if url:
                download_file(url, zip_path, min_bytes=10000)
            else:
                download_file(
                    "https://github.com/ap425q/CutterMCP/"
                    "archive/refs/heads/main.zip", zip_path, min_bytes=10000)
            extract_and_place(zip_path, toolbox / "cuttermcp")
        except (OSError, zipfile.BadZipFile) as exc:
            return step("R1", "CutterMCP", "error",
                        "download/extract failed: %s" % exc)
        actions.append("downloaded cuttermcp release into Toolbox")

    requirements = toolbox / "cuttermcp" / "requirements.txt"
    if requirements.exists() and not check_only:
        ok, output = pip_install(["-r", str(requirements)])
        actions.append("pip install -r requirements.txt %s" %
                       ("OK (mcp==1.5.0, requests==2.32.3)" if ok
                        else "FAILED: %s" % output))

    # Copy the plugin into Cutter's python plugin folder when it can be
    # located; otherwise print the exact manual step (Cutter shows the
    # folder under Edit -> Preferences -> Plugins).
    cutter_exe = find_cutter_exe(toolbox)
    copied = False
    if cutter_exe and plugin_src.exists():
        cutter_root = Path(cutter_exe).parent
        for current, dirs, _files in os.walk(cutter_root):
            current_path = Path(current)
            if current_path.name.lower() == "python" and \
                    any(p.lower() == "plugins" for p in current_path.parts):
                try:
                    shutil.copy2(str(plugin_src),
                                 str(current_path / "CutterMCPPlugin.py"))
                    copied = True
                    actions.append("plugin copied to %s" % current)
                    break
                except OSError as exc:
                    actions.append("plugin copy failed: %s" % exc)
    if not copied:
        actions.append(
            "MANUAL STEP: open Cutter -> Edit -> Preferences -> Plugins to "
            "see the plugin directory, and copy %s into its 'python' "
            "subfolder, then restart Cutter" % plugin_src)
        status = "partial"
        if not check_only:
            mcp_entry(cfg, "cutter", DEFAULT_PORTS["cutter"])
        return step("R1", "CutterMCP", status, detail, actions)

    actions.append("bridge for your MCP host: python %s (stdio)" % bridge)
    actions.append("plugin HTTP endpoint: http://127.0.0.1:8000/ "
                   "(port 8000 is hardcoded)")
    if not check_only:
        entry = mcp_entry(cfg, "cutter", DEFAULT_PORTS["cutter"])
        entry["installed"] = True
    return step("R1", "CutterMCP", "installed", detail, actions)


def step_r1(cfg, check_only, toolbox):
    ida_dir = find_ida_dir()
    if ida_dir:
        return install_ida_mcp(cfg, check_only, toolbox)
    appdata = os.environ.get("APPDATA")
    if appdata and os.path.isdir(os.path.join(appdata, "Hex-Rays",
                                              "IDA Pro")):
        eprint("note: a Hex-Rays IDA user dir exists but no IDA executable "
               "was found in Program Files -> treating as no IDA")
    eprint("IDA Pro not detected -> using the Cutter route (R1)")
    # ensure_* functions record their own steps in STEPS; nothing to extend.
    ensure_cutter(cfg, check_only, toolbox)
    ensure_cuttermcp(cfg, check_only, toolbox)
    return None


# --------------------------------------------------------------------------
# R2 - Blender / Blender MCP (status only unless explicitly installed)
# --------------------------------------------------------------------------

BLENDER_ADDON_URL = ("https://raw.githubusercontent.com/ahujasid/"
                     "mcp-for-blender/main/addon.py")


def blender_listing(versions_html):
    dirs = re.findall(r'href="(Blender\d+\.\d+)/"', versions_html)
    parsed = []
    for entry in dirs:
        match = re.match(r"Blender(\d+)\.(\d+)", entry)
        if match:
            parsed.append((int(match.group(1)), int(match.group(2)), entry))
    if not parsed:
        return None
    return max(parsed)[2]


def blender_zip_url():
    """Resolve the newest blender-X.Y.Z-windows-x64.zip from
    download.blender.org/release/ (verified naming)."""
    html = http_get("https://download.blender.org/release/").decode(
        "utf-8", "replace")
    release_dir = blender_listing(html)
    if not release_dir:
        return None
    inner = http_get(
        "https://download.blender.org/release/%s/" % release_dir).decode(
        "utf-8", "replace")
    zips = re.findall(
        r'href="(blender-\d+\.\d+\.\d+-windows-x64\.zip)"', inner)
    if not zips:
        return None
    newest = max(zips)
    return "https://download.blender.org/release/%s/%s" % (release_dir, newest)


def ensure_blender(cfg, check_only, toolbox):
    exe = find_blender_exe(toolbox)
    if exe:
        return step("R2", "Blender", "ok", "Blender found at %s" % exe)
    if check_only:
        return step("R2", "Blender", "missing",
                    "Blender not detected (installed only via "
                    "`--install blender` AFTER asking the user)")
    eprint("  R2 reminder: this branch may only run after the user answered "
           "YES to: \"%s\"" % R2_QUESTION)
    url = blender_zip_url()
    if not url:
        return step("R2", "Blender", "error",
                    "could not resolve a windows-x64 zip from "
                    "download.blender.org/release/")
    zip_path = toolbox / "_downloads" / url.rsplit("/", 1)[-1]
    try:
        download_file(url, zip_path, min_bytes=10000000)
        extract_and_place(zip_path, toolbox / "blender")
    except (OSError, zipfile.BadZipFile) as exc:
        return step("R2", "Blender", "error", "download/extract failed: %s" % exc)
    exe = find_blender_exe(toolbox)
    if not exe:
        return step("R2", "Blender", "partial",
                    "downloaded but blender.exe not found under %s" %
                    (toolbox / "blender"))
    return step("R2", "Blender", "installed", "portable Blender at %s" % exe)


def ensure_blender_mcp(cfg, check_only, toolbox):
    eprint("  R2 reminder: this branch may only run after the user answered "
           "YES to: \"%s\"" % R2_QUESTION)
    addon = toolbox / "blender-mcp" / "addon.py"
    actions = []
    if not addon.exists():
        if check_only:
            return step("R2", "Blender-MCP", "missing",
                        "Toolbox/blender-mcp/addon.py missing (installed "
                        "only via `--install blender-mcp`)")
        try:
            download_file(BLENDER_ADDON_URL, addon, min_bytes=1000)
        except OSError as exc:
            return step("R2", "Blender-MCP", "error",
                        "addon download failed: %s" % exc)
        actions.append("addon.py downloaded into Toolbox")
    else:
        actions.append("addon.py already present in Toolbox")

    if shutil.which("uvx") is None and shutil.which("uv") is None:
        actions.append("uv/uvx NOT found - install it for the MCP server "
                       "part (`winget install --id=astral-sh.uv -e` or "
                       "`powershell -ExecutionPolicy ByPass -c \"irm "
                       "https://astral.sh/uv/install.ps1 | iex\"`)")
        status = "partial"
    else:
        actions.append("uv/uvx found; the MCP server runs via "
                       "`uvx mcp-for-blender` (stdio)")
        status = "installed" if find_blender_exe(toolbox) else "partial"
    actions.append("MANUAL STEP: Blender -> Edit -> Preferences -> Add-ons "
                   "-> Install from Disk... -> select %s, enable 'MCP for "
                   "Blender', start the server in the N-Panel" % addon)
    if status == "installed" and not check_only:
        entry = mcp_entry(cfg, "blender", DEFAULT_PORTS["blender"])
        entry["installed"] = True
    elif not check_only:
        mcp_entry(cfg, "blender", DEFAULT_PORTS["blender"])
    return step("R2", "Blender-MCP", status,
                "MCP-for-Blender addon + uvx server setup", actions)


def step_r2_status(cfg, check_only, toolbox):
    blender_exe = find_blender_exe(toolbox)
    addon = toolbox / "blender-mcp" / "addon.py"
    uvx = shutil.which("uvx") or shutil.which("uv")
    if blender_exe and addon.exists():
        status = "ok"
    elif blender_exe or addon.exists():
        status = "partial"
    else:
        status = "missing"
    detail = ("Blender: %s; addon: %s; uv/uvx: %s" %
              (blender_exe or "not found",
               str(addon) if addon.exists() else "not found",
               "found" if uvx else "not found"))
    actions = []
    if status != "ok":
        actions.append("ask the user first: \"%s\" - only on YES run "
                       "`python scripts/setup_toolbox.py --install blender` "
                       "and/or `--install blender-mcp`" % R2_QUESTION)
    if not check_only:
        entry = mcp_entry(cfg, "blender", DEFAULT_PORTS["blender"])
        if status == "ok":
            entry["installed"] = True
    return step("R2", "Blender/Blender-MCP status", status, detail, actions)


# --------------------------------------------------------------------------
# R3 - x64dbg + x64dbg-mcp-server
# --------------------------------------------------------------------------

X64DBG_ZIP_PREDICATE = lambda name: (name.startswith("snapshot_") and
                                     name.endswith(".zip"))
X64DBG_MCP_ZIP_PREDICATE = lambda name: name.endswith(".zip")


def ensure_x64dbg(cfg, check_only, toolbox):
    exe = find_x96dbg(toolbox)
    if exe:
        return step("R3", "x64dbg", "ok", "x64dbg found at %s" % exe)
    if check_only:
        return step("R3", "x64dbg", "missing",
                    "x64dbg not found in Toolbox or system-wide (would be "
                    "downloaded automatically in a default run, ~32 MB)")
    url = github_latest_asset("x64dbg/x64dbg", X64DBG_ZIP_PREDICATE)
    if not url:
        return step("R3", "x64dbg", "error",
                    "could not resolve a snapshot zip from the x64dbg/x64dbg "
                    "latest release")
    zip_path = toolbox / "_downloads" / url.rsplit("/", 1)[-1]
    try:
        download_file(url, zip_path, min_bytes=1000000)
        extract_and_place(zip_path, toolbox / "x64dbg")
    except (OSError, zipfile.BadZipFile) as exc:
        return step("R3", "x64dbg", "error", "download/extract failed: %s" % exc)
    exe = find_x96dbg(toolbox)
    if not exe:
        return step("R3", "x64dbg", "partial",
                    "downloaded but x96dbg.exe not found under %s" %
                    (toolbox / "x64dbg"))
    return step("R3", "x64dbg", "installed",
                "portable x64dbg at %s (x96dbg.exe launcher; x32dbg.exe for "
                "the 32-bit debugger, which is the one Crazy Taxi 1 needs)" %
                exe)


def ensure_x64dbg_mcp(cfg, check_only, toolbox):
    detail, actions = "x64dbg-mcp-server (duty1g) plugin", []
    mcp_root = toolbox / "x64dbg-mcp"
    plugin_files = sorted(mcp_root.rglob("*.dp32")) + \
        sorted(mcp_root.rglob("*.dp64"))
    if not plugin_files:
        if check_only:
            return step("R3", "x64dbg-mcp-server", "missing",
                        "Toolbox/x64dbg-mcp has no plugin files (installed "
                        "in a default run or via `--install x64dbg-mcp`)")
        url = github_latest_asset("duty1g/x64dbg-mcp-server",
                                  X64DBG_MCP_ZIP_PREDICATE)
        if not url:
            return step("R3", "x64dbg-mcp-server", "error",
                        "could not resolve a release zip from "
                        "duty1g/x64dbg-mcp-server")
        zip_path = toolbox / "_downloads" / url.rsplit("/", 1)[-1]
        try:
            download_file(url, zip_path, min_bytes=10000)
            extract_and_place(zip_path, mcp_root)
        except (OSError, zipfile.BadZipFile) as exc:
            return step("R3", "x64dbg-mcp-server", "error",
                        "download/extract failed: %s" % exc)
        plugin_files = sorted(mcp_root.rglob("*.dp32")) + \
            sorted(mcp_root.rglob("*.dp64"))
        actions.append("downloaded x64dbg-mcp-server release into Toolbox")
    else:
        detail += "; Toolbox copy present"

    if not check_only:
        # Deploy the plugin files into the matching arch folders of the
        # x64dbg root (layout-agnostic: the plugin's parent.parent folder
        # name decides x32 vs x64).
        dbg_root_exe = find_x96dbg(toolbox)
        if not dbg_root_exe:
            actions.append("x64dbg is not in the Toolbox yet; run "
                           "`--install x64dbg` first, then re-run this step "
                           "to deploy the plugin files")
            mcp_entry(cfg, "x64dbg", DEFAULT_PORTS["x64dbg"])
            return step("R3", "x64dbg-mcp-server", "partial", detail, actions)
        dbg_root = Path(dbg_root_exe).parent
        deployed = 0
        for plugin_file in plugin_files:
            arch_dir = plugin_file.parent.parent.name.lower()
            if arch_dir not in ("x32", "x64"):
                continue
            target = dbg_root / arch_dir / "plugins"
            target.mkdir(parents=True, exist_ok=True)
            shutil.copy2(str(plugin_file), str(target / plugin_file.name))
            deployed += 1
        if deployed:
            actions.append("deployed %d plugin file(s) into %s" %
                           (deployed, dbg_root))
            actions.append("start x32dbg once; then Plugins > x64dbg-MCP "
                           "Server > Configure MCP Server... shows the "
                           "auto-generated Bearer token (server listens on "
                           "http://localhost:9094/ for x64, 9095 for x32)")
            entry = mcp_entry(cfg, "x64dbg", DEFAULT_PORTS["x64dbg"])
            entry["installed"] = True
            status = "installed"
        else:
            actions.append("no deployable .dp32/.dp64 plugin files found "
                           "under %s - inspect the release layout manually"
                           % mcp_root)
            status = "partial"
        return step("R3", "x64dbg-mcp-server", status, detail, actions)
    return step("R3", "x64dbg-mcp-server", "ok",
                detail + "; %d plugin file(s) present" % len(plugin_files))


# --------------------------------------------------------------------------
# R4 - game location (delegates to find_game.py)
# --------------------------------------------------------------------------

def framework_loader_state(game_dir):
    """Presence check for Lyra's Framework Mod (SUBMOD_DEVELOPMENT_GUIDE.md
    ch. 1/6): the dinput8.dll proxy loader in the game root and the host mod
    mods/Framework_MOD/Framework_MOD.dll. Without them a deployed sub-mod is
    never loaded (no Shift+F1 menu)."""
    return (os.path.isfile(os.path.join(game_dir, "dinput8.dll")),
            os.path.isfile(os.path.join(game_dir, "mods", "Framework_MOD",
                                        "Framework_MOD.dll")))


def loader_detail_actions(game_dir):
    has_loader, has_framework = framework_loader_state(game_dir)
    detail = ("; framework loader: dinput8.dll %s, "
              "mods/Framework_MOD/Framework_MOD.dll %s" %
              ("present" if has_loader else "MISSING",
               "present" if has_framework else "MISSING"))
    actions = []
    if not (has_loader and has_framework):
        actions.append(
            "Lyra's Framework Mod loader is incomplete in the game dir - a "
            "deployed sub-mod will NOT load (no Shift+F1 menu, so a 'broken "
            "mod' may really be a missing loader). Install the Framework "
            "Mod first: python scripts/check_framework.py --fix downloads "
            "and installs the verified reference build (consent gate R15 - "
            "it extracts the full archive into the game folder, overwriting "
            "existing files), or build it from Examples/Standalone MOD "
            "(C++ mandatory, 32-bit x86). Both flows are documented in "
            "references/framework-mod.md sections 4-5; with no user "
            "reachable, record this as an open question (R15 non-interactive "
            "fallback) and do not deploy sub-mods.")
    return detail, actions


def step_game(cfg, check_only):
    sys.path.insert(0, str(SCRIPT_DIR))
    try:
        import find_game
    except ImportError as exc:
        return step("R4", "game location", "error",
                    "cannot import find_game.py: %s" % exc)
    stored = cfg.get("crazyTaxiExe")
    if stored and os.path.isfile(stored):
        detail, actions = loader_detail_actions(os.path.dirname(stored))
        return step("R4", "game location", "ok",
                    "Crazy Taxi at %s (from config)%s" % (stored, detail),
                    actions)
    candidates = find_game.search_all()
    confirmed = [c for c in candidates
                 if os.path.basename(c["path"]).lower()
                 in find_game.KNOWN_EXE_NAMES
                 and find_game.is_32bit_pe(c["path"])]
    if confirmed:
        pick = confirmed[0]
        if not check_only:
            cfg["crazyTaxiExe"] = pick["path"]
            cfg["gameDir"] = os.path.dirname(pick["path"])
        detail, actions = loader_detail_actions(os.path.dirname(pick["path"]))
        return step("R4", "game location", "ok",
                    "found at %s (source: %s)%s%s" %
                    (pick["path"], pick["source"],
                     "" if check_only else "; persisted to config", detail),
                    actions)
    return step("R4", "game location", "missing",
                "Crazy Taxi.exe not found. Ask the user for the install "
                "path and store it in config/environment.json "
                "(crazyTaxiExe + gameDir), or let the user point at the "
                "folder. Loose candidates: %s" %
                (", ".join(c["path"] for c in candidates[:5]) or "none"),
                ["ask the user for the Crazy Taxi install path"])


# --------------------------------------------------------------------------
# R5 - C++ / Rust toolchain detection
# --------------------------------------------------------------------------

VSWHERE = r"C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"


def detect_toolchain():
    toolchain = {
        "cpp": {"cl": None, "clNote": None, "gcc": None, "clang": None},
        "rust": {"cargo": None, "rustc": None},
    }
    cl = shutil.which("cl")
    if cl:
        toolchain["cpp"]["cl"] = cl
    elif os.path.isfile(VSWHERE):
        code, out, _ = run_cmd([VSWHERE, "-latest", "-products", "*",
                                "-requires",
                                "Microsoft.VisualStudio.Component.VC.Tools."
                                "x86.x64",
                                "-property", "installationPath"], timeout=60)
        if code == 0 and out:
            toolchain["cpp"]["cl"] = out
            toolchain["cpp"]["clNote"] = (
                "cl.exe available through this Visual Studio installation; "
                "run its vcvarsall.bat x86 first (see "
                "SUBMOD_DEVELOPMENT_GUIDE.md chapter 5)")
    gcc = shutil.which("gcc")
    if gcc:
        toolchain["cpp"]["gcc"] = gcc
    clang = shutil.which("clang")
    if clang:
        toolchain["cpp"]["clang"] = clang
    cargo = shutil.which("cargo")
    if cargo:
        toolchain["rust"]["cargo"] = cargo
    rustc = shutil.which("rustc")
    if rustc:
        toolchain["rust"]["rustc"] = rustc
    cpp_ok = any([toolchain["cpp"]["cl"], toolchain["cpp"]["gcc"],
                  toolchain["cpp"]["clang"]])
    rust_ok = any([toolchain["rust"]["cargo"], toolchain["rust"]["rustc"]])
    # R10: mods in C++ or Rust depending on what is installed; prefer C++.
    preferred = "cpp" if cpp_ok else ("rust" if rust_ok else None)
    return toolchain, preferred, cpp_ok, rust_ok


def step_toolchain(cfg, check_only):
    toolchain, preferred, cpp_ok, rust_ok = detect_toolchain()
    if not check_only:
        cfg["toolchain"] = toolchain
        cfg["preferredCompiler"] = preferred
    if cpp_ok:
        return step("R5", "toolchain", "ok",
                    "C++ toolchain available (preferred); rust=%s; stored in "
                    "config" % ("yes" if rust_ok else "no"))
    if rust_ok:
        return step("R5", "toolchain", "partial",
                    "only Rust available; mods will be built in Rust "
                    "(C++ preferred per R10); stored in config",
                    ["consider installing Visual Studio Build Tools (C++ "
                     "workload) for the preferred C++ route"])
    return step("R5", "toolchain", "missing",
                "no C++ compiler (cl/gcc/clang) and no Rust toolchain "
                "(cargo/rustc) found. Install Visual Studio Build Tools "
                "(C++ workload, needed for 32-bit DLL mods) or Rust "
                "(https://rustup.rs), then re-run this script.",
                ["ask the user which toolchain they want to install"])


# --------------------------------------------------------------------------
# main
# --------------------------------------------------------------------------

def summarize():
    counts = {}
    for entry in STEPS:
        counts[entry["status"]] = counts.get(entry["status"], 0) + 1
    return counts


def exit_code_for(check_only):
    """0 = ok. Fail on errors; on missing/partial steps except R2
    (Blender is ask-first/optional by design). With --check-only any
    missing/partial step counts as 'needs attention'."""
    for entry in STEPS:
        if entry["status"] == "error":
            return 1
    for entry in STEPS:
        if entry["status"] in ("missing", "partial") and entry["id"] != "R2":
            return 1
    if check_only and any(entry["status"] in ("missing", "partial")
                          for entry in STEPS):
        return 1
    return 0


def main():
    parser = argparse.ArgumentParser(
        description="One-entry toolbox setup for the Crazy Taxi 1 modding "
                    "skill (R0-R5, R7 reporting).")
    parser.add_argument("--check-only", action="store_true",
                        help="report status only; no downloads, installs or "
                             "config writes")
    parser.add_argument("--json", action="store_true",
                        help="print a machine-readable JSON report to stdout")
    parser.add_argument("--install", choices=[
        "ida-mcp", "cutter", "cutter-mcp", "blender", "blender-mcp",
        "x64dbg", "x64dbg-mcp"], default=None,
        help="run only this installation branch")
    args = parser.parse_args()

    cfg = load_config()
    toolbox = toolbox_dir(cfg)
    eprint("skill root: %s" % SKILL_ROOT)
    eprint("toolbox:     %s" % toolbox)

    try:
        step_python(args.check_only)

        if args.install == "ida-mcp":
            install_ida_mcp(cfg, args.check_only, toolbox)
        elif args.install == "cutter":
            ensure_cutter(cfg, args.check_only, toolbox)
        elif args.install == "cutter-mcp":
            ensure_cuttermcp(cfg, args.check_only, toolbox)
        elif args.install == "blender":
            ensure_blender(cfg, args.check_only, toolbox)
        elif args.install == "blender-mcp":
            ensure_blender_mcp(cfg, args.check_only, toolbox)
        elif args.install == "x64dbg":
            ensure_x64dbg(cfg, args.check_only, toolbox)
            ensure_x64dbg_mcp(cfg, args.check_only, toolbox)
        elif args.install == "x64dbg-mcp":
            ensure_x64dbg_mcp(cfg, args.check_only, toolbox)
        else:
            # Full default setup run (R0-R5, minus the R2 install branch).
            eprint("default run: R1 (IDA-or-Cutter) + R3 (x64dbg) + R4 (game)"
                   " + R5 (toolchain); R2 Blender is ask-first and stays "
                   "status-only")
            step_r1(cfg, args.check_only, toolbox)
            step_r2_status(cfg, args.check_only, toolbox)
            ensure_x64dbg(cfg, args.check_only, toolbox)
            ensure_x64dbg_mcp(cfg, args.check_only, toolbox)
            step_game(cfg, args.check_only)
            step_toolchain(cfg, args.check_only)

        ports = check_ports(cfg)
        hard_collision = any(p["collision"] for p in ports)
        step("R7", "port check",
             "error" if hard_collision else "ok",
             "; ".join("%s:%d %s" % (p["server"], p["port"],
                                     ("free" if not p["listening"] else
                                      ("owner=%s" % p["owner"] if p["owner"]
                                       else "listening, owner unknown")))
                       for p in ports),
             [p["remedy"] for p in ports if p["collision"]])

        config_written = False
        if not args.check_only:
            save_config(cfg)
            config_written = True

        code = exit_code_for(args.check_only)
        if hard_collision:
            code = 1

        if args.json:
            print(json.dumps({
                "tool": "setup_toolbox.py",
                "arguments": {"check_only": args.check_only,
                              "install": args.install},
                "skillRoot": str(SKILL_ROOT),
                "toolbox": str(toolbox),
                "python": {"version": "%d.%d.%d" % sys.version_info[:3],
                           "executable": sys.executable},
                "steps": STEPS,
                "ports": ports,
                "mcpServers": cfg.get("mcpServers"),
                "configWritten": config_written,
                "exitCode": code,
            }))
        else:
            eprint("")
            print("Setup %s (exit code %d)" %
                  ("finished" if code == 0 else "needs attention", code))
            for entry in STEPS:
                print("  [%s] %-28s %s" % (entry["id"], entry["name"],
                                           entry["status"]))
            if any(entry["actions"] for entry in STEPS):
                print("Follow-up actions:")
                for entry in STEPS:
                    for action in entry["actions"]:
                        print("  - %s" % action)
        return code
    except SystemExit:
        raise
    except Exception as exc:  # pragma: no cover - defensive
        eprint("UNEXPECTED ERROR: %r" % exc)
        if args.json:
            print(json.dumps({"tool": "setup_toolbox.py", "steps": STEPS,
                              "exitCode": 2,
                              "error": repr(exc)}))
        return 2


if __name__ == "__main__":
    sys.exit(main())
