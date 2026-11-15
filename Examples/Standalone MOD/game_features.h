#pragma once
#include <windows.h>
#include <d3d9.h>

extern IDirect3DDevice9* g_pActiveD3DDevice;
void SyncSegaDevicePointers(IDirect3DDevice9* pDevice);

struct Vector3
{
    float x, y, z;
};

// Memory Addresses (reversed from Crazy Taxi.exe, IDA-verified)
constexpr uintptr_t ADDR_GAME_FLAG          = 0x016a2cfc; // 3 = Active Gameplay Session, 0 = Menus/Attract
constexpr uintptr_t ADDR_GAME_MODE          = 0x016a2818; // 4 = Arcade, 0xE (14) = Original / Crazy Box
constexpr uintptr_t ADDR_VEHICLE_STATE      = 0x01661474; // Camera status (8 = Attract AI Demo, < 8 = Player Controlled)
constexpr uintptr_t ADDR_PLAYER_X           = 0x01661574; // Camera look-at X (0 in menus; used for menu detection)
constexpr uintptr_t ADDR_PLAYER_Y           = 0x01661578; // Camera look-at Y
constexpr uintptr_t ADDR_PLAYER_Z           = 0x0166157c; // Camera look-at Z
constexpr uintptr_t ADDR_PLAYER_FALL_SPD    = 0x016614b0; // Camera prev look-at Y (UI diagnostics; seeded to the teleport target by TeleportTaxi)

// Engine Fixes & Camera Debug Hotkeys
constexpr uintptr_t ADDR_CAM_CHEATS_ENABLE     = 0x01b7a7e8; // uint32 enableCameraCheats
constexpr uintptr_t ADDR_CAM_CHEATS_MODE       = 0x01b7a7ec; // uint32 requestedCameraMode (1=Reset, 2=Cinematic, 3=FirstPerson)
constexpr uintptr_t ADDR_NATIVE_SPEEDOMETER    = 0x01b7a7fc; // uint32 drawSpeedometer (1=draw Sega native km/h)

// Real Physics Speed Clamp Constants (.rdata)
constexpr uintptr_t ADDR_VEHICLE_SPEED_LIMIT_DBL = 0x0059fc78; // double 14.0 vehicle physics speed clamp
constexpr uintptr_t ADDR_VEHICLE_SPEED_LIMIT_FLT = 0x005a06c0; // float 14.0 vehicle physics speed clamp

// Surface Material / FFB Telemetry
constexpr uintptr_t ADDR_SURFACE_GROUND_EFFECT = 0x01eb0d74; // uint8 ground surface effect ID
constexpr uintptr_t ADDR_WALL_COLLISION_LEFT   = 0x0166214c; // float left wall collision force
constexpr uintptr_t ADDR_WALL_COLLISION_RIGHT  = 0x016621d0; // float right wall collision force
constexpr uintptr_t ADDR_ORIGINAL_RUMBLE       = 0x016ef93b; // uint8 Sega native rumble flag
constexpr uintptr_t ADDR_CRAZY_MOVES_1         = 0x016ef892; // uint8 Crazy moves bitmask
constexpr uintptr_t ADDR_CRAZY_MOVES_2         = 0x016ef893; // uint8 Crazy moves bitmask

// Sega Native Distance Accumulators
constexpr uintptr_t ADDR_SEGA_DISTANCE_ROUND   = 0x016a41cc; // float round distance accumulator
constexpr uintptr_t ADDR_SEGA_DISTANCE_TOTAL   = 0x016a41d0; // float session distance accumulator

