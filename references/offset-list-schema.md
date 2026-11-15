# offset_list.json - Structure & Maintenance Rules

`offset_list.json` (workspace root, ~813 KB, UTF-8) is the curated address database for Crazy Taxi 1 (PC, Win32 x86). It is the **sole source of truth** for memory addresses, functions, variables, and struct layouts. Never invent an address: if a needed entry does not exist, obtain it through RE work (see `modding-workflow.md`) and add it here at the end of that session.

Every fact below was verified programmatically against the current file (entry count, keysets, value types, category and address-form distributions).

**Deduplication state (2026-11-14 IDA audit):** the original 1058-entry file contained 73 same-address collision groups (audit metric: 2+ entries sharing the same first address token) and 4 identifiers repeated across 9 entries. The audit verified every suspect group against IDA Pro 9.1 (headless `idat` batch + 8 parallel verdict agents) and merged 67 redundant entries + corrected 61 fields; the file now has 991 entries with 991 unique identifiers. Discarded names survive as `search_tags`/alias notes inside the kept entries (exception: a discarded name that is itself non-English is not preserved under the all-English owner requirement - the 2026-11-15 rename to `OriginalBrandNames` carries no alias, the discarded name survives only in the pre-sweep backup `offset_list.pre-final-sweep.backup-20261115.json`). The audit pipeline and the verification ledger it fed: `references/verified-knowledge.md`.

## Top-level shape

- A JSON **array** of exactly **991 objects**. There is no wrapper object and there are no top-level keys.
- The array is **not sorted** by anything (135 interleaved category runs). Consumers must filter by the `category` / `search_tags` fields; never rely on position.
- Every entry has the **same 9 fields, in the same order**, and every field value is a **string** (no numbers, arrays, or nesting; 991 x 9 = 8919 string values).

## Entry schema (9 fields, all values strings)

Field order as it appears in the file:

