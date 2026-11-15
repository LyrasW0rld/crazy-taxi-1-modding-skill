#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlwapi.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <fstream>
#include <sstream>
#include <cmath>
#include <algorithm>
#include <regex>
#include "ModelEngine.h"
#include "Logger.h"

#pragma comment(lib, "shlwapi.lib")

namespace
{

// Dynamic Dreamcast VRAM lookup for PC Crazy Taxi:
// On PC, the game allocates emulated VRAM in the heap and stores the base pointer at 0x01B7A6A8.
// polDC0.bin is located at offset 0x85000 (0x0C390000 - 0x0C30B000) within this emulated VRAM.
static uintptr_t GetPolDc0RamBase()
{
    __try {
        uintptr_t vramBase = *(uintptr_t*)0x01B7A6A8;
        if (vramBase != 0 && vramBase > 0x10000 && vramBase < 0x7FFE0000)
        {
            return vramBase + 0x85000;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return 0;
}

struct Vec3
{
    float x = 0.0f, y = 0.0f, z = 0.0f;
};

struct Vec2
{
    float u = 0.0f, v = 0.0f;
};

struct ObjVertex
{
    float x, y, z;
    float nx, ny, nz;
    float u, v;
};

struct ObjTriangle
{
    int i0, i1, i2;
};

struct ObjGroup
{
    std::string name;
    std::string mapKd;
    std::vector<ObjVertex> vertices;
    std::vector<ObjTriangle> triangles;
};

// One triangle strip: sequential vertex list. The game has no index buffer
// and renders consecutive triples of the strip (with automatic per-triangle
// cull alternation), so section blobs must be strip-encoded.
struct TriStrip
{
    std::vector<uint32_t> verts;
};

static std::string ToLower(const std::string& str)
{
    std::string s = str;
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return s;
}

static bool FileExists(const std::string& path)
{
    DWORD attr = GetFileAttributesA(path.c_str());
    return (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY));
}

static std::vector<uint8_t> ReadBinaryFile(const std::string& path)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.is_open()) return {};
    size_t sz = (size_t)f.tellg();
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> buf(sz);
    f.read((char*)buf.data(), sz);
    return buf;
}

static bool WriteBinaryFile(const std::string& path, const void* data, size_t size)
{
    std::ofstream f(path, std::ios::binary);
    if (!f.is_open()) return false;
    f.write((const char*)data, size);
    return true;
}

static int ParseTextureIndexFromMaterial(const std::string& matName, const std::string& mapKd)
{
    static const std::regex reMat("texDC0[_/\\s-]*(\\d+)", std::regex::icase);
    static const std::regex reMap("__texDC0__(\\d+)__", std::regex::icase);
    std::smatch m;

    if (std::regex_search(matName, m, reMat))
    {
        return std::stoi(m[1].str());
    }

    if (!mapKd.empty())
    {
        std::string fn = mapKd.substr(mapKd.find_last_of("\\/") + 1);
        if (std::regex_search(fn, m, reMap))
        {
            return std::stoi(m[1].str());
        }
    }

    // Try pure integer string
    try {
        if (!matName.empty() && std::all_of(matName.begin(), matName.end(), ::isdigit))
        {
            return std::stoi(matName);
        }
    } catch (...) {}

    return -1;
}

// Rebuild triangle strips from a triangle list for strip-encoded section
// blobs. Storing every triangle as 3 vertices (triangle list) duplicates the
// shared strip vertices up to 3x (3*T instead of T + 2*S vertices) and the
// game, lacking an index buffer, would still draw the section as a strip and
// misread the topology.
// Chaining rule: a triangle continues the current strip iff it contains the
// last two emitted vertices plus exactly one new vertex (one vertex appended
// per step). Extraction writes OBJ faces in original blob order, so for an
// unmodified round-trip this reproduces the original strip layout and its
// vertex count exactly; the worst case (no chaining) degenerates to the old
// triangle-list size, so the blob never grows.
// Known limitation: chaining matches the vertex set of the current edge only,
// not face orientation. Unmodified round-trips stay exact (faces follow the
// original blob order), but a DCC re-export with consistent winding gets
// alternating strip triangles inverted (the game alternates culling per strip
// triangle). The previous list emission was broken differently (list fmt +
// tripled vertices), so this is no regression; verify orientation in-game if
// such models render culled.
static std::vector<TriStrip> BuildTriStrips(const std::vector<ObjTriangle>& tris)
{
    std::vector<TriStrip> strips;
    for (const auto& tri : tris)
    {
        bool appended = false;
        if (!strips.empty())
        {
            TriStrip& cur = strips.back();
            const size_t n = cur.verts.size();
            const int a = (int)cur.verts[n - 2];
            const int b = (int)cur.verts[n - 1];
            const int tv[3] = { tri.i0, tri.i1, tri.i2 };
            bool hasA = false, hasB = false;
            int c = -1;
            for (int v : tv)
            {
                if (v == a) hasA = true;
                else if (v == b) hasB = true;
                else c = v;
            }
            if (hasA && hasB && c >= 0)
            {
                cur.verts.push_back((uint32_t)c);
                appended = true;
            }
        }
        if (!appended)
        {
            TriStrip s;
            s.verts.push_back((uint32_t)tri.i0);
            s.verts.push_back((uint32_t)tri.i1);
            s.verts.push_back((uint32_t)tri.i2);
            strips.push_back(std::move(s));
        }
    }
    return strips;
}

} // namespace

ModelEngine& ModelEngine::Instance()
{
    static ModelEngine s_Instance;
    return s_Instance;
}

ModelEngine::ModelEngine()
{
    InitRegistry();
}

ModelEngine::~ModelEngine()
{
}

