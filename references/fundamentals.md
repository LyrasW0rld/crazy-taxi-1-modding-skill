# Fundamentals - Crazy Taxi 1

> **MANDATORY READING (Workflow Step 0).** Read this file completely BEFORE writing any
> mod code, memory patch, or asset pipeline. These rules prevent the most common classes
> of broken mods: wrong axes, wrongly decoded textures, x64 builds, corrupted physics
> state, corrupted hook arguments, runaway streamers.
>
> **Update obligation:** after EVERY reverse-engineering session, re-verify the rules
> affected by that session's findings and extend this file with newly derived rules -
> the same end-of-session duty as `offset_list.json` (R16). Corrections go in place,
> with evidence. Never delete a user-confirmed rule; if a RE finding contradicts one,
> record the contradiction and the new evidence next to it and ask the user.

## How to read this file

- **Section A** holds the user-confirmed fundamentals (standing conventions and
  version-history facts stated by the user, not derived from the database).
- **Section B** holds derived rules, each backed by evidence from `offset_list.json`,
  cited as `category` / `identifier` / `memory_address`.
- `offset_list.json` remains the ONLY source for addresses. This file holds action rules:
  it cites addresses as evidence but never invents, extends, or redefines them. Schema and
  maintenance rules of the address database: `references/offset-list-schema.md`.

---

## A. User-confirmed fundamentals

### A1 - The game world is upside down: up is negative Y

- **Rule:** treat "up" as the negative-Y direction in world/camera coordinates - the
  higher a vehicle's altitude, the more negative its Y. Before writing ANY height-related
  code (teleport Y, freecam, spawn height), verify the sign empirically for the specific
  coordinate block you are using.
- **Why the caveat:** the only explicit axis-convention note in the database
  (`Player Taxi & Physics` / `ADDR_PLAYER_Y`, `0x01661578`) keeps "-Y up" as the standing
  convention for the camera look-at point while flagging that this is "not separately
  evidenced" for the camera axes, and records "+Y up" for the vehicle-root block
  (`ADDR_VEHICLE_ROOT_Y`). The two blocks are not interchangeable.
- **Corroboration (up = -Y in world/event space):** the helicopter event
  (`Collision & Simulation` / sub_50A9B0) sets initial position `(8860, -200, -3487)`, and
  its tracking (sub_50B140) offsets the player "forward * 1000 m + 200 m height" - 200 m
  up is consistent with Y = -200.
- **Open item resolved by naming audit (2026-11-14):** the vehicle-root block IS in
  the database - `0x01661FA8/AC/B0` are stored as `ADDR_SPAWN_X` / `ADDR_SPAWN_Y` /
  `ADDR_SPAWN_Z` (`Player Taxi & Physics`). Do NOT add a new `ADDR_VEHICLE_ROOT_Y`
  entry - that would recreate a same-address duplicate; cite `ADDR_SPAWN_Y` for the
  vehicle-root Y instead.

### A2 - VQ compression (vector quantization) for textures AND GUI

- **Rule:** assume Sega PVR/VQ encoding plus PowerVR2 Morton twiddle for ALL game
  textures, including GUI sprites. Decode or author replacements as VQ codebook data
  (2048-byte codebook, 2x2 texel blocks) with twiddled pixel order - NOT as plain linear
  D3D bitmaps.
- **Evidence (`offset_list.json`):**
  - `Function Hooks` / Sega VQ Decoder (`0x0048ED08`): "2048-byte codebook + 2x2 texel
    blocks. Decodes vector-quantized texture chunks in the original game".
  - `Function Hooks` / Sega Untwiddle Decoder (`0x0048CBD0`): PowerVR2 Morton bit
    interleaving (Y on even bits, X on odd bits).
  - `Textures` / `SegaContainerEntry::fmt`: layout high byte `0x03/0x10` = Vector
    Quantized (VQ), `0x04/0x11` = VQ with mipmaps; `MasterEntry::flags`: `0x301` =
    layout `0x03` VQ + format `0x01` RGB565, `0x401` = VQ + mipmaps + RGB565.
  - `texDC0_Archive`: contains 1,323 Sega PVR/VQ/twiddled textures.
  - GUI confirmation: `sprGAME` (2D sprite container) entry #156 has `fmt = 0x301` (VQ) -
    so GUI sprites are VQ too.
- **Name trap:** `DecodePvrTexture` (`0x0048DDB0`) is the PVR pixel-data decoder, NOT a
  VQ codebook decoder (its old name "SegaVQDecoder" was corrected by audit).

