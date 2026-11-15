#include "config.h"
#include <windows.h>
#include <fstream>
#include <sstream>
#include "../deps/nlohmann/json.hpp"

using json = nlohmann::json;

ModConfig g_Config;

extern "C" __declspec(dllexport) ModConfig* __cdecl GetModConfig()
{
    return &g_Config;
}

std::string GetModDirectory()
{
    // 1. Locate directory of Framework_MOD.dll (mods\Framework_MOD\)
    if (g_hModule)
    {
        char modPath[MAX_PATH];
        if (GetModuleFileNameA(g_hModule, modPath, MAX_PATH))
        {
            std::string path(modPath);
            size_t lastSlash = path.find_last_of("\\/");
            if (lastSlash != std::string::npos)
            {
                return path.substr(0, lastSlash + 1);
            }
        }
    }

    // 2. Fallback to <GameDir>/mods/Framework_MOD/
    char exePath[MAX_PATH];
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    std::string path(exePath);
    size_t lastSlash = path.find_last_of("\\/");
    if (lastSlash != std::string::npos)
    {
        path = path.substr(0, lastSlash + 1);
    }
    return path + "mods\\Framework_MOD\\";
}

std::string GetGameDirectory()
{
    // Derive Crazy Taxi root directory from mod directory by walking up 2 levels (..\..)
    std::string modDir = GetModDirectory();
    std::string relPath = modDir + "..\\..\\";
    char fullPath[MAX_PATH];
    if (GetFullPathNameA(relPath.c_str(), MAX_PATH, fullPath, NULL))
    {
        std::string res(fullPath);
        if (!res.empty() && res.back() != '\\' && res.back() != '/')
            res += "\\";
        return res;
    }

    // Fallback via executable directory
    char exePath[MAX_PATH];
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    std::string path(exePath);
    size_t lastSlash = path.find_last_of("\\/");
    if (lastSlash != std::string::npos)
    {
        return path.substr(0, lastSlash + 1);
    }
    return path + "\\";
}

std::string GetConfigPath()
{
    return GetModDirectory() + "crazytaxi_mod_config.json";
}