void ModelEngine::InitRegistry()
{
    m_Registry.clear();
    m_NameToRegistryIdx.clear();

    auto AddEntry = [&](const std::string& name, size_t off, size_t maxB, const std::string& cat) {
        ModelRegistryEntry e;
        e.name = name;
        e.offset = off;
        e.maxBytes = maxB;
        e.category = cat;
        e.binFile = "polDC0.bin";
        size_t idx = m_Registry.size();
        m_Registry.push_back(e);
        m_NameToRegistryIdx[ToLower(name)] = idx;
    };

    // Common aliases for ease of use
    auto AddAlias = [&](const std::string& alias, const std::string& target) {
        auto it = m_NameToRegistryIdx.find(ToLower(target));
        if (it != m_NameToRegistryIdx.end()) {
            m_NameToRegistryIdx[ToLower(alias)] = it->second;
        }
    };

    // --- Player Taxis ---
    AddEntry("Axel_Taxi_Body", 0x000038E8, 56580, "Player_Taxi");
    AddEntry("Axel_Taxi_Wheel_FL", 0x000115F0, 3916, "Player_Taxi");
    AddEntry("Axel_Taxi_Wheel_FR", 0x00012540, 4012, "Player_Taxi");
    AddEntry("Axel_Taxi_Wheel_RL", 0x000134F0, 3916, "Player_Taxi");
    AddEntry("Axel_Taxi_Wheel_RR", 0x00014440, 4012, "Player_Taxi");
    AddEntry("Axel_Taxi_SteeringWheel", 0x00021220, 13476, "Player_Taxi");
    AddEntry("Axel_Taxi_Driver", 0x00027C10, 22820, "Player_Taxi");
    AddEntry("Axel_Taxi_Mirror_L", 0x00020F20, 380, "Player_Taxi");
    AddEntry("Axel_Taxi_Mirror_R", 0x000210A0, 380, "Player_Taxi");
    AddEntry("BDJoe_Taxi_Body", 0x00030CE8, 75252, "Player_Taxi");
    // Body_Alt budgets: maxBytes must end at the next real chunk (the Driver
    // below) — the writer zero-pads up to maxBytes (CompileAndInjectObj), so a
    // larger value would overwrite the Driver chunk with zeros.
    AddEntry("BDJoe_Taxi_Body_Alt", 0x000432E0, 24224, "Player_Taxi");
    AddEntry("BDJoe_Taxi_SteeringWheel", 0x0004E628, 6044, "Player_Taxi");
    AddEntry("BDJoe_Taxi_Driver", 0x00049180, 20900, "Player_Taxi");
    AddEntry("BDJoe_Taxi_Mirror_L", 0x0004E328, 380, "Player_Taxi");
    AddEntry("BDJoe_Taxi_Mirror_R", 0x0004E4A8, 380, "Player_Taxi");
    AddEntry("Gus_Taxi_Body", 0x00051568, 58628, "Player_Taxi");
    AddEntry("Gus_Taxi_Body_Alt", 0x00063FE0, 17232, "Player_Taxi");
    AddEntry("Gus_Taxi_Wheel_FL", 0x0005FA70, 3700, "Player_Taxi");
    AddEntry("Gus_Taxi_Wheel_FR", 0x000608E8, 3796, "Player_Taxi");
    AddEntry("Gus_Taxi_Wheel_RL", 0x000617C0, 3700, "Player_Taxi");
    AddEntry("Gus_Taxi_Wheel_RR", 0x00062638, 3796, "Player_Taxi");
    AddEntry("Gus_Taxi_SteeringWheel", 0x0006D880, 3484, "Player_Taxi");
    AddEntry("Gus_Taxi_Driver", 0x00068330, 18300, "Player_Taxi");
    AddEntry("Gus_Taxi_Mirror_L", 0x0006D580, 380, "Player_Taxi");
    AddEntry("Gus_Taxi_Mirror_R", 0x0006D700, 380, "Player_Taxi");
    AddEntry("Gena_Taxi_Body", 0x0006F3C0, 67468, "Player_Taxi");
    AddEntry("Gena_Taxi_Body_Alt", 0x00084120, 25952, "Player_Taxi");
    AddEntry("Gena_Taxi_Wheel_FL", 0x0007FB50, 3700, "Player_Taxi");
    AddEntry("Gena_Taxi_Wheel_FR", 0x000809C8, 3796, "Player_Taxi");
    AddEntry("Gena_Taxi_Wheel_RL", 0x000818A0, 3700, "Player_Taxi");
    AddEntry("Gena_Taxi_Wheel_RR", 0x00082718, 3796, "Player_Taxi");
    AddEntry("Gena_Taxi_SteeringWheel", 0x00091D18, 444, "Player_Taxi");
    AddEntry("Gena_Taxi_Driver", 0x0008A680, 26724, "Player_Taxi");
    AddEntry("Gena_Taxi_Mirror_L", 0x00091A18, 380, "Player_Taxi");
    AddEntry("Gena_Taxi_Mirror_R", 0x00091B98, 380, "Player_Taxi");

    // --- Traffic Vehicles ---
    AddEntry("Traffic_Sedan_Blue", 0x000B1318, 7068, "Traffic");
    AddEntry("Traffic_Sedan_Blue_Wheels", 0x000B36F0, 732, "Traffic");
    AddEntry("Traffic_StationWagon", 0x000AD140, 7140, "Traffic");
    AddEntry("Traffic_StationWagon_Wheels", 0x000AF8F0, 732, "Traffic");
    AddEntry("Traffic_Police_Car", 0x000B5080, 6188, "Traffic");
    AddEntry("Traffic_Police_Car_Wheels", 0x000B71E8, 732, "Traffic");
    AddEntry("Traffic_Van", 0x000A5A48, 5684, "Traffic");
    AddEntry("Traffic_Van_Wheels", 0x000A4238, 1804, "Traffic");
    AddEntry("Traffic_Pickup_Truck", 0x000B8088, 6276, "Traffic");
    AddEntry("Traffic_Pickup_Truck_Wheels", 0x000BA248, 732, "Traffic");
    AddEntry("Traffic_City_Bus", 0x00099AA8, 6644, "Traffic");
    AddEntry("Traffic_City_Bus_Wheels", 0x000980B0, 732, "Traffic");
    AddEntry("Traffic_Sedan_Red", 0x000CF170, 6524, "Traffic");
    AddEntry("Traffic_Sedan_Red_Wheels", 0x000D1458, 732, "Traffic");
    AddEntry("Traffic_Ambulance", 0x000BBF28, 8116, "Traffic");
    AddEntry("Traffic_Ambulance_Wheels", 0x000BE818, 732, "Traffic");
    AddEntry("Traffic_Delivery_Truck", 0x000A1AB0, 6924, "Traffic");
    AddEntry("Traffic_Delivery_Truck_Wheels", 0x000A4238, 1804, "Traffic");
    AddEntry("Traffic_Garbage_Truck", 0x000A9628, 7748, "Traffic");
    AddEntry("Traffic_Garbage_Truck_Wheels", 0x000ABA38, 732, "Traffic");
    AddEntry("Traffic_Highway_Coach", 0x00096848, 5316, "Traffic");
    AddEntry("Traffic_Highway_Coach_Wheels", 0x000980B0, 732, "Traffic");
    AddEntry("Traffic_Cable_Car_Assembled", 0x00161328, 19060, "Traffic");
    AddEntry("Traffic_Cable_Car", 0x00165DA0, 1660, "Traffic");

    // --- Street Props ---
    AddEntry("Prop_City_Bench", 0x00098958, 516, "Prop");
    AddEntry("Prop_Fire_Hydrant", 0x00098B60, 516, "Prop");
    AddEntry("Prop_Street_Lamp", 0x00098D68, 516, "Prop");
    AddEntry("Prop_Traffic_Sign", 0x00098F70, 516, "Prop");

    // --- Individual Character Body Parts ---
    // Axel Parts (1..15)
    AddEntry("Axel1", 0x001D9A18, 3308, "Character");
    AddEntry("Axel2", 0x001DB320, 916, "Character");
    AddEntry("Axel3", 0x001DB6B8, 724, "Character");
    AddEntry("Axel4", 0x001DB990, 1876, "Character");
    AddEntry("Axel5", 0x001DCEF8, 916, "Character");
    AddEntry("Axel6", 0x001DD290, 724, "Character");
    AddEntry("Axel7", 0x001DD568, 1876, "Character");
    AddEntry("Axel8", 0x001DDEB8, 2236, "Character");
    AddEntry("Axel9", 0x001DEFA8, 1804, "Character");
    AddEntry("Axel10", 0x001DF6B8, 2292, "Character");
    AddEntry("Axel11", 0x001DFFB0, 1532, "Character");
    AddEntry("Axel12", 0x001E05B0, 1708, "Character");
    AddEntry("Axel13", 0x001E0C60, 2292, "Character");
    AddEntry("Axel14", 0x001E1558, 1652, "Character");
    AddEntry("Axel15", 0x001E1BD0, 1868, "Character");
    // BDJoe Parts (1..15)
    AddEntry("BDJoe1", 0x001E8A80, 2668, "Character");
    AddEntry("BDJoe2", 0x001EBC48, 1212, "Character");
    AddEntry("BDJoe3", 0x001EC108, 1804, "Character");
    AddEntry("BDJoe4", 0x001EC818, 1708, "Character");
    AddEntry("BDJoe5", 0x001ED2B0, 1212, "Character");
    AddEntry("BDJoe6", 0x001EE508, 1804, "Character");
    AddEntry("BDJoe7", 0x001EEC18, 1708, "Character");
    AddEntry("BDJoe8", 0x001EF6B0, 2516, "Character");
    AddEntry("BDJoe9", 0x001F0088, 2612, "Character");
    AddEntry("BDJoe10", 0x001F0AC0, 1892, "Character");
    AddEntry("BDJoe11", 0x001F1228, 2140, "Character");
    AddEntry("BDJoe12", 0x001F1A88, 2596, "Character");
    AddEntry("BDJoe13", 0x001F24B0, 1868, "Character");
    AddEntry("BDJoe14", 0x001F2C00, 2116, "Character");
    AddEntry("BDJoe15", 0x001F3448, 596, "Character");
    // Gena Parts (1..15)
    AddEntry("Gena1", 0x001061F8, 3748, "Character");
    AddEntry("Gena2", 0x0010C220, 916, "Character");
    AddEntry("Gena3", 0x0010E180, 1364, "Character");
    AddEntry("Gena4", 0x00108170, 1516, "Character");
    AddEntry("Gena5", 0x0010D1D0, 916, "Character");
    AddEntry("Gena6", 0x0010E6D8, 1364, "Character");
    AddEntry("Gena7", 0x00108760, 1516, "Character");
    AddEntry("Gena8", 0x0010A468, 2876, "Character");
    AddEntry("Gena9", 0x00107600, 1460, "Character");
    AddEntry("Gena10", 0x0010B580, 1612, "Character");
    AddEntry("Gena11", 0x00109A88, 1260, "Character");
    AddEntry("Gena12", 0x00107BB8, 1460, "Character");
    AddEntry("Gena13", 0x0010BBD0, 1612, "Character");
    AddEntry("Gena14", 0x00109F78, 1260, "Character");
    AddEntry("Gena15", 0x00108D50, 3132, "Character");
    // Gus Parts (1..8)
    AddEntry("Gus1", 0x001FA510, 3276, "Character");
    AddEntry("Gus2", 0x001FC0D8, 900, "Character");
    AddEntry("Gus3", 0x001FC460, 1412, "Character");
    AddEntry("Gus4", 0x001FD310, 1964, "Character");
    AddEntry("Gus5", 0x001FEA80, 900, "Character");
    AddEntry("Gus6", 0x001FEE08, 1412, "Character");
    AddEntry("Gus7", 0x001FF390, 1924, "Character");
    AddEntry("Gus8", 0x001FFD60, 3964, "Character");

    // --- Seated Drivers ---
    AddEntry("Axel_Driver", 0x00027C10, 22820, "Character");
    AddEntry("BDJoe_Driver", 0x00049180, 20900, "Character");
    AddEntry("Gus_Driver", 0x00068330, 18300, "Character");
    AddEntry("Gena_Driver", 0x0008A680, 26724, "Character");

    // --- Common Aliases ---
    AddAlias("Axel_Taxi", "Axel_Taxi_Body");
    AddAlias("Axel_Body", "Axel_Taxi_Body");
    AddAlias("BDJoe_Taxi", "BDJoe_Taxi_Body");
    AddAlias("BDJoe_Body", "BDJoe_Taxi_Body");
    AddAlias("Gus_Taxi", "Gus_Taxi_Body");
    AddAlias("Gus_Body", "Gus_Taxi_Body");
    AddAlias("Gena_Taxi", "Gena_Taxi_Body");
    AddAlias("Gena_Body", "Gena_Taxi_Body");
    AddAlias("Traffic_Cable_Car_Complete", "Traffic_Cable_Car_Assembled");
    AddAlias("Axel_1", "Axel1");
    AddAlias("Axel_2", "Axel2");
    AddAlias("Axel_3", "Axel3");
    AddAlias("Axel_4", "Axel4");
    AddAlias("Axel_5", "Axel5");
    AddAlias("Axel_6", "Axel6");
    AddAlias("Axel_7", "Axel7");
    AddAlias("Axel_8", "Axel8");
    AddAlias("Axel_9", "Axel9");
    AddAlias("Axel_10", "Axel10");
    AddAlias("Axel_11", "Axel11");
    AddAlias("Axel_12", "Axel12");
    AddAlias("Axel_13", "Axel13");
    AddAlias("Axel_14", "Axel14");
    AddAlias("Axel_15", "Axel15");
    AddAlias("BDJoe_1", "BDJoe1");
    AddAlias("BDJoe_2", "BDJoe2");
    AddAlias("BDJoe_3", "BDJoe3");
    AddAlias("BDJoe_4", "BDJoe4");
    AddAlias("BDJoe_5", "BDJoe5");
    AddAlias("BDJoe_6", "BDJoe6");
    AddAlias("BDJoe_7", "BDJoe7");
    AddAlias("BDJoe_8", "BDJoe8");
    AddAlias("BDJoe_9", "BDJoe9");
    AddAlias("BDJoe_10", "BDJoe10");
    AddAlias("BDJoe_11", "BDJoe11");
    AddAlias("BDJoe_12", "BDJoe12");
    AddAlias("BDJoe_13", "BDJoe13");
    AddAlias("BDJoe_14", "BDJoe14");
    AddAlias("BDJoe_15", "BDJoe15");
    AddAlias("Gena_1", "Gena1");
    AddAlias("Gena_2", "Gena2");
    AddAlias("Gena_3", "Gena3");
    AddAlias("Gena_4", "Gena4");
    AddAlias("Gena_5", "Gena5");
    AddAlias("Gena_6", "Gena6");
    AddAlias("Gena_7", "Gena7");
    AddAlias("Gena_8", "Gena8");
    AddAlias("Gena_9", "Gena9");
    AddAlias("Gena_10", "Gena10");
    AddAlias("Gena_11", "Gena11");
    AddAlias("Gena_12", "Gena12");
    AddAlias("Gena_13", "Gena13");
    AddAlias("Gena_14", "Gena14");
    AddAlias("Gena_15", "Gena15");
    AddAlias("Gus_1", "Gus1");
    AddAlias("Gus_2", "Gus2");
    AddAlias("Gus_3", "Gus3");
    AddAlias("Gus_4", "Gus4");
    AddAlias("Gus_5", "Gus5");
    AddAlias("Gus_6", "Gus6");
    AddAlias("Gus_7", "Gus7");
    AddAlias("Gus_8", "Gus8");
}

