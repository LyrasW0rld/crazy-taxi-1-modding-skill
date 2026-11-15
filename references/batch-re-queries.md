# Parallel & Batch RE Queries (IDA Pro / Cutter)

Concurrent reverse-engineering query protocol for sessions with **multiple agents
(preference: batch queue for any session with more than one agent; tested with 10
parallel agents)**. It exists because naive parallel RE fails in three specific ways:

1. **Request storms** — each agent hits the same MCP server directly. The Cutter
   plugin is single-instance with a hardcoded port (8000, see `mcp-servers.md`), so N
   agents queue on one process or collide; ida-pro-mcp auto-increments its port per
   instance, so a fixed URL can silently hit the WRONG IDA instance. Result: timeouts,
   "too many requests", wrong-binary answers.
2. **Write conflicts** — two agents writing one `.i64`/`.idb`, renaming the same
   function, or editing `offset_list.json` concurrently corrupt state silently.
3. **Lost knowledge** — verdicts live in dead agent contexts instead of the files
   (R16 update duty). Re-running an RE session then re-discovers the same facts.

**Golden rule: one writer per resource.** The IDB has one writer (a batch worker OR a
live GUI/MCP session, never both). `offset_list.json` has one writer (the
coordinator). Every agent else communicates through files with unique names and
atomic writes.

## Mode selection

| Situation | Mode |
| --- | --- |
| More than one agent needs RE data | **batch queue** (this file) |
| Single agent, few addresses, MCP server already verified running | direct MCP call is acceptable |
| Follow-up on one batch result (e.g. "decompile the caller at 0x...") | queue it as a new request - the queue owner answers it; do NOT call the MCP server directly unless you ARE the owner |
| Any state change (rename, patch, offset_list.json edit) | coordinator only — agents never write shared files |

## Queue layout (session work directory)

Root: `<gameDir>/re_queue/<YYYYMMDD>-<topic>/`. **Always inside the game
directory, never inside the skill** (no Python scripts, no IDA databases in the
skill; tidy workspace R17). Create per session:

```text
re_queue/<session>/
  requests/     one JSON file per query, written by agents
  responses/    one JSON file per answered query, written by the worker
  verdicts/     one JSON file per agent: that agent's verified findings
  issues/       one NDJSON log per agent: unanswered queries, worker deaths
  worker.log    append-only log of the worker passes (liveness signal)
```

Session cleanup (R17): the COORDINATOR deletes the whole session dir at session end
AFTER verdicts are merged into the durable files (`offset_list.json`,
`verified_knowledge.json`, `fundamentals.md`) - including the `.i64`/`work.i64`
(the ~80 MB database is reproducible from the exe) and any `*.tmp` strays.

## Request schema (`requests/<agentId>-<seq>-<kind>.json`)

```json
{
  "query_id": "a3f1c2",                    // 6-hex idempotency key: first 6 chars of
                                           // sha256(f"{kind}|{addr}|{json.dumps(extra, sort_keys=True)}")
  "agent": "agent-04",
  "kind": "decompile",                     // see kind table
  "addr": "0x00492230",                    // absolute virtual address, base 0x00400000
  "extra": {"window": 64},                 // optional per-kind parameters ({} default)
  "created": "2026-11-14T17:12:00Z"        // ISO-8601, informational
}
```

`status` assignment rules for responses: `ok` = answered; `unmapped` = address has no
segment (runtime/file-offset addresses); `failed` = address mapped but the worker
could not produce the result (decompile exception, disasm failure); `rejected` =
malformed request (bad JSON, unknown kind, missing addr) or `*.tmp` leftover.

| `kind` | Returns | Notes |
| --- | --- | --- |
| `disasm` | instruction window around `addr` (target line marked) | `extra.window` bytes context, default 80 |
| `decompile` | Hex-Rays pseudocode of the function containing `addr` | batch mode needs Hex-Rays license; falls back to `disasm` |
| `function` | boundaries + name of the function containing `addr` | cheap, use for boundary checks |
| `xrefs_to` / `xrefs_from` | cross-reference lists (with function names) | cap 40 each |
| `data` | raw bytes + dword/float/string interpretation | for globals and `.rdata` constants |
| `name` | current name + segment + flags of `addr` | rename conflicts show up here |

