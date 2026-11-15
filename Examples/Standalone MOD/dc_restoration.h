#pragma once
#include <windows.h>
#include <cstdint>

// ---------------------------------------------------------------------------------
// DREAMCAST RESTORATION 2.0 (CT-DC) - RE-IMPLEMENTATION & INTERFACE
// Original Mod by CookiePLMonster (Silent) & Alexvgz
// Source: https://github.com/CookiePLMonster/CT-DC
// ---------------------------------------------------------------------------------

namespace DCRestoration
{
    // Initializes and applies the Dreamcast restoration patches
    void Initialize();

    // Updates patch state according to config toggles
    void UpdatePatches();

    // Per-frame check to ensure dynamic assets (e.g. common.jpk) are hooked
    void Update();

    // Individual feature patch toggles
    void SetOriginalNames(bool enable);
    void SetFilaDestination(bool enable);
    void SetPizzaHutTextures(bool enable);
    void SetWorldTextures(bool enable);
    void SetUIThumbnails(bool enable);
    void SetPassengerVoices(bool enable);
    void SetLogosZBias(bool enable);

    // Queries whether patches are currently applied
    bool IsActive();
    bool AreNamesRestored();
    bool IsFilaDestinationRestored();
    bool AreTexturesRestored();
    bool AreVoicesRestored();
}