const ModelRegistryEntry* ModelEngine::FindRegistryEntry(const std::string& name) const
{
    auto it = m_NameToRegistryIdx.find(ToLower(name));
    if (it != m_NameToRegistryIdx.end())
    {
        return &m_Registry[it->second];
    }
    return nullptr;
}

bool ModelEngine::Initialize(const std::string& modDir, const std::string& gameDir)
{
    m_ModDir = modDir;
    m_GameDir = gameDir;
    m_Initialized = true;

    std::string poldc0Path = m_GameDir + "\\data_little\\polDC0.bin";
    EnsureVanillaBackup(poldc0Path);

    Logger::LogInfo("[3D Engine] Initialized. GameDir: %s, ModDir: %s, %zu registered chunks.",
        m_GameDir.c_str(), m_ModDir.c_str(), m_Registry.size());

    // Run first-time scan and apply any modified custom models
    Reload3DModels();
    return true;
}

void ModelEngine::Shutdown()
{
    m_Initialized = false;
}

void ModelEngine::SetEnabled(bool enabled)
{
    m_Enabled = enabled;
    if (!m_Enabled)
    {
        RestoreOriginal3DModels();
    }
    else
    {
        Reload3DModels();
    }
}

bool ModelEngine::EnsureVanillaBackup(const std::string& binPath)
{
    if (!FileExists(binPath)) return false;

    std::string bakPath = binPath + ".bak";
    if (!FileExists(bakPath))
    {
        if (CopyFileA(binPath.c_str(), bakPath.c_str(), TRUE))
        {
            Logger::LogInfo("[3D Engine] Created vanilla backup: %s", bakPath.c_str());
            return true;
        }
        else
        {
            Logger::LogError("[3D Engine] Failed to create vanilla backup for %s", binPath.c_str());
            return false;
        }
    }
    return true;
}

