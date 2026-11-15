#pragma once
#include <windows.h>

namespace EngineFixes
{
    void Initialize();
    void Shutdown();
    void UpdateHotkeys();
    bool IsActive();

    // Live toggle functions for the Mod Settings Menu
    void ApplyWindowFix(bool enable);
    void ApplyWheelCrashFix(bool enable);
    void ApplyAnalogDeadzones(bool enable);
}