| # | Field | Content |
|---|-------|---------|
| 1 | `category` | Group label; 29 distinct values (list below). Grouping exists ONLY as this field - there is no physical grouping in the file. |
| 2 | `identifier` | Short name. **991 distinct values across 991 entries - identifiers are unique** since the 2026-11-14 audit (former duplicate names were merged; the discarded name lives on in the kept entry's `search_tags` and an "Alias: ..." note; exception: a discarded name that is itself non-English is not preserved under the all-English owner requirement - the 2026-11-15 rename to `OriginalBrandNames` carries no alias). |
| 3 | `memory_address` | Absolute virtual address of the 32-bit process as a hex string, image base `0x00400000`. NOT an RVA and NOT a file offset - the one documented exception is `0x22B090` (a FILE offset into the motDC container, see caveats). 959/991 entries are canonical (`0x` + 8 hex digits); the 32 non-canonical shapes are listed below and are intentional. |
| 4 | `functions` | Free-text purpose / behavior. One entry has an empty string. |
| 5 | `data_type` | Free-text type or calling convention; 168 distinct values. Most common: `Function (__cdecl / native)` 271x, `Function (__cdecl)` 95x, `float` 76x, `__cdecl` 75x, `polDC0.bin Mesh Chunk` 41x, `Function (MMX / SSE / SIMD)` 34x, `uint32_t` 29x, `int32_t` 27x, `SegaContainerTable` 26x, `CRT / D3D Runtime Routine` 20x. `__usercall` appears in 29 entries. |
| 6 | `operational_notes` | Practical usage guidance: calling conventions, guards, pitfalls, related addresses, hook advice. |
| 7 | `subsystem_description` | Which engine subsystem the entry belongs to. |
| 8 | `additional_infos` | Provenance: `Documented in Section 12 ...` (414x), `Part 2 Section N ...`, plus 31 entries with RE-audit notes dated 2026-11-06 (`RE-audit correction ...` 19x, `New (audit ...` 12x, no overlap; originally German, translated to English 2026-11-14). It references companion RE documentation (Section 12.x "Function Matrix", Part 2 sections, `re_*.md`, `out_*.txt`, `tools/*.ts`, `src/*`) that is NOT present in this workspace - treat it as external provenance, not as local files. |
| 9 | `search_tags` | ONE comma-separated string, typically ~8 tags (range 2-25). Usually contains the identifier, the address, the category, and topical keywords. |

## Address notation

Canonical form: `0x` + exactly 8 hex digits (e.g. `0x00492230`) - absolute virtual address with image base `0x00400000`. The remaining **32** entries are non-canonical by design; a parser (and you) must tolerate all of these shapes:

- **Annotated struct field at an absolute base:** `0x01B09938 (+0x04)`, `0x01B7AC00 (+0x18)`
- **Dual / multi-part address:** `X (role) / Y (role)` - no current instance (the former MasterSlotMap entry was merged in the 2026-11-14 audit); the shape stays reserved.
- **Address tuple or range:** `0x0CB40000, 0x0F000000`, `0x004169F0, 0x00418600`, `0x00491F68 etc.`
- **Relative-offset annotation:** `pData 0x0CC33100 (relOffset 0xF3100)`
- **Symbolic struct-offset notation without an absolute base:** `pTexObj (+0x0C)`, `SegaContainerEntry (+0x08)`, bare `+0x34`
- **Format-size descriptors (metadata, not addresses):** `LTM1_HEADER (16 Bytes)`
- **Game file paths (audio tracks, not memory addresses):** `SoundData/music_adx/Black_Radio.adx` and 6 more like it
- **One 6-digit hex form:** `0x22B090`

Address ranges among the 959 canonical addresses: 644 below `0x00800000` (code / `.rdata`: functions and constants), 203 in `0x00800000`-`0x017FFFFF`, 112 at `0x01800000` and above (globals / heap).

## Categories (29, with entry counts)

`Audio (CRI ADX) & Timing` 239 - `Sectors & Streaming` 122 - `Collision & Simulation` 110 - `Function Hooks` 84 - `Textures` 58 - `Axel` 46 - `Game State & Session` 44 - `Vehicle Rotation & Matrix` 32 - `Player Taxi & Physics` 31 - `D3D9 Crash Guards` 29 - `3D Asset` 29 - `Drivetrain & 4WS Dynamics` 25 - `Camera System & Freecam` 22 - `Input & Controls` 21 - `Economy & Tips` 20 - `Dreamcast Restorations` 19 - `Global & Pointers` 15 - `Sega Developer Cheats` 13 - `D3D9 Resolution & IAT Hook` 6 - `Input & Engine Fixes` 5 - `Configuration & Graphics` 5 - `LTM1 / LTMC Binary Formats` 3 - `Vehicle Physics & Movement` 3 - `Destiny Arrow & Navigation` 2 - `Display & Windowing` 2 - `IAT & Windowing` 2 - `Graphics Shaders & Post-Processing` 2 - `IAT & Audio` 1 - `D3D9 Resolution & Presentation` 1

## Example entry (verbatim from the file)

```json
{
  "category": "Function Hooks",
  "identifier": "LoadModelTexture",
  "memory_address": "0x00492230",
  "functions": "LoadModelTexture (0x00492230), LoadModelTexture (0x00492230) -> assets::formats::load_model_texture",
  "data_type": "Function (__usercall: edx = record-table ptr, stack arg = slot index)",
  "operational_notes": "Calling convention: __usercall (Reg: edx = record-table ptr a1, Stack: slot index a2 at [esp+4]). Verified behavior (idalib decompile): per-slot texture upload from emulated Sega VRAM to D3D. Early-out if version dword *(a1+16*a2+8) already matches dword_1B09948[6*slot]; releases old texture object in dword_1B09938[6*slot]; reads width/height/data-addr from the 8-byte record at a1+16*slot; translate_vram_pointer maps emulated VRAM 0x0C30B000-0x0D30B000; decodes PVR data via DecodePvrTexture (0x0048DDB0) and builds a mipmap chain into new 0x74-byte texture objects (sub_4840B0). Special case: VRAM address 0x0CC0A980 (213952896) loads loose file 'NewTexture\\shadow.dds' from disk instead. Xrefs: 0x499292, 0x504F95, 0x505EA0, 0x5277CB. Ideal for injecting custom HD vehicle and character skins. IDA name: ct_LoadModelTexture. NOTE for streaming: Background chunk streaming (process_background_chunk_streaming_loop at 0x00505BD0) sets baseIdx=2000 for stage chunks (texDC1/2/3) and 1500 for vehicles (texDC0), then immediately resets baseIdx to 0 at 0x00505ED6. Any hook capturing baseIdx must snapshot 0x01B7A6E0 BEFORE the engine call, or infer 2000 for stage tables and 1500 for texDC0.",
  "subsystem_description": "Native executable function hook location for rendering, loading, or gameplay.",
  "additional_infos": "Hook point documented in Section 3. Function signature: __cdecl / Custom Register LoadModelTexture(Stack: tableIndex ([esp+4]), Reg: edx = tablePtr).",
  "search_tags": "LoadModelTexture, 0x00492230, Function Hooks, __cdecl Custom Register, detour, native routine, Texture Master Table, assetsformatsload_model_texture, clean-room rebuild, function matrix"
}
```

## Searching (do this FIRST for every new mod or feature)

Search by keyword across `identifier`, `functions`, and `search_tags`; filter by `category` when you know the subsystem. Read results with `python -I` (no temp script files needed):

```bash
# Keyword search across identifier, functions, and search_tags
python -I -c "import json; d=json.load(open('offset_list.json', encoding='utf-8')); [print(e['identifier'], e['memory_address'], '|', e['category']) for e in d if 'texture' in (e['identifier']+' '+e['functions']+' '+e['search_tags']).lower()]"

# Filter by category
python -I -c "import json; d=json.load(open('offset_list.json', encoding='utf-8')); [print(e['identifier'], e['memory_address']) for e in d if e['category'] == 'Economy & Tips']"
```

If the address you need is not found, it does not exist yet - get it via RE (see `modding-workflow.md` steps 1-2) instead of guessing.

## Maintenance rules (R8 / R16)

1. **First stop:** `offset_list.json` is the first place consulted for every new mod or feature - before any MCP analysis, before any web search.
2. **Update after EVERY RE session:** correct entries that proved wrong, and add every newly verified finding. Knowledge that is not written down here is lost.
3. **Adding entries:** use exactly the 9-field keyset above, in the same order, with all values as strings. Use canonical `0x########` notation for plain absolute addresses (the non-canonical shapes above are reserved for their listed cases). Fill `search_tags` properly: identifier, address, category, plus 3-5 topical keywords. Do not reorder, re-sort, or reformat the array; edit entries in place or append at the end.
4. **Only record what was actually verified** in IDA Pro / Cutter / x64dbg during the session. A hunch is not an entry - keep hunches in your working notes until confirmed.
5. **Do not rename fields or change their order** - downstream tooling parses this exact shape.
6. **Audit after bulk edits:** regenerate and run `audit_offset_list.py` in the session work dir (`<gameDir>/ida_audit/`, protocol: `batch-re-queries.md`) - it flags same-address collisions, identifier conflicts and schema drift against this document. Multi-agent corrections flow through per-agent verdict files; the coordinator merges as the SINGLE writer (backup first, keyset and field order preserved, then re-run the audit). Non-address findings go to the ledger `references/verified_knowledge.json` (`verified-knowledge.md`).

## Data-quality caveats (known, do not "fix" blindly)

- **8 remaining same-base address groups are INTENTIONAL** (verified by the 2026-11-14 IDA audit - do not merge them):
  - `0x01B09938` (8 entries): master texture entry-array base plus `MasterEntry::*` field annotations (+0x00..+0x14, 24-byte slot stride).
  - `0x01B7AB04` and `0x01B7AC00` (2 entries each): pointer bases plus their `(+0x08)` / `(+0x18)` device-chain field annotations (fundamentals B13 chains - different effective addresses, one entry per fact).
  - `0x005342B4` (3 entries): ONE IAT slot, three role views - the exact import name (`IAT_D3DXCreateTextureFromFileInMemoryEx`) plus the JPK and DDS hook-pathway entries.
  - `0x0C56B320`, `0x0C56CEF8`, `0x0C5F0000`, `0x0CB40000`: runtime heap/VRAM staging addresses (statically unverifiable); the per-slot / per-stage / per-container entries there are complementary multiplexed views, not duplicates.
- **1 entry with an empty `functions` field** (`Axel_Struct_RestPose_motDC_Frame0`, `0x22B090`).
- **32 non-canonical `memory_address` strings** (see notation section) - handle them; only change one after re-verification in a debugger/disassembler. Note: `0x22B090` is a FILE offset (below image base, unmapped in VA space), not a virtual address.
- **Runtime-initialized cells:** the WorldMatrix W components (`0x0166153C/4C/5C`) hold `0xFFFFFFFF` in the static image; value claims there are runtime-only until a live check confirms them (ledger VKK-016).
- **English-only owner requirement** (enforced 2026-11-14, final sweep 2026-11-15): the 2026-11-15 sweep pass touched 23 entries (22 German→English field translations plus 1 identifier renamed to `OriginalBrandNames`); the post-sweep audit round fixed 3 further entries (`Axel_Slot8_TorsoFront`, `Axel_TorsoBack_Preview`, `Axel_HandR_Preview_V0_Slot2Base`) - in total 26 entries differ from the pre-sweep backup (25 with German→English field changes + 1 rename). A deterministic German-token gate (blacklist scan over every field) runs since 2026-11-15 and passes, but it is token-based, not a full semantic translation: a minority of entries still contains German-language fragments - mostly audit-note scaffolding in `additional_infos` ('Verifikation: ...; Bericht ...; Beleg: ...') plus scattered field words (e.g. 'Konfiguriert', 'Betritt', 'Schleifen-Ende'); a few entries explicitly mark their German fragments 'kept as-is' (e.g. Sega Untwiddle Decoder, `0x0048CBD0`). Do not assume the file is 100% German-free; new entries are written in English only.
- **Former caveats resolved (2026-11-14 audit):** the 4 duplicated identifiers and 73 same-address collisions were eliminated by merging 67 redundant entries and correcting 61 fields; every merge decision was verified against IDA Pro evidence before it was applied.