### A3 - PC versions (2012/2014/2016): one binary, binary-only patches - the addresses are identical

- **Rule:** treat `offset_list.json` as valid for EVERY Steam PC release of
  Crazy Taxi (2012 base game and the 2014/2016 updates) - the addresses are
  identical across all of them. Do not build version-specific address sets
  and do not expect a version check to be needed WITHIN the Steam builds.
- **Scope:** these are the Steam PC releases. NOT covered: the original
  2002 CD port and the 2011 Dreamcast Collection release - both use
  different addresses (see `environment-setup.md`, "Locate Crazy Taxi.exe"
  version warning). A version check IS needed to tell those two apart from
  the Steam build; within the Steam builds themselves, no version-specific
  address sets are needed.
- **Why (user-confirmed, 2026-11-14):** the PC port is, under the hood, the
  original Dreamcast game, rebuilt - badly and painfully - into a PC version
  in 2012 by a small company that **destroyed its source code after the
  final 2012 build**. Sega therefore had NO source code and shipped its two
  PC updates (2014, 2016) as **binary-code-level patches**:
  1. **Music licenses expired** (an external ADX) - replaced outside the
     game code, no impact on it.
  2. **KFC / Pizza Hut / Fila licenses expired and were not renewed.** Sega
     patched the replacement textures in at the binary level; the OLD
     licensed textures are still present in the container/bin files but are
     no longer loaded.
  3. **Fila:** Sega was angry enough about Fila's refusal to patch the
     customer AI at the binary level so that **customers automatically turn
     down a destination when it is called FILA** - Fila is no longer an
     active customer destination in the shipped game.
- **Consequences for modders:**
  - One address database serves all Steam PC versions; never "correct" an address
    just because the user runs a different update revision.
  - The FILA auto-decline is intentional binary behavior, not a bug - do not
    "fix" it silently; re-enabling Fila as a destination is an RE task on
    its own (locate the patched decision code first).
  - The still-present licensed textures (KFC/Pizza Hut/Fila) are the payload
    for Dreamcast-restoration-style mods - see the opt-in restoration
    patterns with full backup and rollback in
    `Examples/Game Features/dc_restoration.cpp`. Restoring licensed branding
    is consent-gated (R15).
  - The music-license change touched only the external ADX, not the audio
    code - rule B11 (CRI ADX replacement rules) is unaffected.

---

## B. Derived rules (each backed by offset_list.json)

### B1 - Everything is 32-bit x86, image base 0x00400000

- **Rule:** build and inject all mods as 32-bit x86 code against image base
  `0x00400000`. Every address in the database is an absolute virtual address that assumes
  this. Never produce an x64 artifact.
- **Evidence:** `Global & Pointers` / Base Address (`0x00400000`, dtype `ImageBase`):
  "Base load address of Crazy Taxi.exe (Win32 PE x86)"; `Audio (CRI ADX) & Timing` /
  `ADXF_VersionString` (`0x00535E78`): "ADXF/PCx86 Ver.7.71 ..." (the CRI middleware
  build confirms 32-bit x86 binaries).

### B2 - Position authority: never treat ADDR_PLAYER_X/Y/Z as the taxi position

- **Rule:** never read or write `ADDR_PLAYER_X/Y/Z` (`0x01661574/78/7C`) as the taxi
  position. They are the persistent camera look-at point with no code path into vehicle
  physics. Use the vehicle ROOT block `0x01661FA8/AC/B0` and treat sub_519890 as the
  physics authority.