bool ModelEngine::RestoreOriginal3DModels()
{
    std::string poldc0Path = m_GameDir + "\\data_little\\polDC0.bin";
    std::string bakPath = poldc0Path + ".bak";

    if (!FileExists(bakPath))
    {
        Logger::LogWarning("[3D Engine] Cannot restore: backup %s not found.", bakPath.c_str());
        return false;
    }

    std::vector<uint8_t> bakData = ReadBinaryFile(bakPath);
    if (bakData.empty()) return false;

    // 1. Restore disk file
    if (!WriteBinaryFile(poldc0Path, bakData.data(), bakData.size()))
    {
        Logger::LogError("[3D Engine] Failed to restore %s from backup.", poldc0Path.c_str());
        return false;
    }

    // 2. Restore live RAM if running
    uintptr_t poldc0Base = GetPolDc0RamBase();
    if (poldc0Base != 0)
    {
        MEMORY_BASIC_INFORMATION mbi = { 0 };
        if (VirtualQuery((LPCVOID)poldc0Base, &mbi, sizeof(mbi)) != 0 && mbi.State == MEM_COMMIT)
        {
            DWORD oldProt;
            if (VirtualProtect((LPVOID)poldc0Base, bakData.size(), PAGE_EXECUTE_READWRITE, &oldProt))
            {
                memcpy((void*)poldc0Base, bakData.data(), bakData.size());
                VirtualProtect((LPVOID)poldc0Base, bakData.size(), oldProt, &oldProt);
                Logger::LogInfo("[3D Engine] Successfully restored live polDC0 memory to vanilla.");
            }
        }
    }

    Logger::LogInfo("[3D Engine] Vanilla 3D models restored successfully.");
    m_FileTimestamps.clear();
    return true;
}