## Response schema (`responses/<query_id>.response.json`)

```json
{
  "query_id": "a3f1c2",
  "status": "ok",                          // ok | unmapped | failed | rejected
  "error": null,                           // human-readable reason when not ok
  "worker": "idat-batch/9.1",              // which worker answered
  "result": { "...kind-specific payload..." },
  "answered": "2026-11-14T17:14:30Z"
}
```

## Atomic write rules (the anti-corruption core)

- **One query = one file = one atomic write.** Write to `<name>.tmp`, then rename to
  the final name. Never append to a shared file, never write in place.
- **Namespaces:** every file a component creates carries its id (`agent-04-001-decompile.json`).
  Two components can never pick the same name.
- **Idempotency:** `query_id` = hash of `kind + addr + extra`. A duplicate request
  (same id) gets the same answer from the existing response file — the worker skips it.
- **Nobody writes into another component's directory.** Agents → `requests/` and
  `verdicts/`; worker → `responses/` and `worker.log`.

## Worker modes

### Mode A - headless batch (preferred, default)

One `idat.exe` pass answers a whole batch in a single auto-analysis - no shared live
instance, no request rate to manage, reproducible. Two shapes, same mechanics:

- **Target-list shape (simplest, proven):** the coordinator writes ONE
  `ida_targets.json` (hex address strings); the worker dumps evidence for every
  target into one `ida_extract.json` (disasm window with the target line marked,
  Hex-Rays pseudocode, xrefs, raw dwords/floats/string) plus a log. This is exactly
  how the 2026-11-14 offset audit ran; its scripts stay as reference implementations
  in `<gameDir>/ida_audit/` (`ida_batch_extract.py`, `split_verdict_inputs.py`,
  `merge_verdicts.py`, `audit_offset_list.py`) - regenerate them in the session dir.
- **Queue shape (normative):** the worker reads every `requests/*.json`, dedups by
  `query_id`, and writes one `responses/<query_id>.response.json` per query.

```text
idat.exe -A -o"<session>\work.i64" -S"<session>\batch_worker.py" "<gameDir>\Crazy Taxi.exe"
```

The batch worker script (IDAPython, generated per session into the session dir) must:

1. FORCE analysis: `ida_auto.enable_auto(True)` + `ida_auto.plan_and_wait(seg.start,
   seg.end, True)` per segment - in batch mode the analyzer may never be scheduled on
   its own (plain `auto_wait()` returns instantly on an unanalyzed database; the
   2026-11-14 audit needed three runs to learn this);
2. sort queries by address and decode ONCE per function - 10 agents asking about
   addresses in one function cost ONE decompile;
3. write each response atomically into `responses/` (temp + rename), skipping
   `*.tmp` files and malformed JSON (answer them `status: "rejected"`);
4. append a one-line summary per query to `worker.log`, then exit via
   `ida_pro.qexit(0)`.

Expected latency: minutes, not seconds - a fresh auto-analysis of Crazy Taxi.exe
(17 MB, ~4200 functions) took ~9 minutes. Agents size their poll budget for this
(see Agent loop).

### Mode B - live MCP (only when a verified server is already running)

If a batch pass is impossible (e.g. follow-up questions on an existing GUI session),
one component — the queue owner — is the ONLY caller of the MCP server:

- **≤ 1 in-flight request per MCP server.** Every other agent still goes through the
  file queue; the owner drains it serially.
- **Batch inside the call:** collect queued queries and answer them with the fewest
  MCP calls (one `decompile` per function, one `xrefs` per address — never per-agent
  per-address call storms).
- **Verify the instance before trusting answers:** ida-pro-mcp — read
  `server_health` / `idb_path` and confirm the loaded binary is Crazy Taxi.exe (the
  port may have auto-incremented, `mcp-servers.md` §1); cuttermcp — confirm the
  plugin panel shows the right binary and note the single-instance guard.

