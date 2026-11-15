# Mod Logging - per-run log files with rotation (R23)

EVERY mod (submod AND standalone) embeds file logging. The log is the primary
debugging surface: when a mod misbehaves, the AI reads the newest log FIRST (before
x64dbg, before guessing). Requirement set by the user (2026-11-14).

## Hard requirements

1. **One NEW log file per game run.** Create the file at mod init (DLL_PROCESS_ATTACH
   / sub-mod `InitSubMod`), named `<gameDir>/mods/<modname>/logs/run_YYYYMMDD_HHMMSS.log`
   (local time, resolved from the DLL's own directory - relative paths only, R14).
2. **Rotation - never fill the disk:** after creating the new file, if the `logs/`
   folder holds more than **5** files, delete the OLDEST until exactly 5 remain
   (newest included). Total footprint is bounded (~5 files).
3. **Never overflow a single log (do not flood it):**
   - Log levels: ERROR, WARN, INFO (default), DEBUG (only when explicitly
     troubleshooting, via config flag).
   - FORBIDDEN in hot loops (per-frame, per-tick): no per-frame lines. Throttle
     repeated identical messages - same message logged once per run or at most once
     per 10 s (suppress counter in the message: `... (x87 more suppressed)`).
   - Soft cap: 2 MB per file; beyond it, only ERROR/WARN lines are still written.
4. **Content that must always be there:** mod + version, config snapshot at init,
   every hook init result (MinHook MH_CreateHook/MH_EnableHook status, naked-hook
   byte checks), device/pointer chain resolution results, every memory write target
   with VirtualProtect result, SEH/UTD crash handler output (exception code,
   address, registers - the `Examples/Standalone MOD/logger.*` crash-dump pattern),
   and clean shutdown steps.
5. **Crash-safe logging:** the logger itself must never crash the game - mutex-
   protected appends, no allocation in the crash path, flush after every ERROR/WARN
   (a hard crash must leave the log readable up to the last line).
6. **Retrievable:** the AI reads `<newest> run_*.log` in the mod's `logs/` folder
   after any run or crash. Submods may additionally surface the newest log lines in
   the Shift+F1 menu, but the file is the source of truth.

## Implementation

- Pattern reference: `Examples/Standalone MOD/logger.h`/`logger.cpp` (logging +
  x86 crash dump). Reuse the pattern in the mod's OWN code and add the per-run file
  creation + 5-file rotation on top - do not edit the protected Examples.
- One open file handle per run (append mode); the rotation scan runs once at init
  (before the new file is written), never per line.
- Filename collision (two runs within one second): append `_1`, `_2`, ... before
  `.log` instead of overwriting.

## Debugging flow (how the log is used)

1. Run the game with the mod enabled, reproduce the problem.
2. Read the newest `run_*.log` - look for the last INFO line before the first ERROR
   (that boundary localizes the failing step).
3. Game/mod HARD crash (process gone, no shutdown lines): triage with R22 first -
   `Get-CimInstance -ClassName Win32_ReliabilityRecords` names the faulting module
   and exception code - then correlate with the newest log (which init step was the
   last to complete?).
4. Only then escalate to x64dbg (modding-workflow.md Step 2) with the log facts in
   hand: breakpoint where the log says the failure begins.