void ModelEngine::LoadExtractedManifest()
{
    m_VanillaManifest.clear();
    std::string mfPath = m_ModDir + "\\3D\\extracted_manifest.json";
    std::ifstream f(mfPath);
    if (!f.is_open())
    {
        return;
    }

    std::string line;
    std::string currentKey;
    while (std::getline(f, line))
    {
        size_t quote1 = line.find('\"');
        if (quote1 != std::string::npos)
        {
            size_t quote2 = line.find('\"', quote1 + 1);
            if (quote2 != std::string::npos)
            {
                std::string k = line.substr(quote1 + 1, quote2 - quote1 - 1);
                if (k.find(".obj") != std::string::npos)
                {
                    size_t slash = k.find_last_of("/\\");
                    std::string fn = (slash != std::string::npos) ? k.substr(slash + 1) : k;
                    currentKey = ToLower(fn);
                    continue;
                }
            }
        }

        if (!currentKey.empty())
        {
            if (line.find("\"size\":") != std::string::npos)
            {
                size_t colon = line.find(':');
                if (colon != std::string::npos)
                {
                    m_VanillaManifest[currentKey].size = (DWORD)std::strtoul(line.c_str() + colon + 1, nullptr, 10);
                }
            }
            else if (line.find("\"time_low\":") != std::string::npos)
            {
                size_t colon = line.find(':');
                if (colon != std::string::npos)
                {
                    m_VanillaManifest[currentKey].timeLow = (DWORD)std::strtoul(line.c_str() + colon + 1, nullptr, 10);
                }
            }
            else if (line.find("\"time_high\":") != std::string::npos)
            {
                size_t colon = line.find(':');
                if (colon != std::string::npos)
                {
                    m_VanillaManifest[currentKey].timeHigh = (DWORD)std::strtoul(line.c_str() + colon + 1, nullptr, 10);
                }
            }
            else if (line.find('}') != std::string::npos)
            {
                currentKey.clear();
            }
        }
    }

    Logger::LogInfo("[3D Engine] Loaded extracted vanilla manifest with %zu entries.", m_VanillaManifest.size());
}

bool ModelEngine::Reload3DModels()
{
    if (!m_Initialized || !m_Enabled) return false;

    std::string poldc0Path = m_GameDir + "\\data_little\\polDC0.bin";
    std::string bakPath = poldc0Path + ".bak";
    EnsureVanillaBackup(poldc0Path);

    std::vector<uint8_t> vanillaData = ReadBinaryFile(FileExists(bakPath) ? bakPath : poldc0Path);
    if (vanillaData.empty())
    {
        Logger::LogError("[3D Engine] Failed to read base polDC0.bin data!");
        return false;
    }

    LoadExtractedManifest();

    m_Stats = CustomModelStats();

    // Search folders for custom .obj models
    std::vector<std::string> searchDirs = {
        m_ModDir + "\\3D\\custom",
        m_ModDir + "\\3D\\parts",
        m_ModDir + "\\3D\\characters",
        m_ModDir + "\\3D\\assembled",
        m_ModDir + "\\3D"
    };

    for (const auto& entry : m_Registry)
    {
        std::string targetObj;
        bool isCustomDir = false;
        for (const auto& dir : searchDirs)
        {
            std::string candidate = dir + "\\" + entry.name + ".obj";
            if (FileExists(candidate))
            {
                targetObj = candidate;
                if (dir.find("\\custom") != std::string::npos)
                {
                    isCustomDir = true;
                }
                break;
            }
        }

        if (targetObj.empty()) continue;

        WIN32_FILE_ATTRIBUTE_DATA fileInfo;
        if (!GetFileAttributesExA(targetObj.c_str(), GetFileExInfoStandard, &fileInfo))
        {
            continue;
        }

        // Check against vanilla manifest if not in custom folder
        if (!isCustomDir)
        {
            std::string fn = entry.name + ".obj";
            auto vmIt = m_VanillaManifest.find(ToLower(fn));
            if (vmIt != m_VanillaManifest.end())
            {
                // If size and timestamps match the extracted vanilla file, skip compiling!
                if (fileInfo.nFileSizeLow == vmIt->second.size &&
                    fileInfo.ftLastWriteTime.dwLowDateTime == vmIt->second.timeLow &&
                    fileInfo.ftLastWriteTime.dwHighDateTime == vmIt->second.timeHigh)
                {
                    continue; // Untouched vanilla reference file!
                }
            }
        }

        m_Stats.totalScanned++;

        // Check timestamp so we only recompile modified files
        auto it = m_FileTimestamps.find(targetObj);
        if (it != m_FileTimestamps.end())
        {
            if (CompareFileTime(&it->second, &fileInfo.ftLastWriteTime) == 0)
            {
                // Unchanged file
                continue;
            }
        }

        if (CompileAndInjectObj(targetObj, entry, vanillaData))
        {
            m_Stats.totalApplied++;
            m_FileTimestamps[targetObj] = fileInfo.ftLastWriteTime;
        }
        else
        {
            m_Stats.totalErrors++;
        }
    }

    std::ostringstream ss;
    ss << "3D Reload: " << m_Stats.totalApplied << " custom model(s) applied, "
       << m_Stats.totalErrors << " error(s).";
    m_Stats.lastStatus = ss.str();

    Logger::LogInfo("[3D Engine] %s", m_Stats.lastStatus.c_str());
    return (m_Stats.totalApplied > 0 || m_Stats.totalErrors == 0);
}

