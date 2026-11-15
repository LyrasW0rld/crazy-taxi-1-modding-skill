#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <windows.h>

struct ModelRegistryEntry
{
    std::string name;
    std::string category;
    size_t offset;
    size_t maxBytes;
    std::string binFile; // typically "polDC0.bin"
};

struct CustomModelStats
{
    int totalScanned = 0;
    int totalApplied = 0;
    int totalErrors = 0;
    std::string lastStatus;
};

class ModelEngine
{
public:
    static ModelEngine& Instance();

    bool Initialize(const std::string& modDir, const std::string& gameDir);
    void Shutdown();

    // Scans for custom .obj files and compiles/injects them to RAM and disk
    bool Reload3DModels();

    // Restores original models from vanilla backup (.bak)
    bool RestoreOriginal3DModels();

    // Status & Configuration
    bool IsEnabled() const { return m_Enabled; }
    void SetEnabled(bool enabled);
    bool IsAutoRescaleEnabled() const { return m_AutoRescale; }
    void SetAutoRescale(bool enable) { m_AutoRescale = enable; }
    const CustomModelStats& GetStats() const { return m_Stats; }

    const std::vector<ModelRegistryEntry>& GetRegistry() const { return m_Registry; }
    const ModelRegistryEntry* FindRegistryEntry(const std::string& name) const;

private:
    ModelEngine();
    ~ModelEngine();

    void InitRegistry();
    bool EnsureVanillaBackup(const std::string& binPath);
    void LoadExtractedManifest();

    bool CompileAndInjectObj(
        const std::string& objPath,
        const ModelRegistryEntry& entry,
        const std::vector<uint8_t>& vanillaData
    );

    struct VanillaFileInfo {
        DWORD size = 0;
        DWORD timeLow = 0;
        DWORD timeHigh = 0;
    };

    std::string m_ModDir;
    std::string m_GameDir;
    bool m_Initialized = false;
    bool m_Enabled = true;
    bool m_AutoRescale = true;
    std::string m_AxisUp = "auto";
    int m_QuickReload3DKey = 119; // F8 default

    std::vector<ModelRegistryEntry> m_Registry;
    std::unordered_map<std::string, size_t> m_NameToRegistryIdx;
    std::unordered_map<std::string, FILETIME> m_FileTimestamps;
    std::unordered_map<std::string, VanillaFileInfo> m_VanillaManifest;
    CustomModelStats m_Stats;
};

