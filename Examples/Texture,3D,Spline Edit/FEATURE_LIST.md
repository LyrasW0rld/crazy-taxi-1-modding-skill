# Lyra's Texture & 3D Model Mod (Crazy Taxi PC)
## Complete Feature List & Technical Documentation

**Version:** 2.0.0 (High-Speed Real-Time Edition)  
**Target Platform:** Windows x86 (Crazy Taxi 2002 / Steam release)  
**Author:** Lyra & Antigravity Advanced Agentic Engineering  

---

### 1. Real-Time In-Game Texture Replacement
* **Zero-Restart Live Reloading:** Swap, edit, or upgrade any texture in standard PNG/JPG formats and instantly view the changes in-game without restarting the game or losing race progress.
* **Dual-Layer Replacement Architecture:**
  * **Master Texture Table Injection:** Hooks into the global Sega master descriptor array at `0x01B09938` (`masterBase`) with dynamic index offset mapping (`0x01B7A6E0`), replacing active stage and model textures in VRAM.
  * **Direct3D 9 IAT Hooks:** Intercepts `D3DXCreateTextureFromFileInMemoryEx` (`0x005342B4`) for JPK compressed UI packages (`LITTLE_ENDIAN/*.jpk`), high-resolution HUD sprites, and standalone DDS/PNG menu art.
* **Smart Binary Precompilation Cache:**
  * Converts high-resolution PNG textures into optimized binary cache files (`.cache`) with precomputed mipmaps and exact pixel formats.
  * Instantaneous boot times on subsequent launches (0 ms reload for cached assets).
  * Robust, flicker-free progress overlay with graceful multi-threaded destruction that never gets stuck on screen.
* **Universal Color & Alpha Format Handling:**
  * Native hardware support for Sega Dreamcast and PC color formats: **RGB565** (strict 100% opaque), **ARGB1555** (1-bit punch-through alpha), **ARGB4444**, and **DXT1/DXT5** compressed textures.
  * Precise color-key transparency (magenta `#FF00FF`) preserving authentic transparency without clipping opaque textures.

---

### 2. Real-Time 3D Model Replacement & Memory Injection
* **Live In-Memory Geometry Patching:**
  * Replaces 3D model geometry live in running RAM at `0x0C390000` (`polDC0.bin` base) using dynamic page permissions (`VirtualProtect` `PAGE_EXECUTE_READWRITE`).
  * Changes take effect instantly on models rendered by Sega's `dispatch_render_passes` (`nlObjPut` @ `0x00497160`).
* **Disk Synchronization & Safety Backups:**
  * Automatically synchronizes changes to `<gameDir>\data_little\polDC0.bin` so modifications persist across game sessions.
  * Creates an untouched vanilla backup (`polDC0.bin.bak`) on first initialization.
  * Built-in rollback functionality restores original factory models to disk and memory in one click.
* **Strict Chunk Budget Protection & Zero Padding:**
  * Strict chunk budget enforcement (`entry.maxBytes`) prevents geometry files from overflowing into adjacent game records (preventing startup crashes or heap corruptions).
  * Automatically zero-pads injected chunks up to `maxBytes` to ensure clean boundaries and avoid stale record data.
  * Intelligent `extracted_manifest.json` tracking ensures untouched vanilla reference files are never recompiled, while any modified or custom file (in `3D\custom` or edited in `3D\characters` / `3D\parts`) is instantly detected and injected.
* **Universal Wavefront OBJ Compiler:**
  * Ingests custom `.obj` files created in Blender, 3ds Max, Maya, or ZBrush.
  * Handles arbitrary n-gon and quad faces via automatic fan triangulation.
  * Parses associated `.mtl` material libraries and resolves material-to-texture mappings (`texDC0` indices).
* **Automatic Scaling & Coordinate Conversion:**
  * **Auto-Rescaling:** Automatically measures bounding dimensions and scales imported models to fit the exact bounding sphere and collision profile of the vanilla vehicle or character.
  * **Coordinate Transformation:** Automatically converts standard Z-up DCC coordinate systems into Crazy Taxi's native right-handed Y-up coordinate frame (`x, z, -y`).
* **Extensive Model Registry & Modular Character Assemblies:**
  * **Individual Character Body Parts:** High-fidelity modular extraction of all character body parts as separate independent OBJ files (`Axel1`–`Axel15`, `BDJoe1`–`BDJoe15`, `Gena1`–`Gena15`, `Gus1`–`Gus8`). Preserves Sega Ninja joint coordinate systems, eliminating overlapping glitches and enabling drop-in replacement of any individual limb or head.
  * **Player Taxis:** Full body, steering wheels, mirrors, driver figures, and individual wheels (`Axel_Taxi`, `BDJoe_Taxi`, `Gus_Taxi`, `Gena_Taxi`).
  * **Traffic Vehicles:** 12 vehicles supported, including civilian sedans, family station wagons, delivery trucks, city buses, garbage trucks, highway coaches, ambulances, SFPD police cars, and **San Francisco Cable Cars** (`Traffic_Cable_Car`).
  * **Street Furniture:** City benches, fire hydrants, street lamps, and traffic signs.