// Real vehicle physics state (root transform & dynamics block of sub_519890)
constexpr uintptr_t ADDR_VEHICLE_ROOT_X     = 0x01661fa8; // Vehicle root position X (real taxi position)
constexpr uintptr_t ADDR_VEHICLE_ROOT_Y     = 0x01661fac; // Vehicle root position Y (+Y is UP)
constexpr uintptr_t ADDR_VEHICLE_ROOT_Z     = 0x01661fb0; // Vehicle root position Z
constexpr uintptr_t ADDR_VEHICLE_BLOCK_X    = 0x01661f30; // Dynamics block position X (synced with root)
constexpr uintptr_t ADDR_VEHICLE_BLOCK_Y    = 0x01661f34;
constexpr uintptr_t ADDR_VEHICLE_BLOCK_Z    = 0x01661f38;
constexpr uintptr_t ADDR_VEHICLE_PREV_X     = 0x01661fbc; // Previous-frame position X (speed derivation source)
constexpr uintptr_t ADDR_VEHICLE_PREV_Y     = 0x01661fc0;
constexpr uintptr_t ADDR_VEHICLE_PREV_Z     = 0x01661fc4;
constexpr uintptr_t ADDR_VEHICLE_DISP_X     = 0x01661f3c; // Frame displacement X
constexpr uintptr_t ADDR_VEHICLE_DISP_Y     = 0x01661f40;
constexpr uintptr_t ADDR_VEHICLE_DISP_Z     = 0x01661f44;
constexpr uintptr_t ADDR_VEHICLE_VEL_X      = 0x0166202c; // Velocity X (= speed * sin(heading))
constexpr uintptr_t ADDR_VEHICLE_VEL_Y      = 0x01662030; // Vertical velocity
constexpr uintptr_t ADDR_VEHICLE_VEL_Z      = 0x01662034; // Velocity Z (= speed * cos(heading))
constexpr uintptr_t ADDR_VEHICLE_HEADING_BAM  = 0x01662024; // uint16 heading (0..65535 = 0..360 deg)
constexpr uintptr_t ADDR_VEHICLE_ATTITUDE_BAM = 0x01661fb4; // pitch LO16 | yaw HI16
constexpr uintptr_t ADDR_VEHICLE_ROLL_REAL_BAM = 0x01661fb8; // roll LO16
constexpr uintptr_t ADDR_VEHICLE_SPEED_SCALAR = 0x0166208c; // float collision restitution recoil speed buffer (0.0 during normal driving)

// T3 teleport fix: off-world respawn (sub_513E80 0x00513E80, runs at the start of every
// physics step) nulls the velocity mirror (+264/268/272) alongside VEL/DISP/SPEED, so a
// teleport latch must re-assert them too or the mirrors keep the stale pre-teleport
// velocity alive for the dynamics sync.
constexpr uintptr_t ADDR_VEHICLE_VEL_MIRROR_X      = 0x01662038; // velocity mirror X (+264), respawn-nullled
constexpr uintptr_t ADDR_VEHICLE_VEL_MIRROR_Y      = 0x0166203c; // velocity mirror Y (+268), respawn-nullled
constexpr uintptr_t ADDR_VEHICLE_VEL_MIRROR_Z      = 0x01662040; // velocity mirror Z (+272), respawn-nullled
// Prev-frame orientation (+152/154/156): sweep start pose read by sub_5146C0 (layout
// matches attitude +132/134/136: pitch LO16 | yaw HI16 | roll). A teleport must point it
// at the target pose or the swept-sphere solver sweeps across the whole teleport delta.
constexpr uintptr_t ADDR_VEHICLE_PREV_ATTITUDE_BAM = 0x01661fc8; // prev pitch LO16 | prev yaw HI16
constexpr uintptr_t ADDR_VEHICLE_PREV_ROLL_BAM     = 0x01661fcc; // prev roll

constexpr uintptr_t ADDR_TOTAL_MONEY        = 0x016f10e0; // Total accumulated player cash in cents ($1.00 = 100)
constexpr uintptr_t ADDR_CURRENT_FARE_TIPS  = 0x016a41c8; // Current passenger tip pool in cents
constexpr uintptr_t ADDR_GAME_TIMER         = 0x016a41a4; // uint32 (In-Game Round Timer in frames, 60 fps: 3000 = 50s, 356400 = 99m)
constexpr uintptr_t ADDR_FARE_CUTSCENE      = 0x016f2164; // Passenger cutscene animation state (> 0 during pickup/dropoff)
constexpr uintptr_t ADDR_PASSENGER_OBJ      = 0x016f21b0; // Active passenger object pointer
constexpr uintptr_t ADDR_VEHICLE_ACTIVE_INDEX = 0x01661570; // Camera mode selector (1 = chase, 2 = rear view, 4 = attract)

