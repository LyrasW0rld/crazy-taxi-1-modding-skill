#pragma once
#include <windows.h>
#include <d3d9.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include "IniConfig.h"

#pragma pack(push, 1)
struct ManifestHeader
{
    char magic[4];           // "LTM1"
    uint32_t version;        // 1
    uint32_t entryCount;
    uint32_t stringTableLen;
};

struct ManifestEntryRecord
{
    uint32_t crc32;
    uint32_t gbix;
    uint32_t archiveId;
    uint32_t chunkIndex;
    uint32_t dataOffset;
    uint32_t origWidth;
    uint32_t origHeight;
    uint32_t origFormat;
    uint32_t filenameOffset;
    uint32_t rawDataSize;
};

struct CacheHeader
{
    char magic[4];           // "LTMC"
    uint32_t version;        // 2 (v2: min-8 Sega snap + exact magenta keying + contract orientation handling; v1 caches are invalid and recompiled)
    uint32_t width;
    uint32_t height;
    uint32_t d3dFormat;      // D3DFMT_A8R8G8B8 = 21
    uint32_t dataSize;
    uint64_t sourcePngTime;
};
#pragma pack(pop)

// Cache format version. Bumped whenever compiled-pixel behavior changes so stale
// caches can never keep old behavior alive (v5: native aspect ratio for 2D UI / bootscreen logos).
static const uint32_t LTM_CACHE_VERSION = 5;

struct RuntimeTextureInfo
{
    ManifestEntryRecord record;
    std::string filename;
    std::string fullPngPath;           // Baseline PNG path
    std::string fullSourceImagePath;   // Resolved source image (PNG, JPG, or JPEG)
    std::string fullCachePath;
    bool isModified = false;
    uint64_t sourceImageLastWriteTime = 0;
};

class TextureEngine
{
public:
    static TextureEngine& Instance();

    bool Initialize(const std::string& modDir);
    void Shutdown();

    void SetEnabled(bool enabled);
    bool IsEnabled() const { return m_Config.Enabled; }

    void ReloadTextures(bool isFlush = false);
    void FlushCache();

    // In-game Tools & Features (100% Native C++, no external tools or Python)
    bool CreateTexturesZipBackup(std::string* outZipPath = nullptr, int* outCount = nullptr);
    bool StampTestTextures(std::string* outStatus = nullptr);
    bool UnstampTestTextures(std::string* outStatus = nullptr);
    bool IsTexturesStamped() const;
    void RestartGame();

    // Fail-safe & resilience (Missing Texture Fallback)
    IDirect3DTexture9* GetOrCreateMissingTexture();
    bool EnsureMissingTextureExists(const std::string& path);

    // Hook Handlers
    HRESULT HandleD3DXCreateTextureFromFileInMemoryEx(
        LPDIRECT3DDEVICE9 pDevice,
        LPCVOID pSrcData,
        UINT SrcDataSize,
        UINT Width,
        UINT Height,
        UINT MipLevels,
        DWORD Usage,
        D3DFORMAT Format,
        D3DPOOL Pool,
        DWORD Filter,
        DWORD MipFilter,
        D3DCOLOR ColorKey,
        void *pSrcInfo,
        PALETTEENTRY *pPalette,
        LPDIRECT3DTEXTURE9 *ppTexture
    );

    bool HandleSegaCreateTexture(
        void* pRenderer,
        void* pTexStruct,
        UINT width,
        UINT height,
        DWORD formatIndex,
        int* pOutFlag,
        DWORD* pOutFlags
    );

    // In-game round texture replacement handlers
    void HandlePostLoadSegaTextureContainer(void* tablePtr, uint32_t baseIdx);
    void HandlePostLoadModelTexture(void* tablePtr, int tableIndex, uint32_t baseIdx = 0);
    void HandlePostLoadStageTexture(void* entryPtr, uint32_t id);
    void HandlePreSetTexture(void* pTexObj);
    uint32_t GetDefaultBaseIdxForTable(void* tablePtr);

    void ScanAndReplaceAllMasterTextures();
    void RestoreAllVanillaTextures();
    void ToggleRestoreOriginalTextures();
    bool IsInitialized() const { return m_Initialized; }
    bool IsVanillaRestored() const { return m_VanillaRestored; }
    void SetVanillaRestored(bool restored);
    void PrecompileModifiedTextures(const char* title = nullptr, const char* subtitle = nullptr);
    std::string ResolveImagePath(const std::string& manifestFilename);
    std::string ResolvePngPath(const std::string& manifestFilename) { return ResolveImagePath(manifestFilename); }
    std::string ResolveOriginalPath(const std::string& filename);
    void BuildPathCache();
    // NextPowerOfTwo with a configurable lower clamp. Default 16 preserves the
    // historical JPK/UI behavior; Sega containers pass 8 (contract K1: 58 entries
    // with edge 8 must not be upscaled to 16, otherwise bit-exact arrival is impossible).
    static UINT NextPowerOfTwo(UINT n, UINT minValue = 16);

    // Direct3D 9 texture retrieval / creation
    IDirect3DDevice9* GetD3DDevice();
    void SetD3DDevice(IDirect3DDevice9* pDev) { if (pDev) m_pDevice = pDev; }
    IDirect3DTexture9* GetOrCreateD3DTexture(const RuntimeTextureInfo& info);
    void PreloadAllModifiedD3DTextures();

