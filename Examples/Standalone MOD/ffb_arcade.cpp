#include "ffb_arcade.h"
#include "config.h"
#include "logger.h"
#include "game_features.h"
#include <stdint.h>
#include <math.h>

namespace FFBArcade
{
    static bool s_Initialized = false;

    // Dynamic XInput definitions
    typedef struct _XINPUT_VIBRATION {
        WORD wLeftMotorSpeed;
        WORD wRightMotorSpeed;
    } XINPUT_VIBRATION;

    typedef DWORD (WINAPI *pfnXInputSetState)(DWORD dwUserIndex, XINPUT_VIBRATION* pVibration);
    static pfnXInputSetState fpXInputSetState = nullptr;
    static HMODULE s_hXInputDll = NULL;

    static uint8_t s_LastSurface = 0;
    static float   s_CollisionTimer = 0.0f;

    void Initialize()
    {
        if (s_Initialized)
            return;

        Logger::Info("FFBArcade", "Initializing native FFB Arcade & Rumble integration...");

        // Dynamically load modern or legacy XInput
        const char* xinputDlls[] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll" };
        for (const char* dll : xinputDlls)
        {
            s_hXInputDll = LoadLibraryA(dll);
            if (s_hXInputDll)
            {
                fpXInputSetState = (pfnXInputSetState)GetProcAddress(s_hXInputDll, "XInputSetState");
                if (fpXInputSetState)
                {
                    Logger::Info("FFBArcade", "Loaded XInput force feedback from %s", dll);
                    break;
                }
                FreeLibrary(s_hXInputDll);
                s_hXInputDll = NULL;
            }
        }

        s_Initialized = (fpXInputSetState != nullptr);
        if (s_Initialized)
        {
            Logger::Info("FFBArcade", "Native FFB Arcade system ready");
        }
        else
        {
            Logger::Warn("FFBArcade", "XInput DLL not found, gamepad rumble unavailable");
        }
    }

    void Shutdown()
    {
        if (!s_Initialized)
            return;

        // Zero vibration on shutdown
        if (fpXInputSetState)
        {
            XINPUT_VIBRATION zeroVib = { 0, 0 };
            fpXInputSetState(0, &zeroVib);
        }

        if (s_hXInputDll)
        {
            FreeLibrary(s_hXInputDll);
            s_hXInputDll = NULL;
        }

        fpXInputSetState = nullptr;
        s_Initialized = false;
        Logger::Info("FFBArcade", "Native FFB Arcade shut down");
    }