constexpr uintptr_t ADDR_CAM_EYE_X          = 0x01661494;
constexpr uintptr_t ADDR_CAM_EYE_Y          = 0x01661498;
constexpr uintptr_t ADDR_CAM_EYE_Z          = 0x0166149c;
constexpr uintptr_t ADDR_CAM_TARGET_X       = 0x016614f8;
constexpr uintptr_t ADDR_CAM_TARGET_Y       = 0x016614fc;
constexpr uintptr_t ADDR_CAM_TARGET_Z       = 0x01661500;
constexpr uintptr_t ADDR_CAM_ROLL           = 0x016614c8; // 16-bit BAM integer (0..65535 = 0..360 deg)
constexpr uintptr_t ADDR_CAM_VIEW_MATRIX    = 0x01661530; // 16 floats (D3DMATRIX view matrix, rebuilt every frame)

// Camera smoothing state (B1-E): persistent eye is the camera_update smoothing source,
// 0x016614AC/B0/B4 is the prev-frame look-at (delta source in camera_update case 0). A
// teleport must seed both, otherwise the camera glides from the old position and the
// teleport reads as "snapped back".
constexpr uintptr_t ADDR_CAM_PERSISTENT_EYE_X = 0x016614ec;
constexpr uintptr_t ADDR_CAM_PERSISTENT_EYE_Y = 0x016614f0;
constexpr uintptr_t ADDR_CAM_PERSISTENT_EYE_Z = 0x016614f4;
constexpr uintptr_t ADDR_CAM_PREV_LOOKAT_X    = 0x016614ac;
constexpr uintptr_t ADDR_CAM_PREV_LOOKAT_Y    = 0x016614b0;
constexpr uintptr_t ADDR_CAM_PREV_LOOKAT_Z    = 0x016614b4;

constexpr uintptr_t ADDR_STUNT_TIP_ADD      = 0x004a8de7; // add [0x16a41c8], ecx (stunt reward finalized)
constexpr uintptr_t ADDR_CAM_UPDATE_CALL    = 0x0051f582;
constexpr uintptr_t ADDR_CAM_LOOKAT_FN      = 0x0049b200;