bool LoadConfig()
{
    std::string path = GetConfigPath();
    std::ifstream file(path);
    if (!file.is_open())
    {
        g_Config.ResetToDefaults();
        return false;
    }

    try
    {
        json j;
        file >> j;
        if (j.contains("customTimerEnabled")) g_Config.customTimerEnabled = j["customTimerEnabled"].get<bool>();
        if (j.contains("customTimerMinutes"))
        {
            g_Config.customTimerMinutes = j["customTimerMinutes"].get<int>();
            if (g_Config.customTimerMinutes < 1)  g_Config.customTimerMinutes = 1;
            if (g_Config.customTimerMinutes > 16) g_Config.customTimerMinutes = 16;
        }
        if (j.contains("freezeTimer"))        g_Config.freezeTimer        = j["freezeTimer"].get<bool>();
        if (j.contains("stuntMultiplier"))   g_Config.stuntMultiplier   = j["stuntMultiplier"].get<float>();
        if (j.contains("camOffsetX"))        g_Config.camOffsetX        = j["camOffsetX"].get<float>();
        if (j.contains("camOffsetY"))        g_Config.camOffsetY        = j["camOffsetY"].get<float>();
        if (j.contains("camOffsetZ"))        g_Config.camOffsetZ        = j["camOffsetZ"].get<float>();
        if (j.contains("camRotPitch"))       g_Config.camRotPitch       = j["camRotPitch"].get<float>();
        if (j.contains("camRotYaw"))         g_Config.camRotYaw         = j["camRotYaw"].get<float>();
        if (j.contains("camRotRoll"))        g_Config.camRotRoll        = j["camRotRoll"].get<float>();

        if (j.contains("flyMode"))           g_Config.flyMode           = j["flyMode"].get<bool>();
        if (j.contains("flySpeed"))          g_Config.flySpeed          = j["flySpeed"].get<float>();
        if (j.contains("disableTrafficCollision")) g_Config.disableTrafficCollision = j["disableTrafficCollision"].get<bool>();

        if (j.contains("freecamEnabled"))    g_Config.freecamEnabled    = j["freecamEnabled"].get<bool>();
        // Freecam & Mouse Look inversion (permanently set to natural control feel)
        g_Config.freecamInvertPitch   = true;
        g_Config.freecamInvertYaw     = true;
        g_Config.freecamInvertWASD    = true;
        g_Config.mouseLookInvertPitch = false;
        g_Config.mouseLookInvertYaw   = true;

        if (j.contains("freecamSpeed"))         g_Config.freecamSpeed         = j["freecamSpeed"].get<float>();
        if (j.contains("freecamPitch"))         g_Config.freecamPitch         = j["freecamPitch"].get<float>();
        if (j.contains("freecamYaw"))           g_Config.freecamYaw           = j["freecamYaw"].get<float>();
        if (j.contains("freecamRoll"))          g_Config.freecamRoll          = j["freecamRoll"].get<float>();

        if (j.contains("mouseLookEnabled"))     g_Config.mouseLookEnabled     = j["mouseLookEnabled"].get<bool>();
        if (j.contains("mouseLookSensitivity")) g_Config.mouseLookSensitivity = j["mouseLookSensitivity"].get<float>();

        if (j.contains("showSpeedometer"))      g_Config.showSpeedometer      = j["showSpeedometer"].get<bool>();
        if (j.contains("odometerTotalKm"))      g_Config.odometerTotalKm      = j["odometerTotalKm"].get<double>();
        if (j.contains("odometerTripKm"))       g_Config.odometerTripKm       = j["odometerTripKm"].get<double>();
        if (j.contains("feminineGuiMode"))      g_Config.feminineGuiMode      = j["feminineGuiMode"].get<bool>();

        if (j.contains("menuWindowWidth"))      g_Config.menuWindowWidth      = j["menuWindowWidth"].get<float>();
        if (j.contains("menuWindowHeight"))     g_Config.menuWindowHeight     = j["menuWindowHeight"].get<float>();
        if (g_Config.menuWindowWidth < 460.0f)  g_Config.menuWindowWidth      = 460.0f;
        if (g_Config.menuWindowHeight < 300.0f) g_Config.menuWindowHeight     = 300.0f;

        if (j.contains("worldDistanceMultiplier"))     g_Config.worldDistanceMultiplier     = j["worldDistanceMultiplier"].get<float>();
        if (j.contains("renderDistanceMultiplier"))    g_Config.renderDistanceMultiplier    = j["renderDistanceMultiplier"].get<float>();
        if (j.contains("persistentChunkLoader"))       g_Config.persistentChunkLoader       = j["persistentChunkLoader"].get<bool>();
        if (j.contains("chunkLoadDistanceMultiplier")) g_Config.chunkLoadDistanceMultiplier = j["chunkLoadDistanceMultiplier"].get<float>();
        if (j.contains("useOriginalSoundtrack"))       g_Config.useOriginalSoundtrack       = j["useOriginalSoundtrack"].get<bool>();

        if (j.contains("enableSpeedMultiplier")) g_Config.enableSpeedMultiplier = j["enableSpeedMultiplier"].get<bool>();
        if (j.contains("speedMultiplier"))       g_Config.speedMultiplier       = j["speedMultiplier"].get<float>();

        // Clamp speed multiplier to safe max 1.5x
        if (g_Config.speedMultiplier > 1.50f) g_Config.speedMultiplier = 1.50f;
        if (g_Config.speedMultiplier < 1.00f) g_Config.speedMultiplier = 1.00f;

        if (j.contains("nitroEnabled"))      g_Config.nitroEnabled      = j["nitroEnabled"].get<bool>();
        if (j.contains("steeringMode"))      g_Config.steeringMode      = j["steeringMode"].get<int>();
        if (g_Config.steeringMode < 0 || g_Config.steeringMode > 3) g_Config.steeringMode = 0;

        if (j.contains("segaUnlockAll"))     g_Config.segaUnlockAll     = j["segaUnlockAll"].get<bool>();
        if (j.contains("segaAnotherDay"))    g_Config.segaAnotherDay    = j["segaAnotherDay"].get<bool>();
        if (j.contains("segaNoArrows"))      g_Config.segaNoArrows      = j["segaNoArrows"].get<bool>();
        if (j.contains("segaNoDestMark"))    g_Config.segaNoDestMark    = j["segaNoDestMark"].get<bool>();
        if (j.contains("segaBikeMode"))      g_Config.segaBikeMode      = j["segaBikeMode"].get<bool>();
        if (j.contains("unlockAllVehicles")) g_Config.unlockAllVehicles = j["unlockAllVehicles"].get<bool>();

        if (j.contains("dcRestorationEnabled")) g_Config.dcRestorationEnabled = j["dcRestorationEnabled"].get<bool>();
        if (j.contains("dcOriginalNames"))      g_Config.dcOriginalNames      = j["dcOriginalNames"].get<bool>();
        if (j.contains("dcFilaDestination"))    g_Config.dcFilaDestination    = j["dcFilaDestination"].get<bool>();
        if (j.contains("dcDreamcastModels"))    g_Config.dcDreamcastModels    = j["dcDreamcastModels"].get<bool>();
        if (j.contains("dcOriginalVoices"))     g_Config.dcOriginalVoices     = j["dcOriginalVoices"].get<bool>();
        if (j.contains("dcOriginalTextures"))   g_Config.dcOriginalTextures   = j["dcOriginalTextures"].get<bool>();
        if (j.contains("dcLogosZBias"))         g_Config.dcLogosZBias         = j["dcLogosZBias"].get<bool>();
        if (j.contains("engineFixesEnabled"))        g_Config.engineFixesEnabled        = j["engineFixesEnabled"].get<bool>();
        else if (j.contains("silentPatchEnabled"))   g_Config.engineFixesEnabled        = j["silentPatchEnabled"].get<bool>();
        if (j.contains("windowAspectRatioFix"))      g_Config.windowAspectRatioFix      = j["windowAspectRatioFix"].get<bool>();
        else if (j.contains("silentPatchWindowFix")) g_Config.windowAspectRatioFix      = j["silentPatchWindowFix"].get<bool>();
        if (j.contains("steeringWheelCrashFix"))     g_Config.steeringWheelCrashFix     = j["steeringWheelCrashFix"].get<bool>();
        else if (j.contains("silentPatchAnalogFix")) g_Config.steeringWheelCrashFix     = j["silentPatchAnalogFix"].get<bool>();

        if (j.contains("analogDeadzoneFix"))         g_Config.analogDeadzoneFix         = j["analogDeadzoneFix"].get<bool>();
        else if (j.contains("silentPatchAnalogFix")) g_Config.analogDeadzoneFix         = j["silentPatchAnalogFix"].get<bool>();
        if (j.contains("altF4ExitFix"))              g_Config.altF4ExitFix              = j["altF4ExitFix"].get<bool>();
        else if (j.contains("silentPatchAltF4Fix"))  g_Config.altF4ExitFix              = j["silentPatchAltF4Fix"].get<bool>();
        if (j.contains("debugCameraHotkeys"))        g_Config.debugCameraHotkeys        = j["debugCameraHotkeys"].get<bool>();
        else if (j.contains("silentPatchHotkeys"))   g_Config.debugCameraHotkeys        = j["silentPatchHotkeys"].get<bool>();

        if (j.contains("ffbEnabled"))           g_Config.ffbEnabled           = j["ffbEnabled"].get<bool>();
        if (j.contains("ffbStrength"))          g_Config.ffbStrength          = j["ffbStrength"].get<float>();
        if (j.contains("ffbGroundEffects"))     g_Config.ffbGroundEffects     = j["ffbGroundEffects"].get<bool>();
        if (j.contains("ffbEngineRumble"))      g_Config.ffbEngineRumble      = j["ffbEngineRumble"].get<bool>();
        if (j.contains("ffbCollisionEffects"))  g_Config.ffbCollisionEffects  = j["ffbCollisionEffects"].get<bool>();
        if (j.contains("enableDXVK"))           g_Config.enableDXVK           = j["enableDXVK"].get<bool>();

        if (j.contains("controlMode"))       g_Config.controlMode       = j["controlMode"].get<int>();
        if (j.contains("keyForward"))        g_Config.keyForward        = j["keyForward"].get<int>();
        if (j.contains("keyBackward"))       g_Config.keyBackward       = j["keyBackward"].get<int>();
        if (j.contains("keyLeft"))           g_Config.keyLeft           = j["keyLeft"].get<int>();
        if (j.contains("keyRight"))          g_Config.keyRight          = j["keyRight"].get<int>();
        if (j.contains("keyBrake"))          g_Config.keyBrake          = j["keyBrake"].get<int>();
        if (j.contains("keyGearDrive"))      g_Config.keyGearDrive      = j["keyGearDrive"].get<int>();
        if (j.contains("keyGearReverse"))    g_Config.keyGearReverse    = j["keyGearReverse"].get<int>();

        if (j.contains("autoDemo"))          g_Config.autoDemo          = j["autoDemo"].get<bool>();

        // Transient cheat and streaming states must NEVER persist across game sessions!
        g_Config.freecamEnabled        = false;
        g_Config.flyMode               = false;
        g_Config.customTimerEnabled    = false;
        g_Config.freezeTimer           = false;
        g_Config.enableSpeedMultiplier = false;
        g_Config.persistentChunkLoader = false;
        return true;
    }
    catch (...)
    {
        g_Config.ResetToDefaults();
        g_Config.freecamEnabled        = false;
        g_Config.flyMode               = false;
        g_Config.customTimerEnabled    = false;
        g_Config.freezeTimer           = false;
        g_Config.enableSpeedMultiplier = false;
        g_Config.persistentChunkLoader = false;
        return false;
    }
}

