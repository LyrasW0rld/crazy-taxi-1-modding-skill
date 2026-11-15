#pragma once
#include <windows.h>
#include <string>

#define MOD_VERSION "1.2"

enum ControlMode
{
    CONTROL_MODE_STANDARD = 0,
    CONTROL_MODE_WASD     = 1
};

struct ModConfig
{
    // In-Game Timer Cheat (Up to 99 Minutes & Freeze Timer)
    bool  customTimerEnabled      = false;
    int   customTimerMinutes      = 10; // Default 10 minutes (600s, fits cleanly within 3-digit 999s arcade HUD)
    bool  freezeTimer             = false;

    // Economy & Stunt
    float stuntMultiplier         = 1.0f;

    // Camera 3D Offsets (Independent Axes)
    float camOffsetX        = 0.0f; // Lateral (Left / Right)
    float camOffsetY        = 0.0f; // Elevation (Up / Down)
    float camOffsetZ        = 0.0f; // Distance (Forward / Backward)
    float camRotPitch       = 0.0f; // Pitch (Tilt Up / Down)
    float camRotYaw         = 0.0f; // Yaw (Orbit Left / Right)
    float camRotRoll        = 0.0f; // Roll (Horizon Banking)

    // Fly Mode & Collision Controls
    bool  flyMode                 = false;
    float flySpeed                = 25.0f;
    bool  disableTrafficCollision = false;

    // Freecam Mode (Invert Pitch, Invert Yaw, and Invert WASD are permanently enabled as natural controls)
    bool  freecamEnabled        = false;
    float freecamSpeed          = 25.0f;
    float freecamPitch          = 0.0f;
    float freecamYaw            = 0.0f;
    float freecamRoll           = 0.0f;
    bool  freecamInvertPitch    = true;  // Permanently true
    bool  freecamInvertYaw      = true;  // Permanently true
    bool  freecamInvertWASD     = true;  // Permanently true

    // Mouse Look (Free Orbit in Normal Gameplay & Fly Mode - Yaw inverted, Pitch normal)
    bool  mouseLookEnabled      = false;
    bool  mouseLookInvertPitch  = false; // Permanently false
    bool  mouseLookInvertYaw    = true;  // Permanently true
    float mouseLookSensitivity  = 0.03f; // Range: 0.005 to 0.10

    // Unified World Distance (Render, Chunks & NPC Spawns) - Up to 100x!
    float worldDistanceMultiplier     = 2.5f;  // Slider: 1.0x to 100.0x
    float renderDistanceMultiplier    = 2.5f;  // Backward-compat sync
    float chunkLoadDistanceMultiplier = 2.5f;  // Backward-compat sync
    bool  persistentChunkLoader       = false;

    // Original Soundtrack (The Offspring & Bad Religion)
    bool  useOriginalSoundtrack    = true;

    // Vehicle Speed Multiplier (from CrazyTaxiSpeedMod, strictly clamped to max 1.5x)
    bool  enableSpeedMultiplier = false;
    float speedMultiplier       = 1.0f;  // Slider: 1.00x to 1.50x (Max 1.5x to preserve streaming)

    // Nitro Taxi (hold SHIFT for a temporary boost)
    bool  nitroEnabled          = false;

    // Sega Secret Developer Cheats & Unlocks
    bool  segaUnlockAll             = false;
    bool  segaAnotherDay            = false;
    bool  segaNoArrows              = false;
    bool  segaNoDestMark            = false;
    bool  segaBikeMode              = false;
    bool  unlockAllVehicles         = false; // Unlocks and activates alternate vehicle (Bike)

    // Controls & Keybinding (Native Sega Arcade Controls)
    int   controlMode           = CONTROL_MODE_STANDARD; // Default: Native Arcade Controls
    int   steeringMode          = 0; // 0=Standard, 1=4-Wheel (4WS), 2=Drunk Driver, 3=Slippery Ice
    int   keyForward            = VK_UP;             // Forward / Gas
    int   keyBackward           = VK_DOWN;           // Backward / Brake
    int   keyLeft               = VK_LEFT;           // Steer Left
    int   keyRight              = VK_RIGHT;          // Steer Right
    int   keyBrake              = VK_DOWN;           // Brake
    int   keyGearDrive          = 'D';               // Shift Drive
    int   keyGearReverse        = 'S';               // Shift Reverse

    // HUD Options
    bool   showSpeedometer           = false; // Analog km/h gauge (in-game only)
    double odometerTotalKm           = 0.0;   // Real accumulated total distance in KM
    double odometerTripKm            = 0.0;   // Real accumulated trip/round distance in KM
    bool   feminineGuiMode           = false; // Feminine pastel rose/pink theme for Mod Manager and Speedometer

