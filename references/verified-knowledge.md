# Verified Knowledge Ledger (`references/verified_knowledge.json`)

Durable store for **fundamentally verified** RE knowledge that is neither an address
entry (`offset_list.json`) nor an action rule (`fundamentals.md`). Purpose: verified
findings must not die with the session that produced them - re-deriving the same fact
in a later session is wasted RE work and re-introduces avoidable errors.

## Routing: where a finding goes

| Knowledge type | Destination |
| --- | --- |
| A memory address, function, variable, struct layout (one entry's fact) | `offset_list.json` (9-field entry) |
| A recurring action rule ("always X before Y" when modding) | `references/fundamentals.md` (B-rules, with evidence) |
| Verified facts that do not map to ONE address: binary behavior, tool quirks, version differences, format details, mislabeling verdicts, cross-entry contradiction resolutions - INCLUDING verdict rationale that cites several entries of one layout | **`references/verified_knowledge.json`** (this ledger) |
| A hunch, single-source guess, "probably" | working notes only - NEVER persisted in any skill file |

Carve-out: a ledger entry may encode a struct layout when its real payload is the
cross-entry verdict (which entries collide, why they are intentional) - the raw
field offsets still live in `offset_list.json` entries.

## Verification tiers

| Tier | Meaning | Qualifies as "fundamentally verified" |
| --- | --- | --- |
| **T1** | one static method (IDA decompile, Cutter, string/xref analysis) | No - `status: "provisional"`, usable as a lead only |
| **T2** | two INDEPENDENT methods (e.g. IDA decompile + Cutter disasm, IDA + Cheat-Engine memory read, static + x64dbg live) | **Yes** |
| **T3** | dynamically confirmed in the running game (breakpoint hit with expected state, memory write visibly changes behavior) | **Yes** |

Independent = different tool OR different evidence class (static vs. dynamic). A
decompile plus the disassembly of the same IDA database is ONE method. A qualifying
second method is also **session replication**: a fact recorded in a prior session's
RE-audit notes (with concrete byte/xref claims) that a NEW session re-derives
byte-exactly with its own tools counts as two methods - the claim was made by an
independent analysis and survived re-derivation.

**Build gate:** a mod may build on a T2/T3 ledger entry as-is. Building on T1
requires re-verification first (then update the entry to T2 before proceeding).

## Entry schema (plain JSON array, same style as offset_list.json)

```json
{
  "id": "VKK-001",
  "date": "2026-11-14",
  "claim": "one precise sentence - what is true about the binary/tool/format",
  "evidence": "quoted disasm/decompile/observation + where it came from",
  "verification": {
    "tier": 2,
    "methods": ["ida-decompile", "cheat-engine-live"],
    "session": "2026-11-14-offset-audit"
  },
  "confidence": "high",
  "related": [
    {"offset_list_index": 12, "identifier": "ADDR_GAME_TIMER", "address": "0x016A41A4"}
  ],
  "status": "active",
  "supersedes": null,
  "superseded_by": null,
  "tags": ["camera", "physics"]
}
```

- `status`: `active` | `provisional` (T1) | `superseded` | `contradicted`.
- `related` entries must exist in `offset_list.json`; cite `identifier` + `address`
  (range-form addresses are cited by their start address). The array index is
  optional and fragile - merges shift positions - so treat identifier + address as
  the key.
- Values are strings, numbers, null, or the documented arrays (`verification.methods`,
  `tags`, `related` objects); no booleans; keep the keyset stable - consumers parse it.
- A claim must be ONE precise, falsifiable sentence. If an entry mixes certainty
  levels (e.g. verified accessors + unverified default values), put the uncertain
  part into its own sentence and mark it runtime-only/pending - the build gate
  applies per claim, not per entry.

## Update protocol (end of EVERY RE session - same duty as R16)

1. **Search before adding:** grep the ledger for the claim's tags/addresses. Found ->
   update that entry in place (stronger evidence, higher tier), never duplicate.
2. **Append new entries at the end** of the array; ids are stable forever (VKK-###,
   next free number). Do not re-sort.
3. **Contradictions are recorded, not deleted:** old entry -> `status: "contradicted"`,
   `superseded_by: "<new id>"`; new entry carries the counter-evidence. If it
   contradicts a fundamentals.md rule, record it next to that rule too (fundamentals
   protocol) and ask the user when a user-confirmed rule (Section A) is affected.
4. **Promotion:** a ledger claim that keeps guiding mods becomes a fundamentals.md
   B-rule; the ledger entry stays `active` as the evidence pointer.
5. **Single writer:** only the session coordinator edits the ledger (agents write
   verdict files per `batch-re-queries.md`; the coordinator merges).
6. **Consistency sweep:** when `offset_list.json` entries are corrected, re-check the
   ledger entries referencing them in the same session.

## Relationship to the other knowledge files

- `offset_list.json` - the ONLY address source (R8). The ledger never introduces a
  new address on its own; it points at offset entries.
- `references/fundamentals.md` - standing action rules; the ledger is its evidence
  pool and promotion pipeline.
- `references/offset-list-schema.md` - documents the address DB; the audit pipeline
  (`<gameDir>/ida_audit/audit_offset_list.py`, regenerated per session) is the
  mechanical contradiction detector that feeds both files.