bool ModelEngine::CompileAndInjectObj(
    const std::string& objPath,
    const ModelRegistryEntry& entry,
    const std::vector<uint8_t>& vanillaData)
{
    Logger::LogInfo("[3D Engine] Compiling custom 3D model for '%s' from: %s",
        entry.name.c_str(), objPath.c_str());

    std::ifstream objFile(objPath);
    if (!objFile.is_open())
    {
        Logger::LogError("[3D Engine] Cannot open %s", objPath.c_str());
        return false;
    }

    std::vector<Vec3> positions;
    std::vector<Vec3> normals;
    std::vector<Vec2> uvs;
    std::vector<std::string> mtllibs;
    std::unordered_map<std::string, std::string> materialMapKd;

    std::unordered_map<std::string, ObjGroup> groups;
    std::vector<std::string> groupOrder;
    std::string currentGroupName = "default";

    auto GetOrCreateGroup = [&](const std::string& name) -> ObjGroup& {
        auto it = groups.find(name);
        if (it == groups.end())
        {
            ObjGroup g;
            g.name = name;
            groups[name] = g;
            groupOrder.push_back(name);
            return groups[name];
        }
        return it->second;
    };

    std::string line;
    while (std::getline(objFile, line))
    {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream iss(line);
        std::string token;
        iss >> token;

        if (token == "v")
        {
            Vec3 v;
            iss >> v.x >> v.y >> v.z;
            positions.push_back(v);
        }
        else if (token == "vn")
        {
            Vec3 n;
            iss >> n.x >> n.y >> n.z;
            normals.push_back(n);
        }
        else if (token == "vt")
        {
            Vec2 uv;
            iss >> uv.u >> uv.v;
            uvs.push_back(uv);
        }
        else if (token == "usemtl")
        {
            std::string mat;
            iss >> mat;
            if (!mat.empty())
            {
                currentGroupName = mat;
                GetOrCreateGroup(currentGroupName);
            }
        }
        else if (token == "mtllib")
        {
            std::string mtl;
            iss >> mtl;
            if (!mtl.empty()) mtllibs.push_back(mtl);
        }
        else if (token == "f")
        {
            ObjGroup& grp = GetOrCreateGroup(currentGroupName);
            std::vector<int> faceIndices;
            std::string corner;
            while (iss >> corner)
            {
                // Parse v/vt/vn
                int pi = 0, ti = 0, ni = 0;
                size_t s1 = corner.find('/');
                if (s1 == std::string::npos)
                {
                    pi = std::stoi(corner);
                }
                else
                {
                    pi = std::stoi(corner.substr(0, s1));
                    size_t s2 = corner.find('/', s1 + 1);
                    if (s2 == std::string::npos)
                    {
                        std::string tstr = corner.substr(s1 + 1);
                        if (!tstr.empty()) ti = std::stoi(tstr);
                    }
                    else
                    {
                        std::string tstr = corner.substr(s1 + 1, s2 - s1 - 1);
                        if (!tstr.empty()) ti = std::stoi(tstr);
                        std::string nstr = corner.substr(s2 + 1);
                        if (!nstr.empty()) ni = std::stoi(nstr);
                    }
                }

                // Resolve indices
                Vec3 p = { 0, 0, 0 };
                Vec3 n = { 0, 1, 0 };
                Vec2 uv = { 0, 0 };

                int k = (pi > 0) ? (pi - 1) : (int)positions.size() + pi;
                if (k >= 0 && k < (int)positions.size()) p = positions[k];

                if (ni != 0)
                {
                    int kn = (ni > 0) ? (ni - 1) : (int)normals.size() + ni;
                    if (kn >= 0 && kn < (int)normals.size()) n = normals[kn];
                }
                if (ti != 0)
                {
                    int kt = (ti > 0) ? (ti - 1) : (int)uvs.size() + ti;
                    if (kt >= 0 && kt < (int)uvs.size()) uv = uvs[kt];
                }

                ObjVertex vert;
                vert.x = p.x; vert.y = p.y; vert.z = p.z;
                vert.nx = n.x; vert.ny = n.y; vert.nz = n.z;
                vert.u = uv.u; vert.v = uv.v;

                grp.vertices.push_back(vert);
                faceIndices.push_back((int)grp.vertices.size() - 1);
            }

            // Fan triangulation for quads / n-gons
            for (size_t k = 1; k + 1 < faceIndices.size(); k++)
            {
                grp.triangles.push_back({ faceIndices[0], faceIndices[k], faceIndices[k + 1] });
            }
        }
    }

    // Parse referenced .mtl for map_Kd
    std::string objDir = objPath.substr(0, objPath.find_last_of("\\/"));
    for (const auto& mtlName : mtllibs)
    {
        std::string mtlPath = objDir + "\\" + mtlName;
        std::ifstream mtlFile(mtlPath);
        if (!mtlFile.is_open()) continue;

        std::string curMat;
        std::string mline;
        while (std::getline(mtlFile, mline))
        {
            if (mline.empty() || mline[0] == '#') continue;
            std::istringstream miss(mline);
            std::string mtoken;
            miss >> mtoken;
            if (mtoken == "newmtl")
            {
                miss >> curMat;
            }
            else if (mtoken == "map_Kd" && !curMat.empty())
            {
                std::string kd;
                miss >> kd;
                materialMapKd[curMat] = kd;
            }
        }
    }

    std::vector<ObjGroup> validGroups;
    for (const auto& gname : groupOrder)
    {
        auto& g = groups[gname];
        if (!g.triangles.empty())
        {
            g.mapKd = materialMapKd[g.name];
            validGroups.push_back(g);
        }
    }

    if (validGroups.empty())
    {
        Logger::LogError("[3D Engine] No valid triangles found in %s", objPath.c_str());
        return false;
    }

    // 1. Axis conversion to game frame (Y-up, right-handed)
    float minX = 1e9f, maxX = -1e9f;
    float minY = 1e9f, maxY = -1e9f;
    float minZ = 1e9f, maxZ = -1e9f;
    for (const auto& g : validGroups)
    {
        for (const auto& v : g.vertices)
        {
            minX = (std::min)(minX, v.x); maxX = (std::max)(maxX, v.x);
            minY = (std::min)(minY, v.y); maxY = (std::max)(maxY, v.y);
            minZ = (std::min)(minZ, v.z); maxZ = (std::max)(maxZ, v.z);
        }
    }

    bool convertZUp = false;
    if (m_AxisUp == "z" || (m_AxisUp == "auto" && (maxY - minY) > 1.3f * (maxZ - minZ) && (maxY - minY) > 1.3f * (maxX - minX)))
    {
        convertZUp = true;
    }

    if (convertZUp)
    {
        Logger::LogInfo("[3D Engine] Converting source from Z-up to game Y-up: (x, z, -y)");
        for (auto& g : validGroups)
        {
            for (auto& v : g.vertices)
            {
                float ox = v.x, oy = v.y, oz = v.z;
                v.x = ox; v.y = oz; v.z = -oy;
                float onx = v.nx, ony = v.ny, onz = v.nz;
                v.nx = onx; v.ny = onz; v.nz = -ony;
            }
        }
    }

    // 2. Measure bounds after conversion
    minX = 1e9f; maxX = -1e9f;
    minY = 1e9f; maxY = -1e9f;
    minZ = 1e9f; maxZ = -1e9f;
    for (const auto& g : validGroups)
    {
        for (const auto& v : g.vertices)
        {
            minX = (std::min)(minX, v.x); maxX = (std::max)(maxX, v.x);
            minY = (std::min)(minY, v.y); maxY = (std::max)(maxY, v.y);
            minZ = (std::min)(minZ, v.z); maxZ = (std::max)(maxZ, v.z);
        }
    }

    Vec3 newCenter = { (minX + maxX) * 0.5f, (minY + maxY) * 0.5f, (minZ + maxZ) * 0.5f };
    float newExt = (std::max)({ maxX - minX, maxY - minY, maxZ - minZ });

    // 3. Inspect original chunk in vanillaData for reference placement & sphere
    Vec3 refCenter = newCenter;
    float refExt = newExt;
    float refRadius = newExt * 0.5f;
    uint32_t origField1 = 1;
    std::vector<int> origTexIndices;
    std::vector<uint32_t> origRecFmts; // per original record: [fmt] word at blob start (0 = unknown)

    if (entry.offset + 0x18 <= vanillaData.size())
    {
        const uint8_t* pChunk = vanillaData.data() + entry.offset;
        origField1 = *(const uint32_t*)(pChunk + 4);
        refRadius = *(const float*)(pChunk + 8);
        refCenter.x = *(const float*)(pChunk + 12);
        refCenter.y = *(const float*)(pChunk + 16);
        refCenter.z = *(const float*)(pChunk + 20);

        // Walk original chunk mesh records to gather reference texture indices
        size_t rpos = entry.offset + 0x18;
        while (rpos + 0x50 <= vanillaData.size())
        {
            int32_t first = *(const int32_t*)(vanillaData.data() + rpos);
            if (first == 0 || first > 0) break;
            uint32_t texIdx = *(const uint32_t*)(vanillaData.data() + rpos + 0x20);
            origTexIndices.push_back((texIdx == 0xFFFFFFFF) ? -1 : (int)texIdx);
            uint32_t bsize = *(const uint32_t*)(vanillaData.data() + rpos + 0x4C);
            // Capture the original section's [fmt] word (blob start) so the
            // recompiled section reuses the exact strip encoding of the original.
            uint32_t fmt = 0; // 0 = unknown (header-only record / truncated blob)
            if (bsize >= 8 && rpos + 0x58 <= vanillaData.size())
                fmt = *(const uint32_t*)(vanillaData.data() + rpos + 0x50);
            origRecFmts.push_back(fmt);
            if (bsize == 0) { rpos += 0x50; continue; }
            rpos += 0x50 + ((bsize >> 2) << 2);
        }

        if (refRadius > 0.1f) refExt = refRadius * 2.0f;
    }

    // 4. Auto Rescale / Translation if enabled
    if (m_AutoRescale && newExt > 1e-4f && refExt > 1e-4f)
    {
        float scale = refExt / newExt;
        Logger::LogInfo("[3D Engine] Auto-rescale scale factor: %.4f (newExt: %.2f -> refExt: %.2f)",
            scale, newExt, refExt);

        for (auto& g : validGroups)
        {
            for (auto& v : g.vertices)
            {
                v.x = (v.x - newCenter.x) * scale + refCenter.x;
                v.y = (v.y - newCenter.y) * scale + refCenter.y;
                v.z = (v.z - newCenter.z) * scale + refCenter.z;
            }
        }
        newCenter = refCenter;
    }

    // 5. Sega Ninja Binary Chunk Compilation
    std::vector<uint8_t> chunkBinary;
    chunkBinary.resize(0x18);

    *(uint32_t*)(chunkBinary.data() + 0) = 1; // version
    *(uint32_t*)(chunkBinary.data() + 4) = origField1;
    *(float*)(chunkBinary.data() + 8) = refRadius;
    *(float*)(chunkBinary.data() + 12) = newCenter.x;
    *(float*)(chunkBinary.data() + 16) = newCenter.y;
    *(float*)(chunkBinary.data() + 20) = newCenter.z;

    // Positional texIndex fallback assumes OBJ group order == original record
    // order; group merging (repeated usemtl) can shift that mapping — warn
    // when the counts diverge so misbound textures stay traceable.
    if (!origTexIndices.empty() && validGroups.size() != origTexIndices.size())
    {
        Logger::LogWarning("[3D Engine] '%s': %zu material group(s) vs %zu original texture record(s); positional texIndex fallback may be shifted.",
            entry.name.c_str(), validGroups.size(), origTexIndices.size());
    }

    for (size_t gi = 0; gi < validGroups.size(); gi++)
    {
        const auto& g = validGroups[gi];

        // Resolve texture index
        int texIndex = ParseTextureIndexFromMaterial(g.name, g.mapKd);
        if (texIndex < 0)
        {
            // Group name gave no texDC0 index — fall back positionally to the
            // original record's texture (OBJ groups follow record order).
            if (gi < origTexIndices.size())
            {
                texIndex = origTexIndices[gi];
                Logger::LogWarning("[3D Engine] '%s' group %zu ('%s', map_Kd '%s') has no texDC0 name; using original record texIndex %d.",
                    entry.name.c_str(), gi, g.name.c_str(), g.mapKd.c_str(), texIndex);
            }
            else if (!origTexIndices.empty())
            {
                texIndex = origTexIndices.back();
                // All groups beyond the record count would share one texture.
                Logger::LogWarning("[3D Engine] '%s' group %zu ('%s', map_Kd '%s') is beyond the original record count; reusing last texIndex %d.",
                    entry.name.c_str(), gi, g.name.c_str(), g.mapKd.c_str(), texIndex);
            }
            else
            {
                // 'Untextured' is exclusively -1: 0 is a valid texDC0 index and
                // would silently bind texDC0[0].
                texIndex = -1;
                Logger::LogWarning("[3D Engine] '%s' group %zu ('%s', map_Kd '%s') has no texDC0 name and no original records; left untextured (-1).",
                    entry.name.c_str(), gi, g.name.c_str(), g.mapKd.c_str());
            }
        }

        // Measure group bounds
        float gx0 = 1e9f, gx1 = -1e9f, gy0 = 1e9f, gy1 = -1e9f, gz0 = 1e9f, gz1 = -1e9f;
        for (const auto& v : g.vertices)
        {
            gx0 = (std::min)(gx0, v.x); gx1 = (std::max)(gx1, v.x);
            gy0 = (std::min)(gy0, v.y); gy1 = (std::max)(gy1, v.y);
            gz0 = (std::min)(gz0, v.z); gz1 = (std::max)(gz1, v.z);
        }
        Vec3 gCenter = { (gx0 + gx1) * 0.5f, (gy0 + gy1) * 0.5f, (gz0 + gz1) * 0.5f };

        // Vertex section blob: [fmt][nVerts][nVerts * 32B], strip-encoded
        // (V = T + 2*S vertices; see BuildTriStrips — the old triangle-list
        // emission wrote 3*T vertices and inflated the blob ~2-3x).
        std::vector<TriStrip> strips = BuildTriStrips(g.triangles);
        uint32_t nVerts = 0;
        for (const auto& s : strips) nVerts += (uint32_t)s.verts.size();
        uint32_t blobSize = 8 + nVerts * 32;

        // Section format word: reuse the original record's [fmt] for round-trip
        // fidelity (observed section formats: 0x12/0x92/0x16/0x96). Without an
        // original record fall back to 0x12 — the strip encoding (0x1A with the
        // primitive-type bit 0x08 cleared). The game draws every section as a
        // strip (no index buffer), so a triangle-list fmt misreads the blob.
        // The payload above is ALWAYS strip-encoded, so the format word must
        // declare a strip too: copying an original list-fmt (bit 0x08 set)
        // verbatim would make the game misread the shrunk blob (geometry
        // loss). Observed formats never set bit 0x08, so clearing it is a
        // no-op for real round-trips and purely defensive otherwise.
        uint32_t secFmt = (gi < origRecFmts.size() && origRecFmts[gi] != 0)
            ? origRecFmts[gi] : 0x12;
        secFmt &= ~0x08u;

        size_t recStart = chunkBinary.size();
        chunkBinary.resize(recStart + 0x50 + blobSize);
        uint8_t* pRec = chunkBinary.data() + recStart;

        // Mesh record header (0x50 bytes)
        *(uint32_t*)(pRec + 0x00) = 0x8000002C; // bit31 set (mandatory for Sega nlObjPut)
        *(uint32_t*)(pRec + 0x04) = 0x83000000;
        *(uint32_t*)(pRec + 0x08) = 0x00000000;
        *(float*)(pRec + 0x0C) = 0.0f;
        *(float*)(pRec + 0x10) = gCenter.x;
        *(float*)(pRec + 0x14) = gCenter.y;
        *(float*)(pRec + 0x18) = gCenter.z;
        *(int32_t*)(pRec + 0x20) = (texIndex < 0) ? -1 : texIndex;
        *(uint32_t*)(pRec + 0x24) = 0;

        float* rgba1 = (float*)(pRec + 0x28);
        rgba1[0] = 0.75f; rgba1[1] = 1.0f; rgba1[2] = 1.0f; rgba1[3] = 1.0f;

        float* rgba2 = (float*)(pRec + 0x38);
        rgba2[0] = 1.0f; rgba2[1] = 0.0f; rgba2[2] = 1.0f; rgba2[3] = 1.0f;

        *(float*)(pRec + 0x48) = 1.0f;
        *(uint32_t*)(pRec + 0x4C) = blobSize;

        // Write blob (strip-encoded: vertices shared between consecutive strip
        // triangles are stored once)
        uint8_t* pBlob = pRec + 0x50;
        *(uint32_t*)(pBlob + 0) = secFmt;
        *(uint32_t*)(pBlob + 4) = nVerts;

        float* pV = (float*)(pBlob + 8);
        for (const auto& s : strips)
        {
            for (uint32_t vi : s.verts)
            {
                const ObjVertex& vert = g.vertices[vi];
                *pV++ = vert.x;
                *pV++ = vert.y;
                *pV++ = vert.z;
                *pV++ = vert.nx;
                *pV++ = vert.ny;
                *pV++ = vert.nz;
                *pV++ = vert.u;
                *pV++ = vert.v;
            }
        }
    }

    // Terminator dword
    uint32_t terminator = 0;
    chunkBinary.insert(chunkBinary.end(), (uint8_t*)&terminator, (uint8_t*)&terminator + 4);

    // Budget check
    if (chunkBinary.size() > entry.maxBytes)
    {
        Logger::LogError("[3D Engine] Model '%s' exceeds chunk budget! Size: %zu B, Budget: %zu B. Simplify mesh!",
            entry.name.c_str(), chunkBinary.size(), entry.maxBytes);
        return false;
    }

    // Zero-pad up to entry.maxBytes to prevent trailing data leakage
    size_t writeSize = chunkBinary.size();
    if (chunkBinary.size() < entry.maxBytes)
    {
        chunkBinary.resize(entry.maxBytes, 0);
        writeSize = entry.maxBytes;
    }

    // 6. Synchronize to polDC0.bin on disk
    std::string poldc0Path = m_GameDir + "\\data_little\\polDC0.bin";
    std::fstream fs(poldc0Path, std::ios::in | std::ios::out | std::ios::binary);
    if (fs.is_open())
    {
        fs.seekp(entry.offset, std::ios::beg);
        fs.write((const char*)chunkBinary.data(), writeSize);
        fs.close();
        Logger::LogInfo("[3D Engine] Wrote %zu bytes to %s at offset 0x%06X",
            writeSize, poldc0Path.c_str(), entry.offset);
    }
    else
    {
        Logger::LogWarning("[3D Engine] Could not open %s for write on disk (may be locked by running game).",
            poldc0Path.c_str());
    }

    // 7. Live RAM injection if game is currently loaded
    uintptr_t poldc0Base = GetPolDc0RamBase();
    if (poldc0Base != 0)
    {
        uintptr_t targetRam = poldc0Base + entry.offset;
        MEMORY_BASIC_INFORMATION mbi = { 0 };
        if (VirtualQuery((LPCVOID)targetRam, &mbi, sizeof(mbi)) != 0 && mbi.State == MEM_COMMIT)
        {
            DWORD oldProt;
            if (VirtualProtect((LPVOID)targetRam, writeSize, PAGE_EXECUTE_READWRITE, &oldProt))
            {
                memcpy((void*)targetRam, chunkBinary.data(), writeSize);
                VirtualProtect((LPVOID)targetRam, writeSize, oldProt, &oldProt);
                Logger::LogInfo("[3D Engine] SUCCESS: Injected %zu bytes live into RAM at 0x%08X for '%s'!",
                    writeSize, targetRam, entry.name.c_str());
            }
            else
            {
                Logger::LogWarning("[3D Engine] VirtualProtect failed on live RAM at 0x%08X", targetRam);
            }
        }
    }

    return true;
}