    // Mod Menu Window Dimensions
    float  menuWindowWidth           = 540.0f;
    float  menuWindowHeight          = 420.0f;

    // Dreamcast Restoration 2.0 (CT-DC) by CookiePLMonster & Alexvgz
    bool  dcRestorationEnabled      = true;
    bool  dcOriginalNames           = true;
    bool  dcFilaDestination         = true;
    bool  dcDreamcastModels         = true;
    bool  dcOriginalVoices          = true;
    bool  dcOriginalTextures        = true;
    bool  dcLogosZBias              = true;

    // Engine Fixes & Compatibility (Windowed Fix, Analog Controls, Alt+F4, Hotkeys)
    bool  engineFixesEnabled        = true;
    bool  windowAspectRatioFix      = true;
    bool  steeringWheelCrashFix     = true;
    bool  analogDeadzoneFix         = true;
    bool  altF4ExitFix              = true;
    bool  debugCameraHotkeys        = true;

    // FFB Arcade Plugin Integration (Force Feedback & Surface Rumble)
    bool  ffbEnabled                = true;
    float ffbStrength               = 1.0f;  // 0.0f to 2.0f
    bool  ffbGroundEffects          = true;
    bool  ffbEngineRumble           = true;
    bool  ffbCollisionEffects       = true;

    // DXVK Vulkan Translation Layer (Can be deactivated anytime for third-party mod compatibility)
    bool  enableDXVK                = false;

    bool  autoDemo              = false;

    void ResetToDefaults()
    {
        enableDXVK                = false;
        dcRestorationEnabled      = true;
        dcOriginalNames           = true;
        dcFilaDestination         = true;
        dcDreamcastModels         = true;
        dcOriginalVoices          = true;
        dcOriginalTextures        = true;
        dcLogosZBias              = true;
        customTimerEnabled        = false;
        customTimerMinutes        = 10;
        freezeTimer               = false;
        stuntMultiplier           = 1.0f;
        camOffsetX            = 0.0f;
        camOffsetY            = 0.0f;
        camOffsetZ            = 0.0f;
        camRotPitch           = 0.0f;
        camRotYaw             = 0.0f;
        camRotRoll            = 0.0f;
        flyMode               = false;
        flySpeed              = 25.0f;
        disableTrafficCollision = false;
        freecamEnabled        = false;
        freecamSpeed          = 25.0f;
        freecamPitch          = 0.0f;
        freecamYaw            = 0.0f;
        freecamRoll           = 0.0f;
        freecamInvertPitch    = true;
        freecamInvertYaw      = true;
        freecamInvertWASD     = true;
        mouseLookEnabled      = false;
        mouseLookInvertPitch  = false;
        mouseLookInvertYaw    = true;
        mouseLookSensitivity  = 0.03f;

        showSpeedometer       = false;
        odometerTotalKm       = 0.0;
        odometerTripKm        = 0.0;
        feminineGuiMode       = false;
        menuWindowWidth       = 540.0f;
        menuWindowHeight      = 420.0f;

        worldDistanceMultiplier     = 2.5f;
        renderDistanceMultiplier    = 2.5f;
        chunkLoadDistanceMultiplier = 2.5f;
        persistentChunkLoader       = false;

        segaUnlockAll            = false;
        segaAnotherDay           = false;
        segaNoArrows             = false;
        segaNoDestMark           = false;
        segaBikeMode             = false;
        controlMode              = CONTROL_MODE_STANDARD;
        steeringMode             = 0;
        nitroEnabled             = false;
        SetDefaultKeybinds(CONTROL_MODE_STANDARD);
        autoDemo                 = false;
    }

    void SetDefaultKeybinds(int mode = CONTROL_MODE_STANDARD)
    {
        controlMode    = CONTROL_MODE_STANDARD;
        keyForward     = VK_UP;
        keyBackward    = VK_DOWN;
        keyLeft        = VK_LEFT;
        keyRight       = VK_RIGHT;
        keyBrake       = VK_DOWN;
        keyGearDrive   = 'D';
        keyGearReverse = 'S';
    }
};

extern ModConfig g_Config;
extern HMODULE g_hModule;

bool LoadConfig();
bool SaveConfig();
std::string GetModDirectory();
std::string GetGameDirectory();
std::string GetConfigPath();
const char* GetKeyNameString(int vkCode);

extern "C" __declspec(dllexport) ModConfig* __cdecl GetModConfig();