bool SaveConfig()
{
    std::string path = GetConfigPath();
    std::ofstream file(path);
    if (!file.is_open())
        return false;

    try
    {
        json j;
        // Transient cheat states must NEVER persist across game sessions!
        j["customTimerEnabled"] = false;
        j["customTimerMinutes"] = g_Config.customTimerMinutes;
        j["freezeTimer"]        = false;
        j["stuntMultiplier"]   = g_Config.stuntMultiplier;
        j["camOffsetX"]        = g_Config.camOffsetX;
        j["camOffsetY"]        = g_Config.camOffsetY;
        j["camOffsetZ"]        = g_Config.camOffsetZ;
        j["camRotPitch"]       = g_Config.camRotPitch;
        j["camRotYaw"]         = g_Config.camRotYaw;
        j["camRotRoll"]        = g_Config.camRotRoll;

        j["flyMode"]           = false; // Never persist fly mode active
        j["flySpeed"]          = g_Config.flySpeed;
        j["disableTrafficCollision"] = g_Config.disableTrafficCollision;

        // Freecam: Never persist freecam across sessions, keep natural inversion active
        j["freecamEnabled"]     = false;
        j["freecamSpeed"]       = g_Config.freecamSpeed;
        j["freecamPitch"]       = g_Config.freecamPitch;
        j["freecamYaw"]         = g_Config.freecamYaw;
        j["freecamRoll"]        = g_Config.freecamRoll;
        j["freecamInvertPitch"] = true;
        j["freecamInvertYaw"]   = true;
        j["freecamInvertWASD"]  = true;

        j["mouseLookEnabled"]      = g_Config.mouseLookEnabled;
        j["mouseLookInvertPitch"]  = false;
        j["mouseLookInvertYaw"]    = true;
        j["mouseLookSensitivity"]  = g_Config.mouseLookSensitivity;

        j["showSpeedometer"]       = g_Config.showSpeedometer;
        j["odometerTotalKm"]       = g_Config.odometerTotalKm;
        j["odometerTripKm"]        = g_Config.odometerTripKm;
        j["feminineGuiMode"]       = g_Config.feminineGuiMode;
        j["menuWindowWidth"]       = g_Config.menuWindowWidth;
        j["menuWindowHeight"]      = g_Config.menuWindowHeight;


        j["worldDistanceMultiplier"]     = g_Config.worldDistanceMultiplier;
        j["renderDistanceMultiplier"]    = g_Config.worldDistanceMultiplier; // Keep in sync
        j["persistentChunkLoader"]       = false; // Never persist extended chunk loader across game sessions!
        j["chunkLoadDistanceMultiplier"] = g_Config.worldDistanceMultiplier; // Keep in sync
        j["useOriginalSoundtrack"]       = g_Config.useOriginalSoundtrack;

        j["enableSpeedMultiplier"] = false; // Never persist speed boost active
        j["speedMultiplier"]       = (g_Config.speedMultiplier > 1.50f ? 1.50f : (g_Config.speedMultiplier < 1.00f ? 1.00f : g_Config.speedMultiplier));

        j["nitroEnabled"]      = g_Config.nitroEnabled;
        j["steeringMode"]      = g_Config.steeringMode;


        j["segaUnlockAll"]     = g_Config.segaUnlockAll;
        j["segaAnotherDay"]    = g_Config.segaAnotherDay;
        j["segaNoArrows"]      = g_Config.segaNoArrows;
        j["segaNoDestMark"]    = g_Config.segaNoDestMark;
        j["segaBikeMode"]      = g_Config.segaBikeMode;
        j["unlockAllVehicles"] = g_Config.unlockAllVehicles;

        j["dcRestorationEnabled"] = g_Config.dcRestorationEnabled;
        j["dcOriginalNames"]      = g_Config.dcOriginalNames;
        j["dcFilaDestination"]    = g_Config.dcFilaDestination;
        j["dcDreamcastModels"]    = g_Config.dcDreamcastModels;
        j["dcOriginalVoices"]     = g_Config.dcOriginalVoices;
        j["dcOriginalTextures"]   = g_Config.dcOriginalTextures;
        j["dcLogosZBias"]         = g_Config.dcLogosZBias;
        j["engineFixesEnabled"]    = g_Config.engineFixesEnabled;
        j["windowAspectRatioFix"]  = g_Config.windowAspectRatioFix;
        j["steeringWheelCrashFix"] = g_Config.steeringWheelCrashFix;
        j["analogDeadzoneFix"]     = g_Config.analogDeadzoneFix;
        j["altF4ExitFix"]          = g_Config.altF4ExitFix;
        j["debugCameraHotkeys"]    = g_Config.debugCameraHotkeys;

        j["ffbEnabled"]           = g_Config.ffbEnabled;
        j["ffbStrength"]          = g_Config.ffbStrength;
        j["ffbGroundEffects"]     = g_Config.ffbGroundEffects;
        j["ffbEngineRumble"]      = g_Config.ffbEngineRumble;
        j["ffbCollisionEffects"]  = g_Config.ffbCollisionEffects;
        j["enableDXVK"]          = g_Config.enableDXVK;

        j["controlMode"]       = g_Config.controlMode;
        j["keyForward"]        = g_Config.keyForward;
        j["keyBackward"]       = g_Config.keyBackward;
        j["keyLeft"]           = g_Config.keyLeft;
        j["keyRight"]          = g_Config.keyRight;
        j["keyBrake"]          = g_Config.keyBrake;
        j["keyGearDrive"]      = g_Config.keyGearDrive;
        j["keyGearReverse"]    = g_Config.keyGearReverse;

        j["autoDemo"]          = g_Config.autoDemo;
        file << j.dump(4);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

const char* GetKeyNameString(int vkCode)
{
    switch (vkCode)
    {
    case VK_LBUTTON:   return "Mouse Left";
    case VK_RBUTTON:   return "Mouse Right";
    case VK_CANCEL:    return "Cancel";
    case VK_MBUTTON:   return "Mouse Middle";
    case VK_BACK:      return "Backspace";
    case VK_TAB:       return "Tab";
    case VK_CLEAR:     return "Clear";
    case VK_RETURN:    return "Enter";
    case VK_SHIFT:     return "Shift";
    case VK_LSHIFT:    return "Left Shift";
    case VK_RSHIFT:    return "Right Shift";
    case VK_CONTROL:   return "Ctrl";
    case VK_LCONTROL:  return "Left Ctrl";
    case VK_RCONTROL:  return "Right Ctrl";
    case VK_MENU:      return "Alt";
    case VK_LMENU:     return "Left Alt";
    case VK_RMENU:     return "Right Alt";
    case VK_PAUSE:     return "Pause";
    case VK_CAPITAL:   return "Caps Lock";
    case VK_ESCAPE:    return "Esc";
    case VK_SPACE:     return "Space";
    case VK_PRIOR:     return "Page Up";
    case VK_NEXT:      return "Page Down";
    case VK_END:       return "End";
    case VK_HOME:      return "Home";
    case VK_LEFT:      return "Left Arrow";
    case VK_UP:        return "Up Arrow";
    case VK_RIGHT:     return "Right Arrow";
    case VK_DOWN:      return "Down Arrow";
    case VK_SELECT:    return "Select";
    case VK_PRINT:     return "Print";
    case VK_EXECUTE:   return "Execute";
    case VK_SNAPSHOT:  return "Print Screen";
    case VK_INSERT:    return "Insert";
    case VK_DELETE:    return "Delete";
    case VK_HELP:      return "Help";
    case '0':          return "0";
    case '1':          return "1";
    case '2':          return "2";
    case '3':          return "3";
    case '4':          return "4";
    case '5':          return "5";
    case '6':          return "6";
    case '7':          return "7";
    case '8':          return "8";
    case '9':          return "9";
    case 'A':          return "A";
    case 'B':          return "B";
    case 'C':          return "C";
    case 'D':          return "D";
    case 'E':          return "E";
    case 'F':          return "F";
    case 'G':          return "G";
    case 'H':          return "H";
    case 'I':          return "I";
    case 'J':          return "J";
    case 'K':          return "K";
    case 'L':          return "L";
    case 'M':          return "M";
    case 'N':          return "N";
    case 'O':          return "O";
    case 'P':          return "P";
    case 'Q':          return "Q";
    case 'R':          return "R";
    case 'S':          return "S";
    case 'T':          return "T";
    case 'U':          return "U";
    case 'V':          return "V";
    case 'W':          return "W";
    case 'X':          return "X";
    case 'Y':          return "Y";
    case 'Z':          return "Z";
    case VK_NUMPAD0:   return "Numpad 0";
    case VK_NUMPAD1:   return "Numpad 1";
    case VK_NUMPAD2:   return "Numpad 2";
    case VK_NUMPAD3:   return "Numpad 3";
    case VK_NUMPAD4:   return "Numpad 4";
    case VK_NUMPAD5:   return "Numpad 5";
    case VK_NUMPAD6:   return "Numpad 6";
    case VK_NUMPAD7:   return "Numpad 7";
    case VK_NUMPAD8:   return "Numpad 8";
    case VK_NUMPAD9:   return "Numpad 9";
    case VK_MULTIPLY:  return "Numpad *";
    case VK_ADD:       return "Numpad +";
    case VK_SEPARATOR: return "Numpad Sep";
    case VK_SUBTRACT:  return "Numpad -";
    case VK_DECIMAL:   return "Numpad .";
    case VK_DIVIDE:    return "Numpad /";
    case VK_F1:        return "F1";
    case VK_F2:        return "F2";
    case VK_F3:        return "F3";
    case VK_F4:        return "F4";
    case VK_F5:        return "F5";
    case VK_F6:        return "F6";
    case VK_F7:        return "F7";
    case VK_F8:        return "F8";
    case VK_F9:        return "F9";
    case VK_F10:       return "F10";
    case VK_F11:       return "F11";
    case VK_F12:       return "F12";
    default:
        static char buf[32];
        sprintf_s(buf, "Key 0x%02X", vkCode);
        return buf;
    }
}