## Agent loop (pseudocode, poll with backoff + liveness)

```text
write request file atomically (never leave *.tmp behind - the worker skips them)
poll responses/<query_id>.response.json
  backoff 2s, 4s, 8s, 15s, 30s, then every 60s up to 25 minutes total
  (a Mode A pass over a fresh database takes ~10 minutes)
liveness check on every miss: does worker.log grow?
  - log grows -> keep waiting (worker is alive, your answer is coming)
  - log static for 3+ checks AND the last line is a completion line
    but your response is missing -> the worker finished without answering
    your query: re-send ONCE with the same query_id, then escalate
  - log static AND no completion line -> worker died: report to the
    coordinator, who re-runs the worker (safe, idempotent by query_id)
give-up AFTER the budget: append {"query_id": "...", "problem": "no response",
"worker_state": "<what worker.log showed>"} as one JSON line into
issues/<agentId>.log (NDJSON, the coordinator's consumer) and continue
WITHOUT the data - never re-send in a tight loop, never block the session
```

The `issues/` files are read by the COORDINATOR at merge time; unresolved issues
become `flag` verdicts (insufficient evidence) instead of silent gaps.

## Verdicts and the coordinator (state changes)

Agents NEVER edit `offset_list.json`, `references/*.md`, or the IDB. The flow:

1. The coordinator splits the audit findings into per-agent input files
   (`split_verdict_inputs.py` shape: groups + only the IDA evidence those groups
   need) - agents never read the full 800 KB database.
2. Each agent writes `verdicts/<agentId>.verdict.json` (UTF-8). The schema below was introduced for the 2026-11-14 audit, whose merge tool parsed exactly this shape; each session regenerates its own merge tool and may adapt the schema - keep these fields where practical:

```json
{
  "agent": "agent-1",
  "groups": [{
    "group_id": "G001",
    "classification": "<true-duplicate|struct-field-annotations|alias-entries|contradiction|heap-runtime-only|file-offset|rename-needed|other>",
    "summary": "one sentence",
    "verdicts": [{
      "offset_list_index": 302,          // array position HINT; identifier+address is the real key
      "identifier": "adx_stream_get_chunk_size",
      "action": "keep|correct|merge-into|delete|flag",
      "merge_target": null,              // offset_list_index of the KEPT entry (merge-into only)
      "merge_payload": {"search_tags_add": "", "notes_add": ""},
      "corrected": {"data_type": "full new string value"},   // correct only, 9-field keyset only
      "rationale": "1-3 sentences",
      "evidence": "quoted disasm/decompile/data lines",
      "confidence": "high|medium|low"
    }]
  }]
}
```

   One verdict object per affected entry. `merge-into` folds this entry into
   `merge_target`; `delete` removes it; `flag` = insufficient evidence.
3. The **coordinator** (single writer) merges: resolves entries by
   identifier + address (the index is a hint), refuses conflicting merges, applies
   corrections, appends merge payloads idempotently (marker string), then re-runs
   the deterministic audit (`audit_offset_list.py` - zero unexpected drift expected)
   and reads `issues/*.log` into `flag` verdicts.
4. Non-address knowledge (tool quirks, binary behavior, version facts) goes to
   `references/verified_knowledge.json` per `verified-knowledge.md` - the ledger,
   not the address DB.
5. The session dir is deleted at cleanup (see "Queue layout"); the durable record is
   `offset_list.json` + `verified_knowledge.json` + `fundamentals.md`.

## Tool-specific constraints (from `mcp-servers.md`, summarized)

| Server | Constraint the queue solves |
| --- | --- |
| ida-pro-mcp (13337+) | multi-instance port hopping — a fixed URL can hit the wrong instance; verify `server_health.idb_path` |
| cuttermcp (8000) | hardcoded port, single-instance guard, plugin serializes — 10 agents would stack up one stream |
| x64dbg-mcp (9094/9095) | debugger state is global — dynamic-analysis requests always serialize through one owner, no parallel breakpoints |
| blender-mcp (9876) | one server per Blender instance — same queue pattern applies for scripted asset queries |
