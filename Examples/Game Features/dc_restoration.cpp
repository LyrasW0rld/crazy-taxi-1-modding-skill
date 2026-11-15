#include "dc_restoration.h"
#include "config.h"
#include "logger.h"
#include "fila_textures.h"
#include <map>
#include <vector>

namespace DCRestoration
{
    // Authentic licensed brand names (Arcade & Dreamcast)
    static const char* S_PIZZA_HUT    = "Pizza Hut";
    static const char* S_LEVIS_STORE  = "The Original Levi's Store";
    static const char* S_FILA         = "FILA";
    static const char* S_KFC          = "Kentucky Fried Chicken";
    static const char* S_TOWER        = "Tower Records";

    // State tracking
    static bool s_NamesActive = false;
    static bool s_FilaDestActive = false;
    static bool s_PizzaTexActive = false;
    static bool s_WorldTexActive = false;
    static bool s_ThumbnailsActive = false;
    static bool s_VoicesActive = false;
    static bool s_Initialized = false;

    // Original bytes backups
    static uint8_t  s_OrigFilaDestByte = 0x75;
    static uint32_t s_OrigPizzaTex1 = 0x0CBFDF60;
    static uint32_t s_OrigPizzaTex2 = 0x0CE98000;
    static uint32_t s_OrigPizzaTex3 = 0x0CC0B3A0;
    static uint32_t s_OrigPizzaTex4 = 0x0CE98000;
    static uint8_t  s_OrigWorldTexBytes[5] = { 0x3D, 0x80, 0xAC, 0xBF, 0x0C };
    static uint8_t  s_OrigThumbBytes[5]    = { 0x83, 0xF8, 0x03, 0x75, 0x6A };
    static uint8_t  s_OrigVoiceBytes[2]    = { 0x83, 0xF8 };
    static uint8_t  s_OrigVoiceGate2Bytes[10] = { 0x85, 0xFF, 0x75, 0x15, 0x8B, 0x86, 0x40, 0x01, 0x00, 0x00 };

    // Vanilla destination name pointers for exact rollback
    static const char* s_OrigArcadeNamePtrs[5] = { nullptr, nullptr, nullptr, nullptr, nullptr }; // arcade slots 8/11/12/13/14
    static const char* s_OrigCityNamePtrs[5]   = { nullptr, nullptr, nullptr, nullptr, nullptr }; // original city slots 1..5
    static bool s_NamePtrsBackedUp = false;