// Input & Controller State Addresses
constexpr uintptr_t ADDR_MAIN_INPUT_LOOP   = 0x004ea3a8; // call 0x004F2ED0 (per-frame input commit in main loop)
constexpr uintptr_t ADDR_F2ED0_FN          = 0x004f2ed0;
constexpr uintptr_t ADDR_INPUT_BUTTONS      = 0x016a2848; // uint32 bitmask (raw input buffer)
constexpr uintptr_t ADDR_INPUT_BUTTONS_2    = 0x016a287c; // uint32 bitmask (filtered/latched input buffer used by physics/camera)
constexpr uintptr_t ADDR_INPUT_BUFFER_DELTA = 0x34;       // Offset between player 0 raw buffer and latched buffer
constexpr uintptr_t ADDR_INPUT_STEER_AXIS   = 0x016a2858; // int16 signed (-32767..32767)
constexpr uintptr_t ADDR_INPUT_STEER_FLOAT  = 0x016a2d10; // float live steering (-1.0f .. +1.0f) used by vehicle physics
constexpr uintptr_t ADDR_INPUT_THROTTLE_FLOAT= 0x016a2d04; // float live throttle (0.0f .. 1.0f) used by vehicle physics
constexpr uintptr_t ADDR_INPUT_STEER_FLOAT_RAW = 0x016a2810; // float unclipped steering computed in sub_4E99A0
constexpr uintptr_t ADDR_VEHICLE_WHEEL_STEER_BAM= 0x01662044; // int16 front wheel steer angle (-4352 .. +4352)
constexpr uintptr_t ADDR_WHEEL_FL_STEER_BAM = 0x01662150; // int16 front-left wheel steer angle BAM
constexpr uintptr_t ADDR_WHEEL_FR_STEER_BAM = 0x016621d4; // int16 front-right wheel steer angle BAM
constexpr uintptr_t ADDR_WHEEL_RL_STEER_BAM = 0x01662258; // int16 rear-left wheel steer angle BAM (4WS)
constexpr uintptr_t ADDR_WHEEL_RR_STEER_BAM = 0x016622dc; // int16 rear-right wheel steer angle BAM (4WS)
constexpr uintptr_t ADDR_VEHICLE_YAW_RATE   = 0x016620c8; // int16 vehicle yaw rate (BAM/tick, integrated by sub_512F00)
constexpr uintptr_t ADDR_LOAD_CHUNKS_RADIUS = 0x005a04ec; // float 1800.0f native chunk query radius in sub_519890
constexpr uintptr_t ADDR_INPUT_ACCEL_AXIS   = 0x016a285a; // uint16 (0..65535)
constexpr uintptr_t ADDR_INPUT_BRAKE_AXIS   = 0x016a285c; // uint16 (0..65535)
constexpr uintptr_t ADDR_INPUT_BTN_LOOK_DEST= 0x016a2860; // uint8
constexpr uintptr_t ADDR_INPUT_BTN_ACCEL    = 0x016a2863; // uint8 (0 or 1)
constexpr uintptr_t ADDR_INPUT_BTN_BRAKE    = 0x016a2864; // uint8 (0 or 1)
constexpr uintptr_t ADDR_INPUT_BTN_GEAR_D   = 0x016a2865; // uint8 (0 or 1)
constexpr uintptr_t ADDR_INPUT_BTN_GEAR_R   = 0x016a2866; // uint8 (0 or 1)
constexpr uintptr_t ADDR_INPUT_EDGE_DRIVE   = 0x016a2869; // uint8 (rising edge)
constexpr uintptr_t ADDR_INPUT_EDGE_REV     = 0x016a286a; // uint8 (rising edge)
constexpr uintptr_t ADDR_INPUT_EDGE_DRIVE_F = 0x016a286d; // uint8 (falling edge)
constexpr uintptr_t ADDR_INPUT_EDGE_REV_F   = 0x016a286e; // uint8 (falling edge)
constexpr uintptr_t ADDR_INPUT_GEAR_STATE   = 0x016a2873; // uint8 (0/2 = Drive, 1 = Reverse)
constexpr uintptr_t ADDR_INPUT_GEAR_SETTING = 0x016a2874;

// Sega Render-Pass Flag (read by UI diagnostics; never written by the mod)
constexpr uintptr_t ADDR_GROUND_COLLISION   = 0x01661596; // uint8 render-pass flag set by collision_world
constexpr uintptr_t ADDR_VEHICLE_PITCH_BAM  = 0x0166147a; // Camera attitude pitch BAM (UI diagnostics only)
constexpr uintptr_t ADDR_VEHICLE_ROLL_BAM   = 0x0166147c; // Camera attitude roll BAM (UI diagnostics only)

// DirectInput Keyboard Poller in Crazy Taxi.exe
constexpr uintptr_t ADDR_KEYBOARD_POLL_FN   = 0x00486390;

// Master Camera Far-Z Clip Plane
constexpr uintptr_t ADDR_FAR_Z_RDATA        = 0x005a0150; // Default: 15000.0f

// Vehicle Physics Master Loop Call Sites (sub_519890 = real vehicle physics step)
constexpr uintptr_t ADDR_VEHICLE_UPDATE_CALL_1 = 0x005113b7; // call 0x00519890
constexpr uintptr_t ADDR_VEHICLE_UPDATE_CALL_2 = 0x00512029; // call 0x00519890
constexpr uintptr_t ADDR_VEHICLE_UPDATE_FN     = 0x00519890;
constexpr uintptr_t ADDR_SPEED_MULT_PATCH      = 0x00519e90; // 18-byte window in sub_519890 (test edi,80047FFEh / je / mov [0x16622E0],0)
constexpr uintptr_t ADDR_SPEED_MULT_RESUME     = 0x00519ea2; // Resume address after the patched window