---

### 3. Mod Menu Integration & Keyboard Hotkeys
* **Seamless In-Game GUI Integration:**
  * Fully integrated with `Framework_MOD.dll` in-game ImGui overlay.
  * Clicking **"Reload Textures"** in the mod menu triggers unified hot-reloading for both textures and 3D models simultaneously.
* **Exported C-ABI Function Interface:**
  * `ReloadTextureMod()` — Synchronously reloads textures and compiles/injects custom 3D models.
  * `Reload3DModels()` / `Apply3DModels()` — Dedicated hot-reloading for custom 3D models.
  * `RestoreOriginal3DModels()` — Restores vanilla 3D models from backup to disk and live memory.
  * `FlushTextureCache()` — Wipes precompiled binary caches and forces a full re-import.
  * `Set3DModEnabled(bool)` / `Get3DModEnabled()` — Enables or disables 3D model replacement.
  * `SetTextureModEnabled(bool)` / `GetTextureModEnabled()` — Enables or disables texture replacement.
* **Configurable Hotkeys (via `LyrasTextureMod.ini`):**
  * **F9** (`VK_F9`, Keycode 120): Instant Full Reload (Textures + 3D Models).
  * **F8** (`VK_F8`, Keycode 119): Dedicated 3D Model Reload.
  * **F10** (`VK_F10`, Keycode 121): Restore Vanilla Textures.

---

### 4. High-Fidelity Native Asset Extraction
* **Zero-Empty OBJ Guarantee:**
  * Submesh and assembly exporters verify non-zero triangle counts before writing files.
  * Prevents generation of empty 2-line header files.
* **Optimized Stage Geometry Extraction:**
  * Intelligent chunk scanner jumps past completed model records, eliminating duplicate meshes and empty vertex slivers.
* **Multi-Part Assembly JSON Descriptors:**
  * Generates clean assembly manifests (`<Vehicle>_assembly.json`) with relative translation offsets for wheels and accessories.
* **Reverse Texture-to-Model Index:**
  * Outputs `3D_texture_to_model_index.json` cross-referencing every texture filename with its corresponding 3D model and stage usage.

---

### 5. Reverse Engineering & Offset Documentation
* **Modernized `offset_list.json`:**
  * Updated with high-level `"3D Asset"` entries for all player taxis, character models, cable cars, traffic vehicles, and stage containers (without cluttering individual limbs/body parts).
  * Updated with unified `"Textures"` category mapping master tables, stage tables, archive containers, and sprite packages.
* **Complete Standalone Inspection Suite:**
  * `ModelTextureViewer.exe` for interactive 3D model inspection, texture mapping visualization, and assembly validation.

---

### 6. Start Screen & Input Device Synchronization Engine
* **Clean Single-Device UI Rendering (Start Screen Glitch Fix):**
  * Completely resolves the vanilla PC port bug where controller and mouse icons displayed simultaneously and glitched/overlapped into each other on the title/start screen.
  * Enforces strict mutual exclusivity for the navigation prompt sprite at coordinates `(489, 232)` (`sub_4F86C0`), guaranteeing only the active device's icon (Mouse or Controller) is rendered.
* **In-Memory Keycap Offset Patch (`sub_48F3F0`):**
  * Fixes Sega's inverted offset bug at `0x0048F5BA` and `0x0048F5EA` where Keyboard input erroneously loaded Xbox 360 button surfaces (`+0x10`) and Controller input loaded Keyboard surfaces (`+0x0C`).
  * Patches live memory to ensure Keyboard mode strictly displays Keyboard keycaps and Controller mode displays Xbox prompts.
* **Intelligent Analog Deadzone & Jitter Filter (`sub_494E30`):**
  * Introduces a 20% analog deadzone threshold on controller stick axes (axes 1, 2, 3, 4). Sub-deadzone resting stick drift is filtered out, preventing connected controllers from stealing input focus.
* **Dynamic Cursor Movement Detection:**
  * Monitors real-time mouse movement deltas (`GetCursorPos`) and mouse clicks (`VK_LBUTTON`, `VK_RBUTTON`, `VK_MBUTTON`). Moving the mouse instantly and seamlessly switches UI prompts to Keyboard/Mouse mode.
* **State Hysteresis & Anti-Oscillation Debounce:**
  * Prevents rapid 60 Hz frame-to-frame bouncing between devices, ensuring clean, stable UI icons across all menus.