    // Snapshot the 10 vanilla name pointers (arcade slots 8/11/12/13/14 in the table
    // at 0x013B9DD8, original city slots 1..5 in the table at 0x013B9E98) for exact rollback.
    static bool EnsureNamePtrsBackedUp()
    {
        if (s_NamePtrsBackedUp)
            return true;
        __try
        {
            // 0x013B9F0C holds two char* tables: [0] = arcade names, [1] = original city names
            const char*** tables = (const char***)0x013B9F0C;
            if (!tables[0] || !tables[1])
                return false;
            s_OrigArcadeNamePtrs[0] = tables[0][8];
            s_OrigArcadeNamePtrs[1] = tables[0][11];
            s_OrigArcadeNamePtrs[2] = tables[0][12];
            s_OrigArcadeNamePtrs[3] = tables[0][13];
            s_OrigArcadeNamePtrs[4] = tables[0][14];
            s_OrigCityNamePtrs[0] = tables[1][1];
            s_OrigCityNamePtrs[1] = tables[1][2];
            s_OrigCityNamePtrs[2] = tables[1][3];
            s_OrigCityNamePtrs[3] = tables[1][4];
            s_OrigCityNamePtrs[4] = tables[1][5];
            s_NamePtrsBackedUp = true;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    void SetOriginalNames(bool enable)
    {
        if (s_NamesActive == enable)
            return;

        __try
        {
            // 0x013B9F0C is an array of two name pointer tables in writable .data:
            // [0] = 0x013B9DD8 arcade (33 entries), [1] = 0x013B9E98 original city (29 entries).
            // Only swap pointer slots; never write into the packed string blob at 0x0058A0C4 (.rdata).
            const char*** tables = (const char***)0x013B9F0C;
            if (!tables[0] || !tables[1])
                return;

            const char** arcadeNames = tables[0];
            const char** cityNames   = tables[1];

            if (enable)
            {
                if (!EnsureNamePtrsBackedUp())
                {
                    Logger::Error("DCRestore", "Name pointer backup failed, original names not applied");
                    return;
                }
                arcadeNames[8]  = S_PIZZA_HUT;
                arcadeNames[11] = S_FILA;
                arcadeNames[12] = S_LEVIS_STORE;
                arcadeNames[13] = S_TOWER;
                arcadeNames[14] = S_KFC;
                cityNames[1] = S_PIZZA_HUT;
                cityNames[2] = S_FILA;
                cityNames[3] = S_LEVIS_STORE;
                cityNames[4] = S_TOWER;
                cityNames[5] = S_KFC;
                s_NamesActive = true;
                Logger::Info("DCRestore", "Original destination names enabled (arcade[8]='%s', city[2]='%s')",
                             arcadeNames[8], cityNames[2]);
            }
            else
            {
                if (s_NamePtrsBackedUp)
                {
                    arcadeNames[8]  = s_OrigArcadeNamePtrs[0];
                    arcadeNames[11] = s_OrigArcadeNamePtrs[1];
                    arcadeNames[12] = s_OrigArcadeNamePtrs[2];
                    arcadeNames[13] = s_OrigArcadeNamePtrs[3];
                    arcadeNames[14] = s_OrigArcadeNamePtrs[4];
                    cityNames[1] = s_OrigCityNamePtrs[0];
                    cityNames[2] = s_OrigCityNamePtrs[1];
                    cityNames[3] = s_OrigCityNamePtrs[2];
                    cityNames[4] = s_OrigCityNamePtrs[3];
                    cityNames[5] = s_OrigCityNamePtrs[4];
                }
                s_NamesActive = false;
                Logger::Info("DCRestore", "Destination names restored to vanilla");
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            Logger::Error("DCRestore", "Exception in SetOriginalNames");
        }
    }

    void SetFilaDestination(bool enable)
    {
        if (s_FilaDestActive == enable)
            return;

        __try
        {
            uint8_t* pAddr = (uint8_t*)0x004FDBF3;
            DWORD oldProt;
            if (VirtualProtect(pAddr, 1, PAGE_EXECUTE_READWRITE, &oldProt))
            {
                if (enable)
                {
                    *pAddr = 0xEB; // jne -> jmp
                    s_FilaDestActive = true;
                    // Log the session mode (0x16A28F0) once to confirm the city mapping at runtime
                    Logger::Info("DCRestore", "FILA destination fix enabled (0x4FDBF3 = 0xEB, session mode 0x16A28F0 = %u)",
                                 *(uint32_t*)0x16A28F0);
                }
                else
                {
                    *pAddr = s_OrigFilaDestByte; // restore 0x75
                    s_FilaDestActive = false;
                    Logger::Info("DCRestore", "FILA destination fix restored to vanilla (0x4FDBF3 = 0x%02X)", s_OrigFilaDestByte);
                }
                VirtualProtect(pAddr, 1, oldProt, &oldProt);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            Logger::Error("DCRestore", "Exception in SetFilaDestination");
        }
    }

    void SetPizzaHutTextures(bool enable)
    {
        if (s_PizzaTexActive == enable)
            return;

        __try
        {
            DWORD oldProt;
            uint32_t* p1 = (uint32_t*)0x00491F68;
            uint32_t* p2 = (uint32_t*)0x00491F84;
            uint32_t* p3 = (uint32_t*)0x00491FAF;
            uint32_t* p4 = (uint32_t*)0x00491FCB;

            if (VirtualProtect((void*)0x00491F60, 0x80, PAGE_EXECUTE_READWRITE, &oldProt))
            {
                if (enable)
                {
                    *p1 = 0xFFFFFFFF;
                    *p2 = 0xFFFFFFFF;
                    *p3 = 0xFFFFFFFF;
                    *p4 = 0xFFFFFFFF;
                    s_PizzaTexActive = true;
                    Logger::Info("DCRestore", "Pizza Hut texture bypass enabled");
                }
                else
                {
                    *p1 = s_OrigPizzaTex1;
                    *p2 = s_OrigPizzaTex2;
                    *p3 = s_OrigPizzaTex3;
                    *p4 = s_OrigPizzaTex4;
                    s_PizzaTexActive = false;
                    Logger::Info("DCRestore", "Pizza Hut textures restored to vanilla");
                }
                VirtualProtect((void*)0x00491F60, 0x80, oldProt, &oldProt);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            Logger::Error("DCRestore", "Exception in SetPizzaHutTextures");
        }
    }

    // MSVC std::map node structure for common.jpk texture map (dword_1BC45E4)
    struct CommonJpkNode
    {
        CommonJpkNode* left;   // +0x00
        CommonJpkNode* parent; // +0x04
        CommonJpkNode* right;  // +0x08
        int32_t        key;    // +0x0C
        uint32_t       size;   // +0x10
        uint8_t*       pDDS;   // +0x14 (pointer to 4224-byte DXT5 DDS texture)
        uint32_t       dimW;   // +0x18
        uint32_t       dimH;   // +0x1C
        uint32_t       flags;  // +0x20
        uint32_t       idCopy; // +0x24
        uint8_t        color;  // +0x28
        uint8_t        isNil;  // +0x29
    };

    static CommonJpkNode* FindJpkEntry(int key)
    {
        __try
        {
            uintptr_t head = *(uintptr_t*)0x01BC45E4;
            if (!head) return nullptr;
            CommonJpkNode* cur = *(CommonJpkNode**)(head + 4);
            while (cur && !cur->isNil)
            {
                if (cur->key == key)
                    return cur;
                if (key < cur->key)
                    cur = cur->left;
                else
                    cur = cur->right;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        return nullptr;
    }

    static bool s_FilaJpkActive = false;
    static uint8_t s_OrigFilaSign[4224];
    static uint8_t s_OrigFilaWall[4224];
    static uint8_t s_OrigFilaDoor[4224];
    static bool s_FilaJpkBackedUp = false;

    static bool ApplyFilaJpkTextures(bool enable)
    {
        if (s_FilaJpkActive == enable)
            return true;

        __try
        {
            CommonJpkNode* nodeSign = FindJpkEntry(104);
            CommonJpkNode* nodeWall = FindJpkEntry(105);
            CommonJpkNode* nodeDoor = FindJpkEntry(106);

            if (!nodeSign || !nodeWall || !nodeDoor)
                return false;
            if (!nodeSign->pDDS || !nodeWall->pDDS || !nodeDoor->pDDS)
                return false;

            if (enable)
            {
                if (!s_FilaJpkBackedUp)
                {
                    memcpy(s_OrigFilaSign, nodeSign->pDDS, 4224);
                    memcpy(s_OrigFilaWall, nodeWall->pDDS, 4224);
                    memcpy(s_OrigFilaDoor, nodeDoor->pDDS, 4224);
                    s_FilaJpkBackedUp = true;
                }
                memcpy(nodeSign->pDDS, s_FilaSignDDS, 4224);
                memcpy(nodeWall->pDDS, s_FilaWallDDS, 4224);
                memcpy(nodeDoor->pDDS, s_FilaDoorDDS, 4224);
                s_FilaJpkActive = true;
                Logger::Info("DCRestore", "FILA common.jpk texture overrides applied (IDs 104, 105, 106)");
            }
            else
            {
                if (s_FilaJpkBackedUp)
                {
                    memcpy(nodeSign->pDDS, s_OrigFilaSign, 4224);
                    memcpy(nodeWall->pDDS, s_OrigFilaWall, 4224);
                    memcpy(nodeDoor->pDDS, s_OrigFilaDoor, 4224);
                }
                s_FilaJpkActive = false;
                Logger::Info("DCRestore", "FILA common.jpk texture overrides restored to vanilla");
            }
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            Logger::Error("DCRestore", "Exception in ApplyFilaJpkTextures");
            return false;
        }
    }

    void SetWorldTextures(bool enable)
    {
        ApplyFilaJpkTextures(enable);
        s_WorldTexActive = enable;

        // CT-DC World Textures Patch at 0x0048BA38:
        // When enabled, jumps directly over Sega's PC censorship switch statement to 0x0048C09B.
        // This prevents the engine from replacing authentic Dreamcast stage textures (from texDC1.bin /
        // texDC2.bin) with PC common.jpk censored textures (e.g. key 109 which has inverted UVs and
        // causes Levi's store texture to render upside-down / reversed).
        __try
        {
            uint8_t* pAddr = (uint8_t*)0x0048BA38;
            DWORD oldProt;
            if (VirtualProtect(pAddr, 5, PAGE_EXECUTE_READWRITE, &oldProt))
            {
                if (enable)
                {
                    uint8_t jmpPatch[5] = { 0xE9, 0x5E, 0x06, 0x00, 0x00 };
                    memcpy(pAddr, jmpPatch, 5);
                    Logger::Info("DCRestore", "World textures bypass enabled (0x48BA38 -> 0x48C09B, fixes Levi's inverted UVs)");
                }
                else
                {
                    memcpy(pAddr, s_OrigWorldTexBytes, 5);
                    Logger::Info("DCRestore", "World textures restored to vanilla (0x48BA38)");
                }
                VirtualProtect(pAddr, 5, oldProt, &oldProt);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            Logger::Error("DCRestore", "Exception in SetWorldTextures");
        }
    }

    void Update()
    {
        bool wantFila = g_Config.dcRestorationEnabled && (g_Config.dcOriginalTextures || g_Config.dcFilaDestination);
        if (wantFila && !s_FilaJpkActive)
        {
            ApplyFilaJpkTextures(true);
        }
        else if (!wantFila && s_FilaJpkActive)
        {
            ApplyFilaJpkTextures(false);
        }
    }

    void SetUIThumbnails(bool enable)
    {
        if (s_ThumbnailsActive == enable)
            return;

        __try
        {
            // Exact CT-DC hook site: 0x0048C30F
            // Jumps directly to 0x0048C62A
            uint8_t* pAddr = (uint8_t*)0x0048C30F;
            DWORD oldProt;
            if (VirtualProtect(pAddr, 5, PAGE_EXECUTE_READWRITE, &oldProt))
            {
                if (enable)
                {
                    // rel offset to 0x0048C62A = 0x0048C62A - (0x0048C30F + 5) = 0x316
                    uint8_t jmpPatch[5] = { 0xE9, 0x16, 0x03, 0x00, 0x00 };
                    memcpy(pAddr, jmpPatch, 5);
                    s_ThumbnailsActive = true;
                    Logger::Info("DCRestore", "UI thumbnails patch enabled (0x48C30F -> 0x48C62A)");
                }
                else
                {
                    memcpy(pAddr, s_OrigThumbBytes, 5);
                    s_ThumbnailsActive = false;
                    Logger::Info("DCRestore", "UI thumbnails restored to vanilla");
                }
                VirtualProtect(pAddr, 5, oldProt, &oldProt);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            Logger::Error("DCRestore", "Exception in SetUIThumbnails");
        }
    }

    void SetPassengerVoices(bool enable)
    {
        if (s_VoicesActive == enable)
            return;
        __try
        {
            // Two censorship gates in the voice selector sub_4C9B50:
            // Gate 1 at 0x004C9C2A (cmp eax, 8): brand cue assignment, jump to 0x4C9C41 (rel 0x15)
            // Gate 2 at 0x004C9E66 (test edi, edi): playback stop + cue registration, jump to 0x4C9E98 (rel 0x30)
            // Gate 2 is required as well, otherwise the assigned cue is never played.
            uint8_t* pGate1 = (uint8_t*)0x004C9C2A;
            uint8_t* pGate2 = (uint8_t*)0x004C9E66;
            DWORD oldProt;
            bool applied = true;
            if (VirtualProtect(pGate1, 2, PAGE_EXECUTE_READWRITE, &oldProt))
            {
                if (enable)
                {
                    // Short jump: EB 15
                    uint8_t jmpPatch[2] = { 0xEB, 0x15 };
                    memcpy(pGate1, jmpPatch, 2);
                }
                else
                {
                    memcpy(pGate1, s_OrigVoiceBytes, 2);
                }
                VirtualProtect(pGate1, 2, oldProt, &oldProt);
            }
            else
            {
                applied = false;
            }
            if (VirtualProtect(pGate2, sizeof(s_OrigVoiceGate2Bytes), PAGE_EXECUTE_READWRITE, &oldProt))
            {
                if (enable)
                {
                    // Short jump: EB 30
                    uint8_t jmpPatch[2] = { 0xEB, 0x30 };
                    memcpy(pGate2, jmpPatch, 2);
                }
                else
                {
                    memcpy(pGate2, s_OrigVoiceGate2Bytes, sizeof(s_OrigVoiceGate2Bytes));
                }
                VirtualProtect(pGate2, sizeof(s_OrigVoiceGate2Bytes), oldProt, &oldProt);
            }
            else
            {
                applied = false;
            }
            if (applied)
            {
                s_VoicesActive = enable;
                if (enable)
                    Logger::Info("DCRestore", "Passenger voices patch enabled (0x4C9C2A -> 0x4C9C41, 0x4C9E66 -> 0x4C9E98)");
                else
                    Logger::Info("DCRestore", "Passenger voices restored to vanilla");
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            Logger::Error("DCRestore", "Exception in SetPassengerVoices");
        }
    }

    void SetLogosZBias(bool enable)
    {
        // Handled naturally by model rendering
    }

    void Initialize()
    {
        if (s_Initialized)
            return;

        // Backup original bytes for rollback
        __try
        {
            s_OrigFilaDestByte = *(uint8_t*)0x004FDBF3;
            s_OrigPizzaTex1    = *(uint32_t*)0x00491F68;
            s_OrigPizzaTex2    = *(uint32_t*)0x00491F84;
            s_OrigPizzaTex3    = *(uint32_t*)0x00491FAF;
            s_OrigPizzaTex4    = *(uint32_t*)0x00491FCB;
            memcpy(s_OrigWorldTexBytes, (void*)0x0048BA38, 5);
            memcpy(s_OrigThumbBytes,    (void*)0x0048C30F, 5);
            memcpy(s_OrigVoiceBytes,    (void*)0x004C9C2A, 2);
            memcpy(s_OrigVoiceGate2Bytes, (void*)0x004C9E66, sizeof(s_OrigVoiceGate2Bytes));
            EnsureNamePtrsBackedUp();
            Logger::Info("DCRestore", "Original engine bytes successfully backed up for rollback");
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            Logger::Error("DCRestore", "Exception backing up original engine bytes");
        }

        s_Initialized = true;
        UpdatePatches();
    }

    void UpdatePatches()
    {
        if (!s_Initialized)
            Initialize();

        if (g_Config.dcRestorationEnabled)
        {
            SetOriginalNames(g_Config.dcOriginalNames);
            SetFilaDestination(g_Config.dcFilaDestination);
            SetPizzaHutTextures(g_Config.dcDreamcastModels);
            // FILA building textures: also bypass the censored-texture switch when the
            // FILA destination toggle is on, even if dcOriginalTextures is off
            SetWorldTextures(g_Config.dcOriginalTextures || g_Config.dcFilaDestination);
            SetUIThumbnails(g_Config.dcOriginalTextures);
            SetPassengerVoices(g_Config.dcOriginalVoices);
        }
        else
        {
            SetOriginalNames(false);
            SetFilaDestination(false);
            SetPizzaHutTextures(false);
            SetWorldTextures(false);
            SetUIThumbnails(false);
            SetPassengerVoices(false);
        }
    }

    bool IsActive()
    {
        return s_NamesActive || s_FilaDestActive || s_PizzaTexActive || s_WorldTexActive;
    }

    bool AreNamesRestored()
    {
        return s_NamesActive;
    }

    bool IsFilaDestinationRestored()
    {
        return s_FilaDestActive;
    }

    bool AreTexturesRestored()
    {
        return s_WorldTexActive;
    }

    bool AreVoicesRestored()
    {
        return s_VoicesActive;
    }
}