// Sega Secret Developer Cheats & Modes
constexpr uintptr_t ADDR_SEGA_UNLOCK_ALL    = 0x0164fa58; // uint8 (0xFF = Unlock all bikes and crazy box)
constexpr uintptr_t ADDR_SEGA_UNLOCK_MASK   = 0x016aa3c0; // uint32 (0xFFFFFFFF = Unlock all characters/vehicles)
constexpr uintptr_t ADDR_SEGA_CHAR_SELECT   = 0x016a9b30; // int (0..3 = Cabs, 4..7 = Bikes)
constexpr uintptr_t ADDR_SEGA_ANOTHER_DAY   = 0x01662304; // int (1 = Another Day Mode)
constexpr uintptr_t ADDR_SEGA_NO_ARROWS     = 0x016ef0bc; // int (1 = No Arrows)
constexpr uintptr_t ADDR_SEGA_NO_DEST_MARK  = 0x016eeea0; // int (1 = No Destination Mark)
constexpr uintptr_t ADDR_SEGA_BIKE_MODE     = 0x016aa3c4; // int (bit 0x20 = Bike Mode)

// Verified streaming radius constants in .rdata (patched values must stay capped so the
// 1152-slot chunk table keeps recycling via TTL eviction - see UpdateChunkStreaming)
constexpr uintptr_t ADDR_CHUNK_DIST_CAMERA  = 0x005a10a8; // 850.0f replay/demo stream radius (replay-only, harmless)
constexpr uintptr_t ADDR_CHUNK_DIST_FAR     = 0x0059f9f8; // 450.0f pickup/snapshot distance (NOT a visibility cull; harmless)
constexpr uintptr_t ADDR_CHUNK_DIST_SEC_CAM = 0x005a07d8; // 1700.0f non-gameplay view radius + map-1 preload radius
constexpr uintptr_t ADDR_CHUNK_DIST_SEC_FAR = 0x005a10a0; // 900.0f in-game view radius, regime 2 (dword_1662308 != 0)
constexpr uintptr_t ADDR_SECTOR_STREAM_RAD  = 0x005a0908; // 1500.0f sector streamer radius (multi-use)

// In-game view/activation radius sources (sub_4AA130 3-way branch -> flt_16F59DC).
// flt_16F59DC feeds the sector visibility bitmask, clip planes (-(R+30)), ambient
// ped/traffic emitters, actor cull/despawn and the passenger spawn ring - scaling it
// scales all of those with it. configure_navigation_arrow copies 0x5A0788/0x5A078C
// into flt_16A673C per session, so both must be in place BEFORE a session starts
// (written in InitGameFeatures and refreshed every frame).
constexpr uintptr_t ADDR_VIEW_RAD_NORMAL     = 0x005a0788; // 1100.0f normal gameplay (core visibility gate)
constexpr uintptr_t ADDR_VIEW_RAD_INIT       = 0x005a0390; // 1200.0f session init + regime-2 despawn radius
constexpr uintptr_t ADDR_PASSENGER_SPAWN_RAD = 0x005a078c; // 700.0f passenger spawn radius source (map 1)

// Despawn / convoy / actor-update radii (must stay >= the scaled spawn ring R+30)
constexpr uintptr_t ADDR_SPAWN_DIST_TRAFFIC = 0x005a04ec; // 1800.0f actor update radius (regime normal) + traffic convoy
constexpr uintptr_t ADDR_SPAWN_DIST_PED     = 0x005a04e8; // 2000.0f streaming actor despawn radius + map-0 preload
constexpr uintptr_t ADDR_STREAM_SECTOR_RAD_1 = 0x0059f9e4; // 1000.0f actor update radius, regime 2

// Mode 1 Active Gameplay Sector Radii in .rdata (load_chunks_around radii in 0x004AA650)
constexpr uintptr_t ADDR_STREAM_SECTOR_RAD_2 = 0x005a0904; // 800.0f (unverified multi-use, kept)
constexpr uintptr_t ADDR_STREAM_SECTOR_RAD_3 = 0x005a0900; // 600.0f (unverified multi-use, kept)
constexpr uintptr_t ADDR_STREAM_SECTOR_RAD_4 = 0x005a00f0; // 400.0f (unverified multi-use, kept)

