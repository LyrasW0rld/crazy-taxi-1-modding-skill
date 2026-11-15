#include "game_features.h"

#include "input_hook.h"

#include "config.h"

#include "dc_restoration.h"

#include "logger.h"

#include "engine_fixes.h"

void InitEngineFixes() { EngineFixes::Initialize(); }

void ShutdownEngineFixes() { EngineFixes::Shutdown(); }

void UpdateDebugHotkeys() { EngineFixes::UpdateHotkeys(); }

#include <MinHook.h>

#include <cmath>

static BYTE s_OrigCamCallBytes[5] = { 0 };

static bool s_CamHookInstalled = false;

static BYTE s_OrigTipAddBytes[6] = { 0 };

static bool s_TipHookInstalled = false;

static BYTE s_OrigInputCallBytes[5] = { 0 };

static bool s_InputHookInstalled = false;

static BYTE s_OrigVehUpdate1Bytes[5] = { 0 };

static bool s_VehUpdate1HookInstalled = false;

static BYTE s_OrigVehUpdate2Bytes[5] = { 0 };

static bool s_VehUpdate2HookInstalled = false;

static BYTE s_OrigSpeedMultBytes[5] = { 0 };

static bool s_SpeedMultPatchInstalled = false;

// Vehicle world-collision chain (IDA-verified):

//   sub_5146C0 (chassis & wheels) calls resolve_swept_sphere_slide_and_bounce (0x0050CE40)

//   and cast_motion_sphere_query (0x0050CDA0). Both loop over the INNER query

//   raycast_swept_sphere_against_sector_grid (0x0050CBD0) and have native no-hit paths when

//   it returns 0 (resolve: dword_1EB0DB4/D94 = -1; cast: result+0x1C = -1 = "no ground").

//   Returning 0 from the inner query therefore makes the player taxi fully collision-free

//   with zero manual result emulation. Traffic/props use the separate raycast_down_collision

//   family (0x0050C6D0 -> 0x0050BED0) and stay grounded - no asymmetric floating.

typedef void* tRaycastSectorGridFn; // __usercall (1st arg in EAX) - passthrough via jmp only!

static tRaycastSectorGridFn oRaycastSectorGrid = nullptr;

static bool s_RaycastSectorGridHookInstalled = false;

// Fly writer ownership: true only in frames where the PostVehiclePhysics fly writer
// actually owns the vehicle. The naked hooks below gate collision suppression on this
// flag instead of g_Config.flyMode so vanilla physics keeps ground collision while a
// fare cutscene runs (writer parked - see PostVehiclePhysics).
// Referenced by the naked hooks below (must be declared before first use in inline asm)
static bool  s_FlyHasControl = false;

static bool* s_pFlyMode = &g_Config.flyMode;

static bool* s_pFlyControl = &s_FlyHasControl;


static __declspec(naked) void hkRaycastSectorGrid()

{

    __asm {

        // EAX holds the first (__usercall) argument - must not be clobbered before the jmp!

        push eax

        mov eax, s_pFlyControl

        cmp byte ptr [eax], 0

        pop eax

        jz run_original

        // Suppressed only while the fly writer owns the vehicle: report "no hit" (0 is the
        // function's own no-hit return value). During fare cutscenes the flag is false so
        // ground collision stays active and the taxi does not sink while vanilla physics
        // plays the cutscene.

        xor eax, eax

        ret

    run_original:

        jmp oRaycastSectorGrid

    }

}

// Vehicle<->vehicle collision disable: MinHook on solve_rigid_body_impulse (0x00510400).

// In Crazy Taxi, all vehicles (Player Taxi = 0, Traffic Cars = 1..14) use solve_rigid_body_impulse.

// Early-returning 1 bypasses rigid body impulses and separation pushback completely.

typedef int (__cdecl *tSolveRigidBodyFn)(int a1, int a2);

static tSolveRigidBodyFn oSolveRigidBody = nullptr;

static bool s_SolveRigidBodyHookInstalled = false;

static int __cdecl hkSolveRigidBodyImpulse(int a1, int a2)

{

    if (g_Config.disableTrafficCollision || s_FlyHasControl)

    {

        return 1; // All vehicle collision impulses suppressed

    }

    return oSolveRigidBody(a1, a2);

}

// Actor<->prop/pedestrian collision hook on solve_actor_pair_collision_impulse (0x0050EF20).

// Only actors with category >= 15 (pedestrians, props, obstacles) use this callback.

typedef int (__cdecl *tSolveActorPairFn)(int a1, int a2);

static tSolveActorPairFn oSolveActorPair = nullptr;

static bool s_SolvePairHookInstalled = false;

static int __cdecl hkSolveActorPairCollision(int a1, int a2)

{

    if (s_FlyHasControl)

    {

        // Category 0 is the player taxi (dword_1662324 = actor 1 cat, dword_166232C = actor 2 cat)

        DWORD cat1 = *(DWORD*)0x01662324;

        DWORD cat2 = *(DWORD*)0x0166232C;

        if (cat1 == 0 || cat2 == 0)

            return 1;

    }

    return oSolveActorPair(a1, a2);

}

// Legacy fallback ONLY used when the MinHook solver hook could not be installed: the

// writable .data pair callback table at 0x01663390 is filled once per process by

// reset_and_init_actor_collision_subsystem (0x00510D60) from templates; entries equal to

// solve_actor_pair_collision_impulse are exactly the vehicle<->vehicle pairs. Swapping

// them for a no-op stub keeps world/ground collision (a different subsystem) intact.

static __declspec(naked) void CarCarCollisionStub()

{

    __asm {

        mov eax, 1 // Pair callbacks return 1; the dispatcher ignores the value

        ret

    }

}

// Fly Mode: Customer Pickup Prevention Hook at 0x004B862D

static BYTE s_OrigPassengerPickupBytes[5] = { 0 };

static bool s_PassengerPickupHookInstalled = false;

static __declspec(naked) void hkPassengerPickupCheckNaked()

{

    __asm {

        mov eax, s_pFlyMode

        cmp byte ptr [eax], 0

        jne skip_pickup

        mov eax, ADDR_PASSENGER_COPY_FN // 0x004C1810

        call eax

        ret

    skip_pickup:

        add esp, 4

        // The skipped native inline zeroes the cutscene shot index (0x004B863E); replicate
        // that single store so the skip leaves no stale value behind - nothing else resets
        // 0x016F2164 until the next cutscene starts, and a stale value would keep the
        // mod's cutscene gates latched.
        // Hex literal is MANDATORY here: MSVC inline asm resolves constexpr symbols as
        // memory references to the constant object itself (which the linker places in
        // read-only .rdata), so writing through the symbol faults with 0xC0000005
        // instead of clearing the game flag.

        mov dword ptr ds:[016F2164h], 0 // ADDR_FARE_CUTSCENE

        mov eax, ADDR_PASSENGER_SKIP_TARGET // 0x004B8689

        jmp eax

    }

}

// Sega Chunk Eviction Subsystem Hook at 0x005051D0 (Persistent Whole-Map Chunk Loader)

typedef void (__cdecl *tChunkEvictFn)();

static tChunkEvictFn oChunkEvict = nullptr;

static bool s_ChunkEvictHookInstalled = false;

static void __cdecl hkChunkEvict()

{

    // Pure passthrough: 0x005051D0 (evict_and_bind_textures) is BOTH the only async

    // partition loader (one load per call, bind_world_textures 0x00499240 after each load)

    // AND the TTL evictor that recycles the 1152-slot chunk table. It must never be

    // blocked; the persistent loader instead re-requests chunks around the taxi every

    // frame (see TriggerFullMapChunkStreaming) so their TTL stays refreshed.

    if (oChunkEvict)

        oChunkEvict();

}

static DWORD s_OldFarZProtect = 0;

static bool  s_FarZUnprotected = false;

static DWORD s_OldChunkDistProtect1 = 0;

static DWORD s_OldChunkDistProtect2 = 0;

static DWORD s_OldChunkDistProtect3 = 0;

static DWORD s_OldChunkDistProtect4 = 0;

static DWORD s_OldSectorStreamProtect = 0;

static DWORD s_OldChunkBlockProtect = 0;

static bool  s_ChunkDistUnprotected = false;

static bool  s_ExtendedChunkDistUnprotected = false;

static DWORD s_OldSectorRadProtect1 = 0;

static DWORD s_OldSectorRadProtect2 = 0;

static DWORD s_OldSectorRadProtect3 = 0;

static DWORD s_OldSectorRadProtect4 = 0;

static bool  s_Mode1SectorRadUnprotected = false;

static DWORD s_OldSpeedLimitProtect1 = 0;

static DWORD s_OldSpeedLimitProtect2 = 0;

static bool  s_SpeedLimitUnprotected = false;

static DWORD s_OldSpawnDistProtect1 = 0;

static DWORD s_OldSpawnDistProtect2 = 0;

static bool  s_SpawnDistUnprotected = false;

// Defined below (UpdateChunkStreaming section); also called once from InitGameFeatures

// so the per-session passenger spawn radius sources are in place before session start.

static void ApplyStreamingRadii(float mult, bool persistent);

// Speed multiplier factor consumed by hkSpeedScalarCave (1.0f = numeric no-op).

// Re-armed once per frame in UpdateGameFeaturesPerFrame; the cave applies it exactly once.

static float s_SpeedMultFactor = 1.0f;

// Camera state vectors

static Vector3 s_OrigEye     = { 0.0f, 0.0f, 0.0f };

static Vector3 s_OrigAt      = { 0.0f, 0.0f, 0.0f };

static Vector3 s_ModEye      = { 0.0f, 0.0f, 0.0f };

static Vector3 s_ModAt       = { 0.0f, 0.0f, 0.0f };

static int     s_ModRollBams = 0;

// Freecam internal state

static Vector3 s_FreecamPos = { 0.0f, 0.0f, 0.0f };

static bool    s_FreecamInitialized = false;

// Fly mode internal state: the mod owns the flight position integrator; PostVehiclePhysics

// writes it into the vehicle root/dynamics block after every physics step.

static float   s_FlyCurrentAltitude = 0.0f;

static float   s_FlyPosX            = 0.0f;

static float   s_FlyPosZ            = 0.0f;

static float   s_FlyYawDeg          = 0.0f;

static bool    s_FlyInitialized     = false;

// Cutscene handback: set on every frame the fly writer is parked for a fare cutscene,
// consumed once on the cutscene->fly transition for a small resume lift against
// rest-clipping (flight re-initializes from the settled vanilla position).
static bool    s_FlyWasInCutscene   = false;

// Teleport latch: TeleportTaxi writes its target once from the ImGui/EndScene thread and
// the next physics step can snap back (off-world respawn in sub_513E80 0x00513E80 when
// all 4 wheel raycasts miss, root<->block sync 0x00519CAF, stale mirrors). The latch
// write set (TeleportLatchApply) is asserted in PreVehiclePhysics BEFORE sub_519890 - so
// the respawn evaluates its raycasts/respawn decision AT THE TARGET - and again in
// PostVehiclePhysics after the step, so the integration result cannot leak into the
// render pass (render_chassis_and_wheels 0x00517D70 reads ROOT directly).
// Lifecycle (T3 fix plan step 3): hold until a read-raycast finds ground contact at the
// target AND a minimum of s_TeleportLatchFrames frames elapsed, hard-capped by
// s_TeleportLatchWatchdog so genuine off-map/water targets hand back to the vanilla
// rescue (respawn) instead of freezing the taxi forever.
static bool    s_TeleportLatchActive   = false;

static float   s_TeleportLatchX        = 0.0f;

static float   s_TeleportLatchY        = 0.0f;

static float   s_TeleportLatchZ        = 0.0f;

static int     s_TeleportLatchFrames   = 0; // minimum-hold countdown (armed with 10)

static int     s_TeleportLatchWatchdog = 0; // hard cap countdown (armed with 600 = ~10s)

static int     s_TeleportLatchLog      = 0; // frames logged since arming (step-0 diagnostics)


// Mouse Look (Free Orbit in Normal Gameplay & Fly Mode)

static float   s_MouseLookYaw       = 0.0f;

static float   s_MouseLookPitch     = 0.0f;

void ResetMouseLook()

{

    s_MouseLookYaw   = 0.0f;

    s_MouseLookPitch = 0.0f;

}

