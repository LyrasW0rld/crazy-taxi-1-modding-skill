# Crazy Taxi (PC Steam / 2002) — 3D Object & Spline Replacement Guide
**Authoritative Technical Manual for LyrasTextureMod / Sega Naomi & Dreamcast PC Architecture**

*Revision 2 (2026-11-01): rewritten against the IDA-verified engine format (`ida_findings/MODEL_FORMAT.md`, `ida_findings/NAMES_AND_TEXTURES.md`) and the overhauled 3D toolchain (`replace_3d_model.py`, `extract_3d_models.py`, `ModelTextureViewer.exe`). The old "−Y up / manual texture flip / model at −Y up in Blender" workflow is retired — the tool now auto-converts everything.*

---

## Table of Contents
1. [Overview & Architecture](#1-overview--architecture)
2. [Coordinate Systems & UV Convention (Y-Up, Right-Handed)](#2-coordinate-systems--uv-convention-y-up-right-handed)
   - [2.1 The Verified Engine Coordinate System](#21-the-verified-engine-coordinate-system)
   - [2.2 The Single Raw-UV Convention](#22-the-single-raw-uv-convention)
   - [2.3 How the Auto-Conversion Works (`--axis-up`)](#23-how-the-auto-conversion-works---axis-up)
3. [Replacing 3D Objects](#3-replacing-3d-objects)
   - [3.1 Model Hierarchy & Multi-Part Assemblies](#31-model-hierarchy--multi-part-assemblies)
   - [3.2 Quick Start — One Command](#32-quick-start--one-command)
   - [3.3 Preparing Your Mesh (Blender / Any DCC)](#33-preparing-your-mesh-blender--any-dcc)
   - [3.4 Material & Texture Assignment](#34-material--texture-assignment)
   - [3.5 `replace_3d_model.py` Full Reference](#35-replace_3d_modelpy-full-reference)
4. [3D Model Boundaries & Engine Limitations](#4-3d-model-boundaries--engine-limitations)
   - [4.1 Capacity Is Measured in Bytes — 96 B per Triangle](#41-capacity-is-measured-in-bytes--96-b-per-triangle)
   - [4.2 Real Chunk Budgets (Verified Registry)](#42-real-chunk-budgets-verified-registry)
   - [4.3 Visual Meshes (`polDC*.bin`) vs Collision Hulls (`colDC*.bin`)](#43-visual-meshes-poldcbin-vs-collision-hulls-coldcbin)
   - [4.4 Bounding Spheres & Frustum Culling](#44-bounding-spheres--frustum-culling)
   - [4.5 Rigid-Body Parts vs Skinned Meshes — Character Caveat](#45-rigid-body-parts-vs-skinned-meshes--character-caveat)
5. [Replacing Spline Routes (Cable Car & Trains)](#5-replacing-spline-routes-cable-car--trains)
   - [5.1 Spline Architecture & Route Registry](#51-spline-architecture--route-registry)
   - [5.2 32-Byte Waypoint Binary Structure](#52-32-byte-waypoint-binary-structure)
   - [5.3 Using `spline_tool.py`](#53-using-spline_toolpy)
   - [5.4 The 3D DCC (Blender) Spline Workflow](#54-the-3d-dcc-blender-spline-workflow)
   - [5.5 Editing Spline Coordinates in JSON](#55-editing-spline-coordinates-in-json)
6. [Spline Boundaries & Engine Limitations](#6-spline-boundaries--engine-limitations)
7. [ModelTextureViewer.exe Technical Reference](#7-modeltextureviewerexe-technical-reference)
   - [7.1 Resolution Initialization & Selection Sync (Fixed 0x0 Bug)](#71-resolution-initialization--selection-sync-fixed-0x0-bug)
   - [7.2 Raw-UV Texture Upload (Unflipped)](#72-raw-uv-texture-upload-unflipped)
   - [7.3 Applied-Textures Panel, Part Labels & Status Bar](#73-applied-textures-panel-part-labels--status-bar)
   - [7.4 Parser Selftest CLI (`--selftest`)](#74-parser-selftest-cli---selftest)
8. [Summary Workflow Checklist](#8-summary-workflow-checklist)

---

## 1. Overview & Architecture

*Crazy Taxi* (originally developed for the Sega Naomi arcade hardware and Sega Dreamcast in 1999–2000, ported to PC DirectX 8/9 in 2002) utilizes Sega's proprietary **Ninja 3D Graphics Library**.

In the PC Steam edition:
- All visual 3D geometry is packaged inside binary container files in `data_little/`:
  - `polDC0.bin`: Player cabs, wheels, drivers, traffic vehicles, pedestrians, and interactive props.
  - `polDC1.bin`: Stage 1 (Arcade / San Francisco) world terrain, buildings, and skybox.
  - `polDC2.bin`: Stage 2 (Original / Around the Bay) world geometry.
  - `polDC3.bin`: Stage 3 (Crazy Box mini-games and challenges).
- Textures are stored in matching container tables:
  - `texDC0.bin`: 3D object and vehicle textures.
  - `texDC1.bin`, `texDC2.bin`, `texDC3.bin`: Stage and environment textures.
- Vehicle routes and path splines (San Francisco cable cars, freight trains) are embedded within `polDC0.bin` as sequential arrays of 32-byte waypoint records.

*LyrasTextureMod* provides a complete toolchain to extract, inspect, modify, and inject custom 3D models and spline paths:

| Tool | Purpose |
|---|---|
| `build_extract_textures.py` | Extract/restore vanilla textures as right-side-up PNGs. All texture-file rules (formats, dimensions, naming, hot-swap) live in the companion **Crazy Taxi Texture Modding Guide** — this guide does not duplicate them. |
| `extract_3d_models.py` | Export engine-exact 3D data: raw UVs, one material per mesh record, 15-part character assemblies positioned via the real `sub_4BA470` translation chain (`{Model}_assembly.json` with `part_label`, `offset`, `rotation_deg`, `texture_indices`, `pose: "translation-chain"`), and the reverse lookup index `3D_texture_to_model_index.json`. |
| `replace_3d_model.py` | **One-command 3D model replacement**: auto axis conversion, auto rescale + pivot to the original model's bounding box, auto texture assignment per material group, honest capacity check, timestamped backups, dry-run, and post-write verification. |
| `spline_tool.py` | Extract, edit (OBJ or JSON), and inject cable-car & train spline routes. |
| `ModelTextureViewer.exe` | Real-time OpenGL inspector with per-material multi-texture rendering, an "Applied textures" panel, assembly part labels, and a headless `--selftest` mode. |

---

## 2. Coordinate Systems & UV Convention (Y-Up, Right-Handed)

### 2.1 The Verified Engine Coordinate System
Reverse engineering of the render pipeline (`dispatch_render_passes` @ `0x00497160`, projection init `sub_494AD0`, vehicle assembly `sub_517D70` — see `ida_findings/MODEL_FORMAT.md` §3) gives an unambiguous answer:

- **Up axis: +Y.** A vehicle's Y is set from the ground height, its heading is a rotation about Y.
- **Handedness: right-handed.** The engine projects with `D3DXMatrixPerspectiveFovRH` (≈41° FOV, near 3.0, far 15000.0); forward is **−Z**.
- **Scale: 1 world unit ≈ 0.1 m.** An Axel cab is roughly 42 units long, 18 wide, 14 tall.
- **Angles:** 16-bit binary angle units (BAM); `rad = u16 × 2π / 65536`.
- **Matrices:** D3DX row-major, row-vector math (`v' = v × M`).

**Retired myth:** earlier revisions of this guide claimed *"−Y is up (Sega Naomi CRT legacy)"* and instructed you to extract textures flipped, author models at −Y up in Blender at 1:10 scale and center them by hand. That convention does **not** exist in the engine. **Never hand-convert coordinates or flip textures for this game anymore** — `replace_3d_model.py` auto-converts your source model (§2.3), and the extraction/preview chain (§2.2) already agrees with itself end to end.

### 2.2 The Single Raw-UV Convention
Mesh UVs and textures now follow **one convention everywhere**, verified from the draw code (`sub_497A30` / `bind_world_textures` — see `ida_findings/NAMES_AND_TEXTURES.md`):

| Stage | Behavior |
|---|---|
| Engine (draw) | UVs are consumed **raw** — no per-draw scale/offset/transform. `v = 0` samples the **top row** of the texture (D3D semantics). Tiling values > 1.0 are legitimate (baked into the vertices); wrap addressing is **REPEAT** (global, not per-texture). |
| `build_extract_textures.py` | PNGs are written **right-side up on disk** — row 0 of the PNG is the row the engine samples at `v = 0`. |
| `extract_3d_models.py` | Exports **raw UVs** (`vt u v` exactly as stored in the binary) — no flip anywhere. |
| `ModelTextureViewer.exe` | Uploads textures **unflipped** (§7.2) so the preview is pixel-faithful to the game. |
| `replace_3d_model.py` | Writes your OBJ's UV values **raw** into the vertex records — no flip. |
| `LyrasTextureMod.dll` (texture injection) | Normalizes row order for Sega-container textures on load; see the Texture Modding Guide. |

Practical consequences:

1. **What you see in ModelTextureViewer is what the game renders.** The viewer is the ground-truth preview.
2. **Your DCC viewport is the odd one out.** The OBJ spec (and Blender's importer/exporter) treat `v = 0` as the *bottom* of the image, while this engine treats `v = 0` as the *top row*. So a model you UV-mapped in Blender can look vertically mirrored in the viewer even though the OBJ is correct. Check in the viewer; if the mapping is mirrored there, flip your **UVs** vertically (`v → 1 − v`) in the DCC and re-export. **Do not flip the PNG** — the texture chain already agrees with itself, and flipping it would break every other model using that texture.
3. Re-imported extractor output (raw game UVs + extracted PNGs) needs no adjustment at all.

### 2.3 How the Auto-Conversion Works (`--axis-up`)
You can author your mesh in **any axis convention**. `replace_3d_model.py --replace` converts it to the game frame (Y up, RH) for you. The source frame is selected with `--axis-up`:

| Mode | Meaning | Transform to game coords | Triangle winding |
|---|---|---|---|
| `auto` *(default)* | Assumes **+Y up** (the universal OBJ convention) and tells you what it assumed. Prints a hint when the bounding box *looks* like a Z-up export. | identity | kept |
| `y` | Source +Y up | identity | kept |
| `z` | Source **Z up** (Blender default) | game `(x, y, z) = (x_src, z_src, −y_src)` — a pure rotation | kept (determinant +1) |
| `-y` | Source −Y up (the old guide's convention) | `(x, −y, z)` — a mirror | **reversed** automatically (determinant −1) |
| `none` | No conversion at all | identity | kept |

- `auto` = `y` plus a heuristic **hint**: if the OBJ's Y extent is by far the longest dimension (typical of a Z-up export of a long object such as a car), the tool prints `[HINT] ... re-run with --axis-up z`. It is only a hint — genuinely Y-up tall objects (lamps, towers) can also trigger it — and the assumed mapping is always printed before anything is written.
- **When to override:** if your model lies on its side in game, the source was Z-up — re-run with `--axis-up z`. If it appears mirrored, check the mapping the tool printed for your `--axis-up` choice. If the model was already exported in game coordinates (e.g. re-exported `extract_3d_models.py` output), keep the axis conversion off (`--axis-up none` or `--no-flip`) **and** skip the rescale with `--no-rescale` (§3.5).
- Importing extractor output into Blender for reference: the OBJ is Y-up, Blender is Z-up — rotate **−90° about X** on import.

---

## 3. Replacing 3D Objects

### 3.1 Model Hierarchy & Multi-Part Assemblies
Everything drawable in `polDC0.bin` is a **chunk**: one model with a `0x18`-byte header followed by a list of mesh records (§4.1). The replacement registry (`replace_3d_model.py --list`) exposes **90 verified chunks** in four categories — every offset and budget was verified by record-walking the actual binary:

| Category | Entries | Examples |
|---|---|---|
| **Player_Taxi** | 13 | `Axel_Taxi_Body` (0x0038E8), `Axel_Taxi_Wheel_FL/FR/RL/RR`, `Axel_Taxi_SteeringWheel`, `Axel_Taxi_Driver`, `BDJoe/Gus/Gena_Taxi_Body` (+ `_Alt` cab variants) |
| **Traffic** | 13 | `Traffic_Sedan_Blue`, `Traffic_StationWagon`, `Traffic_Police_Car`, `Traffic_Van`, `Traffic_Pickup_Truck`, `Traffic_City_Bus`, `Traffic_Cable_Car_Assembled`, `Traffic_Cable_Car`, `Traffic_Sedan_Red`, `Traffic_Ambulance`, `Traffic_Delivery_Truck`, `Traffic_Garbage_Truck`, `Traffic_Highway_Coach` |
| **Prop** | 4 | `Prop_City_Bench`, `Prop_Fire_Hydrant`, `Prop_Street_Lamp`, `Prop_Traffic_Sign` |
| **Character** | 60 | 15 parts per character: `Axel_Pelvis`, `Axel_Foot_L`, … `Axel_Head` (same scheme for BDJoe, Gena, Gus) |

Notes on the hierarchy:

- **Player taxi** = 7 separate chunks: body, 4 wheels, steering wheel, driver. All 7 are registered for **Axel**; the other three drivers expose their body chunk plus a `_Alt` variant (a cab variant that also carries the driver/interior texture set). The old registry's `BDJoe/Gus/Gena_Taxi_Driver` entries pointed *inside* other chunks (record-walk artefacts, not chunk starts) and were removed.
- **Traffic vehicles** are multi-chunk assemblies; each `Traffic_*` registry entry is one verified chunk of that vehicle, and its budget is the true distance to the next chunk. `Traffic_Cable_Car_Assembled` (offset `0x161328`, budget 19,064 B) is the complete multi-record cable car. Run `--info <name>` to see a chunk's record structure before you commit.
- **Playable characters (Axel, BD Joe, Gena, Gus)** consist of **15 rigid parts each**, read from the per-character part tables in `Crazy Taxi.exe` (VAs `0x013B6754`, `0x013B6794`, `0x013B67D4`, `0x013B6814`). **The engine contains no part names anywhere** (verified by full-binary string scan — see `NAMES_AND_TEXTURES.md` §6), so labels are derived: table order 4–14 carries the classic names (`Foot_L` … `Head`), order 0–3 is inferred from mirrored geometry as `Pelvis` / `Foot_R` / `Calf_R` / `Thigh_R`. The extractor bakes the real `sub_4BA470` translation chain into `3D/characters/{Character}_Character.obj` and `3D/assembled/{Character}_Complete.obj`, with per-part data in `{Character}_assembly.json` (`part_label`, `offset`, `rotation_deg`, `texture_indices`, `pose: "translation-chain"`).

> **CAUTION — Character replacement caveat:** character parts are **bone-local meshes placed by the engine's skeleton chain** (`sub_4BA470` @ `0x004BA470`, animated from `motDC.bin`). Replacing a single part changes that part's **shape only — never the pose**. Also, limb meshes live in raw bone-local orientation (the bone's local +X runs along the limb), so keep your replacement part roughly aligned with the original part's orientation and bounding box. The auto-rescale (§3.5) handles size and pivot; shape is up to you.

### 3.2 Quick Start — One Command
The whole workflow is now three steps. **Golden Rules:**

1. **One material group per texture** you want on the model (`usemtl` groups; `o`/`g` statements are ignored).
2. **Triangles only.** The tool triangulates quads/n-gons for you, but triangulating yourself gives you control over edge loops.
3. **Don't fight coordinates** — any up-axis works; the tool prints exactly what it assumed and converted.
4. **`--dry-run` first.** It prints the full pipeline (axis mapping, rescale, texture table, byte budget) without writing anything.
5. **The capacity check is honest.** If it says the model doesn't fit, it doesn't fit — simplify the mesh (§4.1).
6. **Backups are automatic and timestamped** — you can always restore (§3.5).

```bash
# 1. Model your mesh (any DCC, any up-axis), export OBJ + MTL
# 2. Replace:
python replace_3d_model.py --replace Axel_Taxi_Body my_cab.obj
# 3. Done — axis conversion, rescale/placement, texture assignment,
#    capacity check, backup, write and verification all happen in one go.
```

Optional: `--texture paint.png` applies your own image to every material group; `--dry-run` previews everything without touching `polDC0.bin`.

### 3.3 Preparing Your Mesh (Blender / Any DCC)
1. **Mesh**: triangulate before export (**Modifiers > Triangulate**); n-gons are fan-triangulated by the tool as a fallback.
2. **Materials**: one material per texture you want (§3.4). Grouping is by material only — `o`/`g` statements are ignored because the engine switches textures per mesh record.
3. **Scale & placement**: don't bother. The tool uniformly rescales and re-centers your whole model onto the **original chunk's bounding box** (one shared transform, so proportions and relative part layout survive). Use `--scale f` for an extra multiplier, or `--no-rescale` if your OBJ is already in game coordinates.
4. **UVs**: optional. Corners without UVs get `(0, 0)`; corners without normals get the face normal. Remember the v-convention caveat from §2.2 when authoring new UV maps — verify in the viewer.
5. **Export settings (Wavefront `.obj`)**: check **Include Normals** (`vn`), **Include UVs** (`vt`), **Write Materials** (`mtllib` / `usemtl`), and **Triangulate Faces**. Any up-axis is fine (§2.3).

### 3.4 Material & Texture Assignment
The engine binds one texture per mesh record via an integer index into `texDC0.bin` (`texIndex = -1` means untextured). For each material group in your OBJ, `replace_3d_model.py` picks the texture index using the **first match** of this priority chain:

1. **Material name** containing a `texDC0` reference — `mat_texDC0_127`, `texDC0/127`, or a bare number:
   - `mat_texDC0_127` → texture 127 in `texDC0.bin` (Axel cab body paint).
   - `mat_texDC0_24` → texture 24 (interior/trim), `mat_texDC0_128` → texture 128, etc.
2. **`map_Kd` filename** containing the extractor's naming token `__texDC0__<idx>__`. Extracted texture files look like `3D_OBJECT__texDC0__0127__GBIX_<hex>__FMT_...__128x128__HASH_<crc>.png`, so any material whose `map_Kd` points at an extracted PNG is auto-matched — no renaming needed.
3. **`--texture <image.png>`** — your image is used for every group that has no explicit texDC0 reference. The game keeps each original record's texture slot; the mod's texture engine serves your image for that slot. (A PNG passed via `--texture` is also copied next to your OBJ as `<objname>.png` if it isn't there already.)
4. **Fallback**: reuse the original chunk's texture indices 1:1 — group *i* gets the texture of original record *i*. Keeping the same group order as the extractor's OBJ output therefore preserves the original texture layout even with no naming hints at all.

The full assignment is printed as a table on every run, so you always see which group got which index and why:

```
[5/6] Texture assignment:
        #  OBJ group                    texIndex  origin
        0  mat_texDC0_127                    127  material name 'mat_texDC0_127' (texDC0 ref)
        1  glass                             128  original chunk record 1 (fallback)
        2  tires                              24  map_Kd '3D_OBJECT__texDC0__0024__GBIX_...png'
```

For texture-file rules (PNG format, dimensions, mipmaps, how the in-game hot-swap serves your image), see the companion **Crazy Taxi Texture Modding Guide**.

### 3.5 `replace_3d_model.py` Full Reference

```bash
python replace_3d_model.py --list                          # registry: 90 models, budgets, ~max tris
python replace_3d_model.py --info Axel_Taxi_Body                    # dump a chunk's parsed structure
python replace_3d_model.py --replace <ModelName> <file.obj> [flags] # the one-command replacement
python replace_3d_model.py --replace Axel_Taxi_Body my_cab.obj --texture paint.png --dry-run
python replace_3d_model.py --replace Axel_Foot_L foot.obj --axis-up z
```

| Flag | Effect |
|---|---|
| `--replace <MODEL_NAME> <OBJ_FILE>` | Parse the original chunk, convert, rescale, assign textures, check capacity, back up, write, verify. |
| `--texture, -t <IMAGE>` | Your texture image for all groups without an explicit texDC0 reference (priority 3 in §3.4). |
| `--axis-up {auto,y,z,-y,none}` | Source model's up axis (§2.3). Default `auto`. |
| `--scale, -s <f>` | Extra scale multiplier on top of the auto-rescale. |
| `--no-rescale` | Skip auto rescale/placement — use for OBJs already in game coordinates (extractor output). |
| `--no-flip` | Use OBJ coordinates exactly as-is: no axis conversion, no winding change. |
| `--dry-run` | Print everything (axis mapping, rescale, texture table, sizes) without writing any file. |
| `--list, -l` | List the registry with offsets, byte budgets and ~max triangles. |
| `--info <MODEL_NAME>` | Dump a model's budget, header, bounding box, and every mesh record (texIndex, blob size, sections). |
| `--game-dir, -g <DIR>` | Crazy Taxi root folder (auto-resolves `data_little/`; sensible default otherwise). |

**What happens on `--replace` (the six printed phases):**

1. **`[1/6] Original chunk`** — the tool parses the original model at its registry offset (refusing to write if the chunk doesn't parse as a valid model) and reports record count, parsed verts, chunk size, budget, and the header bounding sphere.
2. **`[2/6] Loaded <obj>`** — your OBJ is parsed into per-material groups (positions, normals, UVs; negative/relative OBJ indices supported).
3. **`[3/6] Axis`** — the chosen/auto axis mapping is applied (§2.3), including automatic winding reversal for mirrored (`-y`) sources.
4. **`[4/6] Rescale`** — one shared uniform scale + translation maps your model's bounding box onto the original geometry's bounding box: centers aligned, largest extents equal. The replacement lands exactly where the old model was — same footprint, same pivot.
5. **`[5/6] Texture assignment`** — the table from §3.4.
6. **`[6/6] Compiled`** — the chunk is compiled (0x18 header with an exact bounding sphere, one 0x50-byte record header + vertex blob per group, 4-byte terminator) and checked against the real byte budget. Over budget → **refused**, with the exact overage and the max triangle count for your group count.

Then, if not `--dry-run`:

- **`[BACKUP]`** — `polDC0.bin.bak_<YYYYMMDD_HHMMSS>` (timestamped, never overwritten). To restore, delete/rename the modified `polDC0.bin` and rename the backup back.
- **`[WRITE]`** — the compiled chunk is written at the chunk offset and all slack bytes up to the chunk boundary are zeroed.
- **`[VERIFY]`** — the written bytes are re-parsed and checked: record count, texIndices, terminator, size vs budget, and geometry bounding box must match the input. `[VERIFY] PASS` before `[SUCCESS]`.

**Capacity math (the honest budget):** triangle-list encoding costs **96 bytes per triangle** (3 inline 32-byte vertices `{x,y,z, nx,ny,nz, u,v}` — this engine has **no index buffers**, vertices are duplicated per corner). A chunk costs

```
bytes = 0x18 (chunk header) + 4 (terminator) + groups × (0x50 + 8) + 96 × triangles
```

**Worked example** — a single-material 560-triangle replacement for `Axel_Taxi_Body` (budget **56,584 B** = true distance to the next chunk):

```
560 tris × 96 B        = 53,760 B   (vertex data)
+ 24 + 4 + 80 + 8      =    116 B   (chunk header, terminator, record + section header)
total                  = 53,876 B ≈ 54 KB  →  95.2 % of the 56.5 KB budget  →  fits,
                         with ~2.7 KB headroom (≈ 28 more triangles; hard max ≈ 588 tris)
```

Splitting the same mesh into more material groups does **not** help: each extra group adds its own 0x58-byte record + section overhead (`--info` prints both the 1-group and 4-group capacities).

---

## 4. 3D Model Boundaries & Engine Limitations

### 4.1 Capacity Is Measured in Bytes — 96 B per Triangle
- **There are no index buffers and no vertex-count limit.** The old "65,535-vertex / uint16 index" claim was fiction: the engine streams 32-byte vertices (`pos3f, nrm3f, u, v`) straight out of each record's sections into its draw buffer (`sub_497A30`), drawing strips or triangle lists implied by the section format bits. Vertices are inline — three per triangle for a list — hence the flat **96 B/triangle** cost (§3.5).
- The **only** hard limit is the chunk's byte budget: the distance to the next chunk in `polDC0.bin` (chunks are packed back-to-back; there is no dynamic heap). `replace_3d_model.py` refuses injections past it and zeroes the remaining slack on write.
- If you are over budget: reduce triangle count. Fewer, larger material groups also help marginally (each group costs 0x58 B of headers).

### 4.2 Real Chunk Budgets (Verified Registry)
The old capacity table ("Axel body: 120,000 bytes", "traffic cars: 80,000–90,000") was fiction — the real budgets are the gaps between adjacent chunks, and they are **much** smaller. `--list` prints all 90; highlights:

| Model | Budget | ~Max Tris (1 group) |
|---|---|---|
| `Axel_Taxi_Body` | 56,584 B | 588 |
| `BDJoe_Taxi_Body` | 75,256 B | 782 |
| `Gus_Taxi_Body` / `Gena_Taxi_Body` | 58,632 / 67,472 B | 609 / 701 |
| `*_Taxi_Body_Alt` (cab variants) | 35,536 – 52,680 B | 368 – 547 |
| `Axel_Taxi_Driver` | 22,824 B | 236 |
| `Axel_Taxi_SteeringWheel` | 13,480 B | 139 |
| `Axel_Taxi_Wheel_*` | 3,920 – 4,016 B | 39 – 40 |
| `Traffic_Cable_Car_Assembled` | 19,064 B | 197 |
| `Traffic_*` (cars, per-chunk) | 1,664 – 2,984 B | 16 – 29 |
| `Prop_*` (bench, hydrant, lamp, sign) | 520 B | 4 |
| Character parts (`*_Pelvis` … `*_Head`) | 600 – 3,968 B | 5 – 40 |

*Boundary Rule:* exceeding `max_bytes` would overwrite the **adjacent model** in `polDC0.bin` (immediate corruption or crashes). The tool hard-refuses instead — trust its numbers.

### 4.3 Visual Meshes (`polDC*.bin`) vs Collision Hulls (`colDC*.bin`)
*Crazy Taxi* strictly separates visual rendering from physical collisions:
- **Visual meshes** reside in `polDC0.bin` (objects) and `polDC1.bin`–`polDC3.bin` (stages).
- **Collision geometry** resides in `colDC0.bin`–`colDC3.bin`: vehicle hulls as oriented bounding boxes (OBBs) and sphere hierarchies; world collisions as static spatial AABB trees.

*Modding Boundary:* replacing a visual model changes **only what you see**. A monster-truck body on `Axel_Taxi_Body` will clip through telephone poles, buildings, and traffic because the collision box is still the original cab's.

### 4.4 Bounding Spheres & Frustum Culling
Every chunk header carries a bounding sphere (radius + center at `+0x08/+0x0C`), and every mesh record carries its own local sphere center (`+0x10`). The engine's visibility culler tests these against the camera frustum; a too-small radius makes parts pop in and out when the camera turns. `replace_3d_model.py` computes the exact minimum enclosing sphere for the compiled chunk and per group — no manual tuning required.

### 4.5 Rigid-Body Parts vs Skinned Meshes — Character Caveat
- The 2002 engine has **no** skinned/weighted skeletal meshes. Pedestrians, drivers, and characters are **rigid-body hierarchies**: each part is a separate rigid chunk transformed by the engine's bone matrices.
- **Characters (15 parts)** are placed by the hardcoded translation/bone chain in `sub_4BA470`, animated from `motDC.bin`. Replacing a part reshapes it; it does **not** re-pose it (§3.1 CAUTION). For best results keep the replacement roughly shaped like the original part and let the auto-rescale match the footprint.
- Drivers seated in cabs are single pre-assembled chunks (`Axel_Taxi_Driver` etc.) — those you can replace like any other model.

---

## 5. Replacing Spline Routes (Cable Car & Trains)

### 5.1 Spline Architecture & Route Registry
San Francisco's iconic hill cable cars and freight trains operate on pre-programmed spline routes stored inside `polDC0.bin`. Traffic AI vehicles interpolate between sequential 3D waypoints.

The primary spline routes available for modification are:

| Route ID | Route Name | File | Binary Offset | Max Waypoints | Description |
|---|---|---|---|---|---|
| **10** | `P_Cable` | `polDC0.bin` | `0x096848` | **35** | San Francisco Cable Car (Downtown hill tracks) |
| **11** | `P_RailRoad_A` | `polDC0.bin` | `0x09E518` | **19** | Freight Train Route Segment A |
| **12** | `P_RailRoad_B` | `polDC0.bin` | `0x0A0698` | **25** | Freight Train Route Segment B |
| **13** | `P_RailRoad_C` | `polDC0.bin` | `0x0A1AB0` | **21** | Freight Train Route Segment C |

### 5.2 32-Byte Waypoint Binary Structure
Every waypoint record in the spline table consists of exactly **32 bytes** (packed little-endian `<IIIfffII`):

```c
struct SplineWaypoint {
    uint32_t flags;       // +0x00: Node state flags (1 = Active, 2 = Stop/Station, 0 = Disabled)
    uint32_t segment_id;  // +0x04: Sequential index of the waypoint (0, 1, 2, ...)
    uint32_t reserved;    // +0x08: Internal engine alignment / tracking DWORD
    float    x;           // +0x0C: East/West world coordinate
    float    y;           // +0x10: Elevation — game coordinates are Y-up (§2.1)
    float    z;           // +0x14: North/South world coordinate
    uint32_t param_flags; // +0x18: Speed curve & tangent interpolation parameter
    uint32_t next_link;   // +0x1C: Index of the next connected waypoint (supports looping)
};
```

### 5.3 Using `spline_tool.py`
`spline_tool.py` manages extraction, 3D DCC conversion, and binary injection of spline tracks:

```bash
# 1. List all available routes and current coordinates
python spline_tool.py --list

# 2. View technical diagnostics and coordinate spans for a route
python spline_tool.py --info 10

# 3. Export route as a 3D Wavefront OBJ line strip for Blender
python spline_tool.py --export-obj 10 --out-obj cable_car_route.obj

# 4. Import modified 3D line strip from Blender and directly inject into game
python spline_tool.py --import-obj 10 cable_car_route.obj --inject

# 5. Alternatively: Dump and inject via human-readable JSON files
python spline_tool.py --dump splines
python spline_tool.py --inject splines/Route_10_P_Cable.json
```

### 5.4 The 3D DCC (Blender) Spline Workflow
Editing splines numerically in text files can be cumbersome. The recommended workflow uses Blender:
1. **Export Spline from Game**:
   ```bash
   python spline_tool.py --export-obj 10 --out-obj cable_car_route.obj
   ```
   The tool writes the line strip with `Y_obj = -Y_game` (its fixed export convention), so the same file always round-trips identically.
2. **Import into Blender**:
   - Go to **File > Import > Wavefront (.obj)**.
   - Select `cable_car_route.obj`. Also import your stage model (e.g. `3D/stages/Stage1_...obj`) into the same scene.
   - The spline appears as an interconnected vertex polyline directly over the street.
3. **Edit Spline Path in Blender**:
   - In **Edit Mode**, move vertices along the road, snap points to pavement surfaces, or adjust hill slopes.
   - *Capacity Warning:* You may delete points, but you must **never exceed the maximum waypoint capacity** (35 for Route 10).
4. **Export from Blender**:
   - Select only the spline object.
   - Go to **File > Export > Wavefront (.obj)**.
   - Under Geometry options, ensure **Lines** is checked.
5. **Convert & Inject into Game**:
   ```bash
   python spline_tool.py --import-obj 10 cable_car_route.obj --inject
   ```
   The tool reads the vertex order from Blender, converts elevation back to game coordinates (the exact inverse of the export step), computes segment distances, warns about oversized gaps, validates that the waypoint count ≤ capacity, creates a safety backup, and injects the new track into `polDC0.bin`. Because export and import apply the same inversion, the round trip is symmetric — edited heights land exactly where you put them.

### 5.5 Editing Spline Coordinates in JSON
When you dump a route, `spline_tool.py` produces an editable JSON file (e.g. `splines/Route_10_P_Cable.json`):

```json
{
  "route_id": 10,
  "name": "P_Cable",
  "description": "San Francisco Cable Car Route (Hill Tracks)",
  "file": "polDC0.bin",
  "offset_hex": "0x096848",
  "offset": 616520,
  "waypoint_count": 35,
  "max_capacity": 35,
  "coordinate_system": "Sega Naomi/Dreamcast: X=East/West, -Y=Up/Sky, +Y=Down/Ground, Z=North/South",
  "waypoints": [
    {
      "index": 0,
      "flags": 1,
      "segment_id": 0,
      "reserved": 0,
      "x": 16.634,
      "y": -7.900,
      "z": 65.440,
      "param_flags": 12,
      "next_link": 1
    }
  ]
}
```

> **Note:** the `coordinate_system` string in the JSON is a legacy label carried over from an earlier build of the tool. The verified engine convention is **Y up** (§2.1). The JSON `y` values are raw game coordinates, and dump/inject round-trips them unchanged.

To modify the path via JSON:
1. Open the JSON in any text editor.
2. Edit the `x`, `y`, and `z` coordinates of each waypoint.
   - Elevation is the **Y axis, and +Y is up** in game coordinates (§2.1): to raise the track over a bridge or hill, **increase** `y`; to lower it, **decrease** `y`. (Earlier revisions said the opposite, based on the since-disproved "−Y up" myth.)
3. Save the JSON file and execute `python spline_tool.py --inject <filename>`.

---

## 6. Spline Boundaries & Engine Limitations

### 6.1 Hard Waypoint Capacity Limits
- **Cable Car (Route 10)**: Strict maximum of **35 waypoints** (35 × 32 = 1,120 bytes).
- **Freight Train A (Route 11)**: Strict maximum of **19 waypoints** (19 × 32 = 608 bytes).
- **Freight Train B (Route 12)**: Strict maximum of **25 waypoints** (25 × 32 = 800 bytes).
- **Freight Train C (Route 13)**: Strict maximum of **21 waypoints** (21 × 32 = 672 bytes).

*Boundary Rule:* You may reduce the number of waypoints (e.g., use 20 waypoints instead of 35 by terminating the link), but you **cannot exceed the maximum capacity**. Injecting 36 waypoints into Route 10 overwrites the adjacent data in `polDC0.bin`, resulting in an immediate crash when entering downtown. `spline_tool.py` validates capacity before every injection.

### 6.2 Tangent Continuity & Maximum Segment Spacing
- **Recommended Segment Distance**: Keep distance between consecutive waypoints between **50.0 and 400.0 world units**.
  `Distance = sqrt((X2-X1)² + (Y2-Y1)² + (Z2-Z1)²)`
- *Boundary Limit:* If the distance between two points exceeds **500.0 units**, the linear interpolation velocity vector snaps abruptly — the cable car or train jerks or teleports across the gap rather than rolling smoothly. (`--import-obj` / `--inject` print a warning for such gaps.)
- *Turn Curvature:* Avoid 90° sharp turns between two adjacent points. The vehicle physics calculates yaw using `atan2(ΔX, ΔZ)` with BAM damping (§2.1). Sharp corners cause the vehicle to flip sideways or detach from its bogies.

### 6.3 Terrain Collision Raycasting & Wheel Snapping
The game engine executes a continuous downward raycast from the vehicle center to align wheels with the road surface:
- **Maximum Elevation Offset**: Keep spline Y within ±30.0 units of the physical street collision mesh (`colDC1.bin`).
- If you place a spline waypoint too high in the air (> 50.0 units above ground), the raycast misses, the vehicle loses traction, and its physics drops into freefall until it resets to waypoint 0.
- If you place a spline below ground level, the vehicle collides with the underside of the road mesh, flipping violently.

### 6.4 Looping vs Terminal Topologies
- **Looping Tracks (Cable Car)**: The final waypoint's `next_link` field must point back to `0` (`"next_link": 0`). This causes the car to loop continuously around its circuit.
- **Terminal Tracks (Trains)**: For a non-looping track, set the final waypoint's `flags` to `0` and `next_link` to its own index. This instructs the train to stop or despawn at the terminal node.

---

## 7. ModelTextureViewer.exe Technical Reference

`ModelTextureViewer.exe` is the real-time OpenGL inspection tool provided with the mod. It renders multi-material models with their per-record textures, lists the textures applied to the current model, resolves part labels from the extractor's assembly manifests, and offers a headless selftest mode — so you can verify a model **before** launching the game.

### 7.1 Resolution Initialization & Selection Sync (Fixed 0x0 Bug)
In earlier versions, the viewer displayed `0x0` under the **Resolution** column for textures until each texture was manually clicked, and selecting a model from the ComboBox left the Info card uninitialized.

**Root Causes & Technical Fixes:**
1. **Unparsed Texture Dimensions**:
   - Textures were initially indexed only by file metadata without parsing pixel dimensions during the startup scan.
   - Fixed in `src/ModelTextureViewer.cpp` via `FastReadPngDimensions()`:
     - **Filename Token Extraction**: Scans the filename for the dimension token (e.g., `_FMT_VQ_ARGB1555_128x128_` → `128x128`).
     - **PNG IHDR Stream Fallback**: If the filename lacks dimensions, the tool reads the 24-byte PNG IHDR header (bytes 16..19 = Width, bytes 20..23 = Height) in ~0.05 ms.
2. **Auto-Selection on Filter & Search**:
   - When filtering by category or typing in the search box, `ApplyTextureFilter()` automatically selects and focuses row 0, firing `OnTextureSelected(0)` so the Info card and 3D preview update synchronously.
3. **Model Selection Dimension Resolution**:
   - When switching models via the Model ComboBox, `SelectModelByName()` computes the model's bounding dimensions, vertex count, triangle count, and queries the primary texture's dimensions from disk, updating the Info card immediately without requiring any item clicks.
4. **Elimination of Single-Submesh Texture Hijack**:
   - Removed a legacy fallback that forcefully slapped whatever texture was highlighted in the list onto single-mesh bodies (the root cause of tire textures appearing on car hoods or train bodies). Submeshes now strictly respect their native material `.mtl` texture bindings.

### 7.2 Raw-UV Texture Upload (Unflipped)
The viewer implements the exact same convention as the game and the extractor (§2.2): textures are uploaded **unflipped**, so GL texture coordinate `v = 0` samples the first row of the PNG — the same row the engine samples at `v = 0`:

```cpp
// UV CONVENTION: game UV v=0 = texture TOP row (D3D semantics).
// Textures are uploaded UNFLIPPED (GL v=0 = first row of the PNG = top row) and
// the extractor writes RAW game UVs, so no vertical row flip is applied here.
```

Sampling uses **GL_REPEAT** wrap in both axes, matching the engine's global REPEAT addressing so pre-tiled UVs (> 1.0) render identically. The practical upshot: **the viewer is the ground-truth preview** — if a model looks right here, it renders right in game (§2.2 explains why DCC viewports can disagree).

### 7.3 Applied-Textures Panel, Part Labels & Status Bar
- **"Applied textures (N)" panel** (bottom-left): lists every texture file used by the currently selected model, one row per texture, together with the part label(s) that use it and a **missing** marker for files that could not be resolved on disk. Clicking a row selects that texture in the main texture list (the category filter is relaxed automatically if the texture is currently filtered out) — perfect for jumping straight to the texture a specific part uses.
- **Per-material multi-texture rendering**: each submesh renders with its own bound texture from its `.mtl`/assembly binding; untextured records (`texIndex = -1`) and unresolvable textures fall back to a grey/checker material.
- **Part labels from assembly JSON**: when a model has a `{Model}_assembly.json` next to it (extractor output), submeshes are labeled with the assembly's `part_label` / `part_name` (e.g. `Torso`, `Wheel_FL`, `Pelvis`) instead of raw group names. The engine has no part names of its own (§3.1) — these are the extractor's geometry-derived labels.
- **Status bar**: `Parts: N | Textures used: N (N missing)` summarizes the current model at a glance.

### 7.4 Parser Selftest CLI (`--selftest`)
Hidden headless mode for pipeline checks and CI:

```bash
ModelTextureViewer.exe --selftest path/to/model.obj
```

Loads the OBJ with the exact in-app parser and prints: the discovered assembly JSON (if any), one line per submesh (`part`, material, texture, vertex/triangle counts, `MISSING` flags), distinct part-label and texture counts, the missing-texture list, the **first raw `vt` line** (to verify the raw-UV convention of §2.2), and total vertex/triangle counts. Exits with code 0 when launched with redirected output.

---

## 8. Summary Workflow Checklist

| Step | Operation | Tool / Command |
|---|---|---|
| **1. Extract** | Restore vanilla textures + export engine-exact 3D data | `python build_extract_textures.py --restore-only` then `python extract_3d_models.py` |
| **2. Inspect** | Browse models, parts, and applied textures | Launch `ModelTextureViewer.exe` (or `--selftest model.obj`) |
| **3. Model** | Create/edit mesh in Blender or any DCC | Triangulate; one material group per texture; any up-axis (§2.3) |
| **4. Textures** | Edit/assign textures (optional) | See the **Crazy Taxi Texture Modding Guide**; or just pass `--texture img.png` |
| **5. Dry run** | Preview the whole pipeline, write nothing | `python replace_3d_model.py --replace <Model> m.obj --dry-run` |
| **6. Inject** | One-command replacement with backup + verification | `python replace_3d_model.py --replace <Model> m.obj` |
| **7. Splines** | Dump / export / edit / inject routes | `python spline_tool.py --dump splines` → `--export-obj 10` → `--import-obj 10 r.obj --inject` |
| **8. Restore** | Roll back any injection | Rename `polDC0.bin.bak_<YYYYMMDD_HHMMSS>` back to `polDC0.bin` |
| **9. Play** | Launch game with mod active | Start `Crazy Taxi.exe` (Steam) |