// Chunk GEOMETRY render cull: flt_1B7A748 scales the mode-1 render radius (x2500),
// the map-0 cull (x3000) and the sub_4DBF90 thresholds (dist/S < T). Game-writable
// .data (sub_489450 writes 2.0), so no VirtualProtect is needed.
constexpr uintptr_t ADDR_CHUNK_RENDER_FACTOR  = 0x01b7a748; // 2.0f global chunk render radius factor
constexpr uintptr_t ADDR_CHUNK_THRESH_MODE0_1 = 0x005a0320; // 2500.0f sub_4DBF90 chunk render threshold
constexpr uintptr_t ADDR_CHUNK_THRESH_MODE0_2 = 0x005a02ec; // 2800.0f sub_4DBF90 chunk render threshold
constexpr uintptr_t ADDR_CHUNK_THRESH_MODE0_3 = 0x005a0334; // 3000.0f sub_4DBF90 chunk render threshold

// Chunk texture load priority TTL coefficients (doubles): TTL = base - k * dist.
// Vanilla k makes every chunk beyond ~450 distance priority 0, so far chunks starve
// behind near re-requests in the one-chunk-per-frame texture pump.
constexpr uintptr_t ADDR_CHUNK_TTL_DIST_K   = 0x0059faf0; // double 0.2 (request_chunk_with_distance_ttl)
constexpr uintptr_t ADDR_CHUNK_TTL_RENDER_K = 0x0059fd80; // double 0.4 (request_and_render_chunk)

// NEVER patch these .rdata floats (IDA-verified, NOT stream radii):
//   0x005A07D4 / 0x005A07D0 / 0x005A07CC = preload anchor POSITION X/Y/Z (375, 1504, 1696)
//     in preload_map_spawn_chunks 0x005044B0 - patching kills the per-tick preload.
//   0x0059F9E8 = clip plane d for course 15 + UI/text positions (NOT fog; no distance
//     fog writer exists in the engine), 0x0059F9E0 = nav arrow/mission distances,
//   0x005A0D54 = intro camera zoom start, 0x005A107C = cable car parameter,
//   0x005A04C0 = unverified single comparison.

// Sega Master Chunk Loading & Collision Functions
constexpr uintptr_t ADDR_LOAD_CHUNKS_AROUND   = 0x00504090; // load_chunks_around (int __cdecl(float,float,float,float), y unused)
constexpr uintptr_t ADDR_CHUNK_EVICT_FN       = 0x005051d0; // evict_and_bind_textures: async loader + TTL evictor (__cdecl void())
// T3 teleport fix: wheel-raycast ground probe. raycast_down_collision (float* __cdecl(
// float pos[3], float range)) returns the static result struct 0x01EB0CE8: +24 = hit
// height, +28 = hit type with -1 = miss. Selection = highest walkable (type != 13)
// surface at or below the query XZ (only surfaces above the query are limited to a
// climbable (v9, v9+range] window); a miss means no walkable polygon at this XZ. Reads
// the SESSION-INIT-resident collision grid (init_stage_collision_grid_and_relocate_
// 50d050 0x0050D050) - independent of chunk streaming (T3 B.1).
constexpr uintptr_t ADDR_RAYCAST_DOWN_COLLISION = 0x0050c6d0; // raycast_down_collision_50c6d0
constexpr uintptr_t ADDR_RAYCAST_RESULT         = 0x01eb0ce8; // result scratch: +24 hit height, +28 hit type (-1 miss)
constexpr uintptr_t ADDR_CHUNK_STREAM_QUEUE_FN  = 0x005045e0; // process_chunk_stream_queue (int __cdecl(void)): loads exactly one queued chunk
constexpr uintptr_t ADDR_RAYCAST_SECTOR_GRID  = 0x0050cbd0; // raycast_swept_sphere_against_sector_grid (__usercall, 1st arg in EAX; 0 = no hit)
constexpr uintptr_t ADDR_PAIR_CALLBACK_TABLE  = 0x01663390; // 21x21 collision pair callback table (.data, writable)
constexpr uintptr_t ADDR_PAIR_TABLE_COUNT     = 21 * 21;
constexpr uintptr_t ADDR_CARCAR_PAIR_CALLBACK = 0x00510400; // solve_rigid_body_impulse = vehicle<->vehicle pair callback (0x00510400, IDA-verified)
constexpr uintptr_t ADDR_SOLVE_RIGID_BODY     = 0x00510400; // solve_rigid_body_impulse, MinHook target for Car<->Car collisions
constexpr uintptr_t ADDR_SOLVE_ACTOR_PAIR     = 0x0050ef20; // solve_actor_pair_collision_impulse, MinHook target for Actor<->Prop/Ped
constexpr uintptr_t ADDR_PLAYER_ACTOR_ENTITY  = 0x01661f30; // fixed player taxi actor entity (sub_518F30 registration)
constexpr uintptr_t ADDR_PLAYER_VEHICLE_PTR   = 0x016f78a8; // live player actor pointer (0 in menus - guard required)