    uint8_t GetCurrentSurface()
    {
        __try
        {
            return *(uint8_t*)ADDR_SURFACE_GROUND_EFFECT;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    void UpdatePerFrame()
    {
        if (!s_Initialized || !fpXInputSetState || !g_Config.ffbEnabled)
            return;

        if (!IsInGame())
        {
            // Zero vibration in menus
            XINPUT_VIBRATION zeroVib = { 0, 0 };
            fpXInputSetState(0, &zeroVib);
            return;
        }

        float leftMotor = 0.0f;
        float rightMotor = 0.0f;

        __try
        {
            float speedMph = 0.0f;
            GetTaxiSpeed(speedMph);
            float speedNorm = speedMph / 100.0f;
            if (speedNorm > 1.0f) speedNorm = 1.0f;
            if (speedNorm < 0.0f) speedNorm = 0.0f;

            uint8_t ground = *(uint8_t*)ADDR_SURFACE_GROUND_EFFECT; // 0x01EB0D74
            float leftWall  = *(float*)ADDR_WALL_COLLISION_LEFT;    // 0x0166214C
            float rightWall = *(float*)ADDR_WALL_COLLISION_RIGHT;   // 0x016621D0
            uint8_t origRumble = *(uint8_t*)ADDR_ORIGINAL_RUMBLE;   // 0x016EF93B
            uint8_t crazy1 = *(uint8_t*)ADDR_CRAZY_MOVES_1;        // 0x016EF892
            uint8_t crazy2 = *(uint8_t*)ADDR_CRAZY_MOVES_2;        // 0x016EF893

            // 1. Engine Idle & Throttle Rumble
            if (g_Config.ffbEngineRumble)
            {
                if (speedMph < 1.0f)
                {
                    // Gentle idle rumble
                    leftMotor += 0.03f;
                    rightMotor += 0.02f;
                }
                else
                {
                    leftMotor += 0.04f + (speedNorm * 0.12f);
                    rightMotor += 0.03f + (speedNorm * 0.10f);
                }
            }

            // 2. Sega Original Arcade Crazy Moves (Crazy Dash / Drift / Jumps)
            if ((crazy1 & 0x10) || crazy2 != 0 || origRumble != 0)
            {
                leftMotor  += 0.65f;
                rightMotor += 0.85f;
            }

            // 3. Collision Effects (Wall impacts & Car hits)
            if (g_Config.ffbCollisionEffects)
            {
                if (leftWall > 0.05f)
                {
                    leftMotor  += 0.90f;
                    rightMotor += 0.40f;
                }
                else if (rightWall > 0.05f)
                {
                    leftMotor  += 0.40f;
                    rightMotor += 0.90f;
                }
                else if (ground == 0xFF) // Solid collision event
                {
                    leftMotor  += 0.85f;
                    rightMotor += 0.85f;
                }
            }

            // 4. Terrain & Ground Surface Effects
            if (g_Config.ffbGroundEffects && speedMph > 2.0f)
            {
                switch (ground)
                {
                case 0x00: // Smooth tarmac road
                    break;
                case 0x03: // Grass: light rumble
                    leftMotor  += 0.25f * speedNorm;
                    rightMotor += 0.35f * speedNorm;
                    break;
                case 0x05: // Shoreline / Wet: moderate drag rumble
                    leftMotor  += 0.35f * speedNorm;
                    rightMotor += 0.25f * speedNorm;
                    break;
                case 0x09: // Sand: heavy friction vibration
                    leftMotor  += 0.45f * speedNorm;
                    rightMotor += 0.35f * speedNorm;
                    break;
                case 0x0A: // Footpath / Sidewalk: high-frequency pavement vibration
                    leftMotor  += 0.20f * speedNorm;
                    rightMotor += 0.50f * speedNorm;
                    break;
                case 0x0E: // Underwater: deep thrumming drag
                    leftMotor  += 0.60f * speedNorm;
                    rightMotor += 0.20f * speedNorm;
                    break;
                case 0x11: // Stairs / Curbs: intense rattling steps
                case 0x12:
                case 0x13:
                case 0x14:
                    leftMotor  += 0.70f * speedNorm;
                    rightMotor += 0.75f * speedNorm;
                    break;
                case 0x15: // Subway tunnel: metallic railroad vibration
                    leftMotor  += 0.40f * speedNorm;
                    rightMotor += 0.55f * speedNorm;
                    break;
                default:
                    break;
                }
            }

            s_LastSurface = ground;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            leftMotor = 0.0f;
            rightMotor = 0.0f;
        }

        // Apply user strength multiplier
        float strength = g_Config.ffbStrength;
        leftMotor  *= strength;
        rightMotor *= strength;

        // Clamp to 0.0 .. 1.0
        if (leftMotor  > 1.0f) leftMotor  = 1.0f;
        if (leftMotor  < 0.0f) leftMotor  = 0.0f;
        if (rightMotor > 1.0f) rightMotor = 1.0f;
        if (rightMotor < 0.0f) rightMotor = 0.0f;

        // Commit to XInput device 0
        XINPUT_VIBRATION vib;
        vib.wLeftMotorSpeed  = static_cast<WORD>(leftMotor  * 65535.0f);
        vib.wRightMotorSpeed = static_cast<WORD>(rightMotor * 65535.0f);
        fpXInputSetState(0, &vib);
    }

    bool IsActive()
    {
        return s_Initialized;
    }
}
