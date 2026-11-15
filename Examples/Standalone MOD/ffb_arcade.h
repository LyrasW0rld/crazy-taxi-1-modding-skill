#pragma once
#include <windows.h>
#include <stdint.h>

namespace FFBArcade
{
    void Initialize();
    void Shutdown();
    void UpdatePerFrame();
    bool IsActive();
    uint8_t GetCurrentSurface();
}
