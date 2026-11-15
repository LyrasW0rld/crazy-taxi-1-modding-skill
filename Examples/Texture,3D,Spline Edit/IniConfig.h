#pragma once
#include <string>

struct ModConfig
{
    bool Enabled = true;
    bool AutoReloadOnToggle = true;
    bool DumpNewRuntimeTextures = false;
    bool ForceHighResSupport = true;
    bool ForceHDAspectAlignment = true;
    bool EnableMipmaps = true;
    bool RestoreOriginals = false;
    int ToggleOriginalsKey = 0x79; // VK_F10
    int QuickReloadKey = 0x78;     // VK_F9
    int LogLevel = 2;
};

class IniConfig
{
public:
    static bool Load(const std::string& iniPath, ModConfig& outConfig);
    static bool Save(const std::string& iniPath, const ModConfig& config);
    static void CreateDefaultIfMissing(const std::string& iniPath);
};
