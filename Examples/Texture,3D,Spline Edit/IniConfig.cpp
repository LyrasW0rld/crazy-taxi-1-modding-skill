#include "IniConfig.h"
#include <windows.h>
#include <stdio.h>

void IniConfig::CreateDefaultIfMissing(const std::string& iniPath)
{
    DWORD attr = GetFileAttributesA(iniPath.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES)
        return; // File exists

    FILE* fp = fopen(iniPath.c_str(), "w");
    if (fp)
    {
        fprintf(fp, "[General]\n");
        fprintf(fp, "Enabled=true\n");
        fprintf(fp, "AutoReloadOnToggle=true\n");
        fprintf(fp, "DumpNewRuntimeTextures=false\n");
        fprintf(fp, "ForceHighResSupport=true\n");
        fprintf(fp, "ForceHDAspectAlignment=true ; Automatically resample HD textures to preserve 3D model aspect ratio\n");
        fprintf(fp, "EnableMipmaps=true ; Full hardware mipmap chains and anisotropic filtering for flicker-free 3D\n");
        fprintf(fp, "RestoreOriginals=false ; Set to true to force native original vanilla textures\n");
        fprintf(fp, "ToggleOriginalsKey=121 ; Virtual key code for live toggle (121 = F10, 122 = F11, 120 = F9)\n");
        fprintf(fp, "QuickReloadKey=120 ; Virtual key code for quick texture reload (120 = F9)\n");
        fprintf(fp, "LogLevel=2 ; 0=Errors Only, 1=Warnings, 2=Info/Debug\n");
        fprintf(fp, "InputDeadzone=25 ; Analog stick deadzone in raw axis units used by the input-mode detection hook\n");
        fclose(fp);
    }
}

bool IniConfig::Load(const std::string& iniPath, ModConfig& outConfig)
{
    CreateDefaultIfMissing(iniPath);

    char buf[128];
    GetPrivateProfileStringA("General", "Enabled", "true", buf, sizeof(buf), iniPath.c_str());
    outConfig.Enabled = (_stricmp(buf, "true") == 0 || _stricmp(buf, "1") == 0);

    GetPrivateProfileStringA("General", "AutoReloadOnToggle", "true", buf, sizeof(buf), iniPath.c_str());
    outConfig.AutoReloadOnToggle = (_stricmp(buf, "true") == 0 || _stricmp(buf, "1") == 0);

    GetPrivateProfileStringA("General", "DumpNewRuntimeTextures", "false", buf, sizeof(buf), iniPath.c_str());
    outConfig.DumpNewRuntimeTextures = (_stricmp(buf, "true") == 0 || _stricmp(buf, "1") == 0);

    GetPrivateProfileStringA("General", "ForceHighResSupport", "true", buf, sizeof(buf), iniPath.c_str());
    outConfig.ForceHighResSupport = (_stricmp(buf, "true") == 0 || _stricmp(buf, "1") == 0);

    GetPrivateProfileStringA("General", "ForceHDAspectAlignment", "true", buf, sizeof(buf), iniPath.c_str());
    outConfig.ForceHDAspectAlignment = (_stricmp(buf, "true") == 0 || _stricmp(buf, "1") == 0);

    GetPrivateProfileStringA("General", "EnableMipmaps", "true", buf, sizeof(buf), iniPath.c_str());
    outConfig.EnableMipmaps = (_stricmp(buf, "true") == 0 || _stricmp(buf, "1") == 0);

    GetPrivateProfileStringA("General", "RestoreOriginals", "false", buf, sizeof(buf), iniPath.c_str());
    outConfig.RestoreOriginals = (_stricmp(buf, "true") == 0 || _stricmp(buf, "1") == 0);

    outConfig.ToggleOriginalsKey = GetPrivateProfileIntA("General", "ToggleOriginalsKey", 121 /* VK_F10 */, iniPath.c_str());
    outConfig.QuickReloadKey = GetPrivateProfileIntA("General", "QuickReloadKey", 120 /* VK_F9 */, iniPath.c_str());
    outConfig.LogLevel = GetPrivateProfileIntA("General", "LogLevel", 2, iniPath.c_str());

    return true;
}

bool IniConfig::Save(const std::string& iniPath, const ModConfig& config)
{
    WritePrivateProfileStringA("General", "Enabled", config.Enabled ? "true" : "false", iniPath.c_str());
    WritePrivateProfileStringA("General", "AutoReloadOnToggle", config.AutoReloadOnToggle ? "true" : "false", iniPath.c_str());
    WritePrivateProfileStringA("General", "DumpNewRuntimeTextures", config.DumpNewRuntimeTextures ? "true" : "false", iniPath.c_str());
    WritePrivateProfileStringA("General", "ForceHighResSupport", config.ForceHighResSupport ? "true" : "false", iniPath.c_str());
    WritePrivateProfileStringA("General", "ForceHDAspectAlignment", config.ForceHDAspectAlignment ? "true" : "false", iniPath.c_str());
    WritePrivateProfileStringA("General", "EnableMipmaps", config.EnableMipmaps ? "true" : "false", iniPath.c_str());
    WritePrivateProfileStringA("General", "RestoreOriginals", config.RestoreOriginals ? "true" : "false", iniPath.c_str());

    char keyBuf[32];
    sprintf_s(keyBuf, "%d", config.ToggleOriginalsKey);
    WritePrivateProfileStringA("General", "ToggleOriginalsKey", keyBuf, iniPath.c_str());

    sprintf_s(keyBuf, "%d", config.QuickReloadKey);
    WritePrivateProfileStringA("General", "QuickReloadKey", keyBuf, iniPath.c_str());

    char levelBuf[32];
    sprintf_s(levelBuf, "%d", config.LogLevel);
    WritePrivateProfileStringA("General", "LogLevel", levelBuf, iniPath.c_str());

    return true;
}