- **Evidence:** `Player Taxi & Physics` / `ADDR_PLAYER_X` (RE audit correction
  2026-11-06): additional_infos evidence quote "There is in the binary NO code path
  that copies 0x01661574/78/7C back into the vehicle physics"; operational_notes:
  "No code path copies 0x01661574 back into the vehicle physics (ROOT/BLOCK/PREV/VEL)"
  ("never a physics source");
  the same entry (`0x01661574`) absorbed the former `WorldMatrix4x4::Position_X`
  annotation entry (audit 2026-11-14; its surviving note adds the view-matrix context:
  "offset +0x44 - first dword after the 16-float matrix [base 0x01661530];
  translation column = camera look-at (= ADDR_PLAYER_X)");
  `MasterFramePhysicsCallback` (`0x00511F86`): "physics authority remains sub_519890".
- **Meta-trap:** this entry's old text ("Documented in Part 2 ... Updated per-frame in
  physics tick") was explicitly refuted and removed by audit - stale DB prose exists and gets
  corrected by audit, so always re-read the current note.
- **Contradiction on record:** `SUBMOD_DEVELOPMENT_GUIDE.md` chapter 7, item 3
  ("Always Check In-Game State") shows "Before writing to `ADDR_PLAYER_X` or vehicle
  velocity, verify `*(int*)ADDR_GAME_FLAG == 3`" as a legitimate write guard. That
  contradicts this rule: `ADDR_PLAYER_X/Y/Z` is the camera look-at point with no code
  path into vehicle physics - the state-flag check does not make it a valid
  taxi-position write target. `ADDR_GAME_FLAG` (`0x016A2CFC`, 3 = active gameplay) is
  verified and its check stays useful - apply it to writes of the physics state set
  (B3), never to the look-at point. Follow B2/B3, not the guide's chapter 7 address
  example. The guide's chapter 4 starter example repeats this class of bug at feature
  level - see the contradiction notes in B5 and B6 before copying any guide example
  address.
- **ROOT-block naming:** the vehicle ROOT block `0x01661FA8/AC/B0` lives in the DB
  under the identifiers `ADDR_SPAWN_X` / `ADDR_SPAWN_Y` / `ADDR_SPAWN_Z`
  (`Player Taxi & Physics`, "Chassis integrator spawn and reference coordinate") -
  there is no separate `ADDR_VEHICLE_ROOT_Y` entry and none is needed (see A1).

### B3 - Teleport/fly: write the physics state as a whole set

- **Rule:** when teleporting or flying, write ROOT (`0x01661FA8/AC/B0`), BLOCK
  (`0x01661F30/34/38`), PREV (`0x01661FBC/C0/C4`), DISP (`0x01661F3C/40/44`) and VEL
  (`0x0166202C/30/34`) together. Stale PREV is re-derived as a false rebound speed; the
  engine's mid-sync only forwards Y when |deltaY| < 18.0.
- **Evidence:** `ADDR_VEHICLE_PREV` (`0x01661FBC`): "stale PREV lets a teleport/movement
  delta be re-derived as rebound speed (which is why the mod also writes PREV on
  teleport/fly/latch)"; `ADDR_VEHICLE_VEL` (`0x0166202C`): "The mod zeroes VEL on
  teleport, fly writer and teleport latch";
  `sub_519CAF_ResetVehicleImpulses`
  (`0x00519CAF`): "mid-sync at 0x00519CAF
  BLOCK(0x01661F30/34/38)→ROOT(0x01661FA8/AC/B0)+PREV(0x01661FBC/C0/C4) ...
  Y only when |ΔY|<18.0; end-sync (0x0051A2CC..0x0051A300) ROOT→BLOCK+PREV+DISP".
  (`ADDR_VEHICLE_BLOCK` = `0x01661F30`, `ADDR_VEHICLE_DISP_X` = `0x01661F3C` have
  own entries.)

### B4 - All time math runs on the 60 Hz frame clock

- **Rule:** drive time-based math from the 60 Hz frame clock, not wall-clock time. The
  engine delta-time defaults to `0.0166667f` per frame and the round timer counts 60 fps
  frames. Convert seconds <-> frames by a factor of 60.
- **Evidence:** `g_FrameDeltaTime` (`0x0134EBB8`): "float Delta time in seconds per frame
  (default 0.0166667f at 60Hz)"; `ADDR_GAME_TIMER` (`0x016A41A4`, uint32_t): "In-game
  round timer measured in 60Hz frames. Example: 3000 = 50 seconds, 36000 = 10 minutes";
  `Main_GameLoop` (`0x004EA2D0`) signature `__cdecl Main_GameLoop(float dt)`.

### B5 - Angles are Sega BAM units, not radians - and data_type can lie

- **Rule:** store and convert all angles in Sega BAM units (0..65535 = 0..360 degrees;
  degrees = bam * 360 / 65536) with shortest-arc signed difference handling - never as
  float radians. Do not trust the `data_type` field blindly.
- **Evidence:** `Vehicle Rotation & Matrix` / Pitch 32-bit accumulator (`0x016614C0`) and
  Roll accumulator (`0x016614C8`): "Sega BAM angular unit (0..65535 = 0..360 deg).
  Formula: deg = (float)bam * (360.0f / 65536.0f)"; `ADDR_VEHICLE_YAW_RATE`
  (`0x016620C8`): int16_t, BAM-scaled ("data_type 'float' is wrong"; IDA-verified
  `word_16620C8`, audit 2026-11-14), derived
  radians -> BAM; drop-off validity check (`0x004FC720`) reads |value| < 16 (BAM);
  `Sectors & Streaming` / sub_500410: "Shortest signed angular difference between two
  16-bit BAM angles". The vehicle motion-vector heading (the slip-angle input) is
  `ADDR_VEHICLE_SLIP_HEADING` (`0x01662024`, `int32_t (BAM)`) - the DB entry is scoped
  to the drift/slip motion-vector direction, not a general heading alias.
- **Contradiction on record:** `SUBMOD_DEVELOPMENT_GUIDE.md` ch. 4's "Instant Fix
  Taxi Orientation" button writes int16 zeros to `0x0166147A`/`0x0166147C` - both sit
  inside the camera state block (`0x01661470..0x01661596`), so this levels the CAMERA,
  not the taxi: `0x0166147C` LOWORD is the camera roll and its HIBYTE the passenger
  occupancy bitmask (DB note: "not a taxi banking value"); taxi roll is
  `ADDR_VEHICLE_ROLL_REAL_BAM` `0x01661FB8` (cited in that DB note; no own entry yet -
  same naming pattern as the ROOT block = `ADDR_SPAWN_*` entries, A1/B2). Level the taxi through the
  physics state set (B3) and the 32-bit BAM accumulators `0x016614C0`/`0x016614C8`
  above, never through the camera block. Same block, same trap: the DB used to carry
  `0x0051D940` under the vehicle-named identifiers `ADDR_VEHICLE_UPDATE_FN` and
  `VehicleUpdate` - both were merged (audit 2026-11-14) into
  `ADDR_CAMERA_UPDATE_FN`, whose decompile is a 12-case camera state machine, NOT
  the vehicle physics (authority sub_519890); re-read notes before building on
  identifier names.

### B6 - Boost speed? Scale wheel angular velocity symmetrically

- **Rule:** whenever you raise the drive speed, scale the wheel angular velocity
  (`0x01662090`) symmetrically with the speed scalar (`0x0166208C`, m/s, hard cap 14.0).
  Otherwise sub_515CD0 detects a false tire-burnout slip and clamps propulsion.
- **Evidence:** `ADDR_VEHICLE_WHEEL_ANGULAR_VEL` (`0x01662090`): "Must be scaled
  symmetrically with ADDR_VEHICLE_SPEED_SCALAR (0x0166208C) to prevent sub_515CD0 from
  detecting false tire burnout slip and clamping propulsion"; `ADDR_VEHICLE_SPEED_SCALAR`
  (`0x0166208C`): "(m/s, Cap 14.0)", wheel RPM derived from it
  (`flt_1662090 = flt_166208C * 3600 / ((2*flt_166205C) * pi)`).
- **Contradiction on record:** `SUBMOD_DEVELOPMENT_GUIDE.md` ch. 4's "Turbo Boost"
  example multiplies `0x0166158C` (DB: `ADDR_PLAYER_SPEED`, "Native speedometer
  speed" MPH float inside the camera state block `0x01661470..0x01661596`; the
  `camera_update` DB note also names `flt_166158C` as FOV - conflicting DB roles,
  do not treat it as physics speed). Boosting per this rule means scaling
  `0x0166208C` while scaling `0x01662090` symmetrically - never writing the
  speedometer/camera-block float.

### B7 - Texture identity: (archiveId, chunkIndex) or master slot - never relOffset/GBIX

- **Rule:** identify textures by `(archiveId CRC32, chunkIndex)` or by master slot -
  never by relOffset or GBIX alone. 163 of 1873 relOffsets are shared across the static
  container tables, and the binary contains zero `GBIX`/`PVRT` strings.
- **Evidence:** `Textures` / `GBIX_AliasCollision_0xF3100` (sprGAME #156 <-> texDC0 #254,
  both `pData = 0x0CC33100`, different formats): "texture identity keys must be
  (archiveId, chunkIndex) or master slot - never relOffset/GBIX alone. All
  ScanAndReplaceAllMasterTextures lookup paths are archive-pinned"; `sprGAME`
  (`0x0135FAF0`): "Identity is the VRAM address (pData), never a GBIX tag".

### B8 - Snapshot the master-texture baseIdx BEFORE the engine loader call

- **Rule:** snapshot the master-texture baseIdx at `0x01B7A6E0` BEFORE calling any engine
  texture loader - or infer it (2000 for stage tables texDC1/2/3, 1500 for texDC0) - because
  the background streaming loop resets baseIdx to 0 immediately after use. Master slots
  0..1999 are shared by font/sprite containers with language-dependent bases.
- **Evidence:** `Function Hooks` / `LoadModelTexture` (`0x00492230`): "Background chunk
  streaming ... sets baseIdx=2000 for stage chunks and 1500 for vehicles (texDC0), then
  immediately resets baseIdx to 0 at 0x00505ED6. Any hook capturing baseIdx must snapshot
  0x01B7A6E0 BEFORE the engine call"; `Textures` / `baseIdx`
  (`0x01B7A6E0` - the former `MasterSlotMap` entry was merged into it, audit
  2026-11-14; the entry-array base is `0x01B09938`): 16384 slots, base map "sprCFRE@225; sprGAME@240;
  sprCGER@460 / sprCSPA@470 (language-dependent); sprBOX@480; texDC0@1500".

### B9 - Check the calling convention before every detour: __usercall exists

- **Rule:** check the `functions`/`data_type` fields for register-argument calling
  conventions before writing any detour. 29 entries in the database carry `__usercall`
  in their `data_type` field; 3 more mention it only in other fields - 32 entries in
  total mention `__usercall` somewhere (counts re-verified after the 2026-11-14 audit
  merge, which corrected several wrongly-claimed `__cdecl` entries to `__usercall` /
  `__thiscall`). Typical marks: arguments in `edx`/`eax`/`ecx`/`esi`, caller
  cleanup. A plain `__cdecl` MinHook typedef on one of them corrupts arguments and
  crashes the game.
- **Evidence (verified counts, 2026-11-14 post-audit: `__usercall` in the
  `data_type` field = 29 - the counting basis cited by
  `references/offset-list-schema.md`; `__usercall` anywhere in the entry = 32):** `LoadModelTexture`
  (`0x00492230`, "edx = record-table ptr, stack arg = slot index");
  `LoadSegaTextureContainer` (`0x00492780`, "eax = tablePtr, paletteTable uint32_t* at
  [esp+4], caller cleanup add esp, 4"); `LoadStageTexture` (`0x0048B7A0`,
  "a1@<eax> = record ptr, a2@<ecx> = slot id"); `SegaRenderer::SetTexture` (`0x00481200`,
  "eax = pTexObj"); `DecodePvrTexture` (`0x0048DDB0`, `__usercall`/native).

### B10 - Streaming constants are read-only .rdata floats with hard caps

- **Rule:** treat streaming/tuning constants as read-only `.rdata` floats
  (`0x0059xxxx`-`0x005Axxxx`) that need `VirtualProtect` to change. Respect the hard
  caps: never raise `ADDR_VIEW_RAD_NORMAL` beyond `1100.0f` (the pedestrian spawner
  suppresses spawns within radius * 0.5), and never abort the chunk-evict state machine.
- **Evidence:** `ADDR_LOAD_CHUNKS_RADIUS` (`0x005A04EC`,
  "1800.0f ... in .rdata"; the former `ADDR_SPAWN_DIST_TRAFFIC` entry is a merged
  alias - audit 2026-11-14: one float, exactly three xref sites sub_519890 /
  sub_525450 / sub_526290), `ADDR_SPAWN_DIST_PED` (`0x005A04E8`, "2000.0f"),
  `ADDR_PROP_STREAM_RAD` (`0x005A07CC`, "1696.0f"), `ADDR_SECTOR_STREAM_RAD`
  (`0x005A0908`, "Default: 1500.0f. Sector streamer radius (Mode 0)"),
  `ADDR_CHUNK_DIST_FAR` (`0x0059F9F8`, "450.0f");
  `ADDR_VIEW_RAD_NORMAL` (`0x005A0788`, "float (.rdata)", default `1100.0f`): "Must not
  be increased beyond 1100.0f. Pedestrian spawner sub_4A9A10 suppresses spawns within
  flt_16F59DC * 0.5 exclusion bubble"; `ADDR_CHUNK_EVICT_FN` (`0x005051D0`): "Must never
  be aborted"; `ADDR_GRAVITY_RDATA` (`0x0059FD4C`): "Master gravity constant in .rdata
  (read-only, requires VirtualProtect)".

### B11 - CRI ADX audio: thread-safe, loop-complete, marker-driven

- **Rule:** keep CRI ADX audio work thread-safe and loop-complete. Mixing, decompression
  and device feeding run on dedicated CRI worker threads behind a `CriCrs` reentrant
  mutex. BGM looping is driven by sample markers written to the loader control block
  (+52 loop start, +60 loop end, +56 iteration counter); playback drift is compensated
  only within +/-60 ms. Any replaced track must carry valid big-endian ADX headers and
  loop markers, or music will not loop or stay in sync.
- **Evidence:** `CRI_WorkerThread_AudioMixer` (`0x0041A300`), `CRI_WorkerThread_Decompress`
  (`0x00419E30`), `CRI_WorkerThread_Device` (`0x0041A360`), `CRI_EnterCriticalSection`
  (`0x00424950`, "CriCrs Reentrant Mutex for Audio-Worker Threading"),
  `g_CriInterlockedRefCounter` (`0x0146A980`); `ADX_Loader_SetLoopStartSample` (offset
  `+52`), `ADX_Loader_SetLoopEndSample` (`+60`), `ADX_Loader_SetLoopCount` (`+56`),
  `ADX_ValidateLoopBounds`, `ADXT_ReadLoopInfo`; `flt_1ED2AC0`: "Playback Timing Drift
  in milliseconds (compensation limit: ±60.0 ms)"; `adx_stream_pump_step` (`0x0042A720`).

### B12 - Mixed endianness and Unicode paths

- **Rule:** CRI data is big-endian (ADX magic `0x8001`) while PC-side formats are
  little-endian - parse each container with its own endianness. The install path is a
  UTF-16 `wchar_t[260]` buffer - use wide-char APIs for game paths.
- **Evidence:** `ADX_ParseHeader` (`0x00428230`): "parses the big-endian ADX magic 0x8001";
  `D3D9 Crash Guards` sub_461440 ("little-endian byte sequence") next to sub_461460
  ("big-endian byte sequence") - the engine ships both readers; `JPK_UI_Packages`
  (`0x005342B4`): "data_little/NewTexture/LITTLE_ENDIAN/*.jpk ... Huffman / LZO ... into
  32-bit ARGB8888"; `LTM1_TextureManifest_Header` (uint32 fields; endianness not
  stated by the DB entry - treat as little-endian only after live verification);
  `g_GameInstallPathW` (`0x01EB08C2`, `wchar_t[260]`, "Unicode buffer for base path of
  Crazy Taxi.exe").

### B13 - Resolve D3D9 through the pointer chain; keep the crash guards

- **Rule:** resolve the D3D9 device through the renderer pointer chain: deref
  `0x01B7AC00`, read `+0x18`; fallback deref `0x01B7AB04`, read `+0x08`. Patch IAT
  entries (e.g. `D3DXCreateTextureFromFileInMemoryEx` at `0x005342B4`) with
  `VirtualProtect`. Preserve the existing naked D3D9 crash guards and the SteamAPI
  RestartApp stub - the vanilla game crashes easily on NULL D3D objects and
  force-terminates without a Steam client.
- **Evidence:** `pSegaRenderer` (`0x01B7AC00`, "At offset +0x18 lies the primary
  IDirect3DDevice9* COM pointer"), `pContainer` (`0x01B7AB04`, fallback "+0x08");
  `IAT_D3DXCreateTextureFromFileInMemoryEx` (`0x005342B4`, "Patched directly in memory
  using VirtualProtect PAGE_EXECUTE_READWRITE"); 29 `D3D9 Crash Guards` entries
  (e.g. `ClearD3DState` `0x00481100`, alias `hkClearD3DStateNaked`, audit
  2026-11-14; `hkCreateVertexShaderNaked` `0x00521FD4`);
  `SteamAPI RestartApp Stub` (`0x0053432C`): "Prevents forced termination without
  running Steam client".

---

## Maintenance

- This file is updated at the end of every RE session, together with `offset_list.json`
  (R16): correct affected rules in place, add newly derived rules with evidence, keep the
  A/B numbering stable so other files can reference rules as "fundamentals.md A1/B7".
- User-confirmed rules (Section A) are never silently demoted or deleted.
- Addresses cited here always exist in `offset_list.json` (X-anchor identifiers of a
  write-set stand for the whole X/Y/Z triple even where only the X entry exists) - if
  the database entry
  changes, update the citation here in the same session.