    // Helpers
    uint32_t CalculateCRC32(const void* data, size_t length);
    // wantW/wantH: the game-requested texture dimensions (0 when unknown). Inside a
    // CRC collision group they name the SLOT and disambiguate cross-container members.
    const RuntimeTextureInfo* FindByCRC(uint32_t crc, uint32_t wantW = 0, uint32_t wantH = 0);
    const RuntimeTextureInfo* FindByArchiveAndGBIX(uint32_t archiveId, uint32_t gbix);
    const RuntimeTextureInfo* FindByOffset(uint32_t archiveId, uint32_t offset);
    const RuntimeTextureInfo* FindByArchiveAndIndex(uint32_t archiveId, uint32_t chunkIndex);
    const RuntimeTextureInfo* FindByArchiveAndOffset(uint32_t archiveId, uint32_t offset);
    const RuntimeTextureInfo* FindByCRC_Locked(uint32_t crc, uint32_t wantW = 0, uint32_t wantH = 0) const;
    const RuntimeTextureInfo* FindByArchiveAndGBIX_Locked(uint32_t archiveId, uint32_t gbix) const;
    const RuntimeTextureInfo* FindByOffset_Locked(uint32_t archiveId, uint32_t offset) const;
    const RuntimeTextureInfo* FindByArchiveAndIndex_Locked(uint32_t archiveId, uint32_t chunkIndex) const;
    // K24: true when a DIFFERENT container claims the same nonzero GBIX (the
    // driver-select portrait sprGAME#0156 and Axel's pants texDC0#0254 both carry
    // GBIX 0x000F3100). Installs for contested GBIX slots are skipped.
    bool IsGbixContested(uint32_t archiveId, uint32_t gbix);
    uint32_t GetArchiveIdFromTablePtr(void* tablePtr);

    ModConfig& GetConfig() { return m_Config; }
    const std::string& GetModDir() const { return m_ModDir; }

private:
    TextureEngine();
    ~TextureEngine();

    bool LoadManifest();
    void ScanModifiedTextures();
    bool LoadOrCreateCache(const RuntimeTextureInfo& info, std::vector<uint8_t>& outPixels, uint32_t& outWidth, uint32_t& outHeight);
    bool CompileImageToCache(const RuntimeTextureInfo& info, const std::string& imagePath, const std::string& cachePath);
    bool CompilePngToCache(const RuntimeTextureInfo& info, const std::string& pngPath, const std::string& cachePath) { return CompileImageToCache(info, pngPath, cachePath); }
    void ApplySmartFormatRules(const RuntimeTextureInfo& info, std::vector<uint8_t>& pixels, UINT width, UINT height, bool sourceHasAlpha);
    bool IsPowerOfTwo(uint32_t n) const { return (n != 0) && ((n & (n - 1)) == 0); }
    static bool IsSegaContainerRecord(const RuntimeTextureInfo& info);
    static bool IsDdsUiRecord(const RuntimeTextureInfo& info);

    // K9 slot-content discipline (implementation and contract in TextureEngine.cpp):
    // recycling-aware classification shared by hooks 2/3/4 and the master scan.
    bool InstallCustomTexture(void* pTexObj, IDirect3DTexture9* pCustomTex, bool evictOriginalOnForeign);
    void RestoreOriginalForTexObj(void* pTexObj, IDirect3DTexture9* pStoredOriginal);
    // K26: persistent object->record binding. ReloadTextures keeps these bindings
    // alive and re-swaps every recorded slot to its freshly compiled custom; the
    // periodic verifier heals game-side slot resets and evicts recycled objects.
    void VerifyInstalledSlots();
    const RuntimeTextureInfo* FindInfoByCacheKey(uint64_t cacheKey);

    std::string m_ModDir;
    std::string m_TexturesDir;
    std::string m_OriginalsDir;
    std::string m_CacheDir;
    std::string m_IniPath;
    std::string m_LogPath;

    ModConfig m_Config;
    bool m_Initialized = false;
    bool m_VanillaRestored = false;

    std::vector<RuntimeTextureInfo> m_Textures;
    std::unordered_map<uint32_t, size_t> m_CrcMap;
    std::unordered_map<uint64_t, size_t> m_OffsetMap;
    std::unordered_map<uint64_t, size_t> m_ArchiveIndexMap;
    std::unordered_map<uint64_t, size_t> m_ArchiveGbixMap;
    std::unordered_map<std::string, std::string> m_PathCache;
    std::unordered_map<std::string, std::string> m_OriginalsPathCache;

    // Fast O(1) set of modified texture keys for nanosecond chunk streaming filtering
    std::unordered_set<uint64_t> m_ModifiedArchiveIndexSet;
    std::unordered_set<uint64_t> m_ModifiedOffsetSet;
    std::unordered_set<uint64_t> m_ModifiedGbixSet;

    // Runtime texture references & D3D textures
    std::unordered_map<uint64_t, IDirect3DTexture9*> m_D3DTextureCache;
    std::unordered_map<void*, IDirect3DTexture9*> m_OriginalTextures;
    std::unordered_map<void*, IDirect3DTexture9*> m_TexObjToCustom;
    // K26: void* texObj -> cache key ((archiveId<<32)|chunkIndex) of the installed
    // custom. Persists across ReloadTextures (unlike the maps above historically)
    // so installed slots can be re-swapped to fresh compiles without a game reload.
    std::unordered_map<void*, uint64_t> m_TexObjToRecordKey;
    IDirect3DDevice9* m_pDevice = nullptr;
    IDirect3DTexture9* m_pMissingTexture = nullptr;

    CRITICAL_SECTION m_CS;
    ULONG_PTR m_GdiplusToken = 0;
};