void SetCameraMode(int mode)
{
    __try
    {
        if (!IsInGame())
            return;

        // Sega Native Camera Cheats (from offset_list.json and EngineFixes reference):
        // 0x01B7A7E8 = enableCameraCheats (1 = enabled, 0 = disabled)
        // 0x01B7A7EC = requestedCameraMode:
        //   1 = Reset / Standard Chase Camera (F5)
        //   2 = Cinematic Camera (F6)
        //   3 = First-Person / Cockpit / Bumper Camera (F7)
        uint32_t reqMode = 1;
        if (mode == 0) reqMode = 1;
        else if (mode == 1) reqMode = 2;
        else if (mode == 2) reqMode = 3;
        else if (mode == 3) reqMode = 3;

        *(uint32_t*)0x01B7A7E8 = 1;
        *(uint32_t*)0x01B7A7EC = reqMode;

        // Reset to normal chase cam if mode == 0
        if (mode == 0)
        {
            *(uint32_t*)0x01B7A7E8 = 0;
            *(uint32_t*)0x01B7A7EC = 1;
        }

        Logger::Info("Camera", "Switched Sega Camera Cheat Mode to %d (reqMode %u, enable %u)",
                     mode, reqMode, *(uint32_t*)0x01B7A7E8);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Real vehicle velocity tracking

static float   s_LastTaxiPx         = 0.0f;

static float   s_LastTaxiPz         = 0.0f;

static float   s_RealSpeedMph       = 0.0f;

static inline Vector3 CrossProduct(const Vector3& a, const Vector3& b)

{

    return {

        a.y * b.z - a.z * b.y,

        a.z * b.x - a.x * b.z,

        a.x * b.y - a.y * b.x

    };

}

void TeleportFreecamToTaxi()

{

    float x = 0, y = 0, z = 0;

    if (GetTaxiPosition(x, y, z))

    {

        s_FreecamPos = { x, y + 6.0f, z - 18.0f };

        g_Config.freecamPitch = 0.0f;

        g_Config.freecamYaw   = 0.0f;

        g_Config.freecamRoll  = 0.0f;

    }

}

void MoveFreecam(float dRight, float dUp, float dFwd)

{

    float wasdMult = -1.0f; // Permanently inverted WASD for natural camera controls

    dFwd   *= wasdMult;

    // dRight is strictly preserved so A moves Left and D moves Right!

    constexpr float DEG2RAD = 3.1415926535f / 180.0f;

    float pitchRad = g_Config.freecamPitch * DEG2RAD;

    float yawRad   = g_Config.freecamYaw   * DEG2RAD;

    float cosP = cosf(pitchRad);

    float sinP = sinf(pitchRad);

    float cosY = cosf(yawRad);

    float sinY = sinf(yawRad);

    Vector3 fwd   = { cosP * sinY, sinP, cosP * cosY };

    Vector3 right = { cosY, 0.0f, -sinY };

    Vector3 up    = { 0.0f, 1.0f, 0.0f };

    s_FreecamPos.x += right.x * dRight + up.x * dUp + fwd.x * dFwd;

    s_FreecamPos.y += right.y * dRight + up.y * dUp + fwd.y * dFwd;

    s_FreecamPos.z += right.z * dRight + up.z * dUp + fwd.z * dFwd;

}

// Returns true if custom camera transformation should be used; false for 100% native passthrough

extern "C" bool __cdecl ProcessCamera(Vector3* pEye, Vector3* pAt)

{

    if (!pEye || !pAt)

        return false;

    s_OrigEye = *pEye;

    s_OrigAt  = *pAt;

    // 1. FREECAM MODE: Camera is completely uncoupled from the taxi

    if (g_Config.freecamEnabled)

    {

        if (!s_FreecamInitialized)

        {

            s_FreecamPos = s_OrigEye;

            s_FreecamInitialized = true;

        }

        constexpr float DEG2RAD = 3.1415926535f / 180.0f;

        float pitchRad = g_Config.freecamPitch * DEG2RAD;

        float yawRad   = g_Config.freecamYaw   * DEG2RAD;

        float cosP = cosf(pitchRad);

        float sinP = sinf(pitchRad);

        float cosY = cosf(yawRad);

        float sinY = sinf(yawRad);

        Vector3 fwd = { cosP * sinY, sinP, cosP * cosY };

        s_ModEye = s_FreecamPos;

        s_ModAt  = { s_FreecamPos.x + fwd.x * 20.0f, s_FreecamPos.y + fwd.y * 20.0f, s_FreecamPos.z + fwd.z * 20.0f };

        s_ModRollBams = (int)(g_Config.freecamRoll * (65536.0f / 360.0f)) & 0xFFFF;

        return true;

    }

    else

    {

        s_FreecamInitialized = false;

    }

    // 2. Do not modify camera if not in active session or during title attract demo

    if (!IsInGame())

        return false;

    int vehicleState = 0;

    __try {

        vehicleState = *(int*)ADDR_VEHICLE_STATE;

    } __except (EXCEPTION_EXECUTE_HANDLER) {

        vehicleState = 8;

    }

    if (vehicleState == 8)

        return false;

    // 3. CUTSCENE PROTECTION: When passenger pickup or drop-off animation is playing, pass through vanilla camera

    int fareCutscene = 0;

    __try {

        fareCutscene = *(int*)ADDR_FARE_CUTSCENE;

    } __except (EXCEPTION_EXECUTE_HANDLER) {}

    if (fareCutscene != 0)

    {

        return false; // Leave camera 100% native during customer cinematics!

    }

    // 4. Fly Mode Camera Coupling:

    // The eye tracks the taxi root Y (0x01661FAC) EXACTLY via a fixed elevation offset

    // and the target stays a fixed lift above the root, so climbing never clips the rig

    // into geometry. The taxi heading BAM provides the horizontal stand-off direction.

    Vector3 elevatedEye = s_OrigEye;

    Vector3 elevatedAt  = s_OrigAt;

    if (g_Config.flyMode)

    {

        // Read the real vehicle root transform directly. GetTaxiPosition's sanity envelope

        // can reject high/far fly positions (which would snap the rig to the origin), so

        // read raw with NaN protection and keep the last known-good position on failure.

        float rootX = 0.0f, rootY = 0.0f, rootZ = 0.0f;

        bool haveRoot = false;

        __try {

            rootX = *(float*)ADDR_VEHICLE_ROOT_X;

            rootY = *(float*)ADDR_VEHICLE_ROOT_Y;

            rootZ = *(float*)ADDR_VEHICLE_ROOT_Z;

            haveRoot = !std::isnan(rootX) && !std::isnan(rootY) && !std::isnan(rootZ);

        } __except (EXCEPTION_EXECUTE_HANDLER) { haveRoot = false; }

        static Vector3 s_FlyLastCarPos = { 0.0f, 0.0f, 0.0f };

        static bool s_FlyLastCarPosValid = false;

        if (haveRoot)

        {

            s_FlyLastCarPos = { rootX, rootY, rootZ };

            s_FlyLastCarPosValid = true;

        }

        if (s_FlyLastCarPosValid)

        {

            // Engine convention: vX = speed * sin(heading), vZ = speed * cos(heading)

            // (sub_519890 velocity decomposition; BAM 0..65535 = 0..360 deg).

            uint16_t headingBam = 0;

            __try {

                headingBam = *(uint16_t*)ADDR_VEHICLE_HEADING_BAM;

            } __except (EXCEPTION_EXECUTE_HANDLER) {}

            constexpr float FLY_BAM2RAD = 6.2831853f / 65536.0f;

            float headingRad = (float)headingBam * FLY_BAM2RAD;

            Vector3 fwd = { sinf(headingRad), 0.0f, cosf(headingRad) };

            constexpr float FLY_CAM_DIST      = 75.0f; // horizontal stand-off along heading

            constexpr float FLY_CAM_ELEVATION = 14.0f; // eye height above the root (fixed)

            constexpr float FLY_CAM_AT_LIFT   = 3.0f;  // target lift above the root (chassis center)

            // Crazy Taxi 3D Coordinate Space: +Y is UP (elevation/sky)

            elevatedEye = {

                s_FlyLastCarPos.x - fwd.x * FLY_CAM_DIST,

                s_FlyLastCarPos.y + FLY_CAM_ELEVATION,

                s_FlyLastCarPos.z - fwd.z * FLY_CAM_DIST

            };

            elevatedAt = {

                s_FlyLastCarPos.x,

                s_FlyLastCarPos.y + FLY_CAM_AT_LIFT,

                s_FlyLastCarPos.z

            };

        }

        // Without any valid root yet: fall through with the vanilla eye/at for this frame.

    }

    // 5. Mouse Look (Free Orbit in Normal Gameplay & Fly Mode)

    float mouseLookYaw = 0.0f;

    float mouseLookPitch = 0.0f;

    if (g_Config.mouseLookEnabled && !g_Config.freecamEnabled && !g_bMenuOpen)

    {

        mouseLookYaw   = s_MouseLookYaw;

        mouseLookPitch = s_MouseLookPitch;

    }

    // Fly Mode Camera Override:

    // When Fly Mode is active, lock camera offsets and rotations to exact user-specified values:

    // X = 2.3m, Y = 19.6m, Z = -110.0m, Pitch = 3.4 deg, Yaw = 180.0 deg, Roll = -2.0 deg.

    // When Fly Mode is inactive, use normal user camera settings from g_Config.

    const float effOffX  = g_Config.flyMode ? 2.3f    : g_Config.camOffsetX;

    const float effOffY  = g_Config.flyMode ? 19.6f   : g_Config.camOffsetY;

    const float effOffZ  = g_Config.flyMode ? -110.0f : g_Config.camOffsetZ;

    const float effRotP  = g_Config.flyMode ? 3.4f    : g_Config.camRotPitch;

    const float effRotY  = g_Config.flyMode ? 180.0f  : g_Config.camRotYaw;

    const float effRotR  = g_Config.flyMode ? -2.0f   : g_Config.camRotRoll;

    bool hasOffsets = (effOffX != 0.0f || effOffY != 0.0f || effOffZ != 0.0f ||

                       effRotP != 0.0f || effRotY != 0.0f || effRotR != 0.0f);

    bool needCustomCam = (g_Config.flyMode || mouseLookYaw != 0.0f || mouseLookPitch != 0.0f || hasOffsets);

    if (!needCustomCam)

    {

        return false;

    }

    // 6. Orthogonal 3D Camera Coordinate Space

    float dx = elevatedAt.x - elevatedEye.x;

    float dz = elevatedAt.z - elevatedEye.z;

    float hDist = sqrtf(dx * dx + dz * dz);

    if (hDist < 0.001f)

    {

        dx = 0.0f;

        dz = 1.0f;

        hDist = 1.0f;

    }

    Vector3 hFwd   = { dx / hDist, 0.0f, dz / hDist };

    Vector3 hRight = { hFwd.z, 0.0f, -hFwd.x };

    Vector3 vUp    = { 0.0f, 1.0f, 0.0f }; // Normal upright basis vector

    Vector3 trans = {

        hRight.x * effOffX + hFwd.x * effOffZ,

        vUp.y    * effOffY,

        hRight.z * effOffX + hFwd.z * effOffZ

    };

    Vector3 baseAt = {

        elevatedAt.x + trans.x,

        elevatedAt.y + trans.y,

        elevatedAt.z + trans.z

    };

    Vector3 diff = { elevatedEye.x - elevatedAt.x, elevatedEye.y - elevatedAt.y, elevatedEye.z - elevatedAt.z };

    float dist = sqrtf(diff.x * diff.x + diff.y * diff.y + diff.z * diff.z);

    if (dist < 0.1f) dist = 1.0f;

    Vector3 fwdEye = { diff.x / dist, diff.y / dist, diff.z / dist };

    Vector3 rightEye = CrossProduct(vUp, fwdEye);

    float rLen = sqrtf(rightEye.x * rightEye.x + rightEye.y * rightEye.y + rightEye.z * rightEye.z);

    if (rLen < 1e-4f)

        rightEye = { 1.0f, 0.0f, 0.0f };

    else

        rightEye = { rightEye.x / rLen, rightEye.y / rLen, rightEye.z / rLen };

    Vector3 upEye = CrossProduct(fwdEye, rightEye);

    constexpr float DEG2RAD = 3.1415926535f / 180.0f;

    float totalYaw = (effRotY + mouseLookYaw) * DEG2RAD;

    float cosY = cosf(totalYaw);

    float sinY = sinf(totalYaw);

    Vector3 fwdY = { fwdEye.x * cosY + rightEye.x * sinY, fwdEye.y * cosY + rightEye.y * sinY, fwdEye.z * cosY + rightEye.z * sinY };

    float totalPitch = (effRotP + mouseLookPitch) * DEG2RAD;

    float cosP = cosf(totalPitch);

    float sinP = sinf(totalPitch);

    Vector3 fwdFinal = { fwdY.x * cosP + upEye.x * sinP, fwdY.y * cosP + upEye.y * sinP, fwdY.z * cosP + upEye.z * sinP };

    s_ModAt  = baseAt;

    s_ModEye = { baseAt.x + fwdFinal.x * dist, baseAt.y + fwdFinal.y * dist, baseAt.z + fwdFinal.z * dist };

    int baseRoll = 0;

    __try {

        baseRoll = *(int*)ADDR_CAM_ROLL;

    } __except (EXCEPTION_EXECUTE_HANDLER) {}

    int addRoll = (int)(effRotR * (65536.0f / 360.0f));

    s_ModRollBams = (baseRoll + addRoll) & 0xFFFF;

    return true;

}

// Naked assembly thunk hooked at 0x0051f582

__declspec(naked) static void hkCameraUpdateThunk()

{

    __asm

    {

        pushad

        push ecx               // ecx = pAt (&0x016614f8)

        push eax               // eax = pEye (&0x01661494)

        call ProcessCamera

        add esp, 8

        test al, al

        jz vanilla_passthrough

        popad

        lea eax, s_ModEye

        lea ecx, s_ModAt

        mov edx, s_ModRollBams

        mov [esp + 4], edx

        mov edx, ADDR_CAM_LOOKAT_FN

        jmp edx

    vanilla_passthrough:

        popad

        mov edx, ADDR_CAM_LOOKAT_FN

        jmp edx

    }

}

// ---------------------------------------------------------------------------------

// FEATURE: NITRO TAXI (Energy tank speed boost system)

// ---------------------------------------------------------------------------------

// Tank Capacity: 75.0 units.

// Consumption: 5.0 units/second when holding SHIFT while accelerating in-game.

// Recharge: +1.5 units/second when not boosting, up to 75.0 max.

// Speed Factor: 1.50f (+50% peak boost).

// Releasing Shift delay: 3.0 seconds cooldown before recharge starts.

// Empty tank penalty: 5.0 seconds delay before recharge starts, AND Nitro is locked

// out from being activated until the tank recovers to at least 40% capacity (30.0 units).

// No nitro in fly mode, cutscenes, freecam or with the mod menu open.

constexpr float NITRO_MAX_CAPACITY        = 75.0f; // maximum tank energy

constexpr float NITRO_DRAIN_PER_SEC       = 18.0f; // energy consumed per second while boosting (~4.2s full boost)

constexpr float NITRO_RECHARGE_PER_SEC    = 2.5f;  // energy recharged per second when idle

constexpr float NITRO_BOOST_FACTOR        = 1.50f; // +50% peak boost

constexpr float NITRO_COMBINED_CAP        = 2.0f;  // total speed factor clamp

constexpr float NITRO_REARM_THRESHOLD     = 30.0f; // 40% of 75.0 units

constexpr float NITRO_EMPTY_DELAY_SECS    = 5.0f;  // 5 seconds delay before regen starts when empty

constexpr float NITRO_RELEASE_DELAY_SECS  = 3.0f;  // 3 seconds delay before regen starts on shift release

static float s_NitroEnergy       = NITRO_MAX_CAPACITY;

static float s_NitroFactor       = 1.0f;

static bool  s_NitroShiftHeld    = false;

static DWORD s_LastNitroTick     = 0;

static float s_NitroDelayTimer   = 0.0f;

static bool  s_NitroLockedOut    = false;

static bool  s_NitroWasBoosting  = false;

float GetNitroEnergy()      { return s_NitroEnergy; }

float GetNitroMaxEnergy()   { return NITRO_MAX_CAPACITY; }

bool  IsNitroActive()       { return s_NitroFactor > 1.05f; }

bool  IsNitroLockedOut()    { return s_NitroLockedOut; }

float GetNitroDelayTimer()  { return s_NitroDelayTimer; }

static void ResetNitroState()

{

    s_NitroEnergy = NITRO_MAX_CAPACITY;

    s_NitroFactor = 1.0f;

    s_LastNitroTick = 0;

    s_NitroDelayTimer = 0.0f;

    s_NitroLockedOut = false;

    s_NitroWasBoosting = false;

}

static void UpdateNitroState()

{

    int gameFlag = 0;

    __try { gameFlag = *(int*)ADDR_GAME_FLAG; } __except (EXCEPTION_EXECUTE_HANDLER) {}

    // In Main Menu (0), Quitting to Menu (7), or outside active round:

    // Instantly refill Nitro tank to 100% full capacity!

    if (gameFlag == 0 || gameFlag == 7 || !IsInGame())

    {

        ResetNitroState();

        return;

    }

    DWORD now = GetTickCount();

    float dt = (s_LastNitroTick == 0) ? (1.0f / 60.0f) : ((now - s_LastNitroTick) / 1000.0f);

    s_LastNitroTick = now;

    if (dt < 0.001f) dt = 0.001f;

    if (dt > 0.1f)   dt = 0.1f;

    bool shiftHeld = s_NitroShiftHeld ||

                     (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0 ||

                     (GetAsyncKeyState(VK_LSHIFT) & 0x8000) != 0 ||

                     (GetAsyncKeyState(VK_RSHIFT) & 0x8000) != 0 ||

                     (GetAsyncKeyState('N') & 0x8000) != 0;

    bool isGas = (GetAsyncKeyState(VK_UP) & 0x8000) != 0 ||

                 (GetAsyncKeyState('W') & 0x8000) != 0 ||

                 (GetAsyncKeyState('D') & 0x8000) != 0 || // Crazy Taxi native PC accelerate key

                 (g_Config.keyForward != 0 && (GetAsyncKeyState(g_Config.keyForward) & 0x8000) != 0) ||

                 (*(uint8_t*)ADDR_INPUT_BTN_ACCEL != 0) ||

                 (*(uint16_t*)ADDR_INPUT_ACCEL_AXIS > 1000);

    int vehState = 0;

    __try { vehState = *(int*)ADDR_VEHICLE_STATE; } __except (EXCEPTION_EXECUTE_HANDLER) {}

    bool inPassengerAnim = (vehState == 10 || vehState == 11);

    bool allowed = g_Config.nitroEnabled && !g_Config.flyMode && !g_Config.freecamEnabled &&

                   !g_bMenuOpen && !inPassengerAnim;

    uint8_t gear = 0;

    __try { gear = *(uint8_t*)ADDR_INPUT_GEAR_STATE; } __except (EXCEPTION_EXECUTE_HANDLER) {}

    // To boost: must be allowed, holding Shift or N, have energy, not locked out, and in forward gear (gear != 1)

    bool canBoost = allowed && !s_NitroLockedOut && (s_NitroEnergy > 0.0f) && shiftHeld && (gear != 1);

    if (canBoost)

    {

        s_NitroWasBoosting = true;

        s_NitroDelayTimer = 0.0f; // Reset any regen delays while actively boosting

        s_NitroEnergy -= NITRO_DRAIN_PER_SEC * dt;

        if (s_NitroEnergy <= 0.0f)

        {

            // Tank completely drained! Apply 5-second regen penalty delay & lock out until >= 40% (30.0 units)

            s_NitroEnergy = 0.0f;

            s_NitroFactor = 1.0f;

            s_NitroLockedOut = true;

            s_NitroDelayTimer = NITRO_EMPTY_DELAY_SECS; // 5.0s wait

            s_NitroWasBoosting = false;

        }

        else

        {

            s_NitroFactor = NITRO_BOOST_FACTOR;

        }

    }

    else

    {

        s_NitroFactor = 1.0f;

        // Transition: if player was actively boosting and released Shift / stopped boosting (and not already locked out)

        if (s_NitroWasBoosting)

        {

            s_NitroWasBoosting = false;

            if (!s_NitroLockedOut && s_NitroEnergy > 0.0f)

            {

                // Non-empty release: 3.0s cooldown before regeneration starts

                s_NitroDelayTimer = NITRO_RELEASE_DELAY_SECS;

            }

        }

        // Advance delay timer

        if (s_NitroDelayTimer > 0.0f)

        {

            s_NitroDelayTimer -= dt;

            if (s_NitroDelayTimer < 0.0f)

                s_NitroDelayTimer = 0.0f;

        }

        // Regenerate energy once delay timer reaches 0

        if (g_Config.nitroEnabled && s_NitroDelayTimer <= 0.0f && s_NitroEnergy < NITRO_MAX_CAPACITY)

        {

            s_NitroEnergy += NITRO_RECHARGE_PER_SEC * dt;

            if (s_NitroEnergy > NITRO_MAX_CAPACITY)

                s_NitroEnergy = NITRO_MAX_CAPACITY;

        }

        // Check if lockout penalty can be released (at or above 40% capacity = 30.0 units)

        if (s_NitroLockedOut && s_NitroEnergy >= NITRO_REARM_THRESHOLD)

        {

            s_NitroLockedOut = false; // Re-armed! Player can use Nitro again

        }

    }

}

// ---------------------------------------------------------------------------------

// DIRECTINPUT HARDWARE KEYBOARD BUFFER (VANILLA NATIVE ARCADE PASSTHROUGH)

// ---------------------------------------------------------------------------------

extern "C" __declspec(dllexport) void ProcessKeyboardBuffer(BYTE* pBuf)

{

    if (!pBuf)

        return;

    // Capture SHIFT (DIK_LSHIFT 0x2A / DIK_RSHIFT 0x36) for the Nitro feature before any

    // neutralization. DirectInput sets bit 0x80 on pressed keys; the state machine in

    // UpdateGameFeaturesPerFrame gates this on menu/fly/cutscene state.

    s_NitroShiftHeld = (pBuf[0x2A] & 0x80) != 0 || (pBuf[0x36] & 0x80) != 0;

    // In Freecam mode or Mod Menu: neutralize driving controls so taxi stays frozen

    if (g_Config.freecamEnabled || g_bMenuOpen)

    {

        pBuf[0xC8] = 0; // DIK_UP

        pBuf[0xD0] = 0; // DIK_DOWN

        pBuf[0xCB] = 0; // DIK_LEFT

        pBuf[0xCD] = 0; // DIK_RIGHT

        pBuf[0x11] = 0; // DIK_W

        pBuf[0x1F] = 0; // DIK_S

        pBuf[0x1E] = 0; // DIK_A

        pBuf[0x20] = 0; // DIK_D

        return;

    }

    // 100% Native Sega Arcade Input Passthrough

    // All inputs (Arrow Up/Down/Left/Right, D=Drive, S=Reverse) are handled directly by Sega!

}

// ---------------------------------------------------------------------------------

// D3D DEVICE SAFE-GUARD AT 0x00481100 (FIXES 0xC0000005 CRASH AT 0x00481103)

// Intercepts Sega's ClearD3DState when device pointer in [esi+0x18] is uninitialized

// ---------------------------------------------------------------------------------

static BYTE s_OrigClearD3DBytes[5] = {0};

static bool s_ClearD3DHookInstalled = false;

__declspec(naked) static void hkClearD3DStateNaked()

{

    __asm

    {

        test esi, esi

        jz safe_exit

        mov eax, dword ptr [esi + 0x18]

        test eax, eax

        jz safe_exit

        // Device is valid: read vtable and jump to 0x00481105

        mov ecx, dword ptr [eax]

        push 0x00481105

        ret

    safe_exit:

        // Null device guard: zero tracking variables in esi and return cleanly

        test esi, esi

        jz just_ret

        push edi

        push ebx

        xor edi, edi

        mov dword ptr [esi + 0x30], edi

        mov dword ptr [esi + 0x34], edi

        mov dword ptr [esi + 0x38], edi

        mov dword ptr [esi + 0x3C], edi

        mov dword ptr [esi + 0x40], edi

        mov dword ptr [esi + 0x44], edi

        mov dword ptr [esi + 0x48], edi

        mov dword ptr [esi + 0x4C], edi

        mov dword ptr [esi + 0x50], edi

        mov dword ptr [esi + 0x54], edi

        mov dword ptr [esi + 0x98], edi

        mov dword ptr [esi + 0x9C], edi

        pop ebx

        pop edi

    just_ret:

        ret

    }

}

// ---------------------------------------------------------------------------------

// D3D VERTEX DECLARATION SAFE-GUARD AT 0x0052227E (FIXES 0xC0000005 CRASH AT 0x00522281)

// Intercepts CreateVertexDeclaration when device pointer in [edx+0x18] is uninitialized

// ---------------------------------------------------------------------------------

static BYTE s_OrigCreateDeclBytes[5] = {0};

static bool s_CreateDeclHookInstalled = false;

__declspec(naked) static void hkCreateVertexDeclNaked()

{

    __asm

    {

        test edx, edx

        jz fail_exit

        mov eax, dword ptr [edx + 0x18]

        test eax, eax

        jz fail_exit

        // Device is valid: read vtable and jump to 0x00522283

        mov ecx, dword ptr [eax]

        push 0x00522283

        ret

    fail_exit:

        // Clean failure exit at 0x0052229a (returns FALSE with ret 8)

        push 0x0052229a

        ret

    }

}

// ---------------------------------------------------------------------------------

// D3D VERTEX SHADER SAFE-GUARD AT 0x00521FD4 (FIXES 0xC0000005 CRASH AT 0x00521FD7)

// Intercepts CreateVertexShader when device pointer in [ecx+0x18] is uninitialized

// ---------------------------------------------------------------------------------

static BYTE s_OrigCreateVSBytes[5] = {0};

static bool s_CreateVSHookInstalled = false;

__declspec(naked) static void hkCreateVertexShaderNaked()

{

    __asm

    {

        test ecx, ecx

        jz fail_exit

        mov eax, dword ptr [ecx + 0x18]

        test eax, eax

        jz fail_exit

        // Device is valid: read vtable and jump back to 0x00521FD9

        mov ecx, dword ptr [eax]

        push 0x00521FD9

        ret

    fail_exit:

        // Jump to standard failure exit at 0x00521FEE (xor al,al; mov esp,ebp; pop ebp; ret 0x10)

        push 0x00521FEE

        ret

    }

}

// ---------------------------------------------------------------------------------

// D3D PIXEL SHADER SAFE-GUARD AT 0x00521F74 (FIXES 0xC0000005 CRASH AT 0x00521F77)

// Intercepts CreatePixelShader when device pointer in [ecx+0x18] is uninitialized

// ---------------------------------------------------------------------------------

static BYTE s_OrigCreatePSBytes[5] = {0};

static bool s_CreatePSHookInstalled = false;

__declspec(naked) static void hkCreatePixelShaderNaked()

{

    __asm

    {

        test ecx, ecx

        jz fail_exit

        mov eax, dword ptr [ecx + 0x18]

        test eax, eax

        jz fail_exit

        // Device is valid: read vtable and jump back to 0x00521F79

        mov ecx, dword ptr [eax]

        push 0x00521F79

        ret

    fail_exit:

        // Jump to standard failure exit at 0x00521F8E (xor al,al; mov esp,ebp; pop ebp; ret 0x10)

        push 0x00521F8E

        ret

    }

}

// ---------------------------------------------------------------------------------

// D3D VERTEX BUFFER SAFE-GUARD AT 0x00521D60 (FIXES 0xC0000005 CRASH AT 0x00521D87)

// Intercepts CreateVertexBuffer when device pointer in [ecx+0x18] is uninitialized

// ---------------------------------------------------------------------------------

static BYTE s_OrigCreateVBBytes[8] = {0};

static bool s_CreateVBHookInstalled = false;

__declspec(naked) static void hkCreateVertexBufferNaked()

{

    __asm

    {

        test ecx, ecx

        jz fail_exit

        cmp dword ptr [ecx + 0x18], 0

        jz fail_exit

        push ebp

        mov ebp, esp

        push ecx

        cmp dword ptr [ebp + 0x18], 0

        push 0x00521D68

        ret

    fail_exit:

        xor al, al

        ret 0x14

    }

}

// ---------------------------------------------------------------------------------

// D3D INDEX BUFFER SAFE-GUARD AT 0x00521E90 (FIXES 0xC0000005 CRASH AT 0x00521EBA)

// Intercepts CreateIndexBuffer when device pointer in [ecx+0x18] is uninitialized

// ---------------------------------------------------------------------------------

static BYTE s_OrigCreateIBBytes[7] = {0};

static bool s_CreateIBHookInstalled = false;

__declspec(naked) static void hkCreateIndexBufferNaked()

{

    __asm

    {

        test ecx, ecx

        jz fail_exit

        cmp dword ptr [ecx + 0x18], 0

        jz fail_exit

        push ebp

        mov ebp, esp

        cmp dword ptr [ebp + 0x18], 0

        push 0x00521E97

        ret

    fail_exit:

        xor al, al

        ret 0x14

    }

}

// ---------------------------------------------------------------------------------

// D3D SHADER CONSTANT MATRIX SAFE-GUARD AT 0x004801B0 (FIXES 0xC0000005 CRASH AT 0x004801C0)

// ---------------------------------------------------------------------------------

static BYTE s_OrigSetShaderMatrixBytes[6] = {0};

static bool s_SetShaderMatrixHookInstalled = false;

__declspec(naked) static void hkSetShaderConstMatrixNaked()

{

    __asm

    {

        test ecx, ecx

        jz fail_exit

        cmp dword ptr [ecx + 0x18], 0

        jz fail_exit

        push ebp

        mov ebp, esp

        mov eax, dword ptr [ebp + 8]

        push 0x004801B6

        ret

    fail_exit:

        ret 0x0C

    }

}

// ---------------------------------------------------------------------------------

// D3D SHADER CONSTANT VECTOR SAFE-GUARD AT 0x00480200

// ---------------------------------------------------------------------------------

static BYTE s_OrigSetShaderVectorBytes[6] = {0};

static bool s_SetShaderVectorHookInstalled = false;

__declspec(naked) static void hkSetShaderConstVectorNaked()

{

    __asm

    {

        test ecx, ecx

        jz fail_exit

        cmp dword ptr [ecx + 0x18], 0

        jz fail_exit

        push ebp

        mov ebp, esp

        mov eax, dword ptr [ebp + 8]

        push 0x00480206

        ret

    fail_exit:

        ret 0x0C

    }

}

// ---------------------------------------------------------------------------------

// D3D SHADER CONSTANT ARRAY FLOAT SAFE-GUARD AT 0x00480250

// ---------------------------------------------------------------------------------

static BYTE s_OrigSetShaderArrayFBytes[6] = {0};

static bool s_SetShaderArrayFHookInstalled = false;

__declspec(naked) static void hkSetShaderConstArrayFNaked()

{

    __asm

    {

        test ecx, ecx

        jz fail_exit

        cmp dword ptr [ecx + 0x18], 0

        jz fail_exit

        push ebp

        mov ebp, esp

        mov eax, dword ptr [ebp + 0x14]

        push 0x00480256

        ret

    fail_exit:

        ret 0x10

    }

}

// ---------------------------------------------------------------------------------

// D3D SHADER CONSTANT ARRAY INT SAFE-GUARD AT 0x004802C0

// ---------------------------------------------------------------------------------

static BYTE s_OrigSetShaderArrayIBytes[6] = {0};

static bool s_SetShaderArrayIHookInstalled = false;

__declspec(naked) static void hkSetShaderConstArrayINaked()

{

    __asm

    {

        test ecx, ecx

        jz fail_exit

        cmp dword ptr [ecx + 0x18], 0

        jz fail_exit

        push ebp

        mov ebp, esp

        mov eax, dword ptr [ebp + 0x14]

        push 0x004802C6

        ret

    fail_exit:

        ret 0x10

    }

}

// ---------------------------------------------------------------------------------

// D3D SHADER CONSTANT ARRAY BOOL SAFE-GUARD AT 0x00480330

// ---------------------------------------------------------------------------------

static BYTE s_OrigSetShaderArrayBBytes[6] = {0};

static bool s_SetShaderArrayBHookInstalled = false;

__declspec(naked) static void hkSetShaderConstArrayBNaked()

{

    __asm

    {

        test ecx, ecx

        jz fail_exit

        cmp dword ptr [ecx + 0x18], 0

        jz fail_exit

        push ebp

        mov ebp, esp

        mov eax, dword ptr [ebp + 0x14]

        push 0x00480336

        ret

    fail_exit:

        ret 0x10

    }

}

// ---------------------------------------------------------------------------------

// D3D RESET PIXEL SHADER SAFE-GUARD AT 0x00480440

// ---------------------------------------------------------------------------------

static BYTE s_OrigResetPSBytes[5] = {0};

static bool s_ResetPSHookInstalled = false;

__declspec(naked) static void hkResetPixelShaderNaked()

{

    __asm

    {

        test ecx, ecx

        jz fail_exit

        cmp dword ptr [ecx + 0x18], 0

        jz fail_exit

        push esi

        mov esi, ecx

        mov eax, dword ptr [esi]

        push 0x00480445

        ret

    fail_exit:

        ret

    }

}

// ---------------------------------------------------------------------------------

// D3D RESET VERTEX SHADER SAFE-GUARD AT 0x00480480

// ---------------------------------------------------------------------------------

static BYTE s_OrigResetVSBytes[5] = {0};

static bool s_ResetVSHookInstalled = false;

__declspec(naked) static void hkResetVertexShaderNaked()

{

    __asm

    {

        test ecx, ecx

        jz fail_exit

        cmp dword ptr [ecx + 0x18], 0

        jz fail_exit

        push esi

        mov esi, ecx

        mov eax, dword ptr [esi]

        push 0x00480485

        ret

    fail_exit:

        ret

    }

}

// ---------------------------------------------------------------------------------

// MESH BUFFER COPY SAFE-GUARD AT 0x0049F967 (FIXES 0xC0000005 CRASH AT 0x0049F976)

// Prevents rep movsd from writing 80 bytes into NULL when Lock failed / returned NULL

// ---------------------------------------------------------------------------------

static BYTE s_OrigMeshBufferCopyBytes[17] = {0};

static bool s_MeshBufferCopyHookInstalled = false;

__declspec(naked) static void hkMeshBufferCopyNaked()

{

    __asm

    {

        mov edi, dword ptr [esp + 0x10]

        test edi, edi

        jz skip_copy

        lea esi, dword ptr [ebx + 0x200]

        mov ecx, 0x14

        rep movsd

    skip_copy:

        push 0x0049F978

        ret

    }

}

// ---------------------------------------------------------------------------------

// SEGA RENDERER CREATE TEXTURE SAFE-GUARD AT 0x00481BB0 (FIXES 0xC0000005 CRASH AT 0x00481BDF)

// ---------------------------------------------------------------------------------

static BYTE s_OrigSegaCreateTexBytes[6] = {0};

static bool s_SegaCreateTexHookInstalled = false;

__declspec(naked) static void hkSegaCreateTextureNaked()

{

    __asm

    {

        test ecx, ecx

        jz fail_exit

        cmp dword ptr [ecx + 0x18], 0

        jnz device_valid

        mov eax, dword ptr [g_pActiveD3DDevice]

        test eax, eax

        jz fail_exit

        mov dword ptr [ecx + 0x18], eax

    device_valid:

        push ebp

        mov ebp, esp

        mov eax, dword ptr [ebp + 0x14]

        push 0x00481BB6

        ret

    fail_exit:

        xor al, al

        ret 0x18

    }

}

// ---------------------------------------------------------------------------------

// SEGA RENDERER GET AVAILABLE TEXTURE MEMORY SAFE-GUARD AT 0x00481C20

// ---------------------------------------------------------------------------------

static BYTE s_OrigSegaGetAvailTexMemBytes[6] = {0};

static bool s_SegaGetAvailTexMemHookInstalled = false;

__declspec(naked) static void hkSegaGetAvailTexMemNaked()

{

    __asm

    {

        test ecx, ecx

        jz fail_exit

        cmp dword ptr [ecx + 0x18], 0

        jnz device_valid

        mov eax, dword ptr [g_pActiveD3DDevice]

        test eax, eax

        jz fail_exit

        mov dword ptr [ecx + 0x18], eax

    device_valid:

        push ebp

        mov ebp, esp

        sub esp, 0x1C

        push 0x00481C26

        ret

    fail_exit:

        mov eax, dword ptr [esp + 4]

        test eax, eax

        jz done

        mov dword ptr [eax], 0

        mov dword ptr [eax + 4], 0

        mov dword ptr [eax + 8], 0

        mov dword ptr [eax + 0x0C], 0

    done:

        ret 4

    }

}

// ---------------------------------------------------------------------------------

// SEGA RENDERER INITIALIZATION HOOK AT 0x004826DD

// Ensures SegaRenderer::m_pDevice is NEVER NULL upon construction

// ---------------------------------------------------------------------------------

static BYTE s_OrigRendererInitBytes[6] = {0};

static bool s_RendererInitHookInstalled = false;

extern "C" void __cdecl LogRendererInit(uintptr_t renderer, uintptr_t container)

{

    std::string logPath = GetModDirectory() + "debug_hook_log.txt";

    FILE* fp = fopen(logPath.c_str(), "a");

    if (fp)

    {

        uintptr_t devFromContainer = (container ? *(uintptr_t*)(container + 8) : 0);

        fprintf(fp, "hkRendererInit: renderer=%p, container=%p, devFromContainer=%p, g_pActiveD3DDevice=%p\n",

            (void*)renderer, (void*)container, (void*)devFromContainer, (void*)g_pActiveD3DDevice);

        fclose(fp);

    }

}

__declspec(naked) static void hkRendererInitNaked()

{

    __asm

    {

        pushad

        pushfd

        push edx

        push esi

        call LogRendererInit

        add esp, 8

        popfd

        popad

        mov eax, dword ptr [edx + 8]

        test eax, eax

        jnz device_ok

        mov eax, dword ptr [g_pActiveD3DDevice]

    device_ok:

        mov dword ptr [esi + 0x18], eax

        test edx, edx

        jz done

        cmp dword ptr [edx + 8], 0

        jnz done

        mov dword ptr [edx + 8], eax

    done:

        push 0x004826E3

        ret

    }

}

// ---------------------------------------------------------------------------------

// SEGA RENDERER SET VIEWPORT SAFE-GUARD AT 0x004816D0 (FIXES 0xC0000005 CRASH AT 0x0048176E)

// ---------------------------------------------------------------------------------

static BYTE s_OrigSetViewportBytes[6] = {0};

static bool s_SetViewportHookInstalled = false;

__declspec(naked) static void hkSegaSetViewportNaked()

{

    __asm

    {

        test ecx, ecx

        jz fail_exit

        cmp dword ptr [ecx + 0x18], 0

        jnz device_valid

        mov eax, dword ptr [g_pActiveD3DDevice]

        test eax, eax

        jz fail_exit

        mov dword ptr [ecx + 0x18], eax

    device_valid:

        push ebp

        mov ebp, esp

        sub esp, 0x20

        push 0x004816D6

        ret

    fail_exit:

        ret 4

    }

}

// ---------------------------------------------------------------------------------

// SEGA KEYBOARD MANAGER VTABLE HOOK AT 0x0059E8C0 (METHOD 7: PollKeyboard)

// Directly intercepts Sega's per-frame hardware keyboard polling and updates buffer

// ---------------------------------------------------------------------------------

typedef void (__thiscall *tKeyboardPoll)(void* pThis);

static tKeyboardPoll s_oKeyboardPoll = (tKeyboardPoll)0x00486390;

static bool s_KeyPollHookInstalled = false;

static DWORD s_OldKeyPollProtect = 0;

static void __fastcall hkKeyboardPoll(void* pThis, void* edx_unused)

{

    // 1. Call original Sega keyboard poller

    s_oKeyboardPoll(pThis);

    // 2. Locate active double-buffer:

    // Offset 0x3A0 is the active buffer index (0 or 1)

    // Offset 0x19C is the buffer base (256 bytes per buffer)

    __try {

        DWORD activeIdx = *(DWORD*)((BYTE*)pThis + 0x3A0);

        BYTE* pBuffer = ((BYTE*)pThis + 0x19C) + (activeIdx << 8);

        ProcessKeyboardBuffer(pBuffer);

    } __except (EXCEPTION_EXECUTE_HANDLER) {}

}

// ---------------------------------------------------------------------------------

// INPUT & CONTROLLER STATE HOOK (FALLBACK SYNTHESIZER IN MAIN LOOP)

// Hooked at 0x004EA3A8 (call 0x004F2ED0 in main per-frame loop)

// ---------------------------------------------------------------------------------

typedef void (__cdecl *fnF2ED0)();

static fnF2ED0 oF2ED0 = (fnF2ED0)ADDR_F2ED0_FN;

// ---------------------------------------------------------------------------------

// FEATURE: STEERING MODES (per-frame transform of the steering consumed by physics)

// ---------------------------------------------------------------------------------

// sub_4E99A0 computes flt_16A2D10 (0x016A2D10) as a float [-1.0f, +1.0f], and sub_519890

// (vehicle physics) reads it directly at 0x00519908 to compute front wheel angle word_1662044

// and lateral tire slip.

//

// Mode 0: Standard (Original arcade handling)

// Mode 1: Enhanced Steering (Sharper response, eliminated deadzone, +25% steer power)

// Mode 2: 4-Wheel Steering (Extreme agile turn-in, 1.45x power, active rear-wheel yaw assist)

// Mode 3: Drunk Driver (Sinusoidal wobble & sluggish reaction)

// Mode 4: Slippery Ice (Heavy inertia EMA lag + low turn authority)

// Mode 5: Reverse Steering (Inverted controls)

constexpr float STEER_FRAME_DT        = 1.0f / 60.0f; // engine simulates at a fixed 60 Hz

constexpr float STEER_DRUNK_WOBBLE_A1 = 0.35f;  // primary wobble amplitude

constexpr float STEER_DRUNK_WOBBLE_F1 = 2.1f;   // primary wobble frequency (rad/s)

constexpr float STEER_DRUNK_WOBBLE_A2 = 0.18f;  // secondary wobble amplitude

constexpr float STEER_DRUNK_WOBBLE_F2 = 4.3f;   // secondary wobble frequency (rad/s)

constexpr float STEER_DRUNK_WOBBLE_PH = 1.2f;   // secondary wobble phase (rad)

constexpr float STEER_DRUNK_LAG_TAU   = 0.22f;  // input sluggishness (EMA time constant, s)

constexpr float STEER_ICE_LAG_TAU     = 0.55f;  // strong steering inertia (EMA time constant, s)

constexpr float STEER_ICE_AUTHORITY   = 0.50f;  // reduced steering authority

static float s_SteerEmaFast    = 0.0f; // mode 3 smoothed input

static float s_SteerEmaSlow    = 0.0f; // mode 4 smoothed input

static float s_SteerWobbleT    = 0.0f; // mode 3 wobble clock (s)

static int   s_SteerActiveMode = 0;    // mode the filter state is valid for

static void ResetSteeringFilter()

{

    s_SteerEmaFast = 0.0f;

    s_SteerEmaSlow = 0.0f;

    s_SteerWobbleT = 0.0f;

    s_SteerActiveMode = 0;

}

static void ApplySteeringModeTransform()

{

    int mode = g_Config.steeringMode;

    if (mode < 0 || mode > 5)

        mode = 0;

    // Vanilla passthrough in menus, fly mode, freecam and passenger cutscenes; the filter

    // state is invalidated so re-entering gameplay never carries stale smoothing.

    bool inCutscene = false;

    __try {

        inCutscene = (*(int*)ADDR_FARE_CUTSCENE != 0);

    } __except (EXCEPTION_EXECUTE_HANDLER) {}

    if (mode <= 0 || !IsInGame() || g_bMenuOpen || g_Config.flyMode || g_Config.freecamEnabled || inCutscene)

    {

        ResetSteeringFilter();

        return;

    }

    if (s_SteerActiveMode != mode)

    {

        s_SteerActiveMode = mode;

        s_SteerEmaFast = 0.0f;

        s_SteerEmaSlow = 0.0f;

        s_SteerWobbleT = 0.0f;

    }

    // Read the fresh steering float from the engine (calculated in sub_4E99A0)

    // -1.0f = full left, +1.0f = full right

    float raw = 0.0f;

    __try {

        raw = *(float*)ADDR_INPUT_STEER_FLOAT;

    } __except (EXCEPTION_EXECUTE_HANDLER) {

        return;

    }

    // Keyboard fallback check (if raw float is centered but arrow/custom keys are down)

    if (fabsf(raw) < 0.05f)

    {

        bool kLeft = (GetAsyncKeyState(VK_LEFT) & 0x8000) != 0 ||

                     (g_Config.keyLeft != 0 && (GetAsyncKeyState(g_Config.keyLeft) & 0x8000) != 0);

        bool kRight = (GetAsyncKeyState(VK_RIGHT) & 0x8000) != 0 ||

                      (g_Config.keyRight != 0 && (GetAsyncKeyState(g_Config.keyRight) & 0x8000) != 0);

        if (kLeft && !kRight)

            raw = -1.0f;

        else if (kRight && !kLeft)

            raw = 1.0f;

    }

    float out = raw;

    switch (mode)

    {

    case 1: // 4-Wheel Steering (4WS): clean raw steering passed to physical 4WS logic

    {

        out = raw;

        break;

    }

    case 2: // Drunk Driver: sinusoidal wobble on top of sluggish EMA input

    {

        s_SteerWobbleT += STEER_FRAME_DT;

        float alpha = STEER_FRAME_DT / (STEER_DRUNK_LAG_TAU + STEER_FRAME_DT);

        s_SteerEmaFast += (raw - s_SteerEmaFast) * alpha;

        float wobble = STEER_DRUNK_WOBBLE_A1 * sinf(STEER_DRUNK_WOBBLE_F1 * s_SteerWobbleT) +

                       STEER_DRUNK_WOBBLE_A2 * sinf(STEER_DRUNK_WOBBLE_F2 * s_SteerWobbleT + STEER_DRUNK_WOBBLE_PH);

        out = s_SteerEmaFast + wobble;

        break;

    }

    case 3: // Slippery Ice: sluggish initial turn-in (understeer into corner)

    {

        float alpha = STEER_FRAME_DT / (0.35f + STEER_FRAME_DT);

        s_SteerEmaSlow += (raw - s_SteerEmaSlow) * alpha;

        out = s_SteerEmaSlow * 0.70f;

        break;

    }

    default:

        ResetSteeringFilter();

        return;

    }

    if (out > 1.0f)  out = 1.0f;

    if (out < -1.0f) out = -1.0f;

    __try {

        // Write transformed steer float into the address consumed by sub_519890

        *(float*)ADDR_INPUT_STEER_FLOAT = out;

        // Also mirror to legacy int16 axes for any secondary consumers

        int16_t steer16 = (int16_t)(out * 32767.0f);

        *(int16_t*)ADDR_INPUT_STEER_AXIS = steer16;

        *(int16_t*)(ADDR_INPUT_STEER_AXIS + ADDR_INPUT_BUFFER_DELTA) = steer16;

    } __except (EXCEPTION_EXECUTE_HANDLER) {}

}

void ProcessCustomControls()

{

    // Note: Steering mode transform is executed exclusively inside PreVehiclePhysics()

    // with a frame guard to prevent double-inversion.

    if (g_bMenuOpen)

        return;

    // Helper lambda to safely write to both input buffers (raw 0x16a2848 and latched 0x16a287c)

    auto SetBothButtons = [](uint32_t val, bool bOr = false) {

        if (bOr) {

            *(uint32_t*)ADDR_INPUT_BUTTONS   |= val;

            *(uint32_t*)ADDR_INPUT_BUTTONS_2 |= val;

        } else {

            *(uint32_t*)ADDR_INPUT_BUTTONS   = val;

            *(uint32_t*)ADDR_INPUT_BUTTONS_2 = val;

        }

    };

    auto MaskBothButtons = [](uint32_t maskToClear) {

        *(uint32_t*)ADDR_INPUT_BUTTONS   &= ~maskToClear;

        *(uint32_t*)ADDR_INPUT_BUTTONS_2 &= ~maskToClear;

    };

    if (!IsInGame())

    {

        bool autoStart = g_Config.autoDemo || (getenv("CRAZYTAXI_AUTO_START") != nullptr);

        static int s_AutoMenuTimer2 = 0;

        if (autoStart)

        {

            s_AutoMenuTimer2++;

            if ((s_AutoMenuTimer2 / 10) % 2 == 1)

            {

                SetBothButtons(0x1000A, true); // BTN_START (0x8) + BTN_A (0x2)

            }

            else

            {

                MaskBothButtons(0x1000A);

            }

            return;

        }

        bool isEnter = (GetAsyncKeyState(VK_RETURN) & 0x8000) != 0;

        bool isSpace = (GetAsyncKeyState(VK_SPACE)  & 0x8000) != 0;

        bool isD     = (GetAsyncKeyState('D')       & 0x8000) != 0;

        if (isEnter || isSpace)

        {

            SetBothButtons(0x1000A, true); // BTN_START (0x8) + BTN_A (0x2)

        }

        else

        {

            MaskBothButtons(0x1000A);

        }

        if (isD)

        {

            SetBothButtons(0x02, true); // BTN_A

        }

        else

        {

            MaskBothButtons(0x02);

        }

        return;

    }

    auto SetBothSteer = [](int16_t steer) {

        *(int16_t*)ADDR_INPUT_STEER_AXIS                       = steer;

        *(int16_t*)(ADDR_INPUT_STEER_AXIS + ADDR_INPUT_BUFFER_DELTA) = steer;

    };

    auto SetBothAccel = [](uint16_t accel) {

        *(uint16_t*)ADDR_INPUT_ACCEL_AXIS                       = accel;

        *(uint16_t*)(ADDR_INPUT_ACCEL_AXIS + ADDR_INPUT_BUFFER_DELTA) = accel;

    };

    auto SetBothBrake = [](uint16_t brake) {

        *(uint16_t*)ADDR_INPUT_BRAKE_AXIS                       = brake;

        *(uint16_t*)(ADDR_INPUT_BRAKE_AXIS + ADDR_INPUT_BUFFER_DELTA) = brake;

    };

    // In Freecam mode: completely neutralize vehicle controls across all buffers so taxi stays frozen!

    if (g_Config.freecamEnabled)

    {

        SetBothButtons(0);

        SetBothSteer(0);

        SetBothAccel(0);

        SetBothBrake(0x7FFF); // Handbrake fully engaged

        *(uint8_t*)ADDR_INPUT_BTN_ACCEL    = 0;

        *(uint8_t*)ADDR_INPUT_BTN_BRAKE    = 1;

        *(uint8_t*)ADDR_INPUT_BTN_GEAR_D   = 0;

        *(uint8_t*)ADDR_INPUT_BTN_GEAR_R   = 0;

        *(uint8_t*)ADDR_INPUT_EDGE_DRIVE   = 0;

        *(uint8_t*)ADDR_INPUT_EDGE_REV     = 0;

        *(uint8_t*)(ADDR_INPUT_BTN_ACCEL  + ADDR_INPUT_BUFFER_DELTA) = 0;

        *(uint8_t*)(ADDR_INPUT_BTN_BRAKE  + ADDR_INPUT_BUFFER_DELTA) = 1;

        *(uint8_t*)(ADDR_INPUT_BTN_GEAR_D + ADDR_INPUT_BUFFER_DELTA) = 0;

        *(uint8_t*)(ADDR_INPUT_BTN_GEAR_R + ADDR_INPUT_BUFFER_DELTA) = 0;

        *(uint8_t*)(ADDR_INPUT_EDGE_DRIVE + ADDR_INPUT_BUFFER_DELTA) = 0;

        *(uint8_t*)(ADDR_INPUT_EDGE_REV   + ADDR_INPUT_BUFFER_DELTA) = 0;

        return;

    }

    // Check if currently in passenger pickup, boarding, or dropoff cutscene:

    // NOTE: ADDR_FARE_CUTSCENE (>0) is the ONLY genuine cutscene flag; ADDR_VEHICLE_ACTIVE_INDEX is always 1 when alive!

    bool inCutscene = false;

    __try {

        inCutscene = (*(int*)ADDR_FARE_CUTSCENE != 0);

    } __except (EXCEPTION_EXECUTE_HANDLER) {}

    if (inCutscene)

    {

        // Strictly protect passenger cutscenes and state transitions!

        return;

    }

    // Native Arcade Controls: Sega naturally decodes arrow keys, D (Drive), and S (Reverse).

    // No artificial control overrides are injected during standard gameplay!

}

static void __cdecl hkMainInputLoop()

{

    oF2ED0(); // Execute native 0x004F2ED0 (soft-reset check etc.)

    ProcessCustomControls(); // Inject synthesized control overrides

}

// ---------------------------------------------------------------------------------

// TELEPORT LATCH HELPERS (T3 fix plan, scratch/re_t3_teleport.md section E)

// ---------------------------------------------------------------------------------

// Ground probe using the engine's own raycast_down_collision (0x0050C6D0).
// Verified semantics (raw disassembly, scratch/out_t3_raycheck.txt): collision_intersect_
// test (0x0050BED0) is a pure XZ polygon test - every overlapping object of the query's
// grid cell contributes its plane height; the selection returns the HIGHEST walkable
// (type != 13) surface at or below the query point, and only surfaces ABOVE the query
// are subject to the 20.0 window (climbed in <=20-unit steps). It reads the session-init-
// resident collision grid (init_stage_collision_grid_and_relocate_50d050 0x0050D050) -
// it does NOT depend on chunk streaming (T3 B.1). A miss therefore means "no walkable
// polygon at this XZ" (void/water/interior column), not "too high above ground".
// Result struct 0x01EB0CE8: +24 = hit height (flt_1EB0D00), +28 = hit type (dword_1EB0D04,
// -1 = miss).
// Thread/shared-state model (review finding): "read-only" applies to the vehicle and
// game state only - the call itself WRITES shared engine scratch (the result struct
// 0x1EB0CE8..0x1EB0D08 and the candidate arrays 0x1EB0D30/0x1EB0D08) on every invocation,
// and the engine's wheel raycasts in sub_513E80 use the same non-reentrant function. The
// result is therefore consumed synchronously right here, immediately after the call
// returns. All mod call paths (TeleportTaxi via the ImGui/EndScene path, the Pre/Post
// latch probes via the physics thunk) run on the engine's single main thread (T3 C.3:
// main_game_loop 0x004EA408 drives both the physics step and the D3D9 present path), so
// the write-then-read cannot interleave with the engine's or the mod's other uses of
// these statics. If that single-thread model ever changes, this probe AND the mod's
// direct ROOT/BLOCK writes from TeleportTaxi would need synchronization together.
static bool TeleportProbeGround(float x, float y, float z, float* groundY)
{
    typedef float* (__cdecl *tRaycastDownCollisionFn)(float* pos, float range);
    tRaycastDownCollisionFn pfnRaycast = (tRaycastDownCollisionFn)ADDR_RAYCAST_DOWN_COLLISION;
    if (!pfnRaycast || !groundY)
        return false;
    float query[3] = { x, y, z };
    __try
    {
        float* res = pfnRaycast(query, 20.0f); // 20.0 = the respawn's own wheel-raycast range
        if (!res || *(int*)(res + 7) == -1)    // res+28 = dword_1EB0D04: -1 = miss
            return false;
        *groundY = res[6];                     // res+24 = flt_1EB0D00: hit height
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// The complete teleport write set, asserted on BOTH sides of the physics step:
// - PreVehiclePhysics (before sub_519890): sub_513E80 (0x00513E80, the off-world respawn)
//   then evaluates its wheel raycasts, ground clamping, waypoint/sector updates and
//   matrix pushes AT THE TARGET - a respawn can only trigger on real target geometry.
// - PostVehiclePhysics (after sub_519890): the integration result (and a respawn that
//   fired inside the step) is overpainted before render_chassis_and_wheels (0x00517D70)
//   reads ROOT for the chassis draw.
// Field set (T3 E steps 1+5): ROOT/BLOCK/PREV = target (root<->block sync 0x00519CAF and
// speed derivation neutralized), VEL + respawn-nullled VEL mirror 0x01662038/3C/40 +
// DISP/SPEED = 0, prev orientation = current attitude (sweep start pose +152/154/156 of
// sub_5146C0), camera look-at rides along for visual/audio consistency (never a physics
// source).
static void TeleportLatchApply()
{
    __try
    {
        float x = s_TeleportLatchX;
        float y = s_TeleportLatchY;
        float z = s_TeleportLatchZ;

        *(float*)ADDR_VEHICLE_ROOT_X  = x;
        *(float*)ADDR_VEHICLE_ROOT_Y  = y;
        *(float*)ADDR_VEHICLE_ROOT_Z  = z;
        *(float*)ADDR_VEHICLE_BLOCK_X = x;
        *(float*)ADDR_VEHICLE_BLOCK_Y = y;
        *(float*)ADDR_VEHICLE_BLOCK_Z = z;
        *(float*)ADDR_VEHICLE_PREV_X  = x;
        *(float*)ADDR_VEHICLE_PREV_Y  = y;
        *(float*)ADDR_VEHICLE_PREV_Z  = z;

        // Camera look-at rides along for visual/audio consistency (never a physics source)
        *(float*)ADDR_PLAYER_X = x;
        *(float*)ADDR_PLAYER_Y = y;
        *(float*)ADDR_PLAYER_Z = z;

        // Zero velocity/displacement/speed so the teleport delta is not re-derived
        // as recoil on the next frame. The VEL mirror (0x01662038/3C/40) is part of the
        // respawn's own write set (sub_513E80 nulls it) and must follow suit.
        *(float*)ADDR_VEHICLE_VEL_X = 0.0f;
        *(float*)ADDR_VEHICLE_VEL_Y = 0.0f;
        *(float*)ADDR_VEHICLE_VEL_Z = 0.0f;
        *(float*)ADDR_VEHICLE_VEL_MIRROR_X = 0.0f;
        *(float*)ADDR_VEHICLE_VEL_MIRROR_Y = 0.0f;
        *(float*)ADDR_VEHICLE_VEL_MIRROR_Z = 0.0f;
        *(float*)ADDR_VEHICLE_DISP_X = 0.0f;
        *(float*)ADDR_VEHICLE_DISP_Y = 0.0f;
        *(float*)ADDR_VEHICLE_DISP_Z = 0.0f;
        *(float*)ADDR_VEHICLE_SPEED_SCALAR = 0.0f;

        // Prev orientation = current attitude: the swept-sphere solver (sub_5146C0) starts
        // its sweep from these fields (+152/154/156), so pointing them at the target pose
        // keeps it from sweeping across the whole teleport delta.
        *(uint16_t*)ADDR_VEHICLE_PREV_ATTITUDE_BAM     = *(uint16_t*)ADDR_VEHICLE_ATTITUDE_BAM;
        *(uint16_t*)(ADDR_VEHICLE_PREV_ATTITUDE_BAM + 2) = *(uint16_t*)(ADDR_VEHICLE_ATTITUDE_BAM + 2);
        *(uint16_t*)ADDR_VEHICLE_PREV_ROLL_BAM         = *(uint16_t*)ADDR_VEHICLE_ROLL_REAL_BAM;

        // Keep the flight integrator on the latched target while Fly Mode is active;
        // the fly writer below then emits the exact same position
        if (g_Config.flyMode)
        {
            s_FlyPosX            = x;
            s_FlyCurrentAltitude = y;
            s_FlyPosZ            = z;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ---------------------------------------------------------------------------------

// VEHICLE PHYSICS HOOK (runs around the REAL physics step sub_519890 via the patched

// call sites 0x005113B7 / 0x00512029). Gravity is neutralized while Fly Mode is active

// by the absolute per-frame position writes below - the gravity constants themselves

// are shared engine values and are never modified.

// ---------------------------------------------------------------------------------

extern "C" void __cdecl PostVehiclePhysics()

{

    // Collision suppression is armed ONLY by the fly writer below; starting every call
    // disarmed makes all early-outs and vanilla paths fail safe to active ground collision.

    s_FlyHasControl = false;

    // Captured once per call: the Pre-assert in PreVehiclePhysics and the teleport latch
    // block below must see the SAME in-game verdict within one physics frame (review
    // finding on the Pre/Post gate asymmetry). Without this capture the latch block could
    // still run through the Fly Mode test-scene exception below while the pre-assert had
    // already skipped for !IsInGame().
    bool inGameNow = IsInGame();

    if (!inGameNow)

    {

        // A teleport latch armed in a session that really ended must never leak into the
        // next one (T3 D.4.1). Only the unambiguous menu states (0 = menu/attract demo,
        // 7 = quit to menu) count as session end: the IsInGame() heuristics flicker during
        // normal gameplay (see UpdateGameFeaturesPerFrame) and must not cancel an active
        // latch mid-flight.
        __try {
            int mainState = *(int*)ADDR_GAME_FLAG;
            if (mainState == 0 || mainState == 7)
                s_TeleportLatchActive = false;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}

        // Allow Fly Mode elevation lock in active 3D world even during test scenes or attract demo

        __try {

            int sub = *(int*)0x016a2804;

            float y = *(float*)ADDR_VEHICLE_ROOT_Y;

            if (!(g_Config.flyMode && sub == 3 && fabs(y) > 50.0f))

            {

                // A pending cutscene handback must not survive the session into the next one

                s_FlyWasInCutscene = false;

                return;

            }

        } __except (EXCEPTION_EXECUTE_HANDLER) {

            s_FlyWasInCutscene = false;

            return;

        }

    }

    // Camera/vehicle view state read BEFORE the cutscene decision: camera states 10/11 are
    // the pickup/drop-off cutscene cameras (the shot index 0x016F2164 alone reads stale
    // after a cutscene - see below).

    int vehState = 0;

    __try {
        vehState = *(int*)ADDR_VEHICLE_STATE;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}

    bool inCutscene = false;

    __try {
        int fareCutscene = 0;
        fareCutscene = *(int*)ADDR_FARE_CUTSCENE;

        // 0x016F2164 is the cutscene shot index and is NEVER reset when a cutscene ends
        // (stays at the last shot 5-7; its only readers are the shot directors, which run
        // exclusively under camera state 10/11). Non-zero with any other camera state is
        // therefore provably stale residue (vanilla leaves it after every drop-off and the
        // pickup skip leaves it too): clear it once so the fly writer resumes instead of
        // gating on the dead value forever.
        if (g_Config.flyMode && fareCutscene != 0 && vehState != 10 && vehState != 11)
        {
            *(int*)ADDR_FARE_CUTSCENE = 0;
            fareCutscene = 0;
        }

        inCutscene = (fareCutscene != 0) || vehState == 10 || vehState == 11;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}

    // Teleport latch: assert the full write set AFTER the physics step (the Pre replay in
    // PreVehiclePhysics already made sub_513E80 evaluate at the target) and run the latch
    // lifecycle. Gated on the same verdict the Pre-assert used (inGameNow) plus the real
    // cutscene camera (state 10/11) - gating on the raw shot index here would let its
    // stale post-cutscene value freeze the countdown forever. Release needs ground
    // contact at the target (read raycast) AND the minimum hold, or the watchdog cap
    // expires - genuine off-map/water targets then hand back to the vanilla rescue
    // instead of freezing the taxi forever (T3 E step 3).
    if (inGameNow && s_TeleportLatchActive && vehState != 10 && vehState != 11)
    {
        // Step-0 diagnostics: capture the engine's post-step ROOT before the replay
        // overpaints it. A value far off the latch target proves the off-world respawn
        // (sub_513E80) or the root<->block sync (0x00519CAF) snapped back during the step.
        float preRootX = 0.0f, preRootY = 0.0f, preRootZ = 0.0f;
        __try {
            preRootX = *(float*)ADDR_VEHICLE_ROOT_X;
            preRootY = *(float*)ADDR_VEHICLE_ROOT_Y;
            preRootZ = *(float*)ADDR_VEHICLE_ROOT_Z;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}

        TeleportLatchApply();

        // Lifecycle: one decrement per physics frame (a cutscene pause above skips both
        // countdowns and the release checks).
        s_TeleportLatchFrames--;   // minimum-hold countdown (armed with 10)
        s_TeleportLatchWatchdog--; // hard cap (~600 physics frames = ~10 s)

        // Ground contact probe: raycast_down_collision (0x0050C6D0) returns the highest
        // walkable surface at or below the query point (verified against the raw
        // disassembly: below-window candidates are unbounded, above-window climbs in
        // (v9, v9+20] steps), so probing 2 units below the latched root sees exactly the
        // surface the taxi rests on - hit = release. The collision grid is XZ-selected
        // and session-resident, so this probe cannot flip later; genuine void targets
        // never release and run into the watchdog. Fly Mode releases after the minimum
        // hold alone: the fly writer owns the position every frame with the identical
        // write set, so pinning the user for up to 10 s would regress fly control without
        // adding any respawn protection.
        float groundY = 0.0f;
        bool groundContact = TeleportProbeGround(s_TeleportLatchX, s_TeleportLatchY - 2.0f, s_TeleportLatchZ, &groundY);
        bool minHoldElapsed  = (s_TeleportLatchFrames <= 0);
        bool watchdogExpired = (s_TeleportLatchWatchdog <= 0);
        // Typed aerial targets (exact Y honored since the clamp removal) never report
        // ground contact: release after 10 min-hold + ~50 more frames (~1 s) so the taxi
        // settles by falling naturally instead of hanging frozen for the 600-frame cap.
        bool aerialFallback  = (s_TeleportLatchFrames <= -50);

        // Step-0 diagnostics: first 10 frames after the click, ROOT before vs. target.
        if (s_TeleportLatchLog < 10)
        {
            Logger::Info("Teleport", "latch f%02d st=%d rootBefore=%.1f/%.1f/%.1f -> target %.1f/%.1f/%.1f groundHit=%d",
                         s_TeleportLatchLog + 1, vehState, preRootX, preRootY, preRootZ,
                         s_TeleportLatchX, s_TeleportLatchY, s_TeleportLatchZ, groundContact ? 1 : 0);
        }
        s_TeleportLatchLog++;

        if ((groundContact && minHoldElapsed) || watchdogExpired || aerialFallback || (g_Config.flyMode && minHoldElapsed))
        {
            int heldFrames = 600 - s_TeleportLatchWatchdog;
            if (watchdogExpired && !(groundContact && minHoldElapsed))
            {
                Logger::Warn("Teleport", "latch watchdog expired after %d frames - target %.1f/%.1f/%.1f has no reachable ground, vanilla rescue takes over",
                             heldFrames, s_TeleportLatchX, s_TeleportLatchY, s_TeleportLatchZ);
            }
            else if (s_TeleportLatchLog > 10)
            {
                Logger::Info("Teleport", "latch released after %d frames at %.1f/%.1f/%.1f (groundHit=%d groundY=%.1f)",
                             heldFrames, s_TeleportLatchX, s_TeleportLatchY, s_TeleportLatchZ,
                             groundContact ? 1 : 0, groundY);
            }
            s_TeleportLatchActive = false;
        }
        else
        {
            // T3 E step 4 pumped the chunk queue 3x per latch frame here - REMOVED (T4
            // live finding): draining queue entries mid-physics-step (the engine expects
            // exactly one pump per frame from main_game_loop 0x004EA408 via
            // evict_and_bind_textures 0x005051D0) corrupted the texture binding pass and
            // left the whole world untextured after a teleport. Combined with the removed
            // 1800-radius pre-warm this returns streaming to 100% engine-owned pacing;
            // short pop-in around the target is vanilla respawn behavior.
        }
    }

    if (g_Config.flyMode && !inCutscene)
    {
        if (!s_FlyInitialized)
        {
            // Initialize flight state from the vehicle root transform (real taxi position)
            __try {
                s_FlyPosX            = *(float*)ADDR_VEHICLE_ROOT_X;
                s_FlyCurrentAltitude = *(float*)ADDR_VEHICLE_ROOT_Y;
                s_FlyPosZ            = *(float*)ADDR_VEHICLE_ROOT_Z;
                s_FlyYawDeg          = (float)(*(uint16_t*)ADDR_VEHICLE_HEADING_BAM) * (360.0f / 65536.0f);
                s_FlyInitialized     = true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }

        // Resume lift after a fare cutscene: flight re-initializes from the settled
        // vanilla position; the extra altitude clears any rest-clipping from the handback.
        if (s_FlyWasInCutscene)
        {
            s_FlyCurrentAltitude += 2.5f;
            s_FlyWasInCutscene = false;
        }

        // Absolute position write AFTER the physics step. This block is idempotent because
        // the flight integrator advances exactly once per frame in UpdateGameFeaturesPerFrame.
        // Writing root + dynamics block + prev fields keeps Sega's root<->block sync
        // (0x00519CAF) from snapping the position back and prevents the movement delta
        // from being re-derived as speed on the next frame.
        __try {
            float x = s_FlyPosX;
            float y = s_FlyCurrentAltitude;
            float z = s_FlyPosZ;

            *(float*)ADDR_VEHICLE_ROOT_X  = x;
            *(float*)ADDR_VEHICLE_ROOT_Y  = y;
            *(float*)ADDR_VEHICLE_ROOT_Z  = z;
            *(float*)ADDR_VEHICLE_BLOCK_X = x;
            *(float*)ADDR_VEHICLE_BLOCK_Y = y;
            *(float*)ADDR_VEHICLE_BLOCK_Z = z;
            *(float*)ADDR_VEHICLE_PREV_X  = x;
            *(float*)ADDR_VEHICLE_PREV_Y  = y;
            *(float*)ADDR_VEHICLE_PREV_Z  = z;

            // Neutralize gravity/impulse feedback: zero velocity, frame displacement, speed
            *(float*)ADDR_VEHICLE_VEL_X = 0.0f;
            *(float*)ADDR_VEHICLE_VEL_Y = 0.0f;
            *(float*)ADDR_VEHICLE_VEL_Z = 0.0f;
            *(float*)ADDR_VEHICLE_DISP_X = 0.0f;
            *(float*)ADDR_VEHICLE_DISP_Y = 0.0f;
            *(float*)ADDR_VEHICLE_DISP_Z = 0.0f;
            *(float*)ADDR_VEHICLE_SPEED_SCALAR = 0.0f;

            // Orientation: yaw from the flight steering, pitch/roll level
            int16_t yawBam = (int16_t)((int)(s_FlyYawDeg * (65536.0f / 360.0f)) & 0xFFFF);
            *(uint16_t*)ADDR_VEHICLE_HEADING_BAM = (uint16_t)yawBam;
            *(uint16_t*)ADDR_VEHICLE_ATTITUDE_BAM = 0;                      // pitch LO16 = 0
            *(uint16_t*)(ADDR_VEHICLE_ATTITUDE_BAM + 2) = (uint16_t)yawBam; // yaw HI16
            *(uint16_t*)ADDR_VEHICLE_ROLL_REAL_BAM = 0;

            // Fly writer owns the vehicle from here: hooks re-arm collision suppression
            // for the next physics step (see s_FlyHasControl).
            s_FlyHasControl = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    else if (g_Config.flyMode)
    {
        // Fare cutscene while flying: vanilla owns the vehicle completely - no writes at
        // all (not even VEL_Y, it would fight the cutscene physics). Park the fly state
        // and remember the transition for the resume lift above.
        s_FlyInitialized   = false;
        s_FlyWasInCutscene = true;
    }
    else
    {
        // Fly Mode turned off: hand control back to the vanilla physics.
        // World/ground collision resumes automatically (the raycast hook passes through again).
        if (s_FlyInitialized)
        {
            __try {
                *(float*)ADDR_VEHICLE_VEL_Y = 0.0f; // Avoid a hard slam on re-entry
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
            s_FlyInitialized = false;
        }
        // Cleared independent of s_FlyInitialized: fly can be switched off mid-cutscene
        // after the parked branch armed the flag, and a stale flag here would fire a
        // spurious resume lift on the next fly enable.
        s_FlyWasInCutscene = false;
    }

    // FEATURE: CONTINUOUS SPEED MULTIPLIER & NITRO ACCELERATION BOOST (+50% TOP SPEED, FORWARD THRUST)
    if (!g_Config.flyMode && vehState != 10 && vehState != 11)
    {
        float speedMultiplier = (g_Config.enableSpeedMultiplier && g_Config.speedMultiplier > 1.0f) ? g_Config.speedMultiplier : 1.0f;
        bool nitroActive = IsNitroActive();

        __try {
            uint8_t gear = *(uint8_t*)ADDR_INPUT_GEAR_STATE; // 0/2 = Drive, 1 = Reverse
            float dispX = *(float*)ADDR_VEHICLE_DISP_X;
            float dispZ = *(float*)ADDR_VEHICLE_DISP_Z;
            float dispLen = sqrtf(dispX * dispX + dispZ * dispZ);

            // 1. Continuous Speed Multiplier Boost on real vehicle displacement
            if (speedMultiplier > 1.0f && dispLen > 0.05f)
            {
                float boostFactor = speedMultiplier - 1.0f;
                float addX = dispX * boostFactor;
                float addZ = dispZ * boostFactor;

                // Apply exclusively to ROOT and BLOCK (never PREV, otherwise delta is canceled!)
                *(float*)ADDR_VEHICLE_ROOT_X  += addX;
                *(float*)ADDR_VEHICLE_ROOT_Z  += addZ;
                *(float*)ADDR_VEHICLE_BLOCK_X += addX;
                *(float*)ADDR_VEHICLE_BLOCK_Z += addZ;
            }

            // 2. Nitro Forward Thrust Boost (+50% Top Speed)
            if (nitroActive && gear != 1)
            {
                uint16_t yawBams = *(uint16_t*)ADDR_VEHICLE_HEADING_BAM;
                float yawRad = (float)yawBams * (6.283185307179586f / 65536.0f);
                float fwdX = sinf(yawRad);
                float fwdZ = cosf(yawRad);

                float maxNitroSpeed = 14.0f * speedMultiplier * 1.50f;
                if (dispLen < maxNitroSpeed)
                {
                    float nitroThrust = 0.35f * speedMultiplier;
                    *(float*)ADDR_VEHICLE_ROOT_X  += fwdX * nitroThrust;
                    *(float*)ADDR_VEHICLE_ROOT_Z  += fwdZ * nitroThrust;
                    *(float*)ADDR_VEHICLE_BLOCK_X += fwdX * nitroThrust;
                    *(float*)ADDR_VEHICLE_BLOCK_Z += fwdZ * nitroThrust;
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    // -------------------------------------------------------------

    // FEATURE: STEERING MODE YAW ASSISTS & WHEEL PHYSICS

    // -------------------------------------------------------------

    if (!g_Config.flyMode && vehState != 10 && vehState != 11)

    {

        int steerMode = g_Config.steeringMode;

        if (steerMode > 0 && steerMode <= 3)

        {

            __try {

                float curSpeed = *(float*)ADDR_VEHICLE_SPEED_SCALAR;

                float steer = *(float*)ADDR_INPUT_STEER_FLOAT;

                float mph = 0.0f;

                GetTaxiSpeed(mph);

                float kmh = mph * 1.609344f;

                // Mode 1: 4-Wheel Steering (4WS) - Dual-Mode Speed Blending

                if (steerMode == 1)

                {

                    // Front wheel steer angle

                    int16_t frontSteer = (int16_t)(-steer * 5000.0f);

                    *(int16_t*)ADDR_VEHICLE_WHEEL_STEER_BAM = frontSteer;

                    *(int16_t*)ADDR_WHEEL_FL_STEER_BAM = frontSteer;

                    *(int16_t*)ADDR_WHEEL_FR_STEER_BAM = frontSteer;

                    // Speed blending formula:

                    // delta_r = delta_f * clamp((v - v_neutral) / v_scale, -k_max, +k_min)

                    // v_neutral = 50 km/h, v_scale = 30 km/h, k_max = 0.75, k_min = 0.25

                    float blendRatio = (kmh - 50.0f) / 30.0f;

                    if (blendRatio < -0.75f) blendRatio = -0.75f;

                    if (blendRatio > 0.25f)  blendRatio = 0.25f;

                    // At low speed (blendRatio < 0): counter-phase (nimble, small turning circle)

                    // At high speed (blendRatio > 0): in-phase (crab-walk, high stability)

                    int16_t rearSteer = (int16_t)(frontSteer * blendRatio);

                    *(int16_t*)ADDR_WHEEL_RL_STEER_BAM = rearSteer;

                    *(int16_t*)ADDR_WHEEL_RR_STEER_BAM = rearSteer;

                    // Counter-phase adds turning agility; in-phase adds lateral stability

                    if (curSpeed > 0.3f && fabsf(steer) > 0.02f)

                    {

                        float yawAssist = steer * (80.0f * (0.25f - blendRatio));

                        *(int16_t*)ADDR_VEHICLE_YAW_RATE += (int16_t)yawAssist;

                    }

                }

                // Mode 2: Drunk Driver

                else if (steerMode == 2)

                {

                    int16_t frontSteer = (int16_t)(-steer * 4352.0f);

                    *(int16_t*)ADDR_VEHICLE_WHEEL_STEER_BAM = frontSteer;

                    *(int16_t*)ADDR_WHEEL_FL_STEER_BAM = frontSteer;

                    *(int16_t*)ADDR_WHEEL_FR_STEER_BAM = frontSteer;

                    *(int16_t*)ADDR_WHEEL_RL_STEER_BAM = 0;

                    *(int16_t*)ADDR_WHEEL_RR_STEER_BAM = 0;

                }

                // Mode 3: Slippery Ice Physics

                else if (steerMode == 3)

                {

                    int16_t frontSteer = (int16_t)(-steer * 3800.0f);

                    *(int16_t*)ADDR_VEHICLE_WHEEL_STEER_BAM = frontSteer;

                    *(int16_t*)ADDR_WHEEL_FL_STEER_BAM = frontSteer;

                    *(int16_t*)ADDR_WHEEL_FR_STEER_BAM = frontSteer;

                    *(int16_t*)ADDR_WHEEL_RL_STEER_BAM = 0;

                    *(int16_t*)ADDR_WHEEL_RR_STEER_BAM = 0;

                    if (curSpeed > 0.15f)

                    {

                        // 1. Get current velocity vector and chassis body yaw

                        float vx = *(float*)ADDR_VEHICLE_VEL_X;

                        float vz = *(float*)ADDR_VEHICLE_VEL_Z;

                        uint16_t bodyYaw = *(uint16_t*)(ADDR_VEHICLE_ATTITUDE_BAM + 2);

                        float yawRad = (float)bodyYaw * (6.28318530718f / 65536.0f);

                        float sinYaw = sinf(yawRad);

                        float cosYaw = cosf(yawRad);

                        // 2. Decompose into forward (vForward) and lateral (vSideways) velocity

                        float vForward  = vx * sinYaw + vz * cosYaw;

                        float vSideways = vx * cosYaw - vz * sinYaw;

                        // 3. Physical Ice Friction:

                        // On asphalt, vSideways is damped by ~90% per frame.

                        // On ice (mu ~ 0.1), preserve 98.5% of lateral momentum so the car slides sideways!

                        vSideways *= 0.985f;

                        // 4. Träge Vektor-Interpolation (Velocity decoupled from Heading)

                        float speed = sqrtf(vx * vx + vz * vz);

                        if (speed > 0.05f)

                        {

                            float newVx = vForward * sinYaw + vSideways * cosYaw;

                            float newVz = vForward * cosYaw - vSideways * sinYaw;

                            float newSpeed = sqrtf(newVx * newVx + newVz * newVz);

                            if (newSpeed > 0.01f)

                            {

                                float scale = speed / newSpeed;

                                *(float*)ADDR_VEHICLE_VEL_X = newVx * scale;

                                *(float*)ADDR_VEHICLE_VEL_Z = newVz * scale;

                            }

                        }

                        // 5. Reduced Yaw Damping (sustained power-slide / oversteer)

                        int16_t yawRate = *(int16_t*)ADDR_VEHICLE_YAW_RATE;

                        if (abs(yawRate) > 10)

                        {

                            *(int16_t*)ADDR_VEHICLE_YAW_RATE = (int16_t)(yawRate * 1.045f);

                        }

                    }

                }

            } __except (EXCEPTION_EXECUTE_HANDLER) {}

        }

    }

}

// Pre-physics hook: runs right before sub_519890 computes wheel angles & tire forces

extern "C" void __cdecl PreVehiclePhysics()

{

    // T3 fix plan step 1 (the core teleport fix): assert the teleport latch write set
    // BEFORE the physics step. sub_513E80 (0x00513E80, the off-world respawn) is the
    // first call inside sub_519890 (single caller 0x005198E2) and evaluates its 4 wheel
    // raycasts, ground clamping, waypoint/sector updates and matrix pushes against the
    // root position - without this pre-assert all of that ran on the PRE-teleport
    // environment, and a respawn firing inside the step wrote waypoint+30Y / zeroed
    // velocities that only the post-step replay could overpaint. With the position
    // already at the target, the respawn decision is made on real target geometry.
    // Same gates as the post-step replay: IsInGame + the real camera state 0x01661474
    // (10/11 = pickup/drop-off cutscene cameras own the vehicle). NEVER gate on the
    // stale shot index 0x016F2164.
    if (s_TeleportLatchActive && IsInGame())
    {
        int latchVehState = 0;
        __try {
            latchVehState = *(int*)ADDR_VEHICLE_STATE;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}

        if (latchVehState != 10 && latchVehState != 11)
            TeleportLatchApply();
    }

    ApplySteeringModeTransform();

    // Mode 3: Slippery Ice physics (wheel spin & traction reduction on throttle)

    if (g_Config.steeringMode == 3 && IsInGame() && !g_Config.flyMode)

    {

        __try {

            float* pThrottle = (float*)ADDR_INPUT_THROTTLE_FLOAT;

            if (pThrottle && *pThrottle > 0.35f)

            {

                *pThrottle = 0.35f + (*pThrottle - 0.35f) * 0.30f; // Limit drive traction on ice

            }

        } __except (EXCEPTION_EXECUTE_HANDLER) {}

    }

    // Nitro: guarantee full throttle into vehicle physics
    if (IsNitroActive() && IsInGame() && !g_Config.flyMode)
    {
        __try {
            float* pThrottle = (float*)ADDR_INPUT_THROTTLE_FLOAT;
            if (pThrottle) *pThrottle = 1.0f;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

}

// Speed scalar multiply cave, entered from the patched window at 0x00519E90 inside

// sub_519890. At this point flt_166208C holds the speed freshly derived from the previous

// frame's displacement and is decomposed into vX/vZ immediately afterwards (0x00519EA2+),

// so the factor scales the engine speed once per frame without accumulating; collision

// impulses never enter the amplification path. With factor 1.0f this is a numeric no-op.

// Original window bytes (IDA-verified):

//   F7 C7 FE 7F 04 80                test edi, 80047FFEh

//   74 0A                            je short 0x00519EA2

//   C7 05 E0 22 66 01 00 00 00 00    mov dword ptr [0x016622E0], 0

// NOTE: the game addresses below are intentionally hard literals (a constexpr symbol in

// [brackets] assembles as a memory load of the constant's storage, NOT an absolute operand).

static_assert(ADDR_VEHICLE_SPEED_SCALAR == 0x0166208c, "hkSpeedScalarCave literal out of sync");

static_assert(ADDR_SPEED_MULT_RESUME   == 0x00519ea2, "hkSpeedScalarCave literal out of sync");

__declspec(naked) static void hkSpeedScalarCave()

{

    __asm

    {

        // speed * factor: scale linear velocity scalar

        fld dword ptr ds:[0x0166208C]

        fmul dword ptr [s_SpeedMultFactor]

        fstp dword ptr ds:[0x0166208C]

        // ALSO scale wheel angular velocity flt_1662090 so tire slip integrator sub_515CD0

        // does not detect false burnout slip and clamp speed down!

        fld dword ptr ds:[0x01662090]

        fmul dword ptr [s_SpeedMultFactor]

        fstp dword ptr ds:[0x01662090]

        // Original instructions

        test edi, 80047FFEh

        je skip_input_reset

        mov dword ptr ds:[0x016622E0], 0

    skip_input_reset:

        // Return to the instruction after the patched window (push/ret preserves everything)

        push 0x00519EA2

        ret

    }

}

// Replaces the original "call sub_519890" at 0x005113B7 / 0x00512029:

// run the pre-physics state (steering transforms), the real vehicle physics step,

// and then the post-physics state (fly mode, nitro throttling, yaw assists).

static_assert(ADDR_VEHICLE_UPDATE_FN == 0x00519890, "hkVehiclePhysicsPostThunk literal out of sync");

__declspec(naked) static void hkVehiclePhysicsPostThunk()

{

    __asm

    {

        pushad

        call PreVehiclePhysics

        popad

        mov eax, 0x00519890 // sub_519890 takes no arguments; EAX is volatile

        call eax

        pushad

        call PostVehiclePhysics

        popad

        ret

    }

}

// ---------------------------------------------------------------------------------

// STUNT TIP MULTIPLIER HOOK

// ---------------------------------------------------------------------------------

extern "C" void __cdecl ApplyStuntTipReward(int baseTip)

{

    if (baseTip <= 0)

        return;

    int scaledTip = (int)(baseTip * g_Config.stuntMultiplier);

    if (scaledTip < 0) scaledTip = 0;

    int currentTips = *(int*)ADDR_CURRENT_FARE_TIPS;

    if (currentTips + scaledTip > 9999900)

    {

        *(int*)ADDR_CURRENT_FARE_TIPS = 9999900;

    }

    else

    {

        *(int*)ADDR_CURRENT_FARE_TIPS += scaledTip;

    }

}

__declspec(naked) static void hkStuntTipDetour()

{

    __asm

    {

        pushad

        push ecx               // ecx = base tip in cents

        call ApplyStuntTipReward

        add esp, 4

        popad

        mov edx, 0x004a8ded

        jmp edx

    }

}

// IAT hook for CreateFileA (Crazy Taxi.exe IAT at 0x00534090)

typedef HANDLE (WINAPI* pfnCreateFileA)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);

static pfnCreateFileA fpCreateFileA = nullptr;

static bool SafeContainsMusicAdx(LPCSTR path)

{

    if ((uintptr_t)path <= 0xFFFF)

        return false;

    __try

    {

        return (strstr(path, "SoundData\\music_adx\\") != nullptr || strstr(path, "SoundData/music_adx/") != nullptr);

    }

    __except (EXCEPTION_EXECUTE_HANDLER)

    {

        return false;

    }

}

static HANDLE WINAPI hkCreateFileA(LPCSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,

                                   LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition,

                                   DWORD dwFlagsAndAttributes, HANDLE hTemplateFile)

{

    if (g_Config.useOriginalSoundtrack && SafeContainsMusicAdx(lpFileName))

    {

        char redirectPath[MAX_PATH];

        strncpy_s(redirectPath, lpFileName, sizeof(redirectPath) - 1);

        char* pos = strstr(redirectPath, "music_adx");

        if (pos && strstr(redirectPath, "music_adx_original") == nullptr)

        {

            char finalPath[MAX_PATH];

            size_t prefixLen = pos - redirectPath;

            strncpy_s(finalPath, redirectPath, prefixLen);

            finalPath[prefixLen] = '\0';

            strcat_s(finalPath, "music_adx_original");

            strcat_s(finalPath, pos + 9);

            DWORD attr = GetFileAttributesA(finalPath);

            if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY))

            {

                return fpCreateFileA ? fpCreateFileA(finalPath, dwDesiredAccess, dwShareMode,

                                                     lpSecurityAttributes, dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile)

                                     : CreateFileA(finalPath, dwDesiredAccess, dwShareMode,

                                                   lpSecurityAttributes, dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);

            }

        }

    }

    return fpCreateFileA ? fpCreateFileA(lpFileName, dwDesiredAccess, dwShareMode, lpSecurityAttributes, dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile)

                         : CreateFileA(lpFileName, dwDesiredAccess, dwShareMode, lpSecurityAttributes, dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);

}

void ApplySoundtrackSetting(bool original)

{

    // With hkCreateFileA active, soundtrack switching is instantaneous, lock-free, and handles

    // all file requests in memory without ever modifying files on disk or corrupting CRI ADX audio!

    g_Config.useOriginalSoundtrack = original;

}

static bool __cdecl hkSteamAPI_RestartAppIfNecessary(uint32_t unOwnAppID)

{

    return false; // Prevent game from exiting or restarting through Steam

}

bool InitGameFeatures()

{

    DWORD oldProtect;

    // 0. Install CreateFileA Hook for lock-free soundtrack redirection via Crazy Taxi IAT (0x00534090)

    if (VirtualProtect((LPVOID)0x00534090, sizeof(void*), PAGE_READWRITE, &oldProtect))

    {

        fpCreateFileA = *(pfnCreateFileA*)0x00534090;

        *(void**)0x00534090 = (void*)&hkCreateFileA;

        VirtualProtect((LPVOID)0x00534090, sizeof(void*), oldProtect, &oldProtect);

        Logger::Info("Audio", "Crazy Taxi IAT 0x00534090 hooked for lock-free soundtrack redirection");

    }

// 1. SteamAPI stub at 0x0053432C

    if (VirtualProtect((LPVOID)0x0053432C, sizeof(void*), PAGE_READWRITE, &oldProtect))

    {

        *(void**)0x0053432C = (void*)&hkSteamAPI_RestartAppIfNecessary;

        VirtualProtect((LPVOID)0x0053432C, sizeof(void*), oldProtect, &oldProtect);

    }

    // 2. Camera Update Call Hook at 0x0051f582

    if (VirtualProtect((LPVOID)ADDR_CAM_UPDATE_CALL, 5, PAGE_EXECUTE_READWRITE, &oldProtect))

    {

        memcpy(s_OrigCamCallBytes, (void*)ADDR_CAM_UPDATE_CALL, 5);

        uintptr_t hookTarget = (uintptr_t)&hkCameraUpdateThunk;

        uintptr_t relOffset = hookTarget - (ADDR_CAM_UPDATE_CALL + 5);

        BYTE patch[5];

        patch[0] = 0xE8; // CALL rel32

        memcpy(&patch[1], &relOffset, 4);

        memcpy((void*)ADDR_CAM_UPDATE_CALL, patch, 5);

        VirtualProtect((LPVOID)ADDR_CAM_UPDATE_CALL, 5, oldProtect, &oldProtect);

        s_CamHookInstalled = true;

    }

    // 3. Stunt Tip Hook at 0x004A8DE7

    if (VirtualProtect((LPVOID)ADDR_STUNT_TIP_ADD, 6, PAGE_EXECUTE_READWRITE, &oldProtect))

    {

        memcpy(s_OrigTipAddBytes, (void*)ADDR_STUNT_TIP_ADD, 6);

        uintptr_t hookTarget = (uintptr_t)&hkStuntTipDetour;

        uintptr_t relOffset = hookTarget - (ADDR_STUNT_TIP_ADD + 5);

        BYTE patch[6];

        patch[0] = 0xE9; // JMP rel32

        memcpy(&patch[1], &relOffset, 4);

        patch[5] = 0x90; // NOP

        memcpy((void*)ADDR_STUNT_TIP_ADD, patch, 6);

        VirtualProtect((LPVOID)ADDR_STUNT_TIP_ADD, 6, oldProtect, &oldProtect);

        s_TipHookInstalled = true;

    }

    // 4. Input Processing Call Hook at 0x004EA3A8 (call 0x004F2ED0 in main per-frame loop)

    if (VirtualProtect((LPVOID)ADDR_MAIN_INPUT_LOOP, 5, PAGE_EXECUTE_READWRITE, &oldProtect))

    {

        memcpy(s_OrigInputCallBytes, (void*)ADDR_MAIN_INPUT_LOOP, 5);

        uintptr_t hookTarget = (uintptr_t)&hkMainInputLoop;

        uintptr_t relOffset = hookTarget - (ADDR_MAIN_INPUT_LOOP + 5);

        BYTE patch[5];

        patch[0] = 0xE8; // CALL rel32

        memcpy(&patch[1], &relOffset, 4);

        memcpy((void*)ADDR_MAIN_INPUT_LOOP, patch, 5);

        VirtualProtect((LPVOID)ADDR_MAIN_INPUT_LOOP, 5, oldProtect, &oldProtect);

        s_InputHookInstalled = true;

    }

    // 5. Vehicle Physics Step Call Site 1 at 0x005113B7 (call 0x00519890 - real vehicle physics)

    if (VirtualProtect((LPVOID)ADDR_VEHICLE_UPDATE_CALL_1, 5, PAGE_EXECUTE_READWRITE, &oldProtect))

    {

        memcpy(s_OrigVehUpdate1Bytes, (void*)ADDR_VEHICLE_UPDATE_CALL_1, 5);

        uintptr_t hookTarget = (uintptr_t)&hkVehiclePhysicsPostThunk;

        uintptr_t relOffset = hookTarget - (ADDR_VEHICLE_UPDATE_CALL_1 + 5);

        BYTE patch[5];

        patch[0] = 0xE8; // CALL rel32

        memcpy(&patch[1], &relOffset, 4);

        memcpy((void*)ADDR_VEHICLE_UPDATE_CALL_1, patch, 5);

        VirtualProtect((LPVOID)ADDR_VEHICLE_UPDATE_CALL_1, 5, oldProtect, &oldProtect);

        s_VehUpdate1HookInstalled = true;

    }

    // 6. Vehicle Physics Step Call Site 2 at 0x00512029 (call 0x00519890 - real vehicle physics)

    if (VirtualProtect((LPVOID)ADDR_VEHICLE_UPDATE_CALL_2, 5, PAGE_EXECUTE_READWRITE, &oldProtect))

    {

        memcpy(s_OrigVehUpdate2Bytes, (void*)ADDR_VEHICLE_UPDATE_CALL_2, 5);

        uintptr_t hookTarget = (uintptr_t)&hkVehiclePhysicsPostThunk;

        uintptr_t relOffset = hookTarget - (ADDR_VEHICLE_UPDATE_CALL_2 + 5);

        BYTE patch[5];

        patch[0] = 0xE8; // CALL rel32

        memcpy(&patch[1], &relOffset, 4);

        memcpy((void*)ADDR_VEHICLE_UPDATE_CALL_2, patch, 5);

        VirtualProtect((LPVOID)ADDR_VEHICLE_UPDATE_CALL_2, 5, oldProtect, &oldProtect);

        s_VehUpdate2HookInstalled = true;

    }

    // 6b. Speed scalar multiply window inside sub_519890 at 0x00519E90 (verified 18-byte

    // window; see hkSpeedScalarCave). Impulse-safe: the multiplier is applied to the engine

    // speed scalar once per frame instead of injecting position displacement.

    // Note: ADDR_SPEED_MULT_PATCH (0x00519E90) is inside collision restitution block
    // (0x519B9F - 0x519EFC) and bypassed during normal driving. Continuous speed multiplier
    // and Nitro are now cleanly applied directly to displacement in PostVehiclePhysics().
    s_SpeedMultPatchInstalled = false;

    // 8. Safe Guard for Uninitialized Direct3D Device Pointer at 0x00481100 (Fixes 0xC0000005 crash at 0x00481103)

    if (VirtualProtect((LPVOID)0x00481100, 5, PAGE_EXECUTE_READWRITE, &oldProtect))

    {

        memcpy(s_OrigClearD3DBytes, (void*)0x00481100, 5);

        uintptr_t hookTarget = (uintptr_t)&hkClearD3DStateNaked;

        uintptr_t relOffset = hookTarget - (0x00481100 + 5);

        BYTE patch[5];

        patch[0] = 0xE9; // JMP rel32

        memcpy(&patch[1], &relOffset, 4);

        memcpy((void*)0x00481100, patch, 5);

        VirtualProtect((LPVOID)0x00481100, 5, oldProtect, &oldProtect);

        s_ClearD3DHookInstalled = true;

    }

    // 9. Unprotect Master Camera Far-Z Clip Constant in .rdata (0x005a0150)

    if (VirtualProtect((LPVOID)ADDR_FAR_Z_RDATA, sizeof(float), PAGE_READWRITE, &s_OldFarZProtect))

    {

        s_FarZUnprotected = true;

    }

    // 10. Safe Guard for Uninitialized Direct3D Device Pointer at 0x0052227E (Fixes 0xC0000005 crash at 0x00522281)

    if (VirtualProtect((LPVOID)0x0052227E, 5, PAGE_EXECUTE_READWRITE, &oldProtect))

    {

        memcpy(s_OrigCreateDeclBytes, (void*)0x0052227E, 5);

        uintptr_t hookTarget = (uintptr_t)&hkCreateVertexDeclNaked;

        uintptr_t relOffset = hookTarget - (0x0052227E + 5);

        BYTE patch[5];

        patch[0] = 0xE9; // JMP rel32

        memcpy(&patch[1], &relOffset, 4);

        memcpy((void*)0x0052227E, patch, 5);

        VirtualProtect((LPVOID)0x0052227E, 5, oldProtect, &oldProtect);

        s_CreateDeclHookInstalled = true;

    }

    // 11. Safe Guard for Uninitialized Direct3D Device Pointer at 0x00521FD4 (Fixes 0xC0000005 crash at 0x00521FD7)

    if (VirtualProtect((LPVOID)0x00521FD4, 5, PAGE_EXECUTE_READWRITE, &oldProtect))

    {

        memcpy(s_OrigCreateVSBytes, (void*)0x00521FD4, 5);

        uintptr_t hookTarget = (uintptr_t)&hkCreateVertexShaderNaked;

        uintptr_t relOffset = hookTarget - (0x00521FD4 + 5);

        BYTE patch[5];

        patch[0] = 0xE9; // JMP rel32

        memcpy(&patch[1], &relOffset, 4);

        memcpy((void*)0x00521FD4, patch, 5);

        VirtualProtect((LPVOID)0x00521FD4, 5, oldProtect, &oldProtect);

        s_CreateVSHookInstalled = true;

    }

    // 12. Safe Guard for Uninitialized Direct3D Device Pointer at 0x00521F74 (Fixes 0xC0000005 crash at 0x00521F77)

    if (VirtualProtect((LPVOID)0x00521F74, 5, PAGE_EXECUTE_READWRITE, &oldProtect))

    {

        memcpy(s_OrigCreatePSBytes, (void*)0x00521F74, 5);

        uintptr_t hookTarget = (uintptr_t)&hkCreatePixelShaderNaked;

        uintptr_t relOffset = hookTarget - (0x00521F74 + 5);

        BYTE patch[5];

        patch[0] = 0xE9; // JMP rel32

        memcpy(&patch[1], &relOffset, 4);

        memcpy((void*)0x00521F74, patch, 5);

        VirtualProtect((LPVOID)0x00521F74, 5, oldProtect, &oldProtect);

        s_CreatePSHookInstalled = true;

    }

    // 13. Safe Guard for Uninitialized Direct3D Device Pointer at 0x00521D60 (CreateVertexBuffer)

    if (VirtualProtect((LPVOID)0x00521D60, 8, PAGE_EXECUTE_READWRITE, &oldProtect))

    {

        memcpy(s_OrigCreateVBBytes, (void*)0x00521D60, 8);

        uintptr_t relOffset = ((uintptr_t)&hkCreateVertexBufferNaked) - (0x00521D60 + 5);

        BYTE patch[8] = { 0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90 };

        memcpy(&patch[1], &relOffset, 4);

        memcpy((void*)0x00521D60, patch, 8);

        VirtualProtect((LPVOID)0x00521D60, 8, oldProtect, &oldProtect);

        s_CreateVBHookInstalled = true;

    }

    // 13b. Safe Guard for Uninitialized Direct3D Device Pointer at 0x00521E90 (CreateIndexBuffer - Fixes 0xC0000005 crash at 0x00521EBA)

    if (VirtualProtect((LPVOID)0x00521E90, 7, PAGE_EXECUTE_READWRITE, &oldProtect))

    {

        memcpy(s_OrigCreateIBBytes, (void*)0x00521E90, 7);

        uintptr_t relOffset = ((uintptr_t)&hkCreateIndexBufferNaked) - (0x00521E90 + 5);

        BYTE patch[7] = { 0xE9, 0, 0, 0, 0, 0x90, 0x90 };

        memcpy(&patch[1], &relOffset, 4);

        memcpy((void*)0x00521E90, patch, 7);

        VirtualProtect((LPVOID)0x00521E90, 7, oldProtect, &oldProtect);

        s_CreateIBHookInstalled = true;

    }

    // 14. Safe Guard for Uninitialized Direct3D Device Pointer at 0x004801B0 (Fixes 0xC0000005 crash at 0x004801C0)

    if (VirtualProtect((LPVOID)0x004801B0, 6, PAGE_EXECUTE_READWRITE, &oldProtect))

    {

        memcpy(s_OrigSetShaderMatrixBytes, (void*)0x004801B0, 6);

        uintptr_t relOffset = ((uintptr_t)&hkSetShaderConstMatrixNaked) - (0x004801B0 + 5);

        BYTE patch[6] = { 0xE9, 0, 0, 0, 0, 0x90 };

        memcpy(&patch[1], &relOffset, 4);

        memcpy((void*)0x004801B0, patch, 6);

        VirtualProtect((LPVOID)0x004801B0, 6, oldProtect, &oldProtect);

        s_SetShaderMatrixHookInstalled = true;

    }

    // 15. Safe Guard for Uninitialized Direct3D Device Pointer at 0x00480200

    if (VirtualProtect((LPVOID)0x00480200, 6, PAGE_EXECUTE_READWRITE, &oldProtect))

    {

        memcpy(s_OrigSetShaderVectorBytes, (void*)0x00480200, 6);

        uintptr_t relOffset = ((uintptr_t)&hkSetShaderConstVectorNaked) - (0x00480200 + 5);

        BYTE patch[6] = { 0xE9, 0, 0, 0, 0, 0x90 };

        memcpy(&patch[1], &relOffset, 4);

        memcpy((void*)0x00480200, patch, 6);

        VirtualProtect((LPVOID)0x00480200, 6, oldProtect, &oldProtect);

        s_SetShaderVectorHookInstalled = true;

    }

    // 16. Safe Guard for Uninitialized Direct3D Device Pointer at 0x00480250

    if (VirtualProtect((LPVOID)0x00480250, 6, PAGE_EXECUTE_READWRITE, &oldProtect))

    {

        memcpy(s_OrigSetShaderArrayFBytes, (void*)0x00480250, 6);

        uintptr_t relOffset = ((uintptr_t)&hkSetShaderConstArrayFNaked) - (0x00480250 + 5);

        BYTE patch[6] = { 0xE9, 0, 0, 0, 0, 0x90 };

        memcpy(&patch[1], &relOffset, 4);

        memcpy((void*)0x00480250, patch, 6);

        VirtualProtect((LPVOID)0x00480250, 6, oldProtect, &oldProtect);

        s_SetShaderArrayFHookInstalled = true;

    }

    // 17. Safe Guard for Uninitialized Direct3D Device Pointer at 0x004802C0

    if (VirtualProtect((LPVOID)0x004802C0, 6, PAGE_EXECUTE_READWRITE, &oldProtect))

    {

        memcpy(s_OrigSetShaderArrayIBytes, (void*)0x004802C0, 6);

        uintptr_t relOffset = ((uintptr_t)&hkSetShaderConstArrayINaked) - (0x004802C0 + 5);

        BYTE patch[6] = { 0xE9, 0, 0, 0, 0, 0x90 };

        memcpy(&patch[1], &relOffset, 4);

        memcpy((void*)0x004802C0, patch, 6);

        VirtualProtect((LPVOID)0x004802C0, 6, oldProtect, &oldProtect);

        s_SetShaderArrayIHookInstalled = true;

    }

    // 18. Safe Guard for Uninitialized Direct3D Device Pointer at 0x00480330

    if (VirtualProtect((LPVOID)0x00480330, 6, PAGE_EXECUTE_READWRITE, &oldProtect))

    {

        memcpy(s_OrigSetShaderArrayBBytes, (void*)0x00480330, 6);

        uintptr_t relOffset = ((uintptr_t)&hkSetShaderConstArrayBNaked) - (0x00480330 + 5);

        BYTE patch[6] = { 0xE9, 0, 0, 0, 0, 0x90 };

        memcpy(&patch[1], &relOffset, 4);

        memcpy((void*)0x00480330, patch, 6);

        VirtualProtect((LPVOID)0x00480330, 6, oldProtect, &oldProtect);

        s_SetShaderArrayBHookInstalled = true;

    }

    // 19. Safe Guard for Uninitialized Direct3D Device Pointer at 0x00480440

    if (VirtualProtect((LPVOID)0x00480440, 5, PAGE_EXECUTE_READWRITE, &oldProtect))

    {

        memcpy(s_OrigResetPSBytes, (void*)0x00480440, 5);

        uintptr_t relOffset = ((uintptr_t)&hkResetPixelShaderNaked) - (0x00480440 + 5);

        BYTE patch[5] = { 0xE9, 0, 0, 0, 0 };

        memcpy(&patch[1], &relOffset, 4);

        memcpy((void*)0x00480440, patch, 5);

        VirtualProtect((LPVOID)0x00480440, 5, oldProtect, &oldProtect);

        s_ResetPSHookInstalled = true;

    }

    // 20. Safe Guard for Uninitialized Direct3D Device Pointer at 0x00480480

    if (VirtualProtect((LPVOID)0x00480480, 5, PAGE_EXECUTE_READWRITE, &oldProtect))

    {

        memcpy(s_OrigResetVSBytes, (void*)0x00480480, 5);

        uintptr_t relOffset = ((uintptr_t)&hkResetVertexShaderNaked) - (0x00480480 + 5);

        BYTE patch[5] = { 0xE9, 0, 0, 0, 0 };

        memcpy(&patch[1], &relOffset, 4);

        memcpy((void*)0x00480480, patch, 5);

        VirtualProtect((LPVOID)0x00480480, 5, oldProtect, &oldProtect);

        s_ResetVSHookInstalled = true;

    }

    // 21. Safe Guard for NULL Destination Buffer in Mesh Copy at 0x0049F967 (Fixes 0xC0000005 crash at 0x0049F976)

    if (VirtualProtect((LPVOID)0x0049F967, 17, PAGE_EXECUTE_READWRITE, &oldProtect))

    {

        memcpy(s_OrigMeshBufferCopyBytes, (void*)0x0049F967, 17);

        uintptr_t relOffset = ((uintptr_t)&hkMeshBufferCopyNaked) - (0x0049F967 + 5);

        BYTE patch[17];

        patch[0] = 0xE9; // JMP rel32

        memcpy(&patch[1], &relOffset, 4);

        memset(&patch[5], 0x90, 12); // NOP remainder of instructions

        memcpy((void*)0x0049F967, patch, 17);

        VirtualProtect((LPVOID)0x0049F967, 17, oldProtect, &oldProtect);

        s_MeshBufferCopyHookInstalled = true;

    }

    // 22. Safe Guard for SegaRenderer::CreateTexture at 0x00481BB0 (Fixes 0xC0000005 crash at 0x00481BDF)

    if (VirtualProtect((LPVOID)0x00481BB0, 6, PAGE_EXECUTE_READWRITE, &oldProtect))

    {

        memcpy(s_OrigSegaCreateTexBytes, (void*)0x00481BB0, 6);

        uintptr_t relOffset = ((uintptr_t)&hkSegaCreateTextureNaked) - (0x00481BB0 + 5);

        BYTE patch[6] = { 0xE9, 0, 0, 0, 0, 0x90 };

        memcpy(&patch[1], &relOffset, 4);

        memcpy((void*)0x00481BB0, patch, 6);

        VirtualProtect((LPVOID)0x00481BB0, 6, oldProtect, &oldProtect);

        s_SegaCreateTexHookInstalled = true;

    }

    // 23. Safe Guard for SegaRenderer::GetAvailableTextureMem at 0x00481C20

    if (VirtualProtect((LPVOID)0x00481C20, 6, PAGE_EXECUTE_READWRITE, &oldProtect))

    {

        memcpy(s_OrigSegaGetAvailTexMemBytes, (void*)0x00481C20, 6);

        uintptr_t relOffset = ((uintptr_t)&hkSegaGetAvailTexMemNaked) - (0x00481C20 + 5);

        BYTE patch[6] = { 0xE9, 0, 0, 0, 0, 0x90 };

        memcpy(&patch[1], &relOffset, 4);

        memcpy((void*)0x00481C20, patch, 6);

        VirtualProtect((LPVOID)0x00481C20, 6, oldProtect, &oldProtect);

        s_SegaGetAvailTexMemHookInstalled = true;

    }

    // 24. SegaRenderer Initialization Hook at 0x004826DD (Ensures m_pDevice is NEVER NULL upon construction)

    if (VirtualProtect((LPVOID)0x004826DD, 6, PAGE_EXECUTE_READWRITE, &oldProtect))

    {

        memcpy(s_OrigRendererInitBytes, (void*)0x004826DD, 6);

        uintptr_t relOffset = ((uintptr_t)&hkRendererInitNaked) - (0x004826DD + 5);

        BYTE patch[6] = { 0xE9, 0, 0, 0, 0, 0x90 };

        memcpy(&patch[1], &relOffset, 4);

        memcpy((void*)0x004826DD, patch, 6);

        VirtualProtect((LPVOID)0x004826DD, 6, oldProtect, &oldProtect);

        s_RendererInitHookInstalled = true;

    }

    // 25. Safe Guard for SegaRenderer::SetViewport at 0x004816D0 (Fixes 0xC0000005 crash at 0x0048176E)

    if (VirtualProtect((LPVOID)0x004816D0, 6, PAGE_EXECUTE_READWRITE, &oldProtect))

    {

        memcpy(s_OrigSetViewportBytes, (void*)0x004816D0, 6);

        uintptr_t relOffset = ((uintptr_t)&hkSegaSetViewportNaked) - (0x004816D0 + 5);

        BYTE patch[6] = { 0xE9, 0, 0, 0, 0, 0x90 };

        memcpy(&patch[1], &relOffset, 4);

        memcpy((void*)0x004816D0, patch, 6);

        VirtualProtect((LPVOID)0x004816D0, 6, oldProtect, &oldProtect);

        s_SetViewportHookInstalled = true;

    }

    // 26. Sega Keyboard Manager VTable Hook at 0x0059E8C0 (Method 7: PollKeyboard)

    if (VirtualProtect((LPVOID)0x0059E8C0, sizeof(void*), PAGE_EXECUTE_READWRITE, &s_OldKeyPollProtect))

    {

        *(void**)0x0059E8C0 = (void*)&hkKeyboardPoll;

        s_KeyPollHookInstalled = true;

    }

    // 27. Dreamcast Restoration 2.0 (CT-DC) Integration

    DCRestoration::Initialize();

    // 28. Unprotect Entire .rdata Chunk, Scenery & Sector Radii block (0x0059F000 - 0x005A2000, 12 KB)
    if (VirtualProtect((LPVOID)0x0059F000, 0x3000, PAGE_READWRITE, &s_OldChunkBlockProtect))
    {
        s_ChunkDistUnprotected = true;
        s_ExtendedChunkDistUnprotected = true;
        s_Mode1SectorRadUnprotected = true;
    }

    // 28b. Write the scaled view/activation radii ONCE at init. configure_navigation_arrow

    // copies 0x5A0788/0x5A078C into the per-session passenger spawn radius flt_16A673C,

    // so the values must be in place BEFORE the first session starts - not just from the

    // first in-game tick. UpdateChunkStreaming keeps them synced with the slider later.

    ApplyStreamingRadii(g_Config.worldDistanceMultiplier, g_Config.persistentChunkLoader);

    // 29. Unprotect Vehicle Speed Limit Constants in .rdata (0x0059fc78 = double 14.0, 0x005a06c0 = float 14.0)

    if (VirtualProtect((LPVOID)ADDR_VEHICLE_SPEED_LIMIT_DBL, sizeof(double), PAGE_READWRITE, &s_OldSpeedLimitProtect1) &&

        VirtualProtect((LPVOID)ADDR_VEHICLE_SPEED_LIMIT_FLT, sizeof(float),  PAGE_READWRITE, &s_OldSpeedLimitProtect2))

    {

        s_SpeedLimitUnprotected = true;

    }

    // 30. Unprotect Traffic and Pedestrian Spawn Distance Constants in .rdata (0x005a04ec, 0x005a04e8)

    if (VirtualProtect((LPVOID)0x005A04EC, sizeof(float), PAGE_READWRITE, &s_OldSpawnDistProtect1) &&

        VirtualProtect((LPVOID)0x005A04E8, sizeof(float), PAGE_READWRITE, &s_OldSpawnDistProtect2))

    {

        s_SpawnDistUnprotected = true;

    }

    // 31. Initialize Soundtrack Setting

    ApplySoundtrackSetting(g_Config.useOriginalSoundtrack);

    // 32. Fly-Mode collision-free hook on the inner sector-grid raycast (0x0050CBD0).

    // A naked hook is mandatory: the function is __usercall with its first argument in EAX,

    // so the passthrough must be a register-preserving jmp to the MinHook trampoline.

    if (MH_CreateHook((LPVOID)ADDR_RAYCAST_SECTOR_GRID, (LPVOID)&hkRaycastSectorGrid, (LPVOID*)&oRaycastSectorGrid) == MH_OK)

    {

        MH_EnableHook((LPVOID)ADDR_RAYCAST_SECTOR_GRID);

        s_RaycastSectorGridHookInstalled = true;

    }

    // 33. Install Chunk Eviction Detour Hook (0x005051D0 - Persistent Whole-Map Chunk Loader)

    if (MH_CreateHook((LPVOID)ADDR_CHUNK_EVICT_FN, (LPVOID)&hkChunkEvict, (LPVOID*)&oChunkEvict) == MH_OK)

    {

        MH_EnableHook((LPVOID)ADDR_CHUNK_EVICT_FN);

        s_ChunkEvictHookInstalled = true;

    }

    // 33a. Vehicle<->vehicle collision impulse hook on solve_rigid_body_impulse (0x00510400).

    // All cars (Player Taxi = 0, Traffic Cars = 1..14) use this callback.

    if (MH_CreateHook((LPVOID)ADDR_SOLVE_RIGID_BODY, (LPVOID)&hkSolveRigidBodyImpulse, (LPVOID*)&oSolveRigidBody) == MH_OK)

    {

        MH_EnableHook((LPVOID)ADDR_SOLVE_RIGID_BODY);

        s_SolveRigidBodyHookInstalled = true;

        Logger::Info("Features", "CarCar: solve_rigid_body_impulse hook installed at 0x00510400");

    }

    else

    {

        Logger::Warn("Features", "CarCar: solve_rigid_body hook install failed - pair-table fallback will be used");

    }

    // 33b. Actor<->prop/pedestrian collision hook on solve_actor_pair_collision_impulse (0x0050EF20).

    if (MH_CreateHook((LPVOID)ADDR_SOLVE_ACTOR_PAIR, (LPVOID)&hkSolveActorPairCollision, (LPVOID*)&oSolveActorPair) == MH_OK)

    {

        MH_EnableHook((LPVOID)ADDR_SOLVE_ACTOR_PAIR);

        s_SolvePairHookInstalled = true;

        Logger::Info("Features", "CarActor: actor pair solver hook installed at 0x0050EF20");

    }

    else

    {

        Logger::Warn("Features", "CarActor: actor pair solver hook install failed");

    }

    // 34. Install Passenger Pickup Check Hook at 0x004B862D (Fly Mode zero-collection)

    if (VirtualProtect((LPVOID)ADDR_PASSENGER_PICKUP_CALL, 5, PAGE_EXECUTE_READWRITE, &oldProtect))

    {

        memcpy(s_OrigPassengerPickupBytes, (void*)ADDR_PASSENGER_PICKUP_CALL, 5);

        uintptr_t hookTarget = (uintptr_t)&hkPassengerPickupCheckNaked;

        uintptr_t relOffset = hookTarget - (ADDR_PASSENGER_PICKUP_CALL + 5);

        BYTE patch[5];

        patch[0] = 0xE8; // CALL rel32

        memcpy(&patch[1], &relOffset, 4);

        memcpy((void*)ADDR_PASSENGER_PICKUP_CALL, patch, 5);

        VirtualProtect((LPVOID)ADDR_PASSENGER_PICKUP_CALL, 5, oldProtect, &oldProtect);

        s_PassengerPickupHookInstalled = true;

    }

    return true;

}

void ShutdownGameFeatures()

{

    DWORD oldProtect;

    g_Config.persistentChunkLoader = false;

    if (fpCreateFileA)

    {

        if (VirtualProtect((LPVOID)0x00534090, sizeof(void*), PAGE_READWRITE, &oldProtect))

        {

            *(void**)0x00534090 = (void*)fpCreateFileA;

            VirtualProtect((LPVOID)0x00534090, sizeof(void*), oldProtect, &oldProtect);

        }

        fpCreateFileA = nullptr;

    }

    if (s_KeyPollHookInstalled)

    {

        *(void**)0x0059E8C0 = (void*)s_oKeyboardPoll;

        VirtualProtect((LPVOID)0x0059E8C0, sizeof(void*), s_OldKeyPollProtect, &s_OldKeyPollProtect);

        s_KeyPollHookInstalled = false;

    }

    if (s_RendererInitHookInstalled)

    {

        if (VirtualProtect((LPVOID)0x004826DD, 6, PAGE_EXECUTE_READWRITE, &oldProtect))

        {

            memcpy((void*)0x004826DD, s_OrigRendererInitBytes, 6);

            VirtualProtect((LPVOID)0x004826DD, 6, oldProtect, &oldProtect);

        }

        s_RendererInitHookInstalled = false;

    }

    if (s_SetViewportHookInstalled)

    {

        if (VirtualProtect((LPVOID)0x004816D0, 6, PAGE_EXECUTE_READWRITE, &oldProtect))

        {

            memcpy((void*)0x004816D0, s_OrigSetViewportBytes, 6);

            VirtualProtect((LPVOID)0x004816D0, 6, oldProtect, &oldProtect);

        }

        s_SetViewportHookInstalled = false;

    }

    if (s_SegaCreateTexHookInstalled)

    {

        if (VirtualProtect((LPVOID)0x00481BB0, 6, PAGE_EXECUTE_READWRITE, &oldProtect))

        {

            memcpy((void*)0x00481BB0, s_OrigSegaCreateTexBytes, 6);

            VirtualProtect((LPVOID)0x00481BB0, 6, oldProtect, &oldProtect);

        }

        s_SegaCreateTexHookInstalled = false;

    }

    if (s_SegaGetAvailTexMemHookInstalled)

    {

        if (VirtualProtect((LPVOID)0x00481C20, 6, PAGE_EXECUTE_READWRITE, &oldProtect))

        {

            memcpy((void*)0x00481C20, s_OrigSegaGetAvailTexMemBytes, 6);

            VirtualProtect((LPVOID)0x00481C20, 6, oldProtect, &oldProtect);

        }

        s_SegaGetAvailTexMemHookInstalled = false;

    }

    if (s_MeshBufferCopyHookInstalled)

    {

        if (VirtualProtect((LPVOID)0x0049F967, 17, PAGE_EXECUTE_READWRITE, &oldProtect))

        {

            memcpy((void*)0x0049F967, s_OrigMeshBufferCopyBytes, 17);

            VirtualProtect((LPVOID)0x0049F967, 17, oldProtect, &oldProtect);

        }

        s_MeshBufferCopyHookInstalled = false;

    }

    if (s_ResetVSHookInstalled)

    {

        if (VirtualProtect((LPVOID)0x00480480, 5, PAGE_EXECUTE_READWRITE, &oldProtect))

        {

            memcpy((void*)0x00480480, s_OrigResetVSBytes, 5);

            VirtualProtect((LPVOID)0x00480480, 5, oldProtect, &oldProtect);

        }

        s_ResetVSHookInstalled = false;

    }

    if (s_ResetPSHookInstalled)

    {

        if (VirtualProtect((LPVOID)0x00480440, 5, PAGE_EXECUTE_READWRITE, &oldProtect))

        {

            memcpy((void*)0x00480440, s_OrigResetPSBytes, 5);

            VirtualProtect((LPVOID)0x00480440, 5, oldProtect, &oldProtect);

        }

        s_ResetPSHookInstalled = false;

    }

    if (s_SetShaderArrayBHookInstalled)

    {

        if (VirtualProtect((LPVOID)0x00480330, 6, PAGE_EXECUTE_READWRITE, &oldProtect))

        {

            memcpy((void*)0x00480330, s_OrigSetShaderArrayBBytes, 6);

            VirtualProtect((LPVOID)0x00480330, 6, oldProtect, &oldProtect);

        }

        s_SetShaderArrayBHookInstalled = false;

    }

    if (s_SetShaderArrayIHookInstalled)

    {

        if (VirtualProtect((LPVOID)0x004802C0, 6, PAGE_EXECUTE_READWRITE, &oldProtect))

        {

            memcpy((void*)0x004802C0, s_OrigSetShaderArrayIBytes, 6);

            VirtualProtect((LPVOID)0x004802C0, 6, oldProtect, &oldProtect);

        }

        s_SetShaderArrayIHookInstalled = false;

    }

    if (s_SetShaderArrayFHookInstalled)

    {

        if (VirtualProtect((LPVOID)0x00480250, 6, PAGE_EXECUTE_READWRITE, &oldProtect))

        {

            memcpy((void*)0x00480250, s_OrigSetShaderArrayFBytes, 6);

            VirtualProtect((LPVOID)0x00480250, 6, oldProtect, &oldProtect);

        }

        s_SetShaderArrayFHookInstalled = false;

    }

    if (s_SetShaderVectorHookInstalled)

    {

        if (VirtualProtect((LPVOID)0x00480200, 6, PAGE_EXECUTE_READWRITE, &oldProtect))

        {

            memcpy((void*)0x00480200, s_OrigSetShaderVectorBytes, 6);

            VirtualProtect((LPVOID)0x00480200, 6, oldProtect, &oldProtect);

        }

        s_SetShaderVectorHookInstalled = false;

    }

    if (s_SetShaderMatrixHookInstalled)

    {

        if (VirtualProtect((LPVOID)0x004801B0, 6, PAGE_EXECUTE_READWRITE, &oldProtect))

        {

            memcpy((void*)0x004801B0, s_OrigSetShaderMatrixBytes, 6);

            VirtualProtect((LPVOID)0x004801B0, 6, oldProtect, &oldProtect);

        }

        s_SetShaderMatrixHookInstalled = false;

    }

    if (s_CreateIBHookInstalled)

    {

        if (VirtualProtect((LPVOID)0x00521E90, 7, PAGE_EXECUTE_READWRITE, &oldProtect))

        {

            memcpy((void*)0x00521E90, s_OrigCreateIBBytes, 7);

            VirtualProtect((LPVOID)0x00521E90, 7, oldProtect, &oldProtect);

        }

        s_CreateIBHookInstalled = false;

    }

    if (s_CreateVBHookInstalled)

    {

        if (VirtualProtect((LPVOID)0x00521D60, 8, PAGE_EXECUTE_READWRITE, &oldProtect))

        {

            memcpy((void*)0x00521D60, s_OrigCreateVBBytes, 8);

            VirtualProtect((LPVOID)0x00521D60, 8, oldProtect, &oldProtect);

        }

        s_CreateVBHookInstalled = false;

    }

    if (s_CreatePSHookInstalled)

    {

        if (VirtualProtect((LPVOID)0x00521F74, 5, PAGE_EXECUTE_READWRITE, &oldProtect))

        {

            memcpy((void*)0x00521F74, s_OrigCreatePSBytes, 5);

            VirtualProtect((LPVOID)0x00521F74, 5, oldProtect, &oldProtect);

        }

        s_CreatePSHookInstalled = false;

    }

    if (s_CreateVSHookInstalled)

    {

        if (VirtualProtect((LPVOID)0x00521FD4, 5, PAGE_EXECUTE_READWRITE, &oldProtect))

        {

            memcpy((void*)0x00521FD4, s_OrigCreateVSBytes, 5);

            VirtualProtect((LPVOID)0x00521FD4, 5, oldProtect, &oldProtect);

        }

        s_CreateVSHookInstalled = false;

    }

    if (s_CreateDeclHookInstalled)

    {

        if (VirtualProtect((LPVOID)0x0052227E, 5, PAGE_EXECUTE_READWRITE, &oldProtect))

        {

            memcpy((void*)0x0052227E, s_OrigCreateDeclBytes, 5);

            VirtualProtect((LPVOID)0x0052227E, 5, oldProtect, &oldProtect);

        }

        s_CreateDeclHookInstalled = false;

    }

    if (s_ClearD3DHookInstalled)

    {

        if (VirtualProtect((LPVOID)0x00481100, 5, PAGE_EXECUTE_READWRITE, &oldProtect))

        {

            memcpy((void*)0x00481100, s_OrigClearD3DBytes, 5);

            VirtualProtect((LPVOID)0x00481100, 5, oldProtect, &oldProtect);

        }

        s_ClearD3DHookInstalled = false;

    }

    if (s_CamHookInstalled)

    {

        if (VirtualProtect((LPVOID)ADDR_CAM_UPDATE_CALL, 5, PAGE_EXECUTE_READWRITE, &oldProtect))

        {

            memcpy((void*)ADDR_CAM_UPDATE_CALL, s_OrigCamCallBytes, 5);

            VirtualProtect((LPVOID)ADDR_CAM_UPDATE_CALL, 5, oldProtect, &oldProtect);

        }

        s_CamHookInstalled = false;

    }

    if (s_TipHookInstalled)

    {

        if (VirtualProtect((LPVOID)ADDR_STUNT_TIP_ADD, 6, PAGE_EXECUTE_READWRITE, &oldProtect))

        {

            memcpy((void*)ADDR_STUNT_TIP_ADD, s_OrigTipAddBytes, 6);

            VirtualProtect((LPVOID)ADDR_STUNT_TIP_ADD, 6, oldProtect, &oldProtect);

        }

        s_TipHookInstalled = false;

    }

    if (s_InputHookInstalled)

    {

        if (VirtualProtect((LPVOID)ADDR_MAIN_INPUT_LOOP, 5, PAGE_EXECUTE_READWRITE, &oldProtect))

        {

            memcpy((void*)ADDR_MAIN_INPUT_LOOP, s_OrigInputCallBytes, 5);

            VirtualProtect((LPVOID)ADDR_MAIN_INPUT_LOOP, 5, oldProtect, &oldProtect);

        }

        s_InputHookInstalled = false;

    }

    if (s_VehUpdate1HookInstalled)

    {

        if (VirtualProtect((LPVOID)ADDR_VEHICLE_UPDATE_CALL_1, 5, PAGE_EXECUTE_READWRITE, &oldProtect))

        {

            memcpy((void*)ADDR_VEHICLE_UPDATE_CALL_1, s_OrigVehUpdate1Bytes, 5);

            VirtualProtect((LPVOID)ADDR_VEHICLE_UPDATE_CALL_1, 5, oldProtect, &oldProtect);

        }

        s_VehUpdate1HookInstalled = false;

    }

    if (s_VehUpdate2HookInstalled)

    {

        if (VirtualProtect((LPVOID)ADDR_VEHICLE_UPDATE_CALL_2, 5, PAGE_EXECUTE_READWRITE, &oldProtect))

        {

            memcpy((void*)ADDR_VEHICLE_UPDATE_CALL_2, s_OrigVehUpdate2Bytes, 5);

            VirtualProtect((LPVOID)ADDR_VEHICLE_UPDATE_CALL_2, 5, oldProtect, &oldProtect);

        }

        s_VehUpdate2HookInstalled = false;

    }

    if (s_SpeedMultPatchInstalled)

    {

        if (VirtualProtect((LPVOID)ADDR_SPEED_MULT_PATCH, 5, PAGE_EXECUTE_READWRITE, &oldProtect))

        {

            memcpy((void*)ADDR_SPEED_MULT_PATCH, s_OrigSpeedMultBytes, 5);

            VirtualProtect((LPVOID)ADDR_SPEED_MULT_PATCH, 5, oldProtect, &oldProtect);

        }

        s_SpeedMultPatchInstalled = false;

    }

    s_SpeedMultFactor = 1.0f;

    if (s_FarZUnprotected)

    {

        *(float*)ADDR_FAR_Z_RDATA = 15000.0f;

        VirtualProtect((LPVOID)ADDR_FAR_Z_RDATA, sizeof(float), s_OldFarZProtect, &s_OldFarZProtect);

        s_FarZUnprotected = false;

    }

    if (s_PassengerPickupHookInstalled)

    {

        if (VirtualProtect((LPVOID)ADDR_PASSENGER_PICKUP_CALL, 5, PAGE_EXECUTE_READWRITE, &oldProtect))

        {

            memcpy((void*)ADDR_PASSENGER_PICKUP_CALL, s_OrigPassengerPickupBytes, 5);

            VirtualProtect((LPVOID)ADDR_PASSENGER_PICKUP_CALL, 5, oldProtect, &oldProtect);

        }

        s_PassengerPickupHookInstalled = false;

    }

    if (s_ChunkEvictHookInstalled)

    {

        MH_DisableHook((LPVOID)ADDR_CHUNK_EVICT_FN);

        s_ChunkEvictHookInstalled = false;

    }

    if (s_RaycastSectorGridHookInstalled)

    {

        MH_DisableHook((LPVOID)ADDR_RAYCAST_SECTOR_GRID);

        s_RaycastSectorGridHookInstalled = false;

    }

    // Disable collision solver hooks BEFORE the pair-table restore

    // below, so no restore/re-stub race can occur.

    if (s_SolveRigidBodyHookInstalled)

    {

        MH_DisableHook((LPVOID)ADDR_SOLVE_RIGID_BODY);

        s_SolveRigidBodyHookInstalled = false;

    }

    if (s_SolvePairHookInstalled)

    {

        MH_DisableHook((LPVOID)ADDR_SOLVE_ACTOR_PAIR);

        s_SolvePairHookInstalled = false;

    }

    // Restore vehicle<->vehicle pair callbacks if they are currently stubbed out

    // (only possible via the fallback path; the solver hook never touches the table)

    __try {

        DWORD* pairTable = (DWORD*)ADDR_PAIR_CALLBACK_TABLE;

        for (int i = 0; i < (int)ADDR_PAIR_TABLE_COUNT; ++i)

        {

            if (pairTable[i] == (DWORD)(uintptr_t)&CarCarCollisionStub)

                pairTable[i] = ADDR_CARCAR_PAIR_CALLBACK;

        }

    } __except (EXCEPTION_EXECUTE_HANDLER) {}

    __try
    {
        DWORD oldBlockProtect = 0;
        BOOL unprotected = VirtualProtect((LPVOID)0x0059F000, 0x3000, PAGE_READWRITE, &oldBlockProtect);

        if (s_ChunkDistUnprotected || s_ExtendedChunkDistUnprotected)
        {
            *(float*)ADDR_CHUNK_DIST_CAMERA = 850.0f;
            *(float*)ADDR_CHUNK_DIST_FAR    = 450.0f;
            *(float*)ADDR_CHUNK_DIST_SEC_CAM = 1700.0f;
            *(float*)ADDR_CHUNK_DIST_SEC_FAR = 900.0f;
            *(float*)ADDR_SECTOR_STREAM_RAD  = 1500.0f;
            *(float*)ADDR_VIEW_RAD_NORMAL     = 1100.0f;
            *(float*)ADDR_VIEW_RAD_INIT       = 1200.0f;
            *(float*)ADDR_PASSENGER_SPAWN_RAD = 700.0f;
            *(float*)ADDR_CHUNK_THRESH_MODE0_1 = 2500.0f;
            *(float*)ADDR_CHUNK_THRESH_MODE0_2 = 2800.0f;
            *(float*)ADDR_CHUNK_THRESH_MODE0_3 = 3000.0f;
            *(double*)ADDR_CHUNK_TTL_DIST_K   = 0.2;
            *(double*)ADDR_CHUNK_TTL_RENDER_K = 0.4;
            s_ChunkDistUnprotected = false;
            s_ExtendedChunkDistUnprotected = false;
        }

        // Global chunk render radius factor: game-writable .data, restore the vanilla 2.0
        *(float*)ADDR_CHUNK_RENDER_FACTOR = 2.0f;

        if (s_Mode1SectorRadUnprotected)
        {
            *(float*)ADDR_STREAM_SECTOR_RAD_1 = 1000.0f;
            *(float*)ADDR_STREAM_SECTOR_RAD_2 = 800.0f;
            *(float*)ADDR_STREAM_SECTOR_RAD_3 = 600.0f;
            *(float*)ADDR_STREAM_SECTOR_RAD_4 = 400.0f;
            s_Mode1SectorRadUnprotected = false;
        }

        if (s_SpeedLimitUnprotected)
        {
            *(float*)0x0059FAA8 = 8.0f;
            *(float*)0x005A07A0 = -8.0f;
            *(double*)ADDR_VEHICLE_SPEED_LIMIT_DBL = 14.0;
            *(float*)ADDR_VEHICLE_SPEED_LIMIT_FLT  = 14.0f;
            s_SpeedLimitUnprotected = false;
        }

        if (s_SpawnDistUnprotected)
        {
            *(float*)0x005A04EC = 1800.0f;
            *(float*)0x005A04E8 = 2000.0f;
            s_SpawnDistUnprotected = false;
        }

        if (unprotected)
        {
            DWORD dummy = 0;
            DWORD targetProtect = (s_OldChunkBlockProtect != 0) ? s_OldChunkBlockProtect : PAGE_READONLY;
            VirtualProtect((LPVOID)0x0059F000, 0x3000, targetProtect, &dummy);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}

}

// -------------------------------------------------------------

// FEATURE: PERSISTENT CHUNK LOADER + WORLD DISTANCE STREAMING

// -------------------------------------------------------------

// Scaling model (IDA fern-streaming report, all values engine-verified):

//  - The per-frame view/activation radius flt_16F59DC is reloaded in sub_4AA130 from

//    .rdata sources depending on regime: 0x5A0788 (1100, normal gameplay - the core

//    visibility gate), 0x5A10A0 (900, regime 2), 0x5A0390 (1200, session init +

//    regime-2 despawn), 0x5A07D8 (1700, non-gameplay + map-1 preload). It feeds the

//    sector visibility bitmask, clip planes (-(R+30)), ambient ped/traffic emitters,

//    actor cull/despawn and the passenger spawn ring, so scaling it scales all of

//    those automatically. The '+30' addends (immediates) are negligible.

//  - 0x5A078C (700) is the map-1 passenger spawn radius source (flt_16A673C); it must

//    be in place BEFORE a session starts, hence the init-time write below.

//  - Chunk GEOMETRY render cull: flt_1B7A748 (2.0) scales the mode-1 radius (2.0x2500),

//    the map-0 cull (2.0x3000) and the sub_4DBF90 thresholds (dist/S < T); the .rdata

//    thresholds 2500/2800/3000 scale alongside (compounds with the S factor, capped).

//  - Texture load priority: TTL = base - k*dist. Vanilla k makes every chunk beyond

//    ~450 distance priority 0, so far chunks starve behind near re-requests in the

//    one-chunk-per-frame texture pump; lowering k gives far chunks positive TTL.

//  - Fog: NO distance-derived fog writer exists in the engine (verified statically);

//    the visible "fog horizon" is the cull edge itself - nothing to patch.

//  - 0x59F9E0 (nav/mission distances) and the other do-not-patch values in

//    game_features.h are left untouched.

static void ApplyStreamingRadii(float mult, bool persistent)

{

    // Effective scale: the slider reaches 100x, but the ENGINE-VISIBLE change is capped

    // at ~6x (slider stays untouched). Per-value caps below bound the engine change.

    float m = mult;

    if (m < 1.0f) m = 1.0f;

    if (m > 6.0f) m = 6.0f; // effective world scale cap

    // Per-value caps: the chunk/sector slot table (byte_169AF00) has 1152 slots that are

    // ONLY recycled via TTL eviction (evict_and_bind_textures 0x005051D0, state 4 -> 0).

    // Uncapped whole-map radii would keep every slot's TTL refreshed forever and

    // find_best_fit (0x005036A0) would silently drop all new chunk requests. Pure chunk

    // LOAD radii stay at 4000 (slot budget); view/activation + despawn radii go to 8000

    // so despawn (>= spawn ring R+30) never undercuts the scaled spawn radius.

    constexpr float STREAM_RAD_CAP   = 4000.0f; // pure chunk load radii (slot budget)

    constexpr float VIEW_RAD_CAP     = 8000.0f; // view/activation + spawn/despawn radii

    constexpr float CHUNK_FACTOR_CAP = 6.0f;    // global chunk render factor (vanilla 2.0)

    bool persistentLoader = persistent;

    auto applyScale = [persistentLoader](uintptr_t addr, float vanilla, float cap, float m) {

        float value = persistentLoader ? cap : vanilla * m;

        if (value > cap) value = cap;

        *(float*)addr = value;

    };

    if (s_ChunkDistUnprotected)

    {

        // Pickup/snapshot + replay distances (no visibility impact, kept for compat)

        applyScale(ADDR_CHUNK_DIST_CAMERA, 850.0f, STREAM_RAD_CAP, m);

        applyScale(ADDR_CHUNK_DIST_FAR,    450.0f, STREAM_RAD_CAP, m);

    }

    if (s_ExtendedChunkDistUnprotected)

    {

        // A) In-game view/activation radii (all branches of sub_4AA130)

        // Keep ADDR_VIEW_RAD_NORMAL at vanilla 1100.0f so flt_16F59DC * 0.5 in sub_4A9A10

        // does not bloat the inner exclusion bubble and eliminate pedestrians/figures!

        applyScale(ADDR_VIEW_RAD_NORMAL,     1100.0f, 1150.0f, 1.0f); // Core fix: keeps pedestrians and figures spawning!

        applyScale(ADDR_CHUNK_DIST_SEC_FAR,   900.0f, VIEW_RAD_CAP, m); // regime 2 view radius

        // DESTINY ARROW FIX: Keep ADDR_VIEW_RAD_INIT (1200.0f) and ADDR_PASSENGER_SPAWN_RAD (700.0f)

        // strictly at their authentic vanilla values! sub_4FE320 and configure_navigation_arrow use these

        // as arrival distance thresholds. Scaling them caused the destiny arrow to trigger across the whole map!

        *(float*)ADDR_VIEW_RAD_INIT = 1200.0f;

        applyScale(ADDR_CHUNK_DIST_SEC_CAM,  1700.0f, VIEW_RAD_CAP, m); // non-gameplay + map-1 preload

        *(float*)ADDR_PASSENGER_SPAWN_RAD = 700.0f;

        applyScale(ADDR_SECTOR_STREAM_RAD,   1500.0f, STREAM_RAD_CAP, m);

        // B) Chunk geometry render cull

        applyScale(ADDR_CHUNK_THRESH_MODE0_1, 2500.0f, VIEW_RAD_CAP, m);

        applyScale(ADDR_CHUNK_THRESH_MODE0_2, 2800.0f, VIEW_RAD_CAP, m);

        applyScale(ADDR_CHUNK_THRESH_MODE0_3, 3000.0f, VIEW_RAD_CAP, m);

        // Game-writable .data (sub_489450 writes the vanilla 2.0) - direct write.

        float factor = 2.0f * m;

        if (factor > CHUNK_FACTOR_CAP) factor = CHUNK_FACTOR_CAP;

        *(float*)ADDR_CHUNK_RENDER_FACTOR = factor;

        // D) Texture load priority TTL coefficients (doubles). Farther chunks get

        // positive TTL and win the max-TTL selection of process_chunk_stream_queue.

        double kTTL = 0.2 / ((double)m * 2.5);

        if (kTTL < 0.015) kTTL = 0.015;

        if (persistentLoader) kTTL = 0.015;

        *(double*)ADDR_CHUNK_TTL_DIST_K   = kTTL;

        *(double*)ADDR_CHUNK_TTL_RENDER_K = kTTL * 2.0;

    }

    if (s_Mode1SectorRadUnprotected)

    {

        applyScale(ADDR_STREAM_SECTOR_RAD_1, 1000.0f, VIEW_RAD_CAP, m); // regime-2 actor update/despawn

        applyScale(ADDR_STREAM_SECTOR_RAD_2,  800.0f, STREAM_RAD_CAP, m);

        applyScale(ADDR_STREAM_SECTOR_RAD_3,  600.0f, STREAM_RAD_CAP, m);

        applyScale(ADDR_STREAM_SECTOR_RAD_4,  400.0f, STREAM_RAD_CAP, m);

    }

    if (s_SpawnDistUnprotected)

    {

        // C) Despawn/convoy & native chunk query radii.

        // ADDR_SPAWN_DIST_TRAFFIC (0x005A04EC) is flt_5A04EC, the radius passed by sub_519890

        // to load_chunks_around every frame! Scaling it to STREAM_RAD_CAP (4000.0f) ensures

        // chunks and their textures are queried every frame, preventing premature eviction in sub_5051D0.

        applyScale(ADDR_SPAWN_DIST_TRAFFIC, 1800.0f, STREAM_RAD_CAP, m);

        applyScale(ADDR_SPAWN_DIST_PED,     2000.0f, VIEW_RAD_CAP, m);

    }

}

static void UpdateChunkStreaming()

{

    // Sync legacy distance multipliers with unified world distance multiplier

    g_Config.renderDistanceMultiplier = g_Config.worldDistanceMultiplier;

    g_Config.chunkLoadDistanceMultiplier = g_Config.worldDistanceMultiplier;

    float mult = g_Config.worldDistanceMultiplier;

    if (mult < 1.0f) mult = 1.0f;

    if (mult > 100.0f) mult = 100.0f;

    ApplyStreamingRadii(mult, g_Config.persistentChunkLoader);

    // Eviction protection for the active player radius: re-request the chunks around the

    // taxi EVERY frame. The TTL window is exactly one evict pass (= one frame), so only a

    // per-frame request keeps near chunks resident; chunks outside this radius age out

    // normally and free their slots for new loads. Never throttled!

    if (g_Config.persistentChunkLoader)

        TriggerFullMapChunkStreaming();

}

void TriggerFullMapChunkStreaming()

{

    if (!IsInGame())

        return;

    // Native chunk & texture streamer (0x00504090, int __cdecl(float x, float y, float z, float rad);

    // the engine ignores y and only uses the XZ distance). Radius scales with the world

    // multiplier but stays capped at 4000 so the 1152-slot table keeps recycling via TTL

    // eviction while chunks in the player radius are kept resident.

    float px = 0.0f, py = 0.0f, pz = 0.0f;

    if (!GetTaxiPosition(px, py, pz))

        return;

    float mult = g_Config.worldDistanceMultiplier;

    if (mult < 1.0f) mult = 1.0f;

    if (mult > 6.0f) mult = 6.0f; // same effective scale cap as ApplyStreamingRadii

    float rad = 3500.0f * mult;

    if (rad > 4000.0f) rad = 4000.0f;

    typedef int (__cdecl *tLoadChunksAroundFn)(float x, float y, float z, float rad);

    tLoadChunksAroundFn pfnLoadChunks = (tLoadChunksAroundFn)ADDR_LOAD_CHUNKS_AROUND;

    if (pfnLoadChunks)

    {

        __try {

            pfnLoadChunks(px, py, pz, rad);

        } __except (EXCEPTION_EXECUTE_HANDLER) {}

    }

}

// Fallback for the player-filtered collision disable, ONLY active if the MinHook solver

// hook failed to install. Idempotent and self-healing: entries re-initialized by

// reset_and_init_actor_collision_subsystem are re-stubbed on the next frame;

// world/ground collision (separate subsystem) is never touched. With the hook active the

// table stays 100% vanilla. Logs the table state once for diagnosis (the previous silent

// failure mode made static misbehavior analysis impossible).

static void UpdateCarCarCollisionPatch()

{

    if (s_SolveRigidBodyHookInstalled)

        return; // robust MinHook on solve_rigid_body_impulse (0x00510400) is active - table stays vanilla

    bool disable = (g_Config.disableTrafficCollision || g_Config.flyMode);

    __try {

        DWORD* pairTable = (DWORD*)ADDR_PAIR_CALLBACK_TABLE;

        if (disable)

        {

            static bool s_CarCarDiagLogged = false;

            if (!s_CarCarDiagLogged)

            {

                s_CarCarDiagLogged = true;

                int stubbed = 0, vanilla = 0, foreign = 0;

                DWORD firstForeign = 0;

                for (int i = 0; i < (int)ADDR_PAIR_TABLE_COUNT; ++i)

                {

                    DWORD v = pairTable[i];

                    if (v == (DWORD)(uintptr_t)&CarCarCollisionStub) ++stubbed;

                    else if (v == ADDR_CARCAR_PAIR_CALLBACK) ++vanilla;

                    else { ++foreign; if (firstForeign == 0) firstForeign = v; }

                }

                Logger::Warn("Features", "CarCar: solver hook NOT installed, pair-table fallback active (stubbed=%d vanilla=%d foreign=%d first=0x%08X)",

                             stubbed, vanilla, foreign, firstForeign);

            }

            for (int i = 0; i < (int)ADDR_PAIR_TABLE_COUNT; ++i)

            {

                if (pairTable[i] == ADDR_CARCAR_PAIR_CALLBACK)

                    pairTable[i] = (DWORD)(uintptr_t)&CarCarCollisionStub;

            }

        }

        else

        {

            for (int i = 0; i < (int)ADDR_PAIR_TABLE_COUNT; ++i)

            {

                if (pairTable[i] == (DWORD)(uintptr_t)&CarCarCollisionStub)

                    pairTable[i] = ADDR_CARCAR_PAIR_CALLBACK;

            }

        }

    } __except (EXCEPTION_EXECUTE_HANDLER) {}

}

void UpdateGameFeaturesPerFrame()

{

    // Check and apply Dreamcast restoration dynamic assets (e.g. common.jpk FILA DDS)

    DCRestoration::Update();

    // Note: persistentChunkLoader is intentionally NOT auto-reset when IsInGame() briefly

    // drops - the heuristics flicker during gameplay and resetting causes a mass

    // evict/reload cycle (textures popping). It stays under user control and is only

    // cleared in ShutdownGameFeatures; the streaming values are safe in every mode.

    // Keep chunk streaming clean and safe

    UpdateChunkStreaming();

    // Vehicle<->vehicle collision toggle (no-op scan when neither feature is active)

    UpdateCarCarCollisionPatch();

    // -------------------------------------------------------------

    // FEATURE: UNIFIED WORLD DISTANCE (RENDER, CHUNKS & NPC SPAWNS UP TO 100X)

    // -------------------------------------------------------------

    if (s_FarZUnprotected)

    {

        float mult = g_Config.worldDistanceMultiplier;

        if (mult < 1.0f) mult = 1.0f;

        if (mult > 100.0f) mult = 100.0f;

        float safeFarZ = 15000.0f * mult;

        // 15000 (vanilla) already covers view radii up to M~6-8; a huge far plane only

        // destroys Z-buffer precision, so the cap is 100000 (was 1500000).

        if (safeFarZ > 100000.0f) safeFarZ = 100000.0f;

        *(float*)ADDR_FAR_Z_RDATA = safeFarZ;

    }

    // Bug #8 Fix: Global .rdata speed limits must remain default 8.0f so NPC cars never glitch!

    if (s_SpeedLimitUnprotected)

    {

        *(float*)0x0059FAA8 = 8.0f;

        *(float*)0x005A07A0 = -8.0f;

    }

    // -------------------------------------------------------------

    // FEATURE: SEGA SECRET DEVELOPER CHEATS & UNLOCKS (ACTIVE IN MENUS!)

    // -------------------------------------------------------------

    __try {

        if (g_Config.segaUnlockAll || g_Config.unlockAllVehicles)

        {

            // Bit 0 = 8 selectable vehicles (cabs + bikes at 0x004f5ab3)

            // Bits 1..5 = Crazy Box mini-games

            *(uint8_t*)ADDR_SEGA_UNLOCK_ALL = 0x3F;

            // Clear pending notification mask so the "You Unlocked..." banner never loops!

            *(uint32_t*)ADDR_SEGA_UNLOCK_MASK = 0;

        }

        if (g_Config.segaAnotherDay)

        {

            *(int*)ADDR_SEGA_ANOTHER_DAY = 1;

        }

        if (g_Config.segaNoArrows)

        {

            *(int*)ADDR_SEGA_NO_ARROWS = 1;

        }

        if (g_Config.segaNoDestMark)

        {

            *(int*)ADDR_SEGA_NO_DEST_MARK = 1;

        }

    } __except (EXCEPTION_EXECUTE_HANDLER) {}

    if (!IsInGame())

    {

        // Reset flight state; vanilla physics resumes on the next session

        s_FlyInitialized = false;

        s_SpeedMultFactor = 1.0f;

        ResetNitroState();

        return;

    }

    extern bool g_bMenuOpen;

    // -------------------------------------------------------------

    // FEATURE: FREECAM NAVIGATION (WASD + Space/Shift + Mouse Look)

    // -------------------------------------------------------------

    if (g_Config.freecamEnabled && !g_bMenuOpen)

    {

        float curX = 0, curY = 0, curZ = 0;

        if (!s_FreecamInitialized && GetTaxiPosition(curX, curY, curZ))

        {

            s_FreecamPos = { curX, curY + 6.0f, curZ - 18.0f };

            s_FreecamInitialized = true;

        }

        // Horizontal flight with WASD, Space for rise, Shift for descend

        bool isW     = (GetAsyncKeyState('W') & 0x8000) != 0;

        bool isS     = (GetAsyncKeyState('S') & 0x8000) != 0;

        bool isA     = (GetAsyncKeyState('A') & 0x8000) != 0;

        bool isD     = (GetAsyncKeyState('D') & 0x8000) != 0;

        bool isSpace = (GetAsyncKeyState(VK_SPACE) & 0x8000) != 0;

        bool isShift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;

        float moveSpeed = 0.5f * (g_Config.freecamSpeed * 0.1f);

        float dFwd = 0.0f, dRight = 0.0f, dUp = 0.0f;

        if (isW) dFwd   += moveSpeed;

        if (isS) dFwd   -= moveSpeed;

        if (isA) dRight -= moveSpeed;

        if (isD) dRight += moveSpeed;

        if (isSpace) dUp += moveSpeed;

        if (isShift) dUp -= moveSpeed;

        MoveFreecam(dRight, dUp, dFwd);

        // Modern 360-Degree Mouse Look

        HWND hFocusWnd = GetActiveWindow();

        if (hFocusWnd && GetForegroundWindow() == hFocusWnd)

        {

            RECT rc;

            GetClientRect(hFocusWnd, &rc);

            POINT center = { (rc.right - rc.left) / 2, (rc.bottom - rc.top) / 2 };

            ClientToScreen(hFocusWnd, &center);

            POINT cur;

            GetCursorPos(&cur);

            int dx = cur.x - center.x;

            int dy = cur.y - center.y;

            if (dx != 0 || dy != 0)

            {

                float yawMult   = -1.0f; // Permanently inverted yaw for natural look

                float pitchMult = -1.0f; // Permanently inverted pitch for natural look

                g_Config.freecamYaw   += (float)dx * 0.20f * yawMult;

                g_Config.freecamPitch -= (float)dy * 0.20f * pitchMult;

                if (g_Config.freecamPitch > 89.0f)  g_Config.freecamPitch = 89.0f;

                if (g_Config.freecamPitch < -89.0f) g_Config.freecamPitch = -89.0f;

                SetCursorPos(center.x, center.y);

            }

        }

    }

    else

    {

        static bool s_FreecamCursorHidden = false;

        if (s_FreecamCursorHidden)

        {

            ShowCursor(TRUE);

            s_FreecamCursorHidden = false;

        }

    }

    // Track real vehicle speed via per-frame world displacement

    float curTaxiX = 0, curTaxiY = 0, curTaxiZ = 0;

    if (GetTaxiPosition(curTaxiX, curTaxiY, curTaxiZ))

    {

        if (fabs(s_LastTaxiPx) > 1.0f && fabs(s_LastTaxiPz) > 1.0f)

        {

            float dx = curTaxiX - s_LastTaxiPx;

            float dz = curTaxiZ - s_LastTaxiPz;

            float step = sqrtf(dx * dx + dz * dz);

            float instantMph = step * 16.1f;

            s_RealSpeedMph = s_RealSpeedMph * 0.70f + instantMph * 0.30f;

        }

        s_LastTaxiPx = curTaxiX;

        s_LastTaxiPz = curTaxiZ;

    }

    // -------------------------------------------------------------

    // FEATURE: MOUSE LOOK (LOOK AROUND / FREE ORBIT IN-GAME)

    // -------------------------------------------------------------

    if (g_Config.mouseLookEnabled && !g_Config.freecamEnabled && !g_bMenuOpen && IsInGame())

    {

        HWND hFocusWnd = GetActiveWindow();

        if (hFocusWnd && GetForegroundWindow() == hFocusWnd)

        {

            RECT rc;

            GetClientRect(hFocusWnd, &rc);

            POINT center = { (rc.right - rc.left) / 2, (rc.bottom - rc.top) / 2 };

            ClientToScreen(hFocusWnd, &center);

            POINT cur;

            GetCursorPos(&cur);

            int dx = cur.x - center.x;

            int dy = cur.y - center.y;

            if (dx != 0 || dy != 0)

            {

                float yawMult   = -1.0f; // Permanently inverted yaw for natural orbit

                float pitchMult = 1.0f;  // Permanently non-inverted pitch for natural orbit

                float sens      = g_Config.mouseLookSensitivity > 0.0f ? g_Config.mouseLookSensitivity : 0.20f;

                s_MouseLookYaw   += (float)dx * sens * yawMult;

                s_MouseLookPitch -= (float)dy * sens * pitchMult;

                if (s_MouseLookPitch > 80.0f)  s_MouseLookPitch = 80.0f;

                if (s_MouseLookPitch < -80.0f) s_MouseLookPitch = -80.0f;

                SetCursorPos(center.x, center.y);

            }

            if (GetAsyncKeyState(VK_MBUTTON) & 0x8000)

            {

                s_MouseLookYaw   = 0.0f;

                s_MouseLookPitch = 0.0f;

            }

        }

    }

    bool inCutscene = false;

    int  vehState   = 0;

    __try {

        vehState = *(int*)ADDR_VEHICLE_STATE;

    } __except (EXCEPTION_EXECUTE_HANDLER) {}

    __try {

        // Same cutscene definition as PostVehiclePhysics: camera states 10/11 are the
        // pickup/drop-off cutscene cameras, the shot index alone reads stale.
        inCutscene = (*(int*)ADDR_FARE_CUTSCENE != 0) || vehState == 10 || vehState == 11;

    } __except (EXCEPTION_EXECUTE_HANDLER) {}

        // Fly Mode position integrator: advances exactly once per frame; PostVehiclePhysics
    // writes the result into the vehicle root/dynamics block (gravity neutralized by the
    // absolute per-frame position writes there). Movement follows the vehicle heading;
    // +Y is UP in this engine.
    if (!g_Config.flyMode && s_FlyInitialized)
    {
        s_FlyInitialized = false; // Vanilla physics takes over again
    }
    else if (g_Config.flyMode && !g_bMenuOpen && !inCutscene && s_FlyInitialized)
    {
        bool isSteerL = (GetAsyncKeyState(VK_LEFT) & 0x8000) != 0 || (GetAsyncKeyState('A') & 0x8000) != 0;
        bool isSteerR = (GetAsyncKeyState(VK_RIGHT) & 0x8000) != 0 || (GetAsyncKeyState('D') & 0x8000) != 0;
        bool isFwd    = (GetAsyncKeyState(VK_UP) & 0x8000) != 0 || (GetAsyncKeyState('W') & 0x8000) != 0;
        bool isBack   = (GetAsyncKeyState(VK_DOWN) & 0x8000) != 0 || (GetAsyncKeyState('S') & 0x8000) != 0;
        bool isAscend = (GetAsyncKeyState(VK_SPACE) & 0x8000) != 0;
        bool isDescend = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0 || (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;

        float flySpeed = g_Config.flySpeed * 0.12f;
        if (flySpeed < 0.5f) flySpeed = 0.5f;

        // Yaw steering (Turn Left / Right) - corrected Sega heading direction
        if (isSteerL && !isSteerR)
        {
            s_FlyYawDeg += 2.2f;
        }
        else if (isSteerR && !isSteerL)
        {
            s_FlyYawDeg -= 2.2f;
        }

        float yawRad = s_FlyYawDeg * (3.1415926535f / 180.0f);
        // Engine velocity convention: vX = sin(heading), vZ = cos(heading)
        Vector3 fwd = { sinf(yawRad), 0.0f, cosf(yawRad) };

        float dx = 0.0f, dy = 0.0f, dz = 0.0f;
        if (isFwd && !isBack)
        {
            dx += fwd.x * flySpeed;
            dz += fwd.z * flySpeed;
        }
        else if (isBack && !isFwd)
        {
            dx -= fwd.x * flySpeed;
            dz -= fwd.z * flySpeed;
        }

        // Also read gamepad analog stick and trigger / buttons
        __try {
            int16_t padSteer = *(int16_t*)ADDR_INPUT_STEER_AXIS;
            if (padSteer < -4000) s_FlyYawDeg += 2.2f * (-(float)padSteer / 32768.0f);
            else if (padSteer > 4000) s_FlyYawDeg -= 2.2f * ((float)padSteer / 32768.0f);

            uint16_t padAccel = *(uint16_t*)ADDR_INPUT_ACCEL_AXIS;
            if (padAccel > 4000) {
                float factor = (float)padAccel / 32768.0f;
                dx += fwd.x * flySpeed * factor;
                dz += fwd.z * flySpeed * factor;
            }
            uint16_t padBrake = *(uint16_t*)ADDR_INPUT_BRAKE_AXIS;
            if (padBrake > 4000) {
                float factor = (float)padBrake / 32768.0f;
                dx -= fwd.x * flySpeed * factor;
                dz -= fwd.z * flySpeed * factor;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}

        // Vertical flight: ascend (+Y up), descend
        if (isAscend && !isDescend)
        {
            dy += flySpeed * 0.8f;
        }
        else if (isDescend && !isAscend)
        {
            dy -= flySpeed * 0.8f;
        }

        s_FlyPosX += dx;
        s_FlyCurrentAltitude += dy;
        s_FlyPosZ += dz;
    }

    // FEATURE: VEHICLE SPEED MULTIPLIER + NITRO TAXI
    s_SpeedMultFactor = 1.0f;

    // Nitro Taxi state machine: SHIFT ramps the boost factor (see UpdateNitroState).
    UpdateNitroState();

    if (!g_Config.flyMode && vehState != 10 && vehState != 11)
    {
        float mult = 1.0f;
        if (g_Config.enableSpeedMultiplier)
        {
            mult = g_Config.speedMultiplier;
            if (mult > 3.00f) mult = 3.00f;
            if (mult < 1.00f) mult = 1.00f;
        }

        // Nitro combines multiplicatively with the user speed multiplier
        float nitroMult = IsNitroActive() ? 1.50f : 1.0f;
        float combined = mult * nitroMult;

        if (combined > 1.001f)
        {
            bool isGas = (GetAsyncKeyState(VK_UP) & 0x8000) != 0 ||
                         (GetAsyncKeyState('W') & 0x8000) != 0 ||
                         (GetAsyncKeyState('D') & 0x8000) != 0 || // Crazy Taxi native PC accelerate key
                         (g_Config.keyForward != 0 && (GetAsyncKeyState(g_Config.keyForward) & 0x8000) != 0) ||
                         (*(uint8_t*)ADDR_INPUT_BTN_ACCEL != 0) ||
                         (*(uint16_t*)ADDR_INPUT_ACCEL_AXIS > 1000);

            bool isBrake = (GetAsyncKeyState(VK_DOWN) & 0x8000) != 0 ||
                           (GetAsyncKeyState('S') & 0x8000) != 0 ||
                           (g_Config.keyBackward != 0 && (GetAsyncKeyState(g_Config.keyBackward) & 0x8000) != 0) ||
                           (*(uint8_t*)ADDR_INPUT_BTN_BRAKE != 0) ||
                           (*(uint16_t*)ADDR_INPUT_BRAKE_AXIS > 1000);

            uint8_t gear = *(uint8_t*)ADDR_INPUT_GEAR_STATE; // 0/2 = Drive, 1 = Reverse

            if ((isGas || IsNitroActive()) && !isBrake && gear != 1)
            {
                s_SpeedMultFactor = combined;
            }
        }

        // Dynamic speed clamp update in .rdata (0x0059FC78 double & 0x005A06C0 float)
        if (s_SpeedLimitUnprotected)
        {
            float targetFactor = (s_SpeedMultFactor > 1.001f) ? s_SpeedMultFactor : mult;
            if (IsNitroActive() && targetFactor < mult * 1.50f)
                targetFactor = mult * 1.50f;
            *(double*)ADDR_VEHICLE_SPEED_LIMIT_DBL = 14.0 * (double)targetFactor;
            *(float*)ADDR_VEHICLE_SPEED_LIMIT_FLT  = 14.0f * targetFactor;
        }
    }
    else
    {
        if (s_SpeedLimitUnprotected)
        {
            *(double*)ADDR_VEHICLE_SPEED_LIMIT_DBL = 14.0;
            *(float*)ADDR_VEHICLE_SPEED_LIMIT_FLT  = 14.0f;
        }
    }

    // CHEAT: IN-GAME TIMER (UP TO 16 MINUTES / 960s & FREEZE TIMER)

    // -------------------------------------------------------------

    static bool s_TimerInjectedForRound = false;

    static int  s_LastInjectedMinutes   = -1;

    if (!IsInGame())

    {

        s_TimerInjectedForRound = false;

        s_LastInjectedMinutes   = -1;

    }

    else if (g_Config.customTimerEnabled)

    {

        __try {

            int currentTimer = *(int*)ADDR_GAME_TIMER;

            int desiredMinutes = g_Config.customTimerMinutes;

            if (desiredMinutes < 1)  desiredMinutes = 1;

            if (desiredMinutes > 16) desiredMinutes = 16;

            int desiredFrames = desiredMinutes * 60 * 60;

            if (desiredMinutes != s_LastInjectedMinutes)

            {

                *(int*)ADDR_GAME_TIMER = desiredFrames;

                s_LastInjectedMinutes = desiredMinutes;

                s_TimerInjectedForRound = true;

            }

            else if (currentTimer > 0 && currentTimer <= 36000 && !s_TimerInjectedForRound)

            {

                *(int*)ADDR_GAME_TIMER = desiredFrames;

                s_LastInjectedMinutes = desiredMinutes;

                s_TimerInjectedForRound = true;

            }

            if (g_Config.freezeTimer && currentTimer > 0)

            {

                *(int*)ADDR_GAME_TIMER = desiredFrames;

            }

        } __except (EXCEPTION_EXECUTE_HANDLER) {}

    }

    else

    {

        s_LastInjectedMinutes = -1;

    }

}

void SetInGameTimerSeconds(int seconds)

{

    __try {

        if (seconds < 1) seconds = 1;

        if (seconds > 999) seconds = 999; // Max 999 seconds (fits within 3-digit arcade HUD)

        *(int*)ADDR_GAME_TIMER = seconds * 60;

    } __except (EXCEPTION_EXECUTE_HANDLER) {}

}

void SetInGameTimerMinutes(int minutes)

{

    SetInGameTimerSeconds(minutes * 60);

}

int GetInGameTimerSeconds()

{

    __try {

        int frames = *(int*)ADDR_GAME_TIMER;

        if (frames < 0) return 0;

        return frames / 60;

    } __except (EXCEPTION_EXECUTE_HANDLER) {

        return 0;

    }

}

bool GetGroundCollision()

{

    __try {

        return (*(uint8_t*)ADDR_GROUND_COLLISION != 0);

    } __except (EXCEPTION_EXECUTE_HANDLER) {

        return false;

    }

}

bool IsInGame()
{
    __try
    {
        // 1. Master Game Status Check from offset_list.json (ADDR_GAME_FLAG 0x016A2CFC):
        // 0 = menu / Attract demo, 1 = 3D city scene active / WorldLoaded,
        // 2 = Fade transition, 3 = Active gameplay round started, 7 = QuitToMenu.
        int mainState = *(int*)ADDR_GAME_FLAG; // 0x016A2CFC
        if (mainState == 0 || mainState == 7)
            return false;

        // 2. Attract Mode AI Demo check (State 8 = CPU Attract demo)
        // Note: vehState 10 (passenger pickup) and 11 (passenger dropoff) are valid in-game states!
        int vehState = *(int*)ADDR_VEHICLE_STATE; // 0x01661474
        if (vehState == 8)
            return false;

        // 3. Player coordinates sanity check (In menus: 0.0, 0.0, 0.0)
        float px = *(float*)ADDR_PLAYER_X;
        float py = *(float*)ADDR_PLAYER_Y;
        float pz = *(float*)ADDR_PLAYER_Z;
        if (std::isnan(px) || std::isnan(py) || std::isnan(pz))
            return false;

        // Coordinates are 0.0 in all menus (Title screen, Arcade/Original select, Character select)
        if (fabs(px) < 1.0f && fabs(py) < 1.0f && fabs(pz) < 1.0f)
            return false;

        // Fly Mode exploration envelope
        if (fabs(px) > 500000.0f || fabs(pz) > 500000.0f)
            return false;

        // 4. In 3D world, mainState is 1 or 3, or round timer is active
        int timerFrames = *(int*)ADDR_GAME_TIMER; // 0x016a41a4
        if (timerFrames > 0 || mainState == 1 || mainState == 3)
            return true;

        return false;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool GetTaxiPosition(float& x, float& y, float& z)

{

    __try

    {

        // Reads the REAL vehicle root transform (0x01661FA8/AC/FB0), not the camera look-at

        float px = *(float*)ADDR_VEHICLE_ROOT_X;

        float py = *(float*)ADDR_VEHICLE_ROOT_Y;

        float pz = *(float*)ADDR_VEHICLE_ROOT_Z;

        if (std::isnan(px) || std::isnan(py) || std::isnan(pz))

            return false;

        if (fabs(px) < 0.001f && fabs(py) < 0.001f && fabs(pz) < 0.001f)

            return false;

        if (fabs(px) > 300000.0f || fabs(pz) > 300000.0f)

            return false;

        x = px;

        y = py;

        z = pz;

        return true;

    }

    __except (EXCEPTION_EXECUTE_HANDLER)

    {

        return false;

    }

}

bool GetTaxiSpeed(float& speed)

{

    __try

    {

        if (!IsInGame())

        {

            speed = 0.0f;

            return false;

        }

        int vehState = *(int*)ADDR_VEHICLE_STATE;

        // In attract / demo / cutscene: force 0 speed

        if (vehState >= 8 && vehState != 10 && vehState != 11)

        {

            speed = 0.0f;

            return true;

        }

        if (g_Config.flyMode)
        {
            speed = g_Config.flySpeed * 4.2857f;
            return true;
        }

        // Read true Sega per-frame spatial displacement
        float dispX = *(float*)ADDR_VEHICLE_DISP_X;
        float dispZ = *(float*)ADDR_VEHICLE_DISP_Z;
        float dispLen = sqrtf(dispX * dispX + dispZ * dispZ);

        if (dispLen < 0.005f)

        {

            speed = 0.0f;

            return true;

        }

        // Sega's verified conversion: 14.0 scalar ≈ 60 MPH (≈ 96.5 km/h)

        speed = dispLen * 4.2857f;

        return true;

    }

    __except (EXCEPTION_EXECUTE_HANDLER)

    {

        speed = 0.0f;

        return false;

    }

}

bool GetTaxiMoney(float& totalDollars, float& rideTips)

{

    __try

    {

        int totalCents = *(int*)ADDR_TOTAL_MONEY;

        int tipCents   = *(int*)ADDR_CURRENT_FARE_TIPS;

        totalDollars = (float)totalCents / 100.0f;

        rideTips     = (float)tipCents   / 100.0f;

        return true;

    }

    __except (EXCEPTION_EXECUTE_HANDLER)

    {

        return false;

    }

}

bool TeleportTaxi(float newX, float newY, float newZ, bool clampToGroundY)

{
    // T3 hygiene (D.4.1): the teleport button is UI-global. Arming the latch outside an
    // active session parked it until the NEXT session's first physics frames overpainted
    // them with the stale target. Require a real session and reject the attract-demo
    // camera (state 8) explicitly.
    if (!IsInGame())

        return false;

    float curX = 0, curY = 0, curZ = 0;

    if (!GetTaxiPosition(curX, curY, curZ))

        return false;

    // Never fight an active fare cutscene for the vehicle position - vanilla owns the
    // taxi completely while the pickup/drop-off cutscene camera (state 10/11) runs and
    // the attract demo (state 8) drives the taxi itself. The shot index 0x016F2164 alone
    // reads stale after a cutscene (stays at the last shot 5-7, see PostVehiclePhysics)
    // and would block teleports with Fly Mode off, so the camera state is the deciding
    // bit here.

    int vehState = 0;

    __try

    {

        vehState = *(int*)ADDR_VEHICLE_STATE;

        if (vehState == 8 || vehState == 10 || vehState == 11)

            return false;

    } __except (EXCEPTION_EXECUTE_HANDLER) {}

    // T3 fix plan step 2: validate the target Y with the engine's own read-only raycast
    // (0x0050C6D0). Its selection returns the highest walkable surface at or below the
    // query point (verified against the raw disassembly: the below-window is unbounded,
    // the above-window climbs in (v9, v9+20] steps), so a hit height +1.0 is a safe
    // settle height right above real ground. Without this, a target parked far above a
    // drivable surface kept the latch fighting the physics for no benefit. Two callers
    // are exempt from the clamp:
    // - Fly Mode: its writer owns the position every frame and the user explicitly asked
    //   for an altitude - clamping it would regress fly-teleports.
    // - The '+15m (Unstuck)' button (clampToGroundY=false): it intentionally targets
    //   curY+15 to drop back onto the surface; probing that Y would hit the ground UNDER
    //   the taxi (unbounded below-window) and collapse the elevate to groundY+1.
    float latchY = newY;

    float groundY = 0.0f;

    bool rayHit = false;

    if (clampToGroundY && !g_Config.flyMode && TeleportProbeGround(newX, newY, newZ, &groundY))

    {

        rayHit = true;

        latchY = groundY + 1.0f;

    }

    __try

    {
        // Root + dynamics block + prev fields: keeps Sega's root<->block sync (0x00519CAF)

        // from snapping the position back and prevents the teleport delta from being

        // re-derived as speed on the next frame.

        *(float*)ADDR_VEHICLE_ROOT_X  = newX;

        *(float*)ADDR_VEHICLE_ROOT_Y  = latchY;

        *(float*)ADDR_VEHICLE_ROOT_Z  = newZ;

        *(float*)ADDR_VEHICLE_BLOCK_X = newX;

        *(float*)ADDR_VEHICLE_BLOCK_Y = latchY;

        *(float*)ADDR_VEHICLE_BLOCK_Z = newZ;

        *(float*)ADDR_VEHICLE_PREV_X  = newX;

        *(float*)ADDR_VEHICLE_PREV_Y  = latchY;

        *(float*)ADDR_VEHICLE_PREV_Z  = newZ;

        *(float*)ADDR_VEHICLE_VEL_X = 0.0f;

        *(float*)ADDR_VEHICLE_VEL_Y = 0.0f;

        *(float*)ADDR_VEHICLE_VEL_Z = 0.0f;

        // The velocity mirror (0x01662038/3C/40) is part of the off-world respawn's own
        // write set (sub_513E80 nulls it) - leaving it stale would keep the pre-teleport
        // velocity alive for the dynamics sync.

        *(float*)ADDR_VEHICLE_VEL_MIRROR_X = 0.0f;

        *(float*)ADDR_VEHICLE_VEL_MIRROR_Y = 0.0f;

        *(float*)ADDR_VEHICLE_VEL_MIRROR_Z = 0.0f;

        *(float*)ADDR_VEHICLE_DISP_X = 0.0f;

        *(float*)ADDR_VEHICLE_DISP_Y = 0.0f;

        *(float*)ADDR_VEHICLE_DISP_Z = 0.0f;

        *(float*)ADDR_VEHICLE_SPEED_SCALAR = 0.0f;

        // Prev orientation = current attitude: sweep start pose of sub_5146C0 (+152/154/156)

        *(uint16_t*)ADDR_VEHICLE_PREV_ATTITUDE_BAM       = *(uint16_t*)ADDR_VEHICLE_ATTITUDE_BAM;

        *(uint16_t*)(ADDR_VEHICLE_PREV_ATTITUDE_BAM + 2) = *(uint16_t*)(ADDR_VEHICLE_ATTITUDE_BAM + 2);

        *(uint16_t*)ADDR_VEHICLE_PREV_ROLL_BAM           = *(uint16_t*)ADDR_VEHICLE_ROLL_REAL_BAM;

        // Keep the flight integrator in sync while Fly Mode is active

        if (g_Config.flyMode)

        {

            s_FlyPosX = newX;

            s_FlyCurrentAltitude = newY;

            s_FlyPosZ = newZ;

        }

        // Move the camera rig with the taxi

        float dx = newX - curX;

        float dy = latchY - curY;

        float dz = newZ - curZ;

        *(float*)ADDR_CAM_EYE_X += dx;

        *(float*)ADDR_CAM_EYE_Y += dy;

        *(float*)ADDR_CAM_EYE_Z += dz;

        *(float*)ADDR_CAM_TARGET_X = newX;

        *(float*)ADDR_CAM_TARGET_Y = latchY + 5.0f;

        *(float*)ADDR_CAM_TARGET_Z = newZ;

        // T3 hygiene: seed the camera smoothing state so the first camera_update after
        // the teleport produces a zero delta instead of gliding from the old position.
        // 0x016614EC/F0/F4 = persistent eye (smoothing source), 0x016614AC/B0/B4 = prev
        // frame look-at (delta source in camera_update case 0).

        *(float*)ADDR_CAM_PERSISTENT_EYE_X = *(float*)ADDR_CAM_EYE_X;

        *(float*)ADDR_CAM_PERSISTENT_EYE_Y = *(float*)ADDR_CAM_EYE_Y;

        *(float*)ADDR_CAM_PERSISTENT_EYE_Z = *(float*)ADDR_CAM_EYE_Z;

        *(float*)ADDR_CAM_PREV_LOOKAT_X = newX;

        *(float*)ADDR_CAM_PREV_LOOKAT_Y = latchY + 5.0f;

        *(float*)ADDR_CAM_PREV_LOOKAT_Z = newZ;

        // T3 fix plan step 4 was a 1800-radius load_chunks_around pre-warm here - REMOVED
        // (T4 live finding): the queued far-area flood starved the 1152-slot texture
        // binder, leaving the whole world untextured after a teleport. The engine's own
        // per-frame streaming (main_game_loop 0x004EA408 -> evict_and_bind_textures
        // 0x005051D0, look-ahead ROOT+30) repopulates the target area at vanilla rate;
        // the collision grid is session-resident, so removing the pre-warm cannot affect
        // the latch's ground-contact release.

        // Arm the latch so PreVehiclePhysics/PostVehiclePhysics replay this target around
        // every physics step - a one-shot write from this (render) thread gets snapped
        // back by the off-world respawn / root<->block sync on the very next physics step
        // otherwise. Release logic lives in PostVehiclePhysics (ground contact + minimum
        // hold, watchdog cap hands genuine void targets back to the vanilla rescue).

        s_TeleportLatchX = newX;

        s_TeleportLatchY = latchY;

        s_TeleportLatchZ = newZ;

        s_TeleportLatchFrames = 10;    // minimum hold before the ground check can release

        s_TeleportLatchWatchdog = 600; // ~10 s at 60 fps, then vanilla rescue takes over

        s_TeleportLatchLog = 0;

        s_TeleportLatchActive = true;

        // T3 fix plan step 0 (diagnostics): one line per teleport with the decision
        // inputs - discriminates "latch never ran" from "post-latch snap" in the log.
        Logger::Info("Teleport", "teleport target %.1f/%.1f/%.1f latchY=%.1f vehState=%d inGame=1 rootBefore=%.1f/%.1f/%.1f rayHit=%d fly=%d",

                     newX, newY, newZ, latchY, vehState, curX, curY, curZ, rayHit ? 1 : 0, g_Config.flyMode ? 1 : 0);

        if (clampToGroundY && !rayHit && !g_Config.flyMode)

        {

            // Only when the step-2 probe was actually attempted: no walkable surface

            // polygon contains the target XZ (true void/water/interior column - lowering

            // Y cannot help, the collision grid cell is XZ-selected). The latch watchdog

            // hands this target back to the vanilla waypoint rescue. Unstuck calls skip

            // the probe on purpose and must not log this.

            Logger::Warn("Teleport", "target %.1f/%.1f/%.1f has no walkable surface - respawn field, vanilla rescue applies after watchdog",

                         newX, newY, newZ);

        }

        return true;

    }

    __except (EXCEPTION_EXECUTE_HANDLER)

    {

        return false;

    }

}

bool GetCameraEyePosition(float& x, float& y, float& z)

{

    __try

    {

        x = *(float*)ADDR_CAM_EYE_X;

        y = *(float*)ADDR_CAM_EYE_Y;

        z = *(float*)ADDR_CAM_EYE_Z;

        return true;

    }

    __except (EXCEPTION_EXECUTE_HANDLER)

    {

        return false;

    }

}

bool GetCameraTargetPosition(float& x, float& y, float& z)

{

    __try

    {

        x = *(float*)ADDR_CAM_TARGET_X;

        y = *(float*)ADDR_CAM_TARGET_Y;

        z = *(float*)ADDR_CAM_TARGET_Z;

        return true;

    }

    __except (EXCEPTION_EXECUTE_HANDLER)

    {

        return false;

    }

}

// ---------------------------------------------------------------------------------

// VULKAN (DXVK) INTER-MOD EXPORT APIS

// ---------------------------------------------------------------------------------

extern "C" {

    __declspec(dllexport) bool __cdecl ModAPI_IsVulkanEnabled()

    {

        DWORD attr = GetFileAttributesA("d3d9.dll");

        bool fileExists = (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY));

        return (fileExists && g_Config.enableDXVK);

    }

    __declspec(dllexport) bool __cdecl ModAPI_SetVulkanEnabled(bool enable)

    {

        g_Config.enableDXVK = enable;

        bool ok = false;

        if (enable)

        {

            if (CopyFileA("dxvk_optional\\d3d9.dll", "d3d9.dll", FALSE))

            {

                ok = true;

                Logger::Info("ModAPI", "Vulkan (DXVK) activated via Mod API. Restart game to apply.");

            }

            else

            {

                Logger::Error("ModAPI", "Failed to activate Vulkan: dxvk_optional\\d3d9.dll not found.");

            }

        }

        else

        {

            if (DeleteFileA("d3d9.dll"))

            {

                ok = true;

                Logger::Info("ModAPI", "Vulkan (DXVK) deactivated via Mod API. Native D3D9 active.");

            }

            else

            {

                if (MoveFileExA("d3d9.dll", NULL, MOVEFILE_DELAY_UNTIL_REBOOT))

                {

                    ok = true;

                    Logger::Info("ModAPI", "Vulkan (DXVK) removal scheduled for game restart.");

                }

            }

        }

        SaveConfig();

        return ok;

    }

    __declspec(dllexport) bool __cdecl ModAPI_ToggleVulkan()

    {

        return ModAPI_SetVulkanEnabled(!ModAPI_IsVulkanEnabled());

    }

}