// Passenger Pickup Prevention Hook Addresses (Fly Mode zero-collection)
constexpr uintptr_t ADDR_PASSENGER_PICKUP_CALL  = 0x004b862d; // Call site for customer pickup trigger (call 0x004C1810)
constexpr uintptr_t ADDR_PASSENGER_COPY_FN      = 0x004c1810; // Vanilla customer assignment function
constexpr uintptr_t ADDR_PASSENGER_SKIP_TARGET  = 0x004b8689; // Skip customer pickup and continue normally at 0x004B8689

bool InitGameFeatures();
void ShutdownGameFeatures();
void UpdateGameFeaturesPerFrame();
void InitEngineFixes();
void ShutdownEngineFixes();
void UpdateDebugHotkeys();

void InitFFB();
void ShutdownFFB();
void UpdateFFBPerFrame();
void ApplySoundtrackSetting(bool original);
void TriggerFullMapChunkStreaming();

bool IsInGame();
bool GetTaxiPosition(float& x, float& y, float& z); // Reads the real vehicle root transform
bool GetTaxiSpeed(float& speed);
bool GetTaxiMoney(float& totalDollars, float& rideTips);
void SetInGameTimerSeconds(int seconds);
void SetInGameTimerMinutes(int minutes);
int  GetInGameTimerSeconds();
// Teleports the taxi. clampToGroundY (default true) runs the step-2 ground probe and
// settles the latch at hit height +1.0; the '+15m (Unstuck)' button passes false so its
// elevate semantics survive (the probe's unbounded below-window would clamp a +15m
// target straight back to the ground under the taxi).
bool TeleportTaxi(float x, float y, float z, bool clampToGroundY = true);
bool GetGroundCollision();

bool GetCameraEyePosition(float& x, float& y, float& z);
bool GetCameraTargetPosition(float& x, float& y, float& z);

// Freecam & Mouse Look APIs
void TeleportFreecamToTaxi();
void MoveFreecam(float dRight, float dUp, float dFwd);
void ResetMouseLook();
void SetCameraMode(int mode); // 0=Reset/Chase, 1=Cinematic, 2=First-Person, 3=Bumper

// Nitro Energy Tank APIs
float GetNitroEnergy();
float GetNitroMaxEnergy();
bool  IsNitroActive();
bool  IsNitroLockedOut();
float GetNitroDelayTimer();

// Vehicle Physics Hooks
extern "C" void __cdecl PreVehiclePhysics();
extern "C" void __cdecl PostVehiclePhysics();

// DirectInput Hardware Keyboard Buffer Interceptor
extern "C" __declspec(dllexport) void ProcessKeyboardBuffer(BYTE* pBuf);

// Vulkan (DXVK) Inter-Mod APIs
extern "C" {
    __declspec(dllexport) bool __cdecl ModAPI_IsVulkanEnabled();
    __declspec(dllexport) bool __cdecl ModAPI_SetVulkanEnabled(bool enable);
    __declspec(dllexport) bool __cdecl ModAPI_ToggleVulkan();
}
