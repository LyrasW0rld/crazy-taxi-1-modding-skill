#include <windows.h>
#include <stdio.h>
#include <MinHook.h>
#include "config.h"
#include "game_features.h"
#include "render_hook.h"
#include "logger.h"
#include "engine_fixes.h"
#include "ffb_arcade.h"

HMODULE g_hModule = NULL;

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpReserved)
{
    switch (fdwReason)
    {
    case DLL_PROCESS_ATTACH:
    {
        g_hModule = hinstDLL;
        DisableThreadLibraryCalls(hinstDLL);

        char exePath[MAX_PATH];
        GetModuleFileNameA(NULL, exePath, MAX_PATH);
        const char* exeName = strrchr(exePath, '\\');
        if (!exeName) exeName = strrchr(exePath, '/');
        if (exeName) exeName++;
        else exeName = exePath;

        // ONLY initialize mod features for Crazy Taxi.exe!
        // AppLauncher.exe and any other tools must remain completely untouched!
        if (_stricmp(exeName, "Crazy Taxi.exe") != 0)
        {
            return TRUE;
        }

        // Initialize clean logging subsystem and crash handler
        Logger::Initialize();
        SetUnhandledExceptionFilter(Logger::CrashHandler);
        Logger::Info("Init", "Framework_MOD attached to Crazy Taxi.exe (PID: %lu)", GetCurrentProcessId());

        // Initialize MinHook core engine
        MH_Initialize();
        Logger::Info("Init", "MinHook engine initialized");

        // 1. Load configuration
        LoadConfig();
        Logger::Info("Config", "Loaded mod configuration");

        // 2. Install Game Features, Steam patch, Stunt & Camera hooks
        InitGameFeatures();
        Logger::Info("Features", "Game features and hooks initialized");

        // 2b. Install Engine Fixes (Window aspect ratio fix, Alt+F4 quick exit, DirectInput steering wheel fix, analog controls)
        EngineFixes::Initialize();
        Logger::Info("EngineFixes", "Core engine fixes initialized");

        // 2c. Install FFB Arcade Plugin (Force feedback & surface rumble)
        FFBArcade::Initialize();
        Logger::Info("FFB", "FFB Arcade subsystem initialized");

        // 3. Install D3D9 Render Hook (EndScene & Reset)
        InitRenderHook();
        Logger::Info("Render", "Direct3D 9 render hooks installed");
        break;
    }
    case DLL_PROCESS_DETACH:
        Logger::Info("Shutdown", "Detaching Framework_MOD...");
        ShutdownRenderHook();
        ShutdownGameFeatures();
        FFBArcade::Shutdown();
        EngineFixes::Shutdown();
        MH_Uninitialize();
        Logger::Shutdown();
        break;
    }
    return TRUE;
}
