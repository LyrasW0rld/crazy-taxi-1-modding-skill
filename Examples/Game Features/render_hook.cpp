#include "render_hook.h"
#include "engine_fixes.h"
#include "ffb_arcade.h"
#include "input_hook.h"
#include "game_features.h"
#include "config.h"
#include "dc_restoration.h"
#include "logger.h"

#include "../deps/imgui/imgui.h"
#include "../deps/imgui/imgui_internal.h" // IM_PI (not exposed by imgui.h in 1.90.9)
#include "../deps/imgui/backends/imgui_impl_dx9.h"
#include "../deps/imgui/backends/imgui_impl_win32.h"
#include <MinHook.h>

#include <string>
#include <vector>
#include <stdio.h>
#include <math.h>
#include <tlhelp32.h>

// Soft rose UI palette (menu, HUD & speedometer) - muted pastel accents, no neon
static const ImVec4 kColRose       = ImVec4(0.93f, 0.64f, 0.72f, 1.00f); // primary accent
static const ImVec4 kColRoseSoft   = ImVec4(0.93f, 0.64f, 0.72f, 0.35f); // accent, translucent
static const ImVec4 kColSage       = ImVec4(0.62f, 0.86f, 0.68f, 1.00f); // active / ok
static const ImVec4 kColChampagne  = ImVec4(0.96f, 0.87f, 0.65f, 1.00f); // author / highlight
static const ImVec4 kColPeriwinkle = ImVec4(0.66f, 0.76f, 0.95f, 1.00f); // live telemetry
static const ImVec4 kColPeach      = ImVec4(0.97f, 0.68f, 0.52f, 1.00f); // warnings
static const ImVec4 kColCoral      = ImVec4(0.96f, 0.55f, 0.55f, 1.00f); // errors / expert mode
static const ImVec4 kColMuted      = ImVec4(0.82f, 0.78f, 0.80f, 1.00f); // secondary text

typedef IDirect3D9* (WINAPI *tDirect3DCreate9)(UINT SDKVersion);
static tDirect3DCreate9 oDirect3DCreate9 = nullptr;

typedef HRESULT (APIENTRY *tCreateDevice)(
    IDirect3D9* pD3D,
    UINT Adapter,
    D3DDEVTYPE DeviceType,
    HWND hFocusWindow,
    DWORD BehaviorFlags,
    D3DPRESENT_PARAMETERS* pPresentationParameters,
    IDirect3DDevice9** ppReturnedDeviceInterface
);
static tCreateDevice oCreateDevice = nullptr;

typedef HRESULT (APIENTRY *tEndScene)(IDirect3DDevice9* pDevice);
typedef HRESULT (APIENTRY *tReset)(IDirect3DDevice9* pDevice, D3DPRESENT_PARAMETERS* pPresentationParameters);

static tEndScene oEndScene = nullptr;
static tReset oReset = nullptr;

static bool s_ImGuiInitialized = false;
static bool s_D3DDeviceHooked = false;

IDirect3DDevice9* g_pActiveD3DDevice = nullptr;

void SyncSegaDevicePointers(IDirect3DDevice9* pDevice)
{
    if (!pDevice) return;
    g_pActiveD3DDevice = pDevice;
    __try {
        uintptr_t pSegaRenderer = *(uintptr_t*)0x01B7AC00;
        if (pSegaRenderer && *(uintptr_t*)(pSegaRenderer + 0x18) == 0) {
            *(uintptr_t*)(pSegaRenderer + 0x18) = (uintptr_t)pDevice;
        }
        uintptr_t pContainer = *(uintptr_t*)0x01B7AB00;
        if (pContainer && *(uintptr_t*)(pContainer + 8) == 0) {
            *(uintptr_t*)(pContainer + 8) = (uintptr_t)pDevice;
        }
        uintptr_t pContainer2 = *(uintptr_t*)0x01B7AB04;
        if (pContainer2 && *(uintptr_t*)(pContainer2 + 8) == 0) {
            *(uintptr_t*)(pContainer2 + 8) = (uintptr_t)pDevice;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Teleport input buffers
static float s_TeleportX = 0.0f;
static float s_TeleportY = 0.0f;
static float s_TeleportZ = 0.0f;
static std::string s_StatusMessage = "";
static DWORD s_StatusMessageTime = 0;

void SetStatusMessage(const std::string& msg)
{
    s_StatusMessage = msg;
    s_StatusMessageTime = GetTickCount();
}

// -------------------------------------------------------------
// Compact Bottom-Left HUD (Prompt + Telemetry + Version)
// Scaled down significantly for clean, non-intrusive gameplay
// -------------------------------------------------------------
void RenderPermanentCoordinateHUD()
{
    if (g_bMenuOpen)
        return;

    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetForegroundDrawList();

    // 1. Status banner if active
    if (GetTickCount() - s_StatusMessageTime < 3500 && !s_StatusMessage.empty())
    {
        ImVec2 msgPos(14.0f, io.DisplaySize.y - 48.0f);
        ImVec2 sz = ImGui::CalcTextSize(s_StatusMessage.c_str());
        dl->AddRectFilled(ImVec2(msgPos.x - 4.0f, msgPos.y - 2.0f),
                          ImVec2(msgPos.x + sz.x + 8.0f, msgPos.y + sz.y + 2.0f),
                          IM_COL32(18, 20, 24, 210), 3.0f);
        dl->AddText(msgPos, IM_COL32(130, 220, 160, 240), s_StatusMessage.c_str());
    }

    // 2. Compact Bottom-Left Badge: Shift+F1 Menu & Player Position
    float curX = 0, curY = 0, curZ = 0;
    bool inGame = IsInGame() && GetTaxiPosition(curX, curY, curZ);

    char hudText[128];
    if (inGame)
    {
        sprintf_s(hudText, sizeof(hudText), "Shift+F1 Menu | Pos: %.0f, %.0f, %.0f", curX, curY, curZ);
    }
    else
    {
        sprintf_s(hudText, sizeof(hudText), "Shift+F1 Menu");
    }

    ImVec2 badgePos(14.0f, io.DisplaySize.y - 22.0f);
    ImVec2 badgeSz = ImGui::CalcTextSize(hudText);

    dl->AddRectFilled(ImVec2(badgePos.x - 4.0f, badgePos.y - 2.0f),
                      ImVec2(badgePos.x + badgeSz.x + 6.0f, badgePos.y + badgeSz.y + 2.0f),
                      IM_COL32(14, 16, 20, 175), 3.0f);
    dl->AddRect(ImVec2(badgePos.x - 4.0f, badgePos.y - 2.0f),
                ImVec2(badgePos.x + badgeSz.x + 6.0f, badgePos.y + badgeSz.y + 2.0f),
                IM_COL32(65, 70, 80, 160), 3.0f, 0, 1.0f);
    dl->AddText(badgePos, IM_COL32(195, 200, 210, 215), hudText);
}

// -------------------------------------------------------------
// Vintage Classic Car Instrument Cluster: Horizontal Linear Speedometer
// (0 to 200 km/h, 140 km/h Redline) + Real Digital/Mechanical Odometer
// (Total KM + Trip KM per round) with English labels.
// -------------------------------------------------------------
static void RenderSpeedometerHUD()
{
    float mph = 0.0f;
    if (!g_Config.showSpeedometer || !IsInGame() || !GetTaxiSpeed(mph))
        return;

    const float kmh = mph * 1.609344f;
    constexpr float KMH_MAX = 200.0f;

    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetForegroundDrawList();

    // Trip odometer auto-resets when starting a new round
    static bool s_PrevInGame = false;
    bool curInGame = IsInGame();
    if (curInGame && !s_PrevInGame)
    {
        g_Config.odometerTripKm = 0.0;
    }
    s_PrevInGame = curInGame;

    // Real distance accumulator: strictly clamped to physical vehicle capabilities
    // Prevents runaway mileage caused by spawn deltas or frametime hitches!
    if (curInGame && kmh > 0.5f && io.DeltaTime > 0.0f && io.DeltaTime < 0.2f)
    {
        double safeKmh = (kmh > 250.0f) ? 250.0 : (double)kmh;
        double frameDt = (io.DeltaTime > 0.1f) ? 0.1 : (double)io.DeltaTime;
        double deltaKm = (safeKmh / 3600.0) * frameDt;
        if (deltaKm > 0.004) deltaKm = 0.004; // Hard physical ceiling: max 4 meters per frame
        g_Config.odometerTotalKm += deltaKm;
        g_Config.odometerTripKm  += deltaKm;
    }

    // Periodic persist so mileage is never lost
    static DWORD s_LastOdoSave = 0;
    DWORD nowTick = GetTickCount();
    if (nowTick - s_LastOdoSave > 30000)
    {
        s_LastOdoSave = nowTick;
        SaveConfig();
    }

    // Smooth needle movement (frame-rate independent exponential approach)
    static float s_NeedleKmh = 0.0f;
    float t = io.DeltaTime * 7.5f;
    if (t > 1.0f) t = 1.0f;
    s_NeedleKmh += (kmh - s_NeedleKmh) * t;

    // Classic cable flutter / engine vibration when driving
    float flutter = 0.0f;
    if (s_NeedleKmh > 2.0f)
    {
        float speedRatio = s_NeedleKmh / KMH_MAX;
        flutter = sinf((float)GetTickCount() * 0.045f) * (0.6f * speedRatio);
    }
    float displayKmh = s_NeedleKmh + flutter;
    if (displayKmh < 0.0f) displayKmh = 0.0f;
    if (displayKmh > KMH_MAX) displayKmh = KMH_MAX;

    // Panel Geometry: Compact vintage cluster repositioned further left and lower down
    const float panelW = 190.0f;
    const float panelH = 78.0f; // Expanded to 78px to house the integrated Nitro HUD
    const float panelX1 = io.DisplaySize.x - 220.0f;
    const float panelX0 = panelX1 - panelW;
    const float panelY1 = io.DisplaySize.y - 4.0f;
    const float panelY0 = panelY1 - panelH;

    const bool isFeminineMode = g_Config.feminineGuiMode;

    // 1. Drop Shadow behind cluster
    dl->AddRectFilled(ImVec2(panelX0 - 3.0f, panelY0 - 3.0f),
                      ImVec2(panelX1 + 3.0f, panelY1 + 3.0f),
                      IM_COL32(0, 0, 0, 140), 6.0f);

    // 2. Bezel (Vibrant Barbie Hot Pink & Rose Gold in Feminine Mode, Chrome/Steel in Standard Mode)
    ImU32 bezelBg     = isFeminineMode ? IM_COL32(225, 30, 125, 252) : IM_COL32(65, 68, 76, 250);
    ImU32 bezelBorder = isFeminineMode ? IM_COL32(255, 195, 230, 255) : IM_COL32(190, 195, 205, 230);
    ImU32 bezelInner  = isFeminineMode ? IM_COL32(110, 15, 65, 255)  : IM_COL32(35, 38, 44, 255);

    dl->AddRectFilled(ImVec2(panelX0, panelY0), ImVec2(panelX1, panelY1), bezelBg, 5.0f);
    dl->AddRect(ImVec2(panelX0, panelY0), ImVec2(panelX1, panelY1), bezelBorder, 5.0f, 0, 2.0f);
    dl->AddRect(ImVec2(panelX0 + 2.0f, panelY0 + 2.0f), ImVec2(panelX1 - 2.0f, panelY1 - 2.0f), bezelInner, 4.0f, 0, 1.2f);

    // Bezel Corner Screws (Cute Rosette Pearl Jewels in Barbie Mode)
    auto DrawScrew = [&](float cx, float cy) {
        if (isFeminineMode)
        {
            dl->AddCircleFilled(ImVec2(cx, cy), 3.2f, IM_COL32(255, 105, 180, 255), 10);
            dl->AddCircleFilled(ImVec2(cx, cy), 1.6f, IM_COL32(255, 240, 252, 255), 8);
        }
        else
        {
            ImU32 screwCol = IM_COL32(170, 175, 185, 255);
            dl->AddCircleFilled(ImVec2(cx, cy), 2.5f, screwCol, 8);
            dl->AddCircle(ImVec2(cx, cy), 2.5f, IM_COL32(50, 42, 50, 255), 8, 1.0f);
            dl->AddLine(ImVec2(cx - 1.5f, cy - 1.0f), ImVec2(cx + 1.5f, cy + 1.0f), IM_COL32(40, 32, 40, 255), 1.0f);
        }
    };
    DrawScrew(panelX0 + 5.5f, panelY0 + 5.5f);
    DrawScrew(panelX1 - 5.5f, panelY0 + 5.5f);
    DrawScrew(panelX0 + 5.5f, panelY1 - 5.5f);
    DrawScrew(panelX1 - 5.5f, panelY1 - 5.5f);

    // 3. Recessed Gauge Face
    const float faceX0 = panelX0 + 5.0f;
    const float faceY0 = panelY0 + 5.0f;
    const float faceX1 = panelX1 - 5.0f;
    const float faceY1 = panelY1 - 5.0f;
    dl->AddRectFilled(ImVec2(faceX0, faceY0), ImVec2(faceX1, faceY1),
                      IM_COL32(16, 17, 20, 245), 3.0f);

    // Ambient Face Glow (Radiant Hot Pink shimmer in Feminine Mode, Tungsten Amber in Standard Mode)
    if (isFeminineMode)
    {
        dl->AddRectFilledMultiColor(ImVec2(faceX0, faceY0), ImVec2(faceX1, faceY1),
                                    IM_COL32(255, 105, 180, 55), IM_COL32(255, 40, 150, 45),
                                    IM_COL32(230, 20, 120, 25),  IM_COL32(255, 120, 190, 40));

        // Decorative Barbie Ribbon Bow Flourish at top of bezel
        float midX = (panelX0 + panelX1) * 0.5f;
        dl->AddCircleFilled(ImVec2(midX - 5.0f, panelY0 + 3.0f), 2.2f, IM_COL32(255, 190, 230, 240), 8);
        dl->AddCircleFilled(ImVec2(midX + 5.0f, panelY0 + 3.0f), 2.2f, IM_COL32(255, 190, 230, 240), 8);
        dl->AddCircleFilled(ImVec2(midX, panelY0 + 3.0f), 1.6f, IM_COL32(255, 245, 252, 255), 6);
    }
    else
    {
        dl->AddRectFilledMultiColor(ImVec2(faceX0, faceY0), ImVec2(faceX1, faceY1),
                                    IM_COL32(235, 175, 75, 30), IM_COL32(235, 175, 75, 20),
                                    IM_COL32(235, 175, 75, 10), IM_COL32(235, 175, 75, 15));
    }

    // 4. Horizontal Linear Measurement Scale (0 to 200 km/h, redline starts at 140 km/h)
    const float scaleMarginX = 14.0f;
    const float sx0 = faceX0 + scaleMarginX;
    const float sx1 = faceX1 - scaleMarginX;
    const float scaleWidth = sx1 - sx0;
    const float scaleY = faceY0 + 22.0f; // Scale line baseline

    // Inset track groove for the scale
    dl->AddRectFilled(ImVec2(sx0 - 2.0f, scaleY - 1.5f), ImVec2(sx1 + 2.0f, scaleY + 2.0f),
                      IM_COL32(8, 9, 12, 230), 1.0f);

    // Base horizontal scale line: normal zone (0-140 km/h) in warm silver
    const float redlineKmh = 140.0f;
    const float redlineX = sx0 + (redlineKmh / KMH_MAX) * scaleWidth;
    dl->AddLine(ImVec2(sx0, scaleY), ImVec2(redlineX, scaleY),
                IM_COL32(220, 215, 200, 220), 1.8f);
    // Redline danger zone (140-200 km/h) in safety crimson (only reachable downhill/nitro)
    dl->AddLine(ImVec2(redlineX, scaleY), ImVec2(sx1, scaleY),
                IM_COL32(235, 50, 40, 245), 2.2f);

    // Minor ticks every 10 km/h
    for (int v = 10; v < (int)KMH_MAX; v += 10)
    {
        if ((v % 20) == 0) continue; // Skip major ticks
        float tx = sx0 + ((float)v / KMH_MAX) * scaleWidth;
        ImU32 tickCol = (v >= 140) ? IM_COL32(225, 95, 75, 200) : IM_COL32(195, 190, 175, 170);
        dl->AddLine(ImVec2(tx, scaleY - 4.0f), ImVec2(tx, scaleY), tickCol, 1.0f);
    }

    // Major ticks every 20 km/h with numeral labels every 40 km/h (0, 40, 80, 120, 160, 200)
    char buf[16];
    for (int v = 0; v <= (int)KMH_MAX; v += 20)
    {
        float tx = sx0 + ((float)v / KMH_MAX) * scaleWidth;
        bool isRed = (v >= 140);
        ImU32 tickCol = isRed ? IM_COL32(245, 70, 50, 255) : IM_COL32(240, 235, 220, 235);

        // Major tick mark
        dl->AddLine(ImVec2(tx, scaleY - 7.0f), ImVec2(tx, scaleY), tickCol, 1.5f);

        // Numeral label above tick (every 40 km/h to avoid cramping)
        if ((v % 40) == 0)
        {
            sprintf_s(buf, sizeof(buf), "%d", v);
            ImVec2 sz = ImGui::CalcTextSize(buf);
            dl->AddText(ImVec2(tx - sz.x * 0.5f, scaleY - 20.0f), tickCol, buf);
        }
    }

    // 5. Classic Orange Needle / Pointer
    float frac = displayKmh / KMH_MAX;
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;
    const float needleX = sx0 + frac * scaleWidth;

    // Needle shadow
    dl->AddLine(ImVec2(needleX + 1.2f, scaleY - 11.0f), ImVec2(needleX + 1.2f, scaleY + 7.0f),
                IM_COL32(0, 0, 0, 160), 2.5f);
    const bool isFem = g_Config.feminineGuiMode;

    // Needle body: fluorescent safety orange in standard mode, hot magenta pink in feminine mode
    const ImU32 pointerCol = isFem ? IM_COL32(255, 85, 160, 255) : IM_COL32(255, 95, 12, 255);
    dl->AddLine(ImVec2(needleX, scaleY - 11.0f), ImVec2(needleX, scaleY + 6.0f),
                pointerCol, 2.0f);
    // Triangular pointer indicator at the top
    ImVec2 pTop(needleX, scaleY - 12.0f);
    ImVec2 pLeft(needleX - 3.0f, scaleY - 7.5f);
    ImVec2 pRight(needleX + 3.0f, scaleY - 7.5f);
    dl->AddTriangleFilled(pTop, pLeft, pRight, pointerCol);
    // Tiny needle pivot bead
    dl->AddCircleFilled(ImVec2(needleX, scaleY), 2.0f, isFem ? IM_COL32(255, 210, 230, 255) : IM_COL32(255, 230, 190, 255), 8);

    // 5b. Integrated Horizontal Nitro Bar (placed directly below speed scale and above KM odometer)
    if (g_Config.nitroEnabled)
    {
        const float energy = GetNitroEnergy();
        const float maxEnergy = GetNitroMaxEnergy();
        float nitroFrac = (maxEnergy > 0.0f) ? (energy / maxEnergy) : 0.0f;
        if (nitroFrac < 0.0f) nitroFrac = 0.0f;
        if (nitroFrac > 1.0f) nitroFrac = 1.0f;

        const bool isBoosting  = IsNitroActive();
        const bool isLockedOut = IsNitroLockedOut();

        const float nbX0 = sx0;
        const float nbX1 = sx1;
        const float nbY0 = scaleY + 8.5f;
        const float nbY1 = nbY0 + 4.0f;

        // Recessed track slot
        dl->AddRectFilled(ImVec2(nbX0 - 1.0f, nbY0 - 1.0f), ImVec2(nbX1 + 1.0f, nbY1 + 1.0f),
                          IM_COL32(10, 12, 16, 235), 2.0f);
        dl->AddRect(ImVec2(nbX0 - 1.0f, nbY0 - 1.0f), ImVec2(nbX1 + 1.0f, nbY1 + 1.0f),
                    isFem ? IM_COL32(110, 60, 80, 200) : IM_COL32(45, 50, 60, 200), 2.0f, 0, 1.0f);

        // Fluid fill color: blue in standard mode, vibrant rose orchid in feminine mode
        ImU32 nitroCol;
        if (isFem)
        {
            if (isBoosting)       nitroCol = IM_COL32(255, 120, 180, 255); // Fluorescent hot pink
            else if (isLockedOut) nitroCol = IM_COL32(235, 60, 90, 220);   // Ruby alert
            else                  nitroCol = IM_COL32(220, 75, 175, 245);  // Radiant rose orchid
        }
        else
        {
            if (isBoosting)       nitroCol = IM_COL32(255, 150, 25, 255);  // Fiery orange boost
            else if (isLockedOut) nitroCol = IM_COL32(235, 45, 45, 220);   // Warning lockout
            else                  nitroCol = IM_COL32(35, 150, 255, 245);  // Electric cobalt blue
        }

        float fillW = (nbX1 - nbX0) * nitroFrac;
        if (fillW > 0.5f)
        {
            dl->AddRectFilled(ImVec2(nbX0, nbY0), ImVec2(nbX0 + fillW, nbY1), nitroCol, 2.0f);
        }
    }

    // 6. Lower Instrument Section: Centered Mechanical Roller Odometer (Total KM only)
    const float odoBoxW = 75.0f;
    const float odoBoxH = 16.0f;
    const float odoX0 = faceX0 + (faceX1 - faceX0 - odoBoxW) * 0.5f;
    const float odoY0 = faceY1 - odoBoxH - 3.5f;
    const float odoX1 = odoX0 + odoBoxW;
    const float odoY1 = odoY0 + odoBoxH;

    // Recessed odometer frame with chrome bevel (or hot pink filigree border in Barbie mode)
    dl->AddRectFilled(ImVec2(odoX0, odoY0), ImVec2(odoX1, odoY1), IM_COL32(10, 11, 14, 255), 2.0f);
    dl->AddRect(ImVec2(odoX0, odoY0), ImVec2(odoX1, odoY1),
                isFem ? IM_COL32(255, 105, 180, 240) : IM_COL32(90, 95, 105, 220), 2.0f, 0, 1.2f);

    if (isFem)
    {
        // Cute sparkling pearl rosettes flanking the odometer
        dl->AddCircleFilled(ImVec2(odoX0 - 6.0f, odoY0 + odoBoxH * 0.5f), 2.5f, IM_COL32(255, 105, 180, 255), 8);
        dl->AddCircleFilled(ImVec2(odoX1 + 6.0f, odoY0 + odoBoxH * 0.5f), 2.5f, IM_COL32(255, 105, 180, 255), 8);
        dl->AddCircleFilled(ImVec2(odoX0 - 6.0f, odoY0 + odoBoxH * 0.5f), 1.2f, IM_COL32(255, 240, 252, 255), 6);
        dl->AddCircleFilled(ImVec2(odoX1 + 6.0f, odoY0 + odoBoxH * 0.5f), 1.2f, IM_COL32(255, 240, 252, 255), 6);
    }

    // Centered English "TOTAL KM" label above odometer drums
    const char* odoLabel = "TOTAL KM";
    ImVec2 lblSz = ImGui::CalcTextSize(odoLabel);
    dl->AddText(ImVec2(odoX0 + (odoBoxW - lblSz.x) * 0.5f, odoY0 - 11.0f), IM_COL32(175, 170, 155, 200), odoLabel);

    // Total KM Digits (4 whole digits + 1 tenths digit = 5 tumblers)
    int totalInt = (int)g_Config.odometerTotalKm;
    if (totalInt > 9999) totalInt %= 10000;
    int digit1000 = (totalInt / 1000) % 10;
    int digit100  = (totalInt / 100) % 10;
    int digit10   = (totalInt / 10) % 10;
    int digit1    = totalInt % 10;
    int digit10th = (int)(fmod(g_Config.odometerTotalKm * 10.0, 10.0));

    const float drumW = odoBoxW / 5.0f;
    for (int d = 0; d < 5; ++d)
    {
        float dx0 = odoX0 + d * drumW;
        float dx1 = dx0 + drumW;
        bool isTenths = (d == 4);

        if (isTenths)
        {
            ImU32 tenthsCol = isFem ? IM_COL32(215, 60, 115, 240) : IM_COL32(195, 55, 35, 240);
            dl->AddRectFilled(ImVec2(dx0 + 0.5f, odoY0 + 1.0f), ImVec2(dx1 - 0.5f, odoY1 - 1.0f),
                              tenthsCol);
        }

        if (d > 0)
        {
            dl->AddLine(ImVec2(dx0, odoY0 + 1.0f), ImVec2(dx0, odoY1 - 1.0f),
                        IM_COL32(35, 38, 45, 240), 1.0f);
        }

        char digChar = '0';
        if (d == 0) digChar = (char)('0' + digit1000);
        else if (d == 1) digChar = (char)('0' + digit100);
        else if (d == 2) digChar = (char)('0' + digit10);
        else if (d == 3) digChar = (char)('0' + digit1);
        else if (d == 4) digChar = (char)('0' + digit10th);

        char dStr[2] = { digChar, 0 };
        ImVec2 dSz = ImGui::CalcTextSize(dStr);
        float dtx = dx0 + (drumW - dSz.x) * 0.5f;
        float dty = odoY0 + (odoBoxH - dSz.y) * 0.5f;
        dl->AddText(ImVec2(dtx, dty),
                    isTenths ? IM_COL32(255, 255, 255, 255) : IM_COL32(245, 240, 225, 240),
                    dStr);
    }
}

static void RenderNitroHUD()
{
    if (!g_Config.nitroEnabled || !IsInGame())
        return;

    const float energy = GetNitroEnergy();
    const float maxEnergy = GetNitroMaxEnergy();
    float frac = (maxEnergy > 0.0f) ? (energy / maxEnergy) : 0.0f;
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;

    const bool isBoosting   = IsNitroActive();
    const bool isLockedOut  = IsNitroLockedOut();
    const float delayTimer  = GetNitroDelayTimer();

    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetForegroundDrawList();

    // Bottom-right placement (scaled down 10% and shifted 20% down toward bottom edge)
    const float botRightX = io.DisplaySize.x - 22.0f;
    const float botRightY = io.DisplaySize.y - 6.0f;

    const float bodyW = 39.6f;
    const float bodyH = 90.0f;
    const float bodyX0 = botRightX - bodyW;
    const float bodyY1 = botRightY;
    const float bodyY0 = bodyY1 - bodyH;

    // Valve & neck assembly on top of the bottle (10% smaller)
    const float neckW = 12.6f;
    const float neckH = 8.1f;
    const float neckX0 = bodyX0 + (bodyW - neckW) * 0.5f;
    const float neckY0 = bodyY0 - neckH;

    const float valveW = 21.6f;
    const float valveH = 6.3f;
    const float valveX0 = bodyX0 + (bodyW - valveW) * 0.5f;
    const float valveY0 = neckY0 - valveH;

    const float dialRadius = 5.4f;
    const ImVec2 dialCenter(neckX0 - 3.5f, neckY0 + 2.0f);

    // 1. Drop shadow / dark silhouette backing
    dl->AddRectFilled(ImVec2(bodyX0 - 3.0f, valveY0 - 3.0f),
                      ImVec2(bodyX0 + bodyW + 3.0f, bodyY1 + 3.0f),
                      IM_COL32(0, 0, 0, 110), 8.0f);

    // 2. Valve Knob (metallic brass/silver)
    dl->AddRectFilled(ImVec2(valveX0, valveY0), ImVec2(valveX0 + valveW, valveY0 + valveH),
                      IM_COL32(185, 190, 200, 240), 2.0f);
    dl->AddRect(ImVec2(valveX0, valveY0), ImVec2(valveX0 + valveW, valveY0 + valveH),
                IM_COL32(70, 75, 85, 255), 2.0f, 0, 1.5f);

    // 3. Neck fitting
    dl->AddRectFilled(ImVec2(neckX0, neckY0), ImVec2(neckX0 + neckW, neckY0 + neckH + 2.0f),
                      IM_COL32(150, 155, 165, 240));
    dl->AddRect(ImVec2(neckX0, neckY0), ImVec2(neckX0 + neckW, neckY0 + neckH),
                IM_COL32(60, 65, 75, 255), 0.0f, 0, 1.5f);

    // 4. Pressure gauge dial
    dl->AddCircleFilled(dialCenter, dialRadius, IM_COL32(235, 235, 240, 245), 12);
    dl->AddCircle(dialCenter, dialRadius, IM_COL32(60, 65, 75, 255), 12, 1.5f);
    // Gauge needle
    dl->AddLine(dialCenter, ImVec2(dialCenter.x + 3.5f, dialCenter.y - 3.5f),
                IM_COL32(210, 30, 30, 255), 1.5f);

    // 5. Main Canister Body (NOS Bottle)
    // Dynamic border: Boosting (Orange), Locked Out (Red pulse), Delay (Yellow/Amber), Normal (Cyan/Blue)
    ImU32 borderCol;
    float borderThick;
    if (isBoosting)
    {
        borderCol = IM_COL32(255, 140, 20, 255);
        borderThick = 2.5f;
    }
    else if (isLockedOut)
    {
        int pulse = (int)(sinf((float)GetTickCount() * 0.008f) * 45.0f + 210.0f);
        borderCol = IM_COL32(pulse, 40, 40, 245);
        borderThick = 2.0f;
    }
    else if (delayTimer > 0.05f)
    {
        borderCol = IM_COL32(235, 185, 45, 230);
        borderThick = 1.8f;
    }
    else
    {
        borderCol = IM_COL32(65, 145, 195, 200);
        borderThick = 1.5f;
    }

    // Body background
    dl->AddRectFilled(ImVec2(bodyX0, bodyY0), ImVec2(bodyX0 + bodyW, bodyY1),
                      IM_COL32(14, 18, 25, 210), 6.0f);
    dl->AddRect(ImVec2(bodyX0, bodyY0), ImVec2(bodyX0 + bodyW, bodyY1),
                borderCol, 6.0f, 0, borderThick);

    // 6. Vertical Fluid Progress Bar (Inner chamber)
    const float padX = 4.0f;
    const float padY = 5.0f;
    const float innerX0 = bodyX0 + padX;
    const float innerX1 = bodyX0 + bodyW - padX;
    const float innerY0 = bodyY0 + padY;
    const float innerY1 = bodyY1 - padY;
    const float innerH  = innerY1 - innerY0;

    // Empty chamber background
    dl->AddRectFilled(ImVec2(innerX0, innerY0), ImVec2(innerX1, innerY1),
                      IM_COL32(10, 14, 20, 190), 4.0f);

    // Vertical fill (bottom to top)
    const float fillH = innerH * frac;
    const float fillY0 = innerY1 - fillH;
    if (fillH > 0.5f)
    {
        ImU32 colTop = isBoosting  ? IM_COL32(255, 210, 60, 245) :
                       isLockedOut ? IM_COL32(245, 70, 50, 230)  :
                                     IM_COL32(80, 220, 255, 240);
        ImU32 colBot = isBoosting  ? IM_COL32(230, 60, 20, 245)  :
                       isLockedOut ? IM_COL32(180, 20, 20, 230)  :
                                     IM_COL32(0, 120, 225, 240);
        dl->AddRectFilledMultiColor(ImVec2(innerX0, fillY0), ImVec2(innerX1, innerY1),
                                    colTop, colTop, colBot, colBot);
    }

    // 40% re-arm threshold marker line (30.0 units)
    float ty40 = innerY1 - innerH * 0.40f;
    ImU32 col40 = isLockedOut ? IM_COL32(255, 75, 75, 230) : IM_COL32(255, 215, 80, 140);
    dl->AddLine(ImVec2(innerX0, ty40), ImVec2(innerX1, ty40), col40, 1.5f);

    // Tick marks along the side (25%, 50%, 75%)
    for (int t = 1; t <= 3; ++t)
    {
        float p = t * 0.25f;
        float ty = innerY1 - innerH * p;
        dl->AddLine(ImVec2(innerX1 - 5.0f, ty), ImVec2(innerX1 - 1.0f, ty),
                    IM_COL32(255, 255, 255, 120), 1.0f);
    }

    // 7. Labels & numeric readouts
    const char* title = "NITRO";
    ImU32 titleCol = IM_COL32(220, 245, 255, 235);
    if (isBoosting)
    {
        title = "BOOST!";
        titleCol = IM_COL32(255, 240, 180, 255);
    }
    else if (isLockedOut)
    {
        title = "LOCK";
        titleCol = IM_COL32(255, 90, 90, 250);
    }
    else if (delayTimer > 0.05f)
    {
        title = "WAIT";
        titleCol = IM_COL32(255, 220, 90, 240);
    }

    const ImVec2 titleSz = ImGui::CalcTextSize(title);
    const float tx = bodyX0 + (bodyW - titleSz.x) * 0.5f;
    const float ty = bodyY0 + 10.0f;
    dl->AddText(ImVec2(tx + 1.0f, ty + 1.0f), IM_COL32(0, 0, 0, 220), title);
    dl->AddText(ImVec2(tx, ty), titleCol, title);

    // Subtitle / status readout (countdown timer or 40% re-arm prompt)
    if (delayTimer > 0.05f)
    {
        char cdBuf[16];
        sprintf_s(cdBuf, sizeof(cdBuf), "%.1fs", delayTimer);
        const ImVec2 cdSz = ImGui::CalcTextSize(cdBuf);
        const float cdx = bodyX0 + (bodyW - cdSz.x) * 0.5f;
        const float cdy = ty + titleSz.y + 1.0f;
        dl->AddText(ImVec2(cdx + 1.0f, cdy + 1.0f), IM_COL32(0, 0, 0, 220), cdBuf);
        dl->AddText(ImVec2(cdx, cdy), IM_COL32(255, 230, 110, 240), cdBuf);
    }
    else if (isLockedOut)
    {
        const char* lockMsg = "40%";
        const ImVec2 lSz = ImGui::CalcTextSize(lockMsg);
        const float lx = bodyX0 + (bodyW - lSz.x) * 0.5f;
        const float ly = ty + titleSz.y + 1.0f;
        dl->AddText(ImVec2(lx + 1.0f, ly + 1.0f), IM_COL32(0, 0, 0, 220), lockMsg);
        dl->AddText(ImVec2(lx, ly), IM_COL32(255, 110, 110, 240), lockMsg);
    }

    // Numeric energy value
    char numBuf[16];
    sprintf_s(numBuf, sizeof(numBuf), "%.0f", energy);
    const ImVec2 numSz = ImGui::CalcTextSize(numBuf);
    const float nx = bodyX0 + (bodyW - numSz.x) * 0.5f;
    const float ny = bodyY1 - 22.0f;
    dl->AddText(ImVec2(nx + 1.0f, ny + 1.0f), IM_COL32(0, 0, 0, 230), numBuf);
    dl->AddText(ImVec2(nx, ny), IM_COL32(255, 255, 255, 245), numBuf);
}

// ---------------------------------------------------------------------------------
// Keybinding Capture State
// ---------------------------------------------------------------------------------
static int s_WaitingForAction = -1;
static DWORD s_ListenStartTime = 0;

static int SafeReadInt(uintptr_t addr, int fallback = 0)
{
    __try {
        return *(int*)addr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return fallback;
    }
}

static float SafeReadFloat(uintptr_t addr, float fallback = 0.0f)
{
    __try {
        return *(float*)addr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return fallback;
    }
}

static int16_t SafeReadShort(uintptr_t addr, int16_t fallback = 0)
{
    __try {
        return *(int16_t*)addr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return fallback;
    }
}

static uint8_t SafeReadByte(uintptr_t addr, uint8_t fallback = 0)
{
    __try {
        return *(uint8_t*)addr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return fallback;
    }
}

static void SafeWriteInt(uintptr_t addr, int val)
{
    __try {
        *(int*)addr = val;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static void SafeWriteByte(uintptr_t addr, uint8_t val)
{
    __try {
        *(uint8_t*)addr = val;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

struct SubModPlugin
{
    HMODULE hModule = NULL;
    std::string name;
    std::string author;
    std::string version;
    std::string description;
    bool (*pfnIsEnabled)() = nullptr;
    void (*pfnSetEnabled)(bool) = nullptr;
    void (*pfnRenderUI)(void*) = nullptr;
    void (*pfnInit)(void*, IDirect3DDevice9*) = nullptr;

    // Real-Time Safety Gate State
    int   errorCount = 0;
    bool  isQuarantined = false;
    DWORD lastExceptionCode = 0;
    DWORD lastErrorTime = 0;
    int   consecutiveSuccessFrames = 0;
};

// Real-Time SubMod Safety Gate: Unrestricted execution with real-time exception trapping and auto-quarantine
static bool GateCallIsEnabled(SubModPlugin& mod)
{
    if (mod.isQuarantined || !mod.pfnIsEnabled) return false;
    __try {
        return mod.pfnIsEnabled();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        mod.lastExceptionCode = GetExceptionCode();
        mod.lastErrorTime = GetTickCount();
        mod.errorCount++;
        if (mod.errorCount >= 3)
        {
            mod.isQuarantined = true;
        }
        return false;
    }
}

static void GateCallSetEnabled(SubModPlugin& mod, bool val)
{
    if (mod.isQuarantined || !mod.pfnSetEnabled) return;
    __try {
        mod.pfnSetEnabled(val);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        mod.lastExceptionCode = GetExceptionCode();
        mod.lastErrorTime = GetTickCount();
        mod.errorCount++;
        if (mod.errorCount >= 3)
        {
            mod.isQuarantined = true;
        }
    }
}

static bool GateCallRenderUI(SubModPlugin& mod, void* ctx)
{
    if (mod.isQuarantined || !mod.pfnRenderUI) return false;
    __try {
        mod.pfnRenderUI(ctx);
        mod.consecutiveSuccessFrames++;
        if (mod.consecutiveSuccessFrames > 120 && mod.errorCount > 0)
        {
            mod.errorCount--;
            mod.consecutiveSuccessFrames = 0;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        mod.lastExceptionCode = GetExceptionCode();
        mod.lastErrorTime = GetTickCount();
        mod.errorCount++;
        mod.consecutiveSuccessFrames = 0;
        if (mod.errorCount >= 3)
        {
            mod.isQuarantined = true;
        }
        return false;
    }
}

static void GateCallInitSubMod(SubModPlugin& mod, void* ctx, IDirect3DDevice9* dev)
{
    if (!mod.pfnInit) return;
    __try {
        mod.pfnInit(ctx, dev);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        mod.lastExceptionCode = GetExceptionCode();
        mod.lastErrorTime = GetTickCount();
        mod.errorCount++;
        if (mod.errorCount >= 3)
        {
            mod.isQuarantined = true;
        }
    }
}

static bool SafeCallIsEnabled(bool (*pfn)())
{
    __try {
        return pfn ? pfn() : false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static void SafeCallSetEnabled(void (*pfn)(bool), bool val)
{
    __try {
        if (pfn) pfn(val);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static bool SafeCallRenderUI(void (*pfn)(void*), void* ctx)
{
    __try {
        if (pfn) pfn(ctx);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool SafeGetSubModStr(const char* (*pfn)(), char* outBuf, size_t outBufSize, const char* fallback = "")
{
    if (!outBuf || outBufSize == 0) return false;
    outBuf[0] = '\0';
    if (!pfn)
    {
        if (fallback) strncpy_s(outBuf, outBufSize, fallback, _TRUNCATE);
        return false;
    }
    __try {
        const char* s = pfn();
        if (!s || IsBadStringPtrA(s, 2048))
        {
            if (fallback) strncpy_s(outBuf, outBufSize, fallback, _TRUNCATE);
            return false;
        }
        strncpy_s(outBuf, outBufSize, s, _TRUNCATE);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        if (fallback) strncpy_s(outBuf, outBufSize, fallback, _TRUNCATE);
        return false;
    }
}

static void SafeCallInitSubMod(void (*pfn)(void*, IDirect3DDevice9*), void* ctx, IDirect3DDevice9* dev)
{
    __try {
        if (pfn) pfn(ctx, dev);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static std::vector<SubModPlugin> s_DiscoveredSubMods;
static DWORD s_LastSubModScanTime = 0;

static void RefreshSubMods()
{
    DWORD now = GetTickCount();
    if (now - s_LastSubModScanTime < 3000)
        return;
    s_LastSubModScanTime = now;

    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (hSnap == INVALID_HANDLE_VALUE)
        return;

    MODULEENTRY32W me;
    me.dwSize = sizeof(me);
    if (Module32FirstW(hSnap, &me))
    {
        do
        {
            // Skip core framework and host modules
            if (_wcsicmp(me.szModule, L"Crazy Taxi.exe") == 0 ||
                _wcsicmp(me.szModule, L"Framework_MOD.dll") == 0 ||
                _wcsicmp(me.szModule, L"LyrasTextureMod.dll") == 0 ||
                _wcsicmp(me.szModule, L"dinput8.dll") == 0 ||
                _wcsicmp(me.szModule, L"steam_api.dll") == 0)
            {
                continue;
            }

            bool alreadyRegistered = false;
            for (const auto& existing : s_DiscoveredSubMods)
            {
                if (existing.hModule == me.hModule)
                {
                    alreadyRegistered = true;
                    break;
                }
            }
            if (alreadyRegistered) continue;

            typedef const char* (*tGetSubModStr)();
            typedef bool (*tGetSubModBool)();
            typedef void (*tSetSubModBool)(bool);
            typedef void (*tSubModUI)(void*);
            typedef void (*tSubModInit)(void*, IDirect3DDevice9*);

            tGetSubModStr pfnName = (tGetSubModStr)GetProcAddress(me.hModule, "GetSubModName");
            if (pfnName)
            {
                SubModPlugin plugin;
                plugin.hModule = me.hModule;

                char modNameA[MAX_MODULE_NAME32 + 1] = { 0 };
                WideCharToMultiByte(CP_ACP, 0, me.szModule, -1, modNameA, sizeof(modNameA), NULL, NULL);

                char bufName[256] = { 0 };
                char bufAuthor[256] = { 0 };
                char bufVer[64] = { 0 };
                char bufDesc[512] = { 0 };

                SafeGetSubModStr(pfnName, bufName, sizeof(bufName), modNameA);
                plugin.name = bufName;

                tGetSubModStr pfnAuthor = (tGetSubModStr)GetProcAddress(me.hModule, "GetSubModAuthor");
                SafeGetSubModStr(pfnAuthor, bufAuthor, sizeof(bufAuthor), "Community");
                plugin.author = bufAuthor;

                tGetSubModStr pfnVer = (tGetSubModStr)GetProcAddress(me.hModule, "GetSubModVersion");
                SafeGetSubModStr(pfnVer, bufVer, sizeof(bufVer), "1.0");
                plugin.version = bufVer;

                tGetSubModStr pfnDesc = (tGetSubModStr)GetProcAddress(me.hModule, "GetSubModDescription");
                SafeGetSubModStr(pfnDesc, bufDesc, sizeof(bufDesc), "");
                plugin.description = bufDesc;

                plugin.pfnIsEnabled = (tGetSubModBool)GetProcAddress(me.hModule, "IsSubModEnabled");
                plugin.pfnSetEnabled = (tSetSubModBool)GetProcAddress(me.hModule, "SetSubModEnabled");
                plugin.pfnRenderUI = (tSubModUI)GetProcAddress(me.hModule, "RenderSubModUI");
                plugin.pfnInit = (tSubModInit)GetProcAddress(me.hModule, "InitSubMod");

                if (plugin.pfnInit)
                {
                    GateCallInitSubMod(plugin, (void*)ImGui::GetCurrentContext(), g_pActiveD3DDevice);
                }

                s_DiscoveredSubMods.push_back(plugin);
            }
        } while (Module32NextW(hSnap, &me));
    }
    CloseHandle(hSnap);
}

static int s_SelectedModIndex = 0; // 0 = Lyras Framework Mod, 1 = Dreamcast Restoration 2.0 (CT-DC)
static int s_ForceLyraTab = -1;

static void RenderModMenuInternal()
{

    ImVec2 defaultMenuSize(
        g_Config.menuWindowWidth >= 460.0f ? g_Config.menuWindowWidth : 540.0f,
        g_Config.menuWindowHeight >= 300.0f ? g_Config.menuWindowHeight : 420.0f
    );
    ImGui::SetNextWindowPos(ImVec2(100.0f, 30.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(defaultMenuSize, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(460.0f, 300.0f), ImVec2(1920.0f, 1080.0f));

    if (g_Config.feminineGuiMode)
    {
        // Radiant Barbie Glamour Theme: Hot Barbie Pink, Bubblegum & Shimmering Rose
        ImGui::PushStyleColor(ImGuiCol_WindowBg,         ImVec4(0.18f, 0.05f, 0.14f, 0.96f)); // Deep velvet berry
        ImGui::PushStyleColor(ImGuiCol_TitleBg,          ImVec4(0.88f, 0.12f, 0.55f, 0.96f)); // Hot Barbie Pink (#FF1493)
        ImGui::PushStyleColor(ImGuiCol_TitleBgActive,    ImVec4(1.00f, 0.20f, 0.65f, 1.00f)); // Electric Barbie Magenta
        ImGui::PushStyleColor(ImGuiCol_Header,           ImVec4(0.80f, 0.22f, 0.55f, 0.55f)); // Shimmer rose header
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered,    ImVec4(0.95f, 0.35f, 0.70f, 0.85f));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive,     ImVec4(1.00f, 0.45f, 0.78f, 0.95f));
        ImGui::PushStyleColor(ImGuiCol_Button,           ImVec4(0.88f, 0.18f, 0.56f, 0.88f)); // Barbie Pink Button
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,    ImVec4(1.00f, 0.38f, 0.72f, 0.98f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,     ImVec4(1.00f, 0.55f, 0.82f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_FrameBg,          ImVec4(0.32f, 0.10f, 0.25f, 0.94f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,   ImVec4(0.48f, 0.16f, 0.38f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive,    ImVec4(0.62f, 0.22f, 0.48f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_SliderGrab,       ImVec4(1.00f, 0.45f, 0.78f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ImVec4(1.00f, 0.75f, 0.90f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_CheckMark,        ImVec4(1.00f, 0.85f, 0.94f, 1.00f)); // Pearl checkmark
        ImGui::PushStyleColor(ImGuiCol_Separator,        ImVec4(1.00f, 0.35f, 0.70f, 0.65f));
        ImGui::PushStyleColor(ImGuiCol_Border,           ImVec4(1.00f, 0.45f, 0.78f, 0.80f)); // Filigree border
        ImGui::PushStyleColor(ImGuiCol_Text,             ImVec4(1.00f, 0.95f, 0.98f, 1.00f)); // Sparkle white
    }
    else
    {
        // Standard Dark Arcade Theme: Charcoal, slate, racing accents & Crazy Taxi gold
        ImGui::PushStyleColor(ImGuiCol_WindowBg,         ImVec4(0.08f, 0.08f, 0.10f, 0.94f));
        ImGui::PushStyleColor(ImGuiCol_TitleBg,          ImVec4(0.12f, 0.13f, 0.16f, 0.95f));
        ImGui::PushStyleColor(ImGuiCol_TitleBgActive,    ImVec4(0.22f, 0.24f, 0.30f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_Header,           ImVec4(0.20f, 0.22f, 0.26f, 0.50f));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered,    ImVec4(0.28f, 0.32f, 0.38f, 0.70f));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive,     ImVec4(0.36f, 0.40f, 0.48f, 0.85f));
        ImGui::PushStyleColor(ImGuiCol_Button,           ImVec4(0.22f, 0.24f, 0.28f, 0.85f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,    ImVec4(0.32f, 0.36f, 0.42f, 0.95f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,     ImVec4(0.42f, 0.48f, 0.56f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_FrameBg,          ImVec4(0.14f, 0.15f, 0.18f, 0.92f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,   ImVec4(0.20f, 0.22f, 0.26f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive,    ImVec4(0.26f, 0.28f, 0.32f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_SliderGrab,       ImVec4(0.92f, 0.72f, 0.15f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ImVec4(1.00f, 0.82f, 0.25f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_CheckMark,        ImVec4(0.95f, 0.75f, 0.18f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_Separator,        ImVec4(0.30f, 0.32f, 0.38f, 0.45f));
        ImGui::PushStyleColor(ImGuiCol_Border,           ImVec4(0.38f, 0.42f, 0.48f, 0.45f));
        ImGui::PushStyleColor(ImGuiCol_Text,             ImVec4(0.92f, 0.93f, 0.95f, 1.00f));
    }

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, 6.0f);

    bool p_open = true;
    const char* winTitle = g_Config.feminineGuiMode ? "~*~  BARBIE GLAMOUR MOD MANAGER  ~*~" : "CRAZY TAXI MOD MANAGER & FRAMEWORK v1.2";
    if (ImGui::Begin(winTitle, &p_open, ImGuiWindowFlags_NoCollapse))
    {
        ImVec2 curWinSize = ImGui::GetWindowSize();
        if (curWinSize.x >= 460.0f && curWinSize.y >= 300.0f)
        {
            if (fabsf(curWinSize.x - g_Config.menuWindowWidth) > 1.0f || fabsf(curWinSize.y - g_Config.menuWindowHeight) > 1.0f)
            {
                g_Config.menuWindowWidth = curWinSize.x;
                g_Config.menuWindowHeight = curWinSize.y;
            }
        }
        if (g_Config.feminineGuiMode)
        {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 wPos = ImGui::GetWindowPos();
            ImVec2 wSize = ImGui::GetWindowSize();

            // Ornate Outer Filigree Double Border: Hot Barbie Pink & Pearl White
            dl->AddRect(wPos, ImVec2(wPos.x + wSize.x, wPos.y + wSize.y),
                        IM_COL32(255, 30, 145, 255), 10.0f, 0, 2.0f);
            dl->AddRect(ImVec2(wPos.x + 3.0f, wPos.y + 3.0f),
                        ImVec2(wPos.x + wSize.x - 3.0f, wPos.y + wSize.y - 3.0f),
                        IM_COL32(255, 210, 235, 180), 8.0f, 0, 1.0f);

            // 4 Decorative Filigree Corner Flourishes ("Schnörkel")
            auto DrawCornerFlourish = [&](float cx, float cy, float dx, float dy) {
                // Curved filigree arc
                dl->AddBezierCubic(
                    ImVec2(cx, cy + dy * 22.0f),
                    ImVec2(cx + dx * 8.0f, cy + dy * 14.0f),
                    ImVec2(cx + dx * 14.0f, cy + dy * 8.0f),
                    ImVec2(cx + dx * 22.0f, cy),
                    IM_COL32(255, 130, 195, 230), 1.6f
                );
                // Sparkling pearl center bead
                dl->AddCircleFilled(ImVec2(cx + dx * 10.0f, cy + dy * 10.0f), 2.5f,
                                    IM_COL32(255, 240, 250, 255), 8);
                // Sparkle cross tick
                dl->AddLine(ImVec2(cx + dx * 10.0f - 4.0f, cy + dy * 10.0f),
                            ImVec2(cx + dx * 10.0f + 4.0f, cy + dy * 10.0f),
                            IM_COL32(255, 105, 180, 240), 1.0f);
                dl->AddLine(ImVec2(cx + dx * 10.0f, cy + dy * 10.0f - 4.0f),
                            ImVec2(cx + dx * 10.0f, cy + dy * 10.0f + 4.0f),
                            IM_COL32(255, 105, 180, 240), 1.0f);
            };

            DrawCornerFlourish(wPos.x + 4.0f, wPos.y + 4.0f, 1.0f, 1.0f);                      // Top-Left
            DrawCornerFlourish(wPos.x + wSize.x - 4.0f, wPos.y + 4.0f, -1.0f, 1.0f);           // Top-Right
            DrawCornerFlourish(wPos.x + 4.0f, wPos.y + wSize.y - 4.0f, 1.0f, -1.0f);           // Bottom-Left
            DrawCornerFlourish(wPos.x + wSize.x - 4.0f, wPos.y + wSize.y - 4.0f, -1.0f, -1.0f); // Bottom-Right
        }

        ImGui::SetWindowFontScale(0.90f);

        if (!p_open)
        {
            SetMenuOpenState(false);
        }

        // Status banner
        if (GetTickCount() - s_StatusMessageTime < 3500 && !s_StatusMessage.empty())
        {
            ImGui::PushStyleColor(ImGuiCol_Text, kColSage);
            std::string banner = ">> " + s_StatusMessage;
            ImGui::TextUnformatted(banner.c_str());
            ImGui::PopStyleColor();
            ImGui::Separator();
        }

        // -------------------------------------------------------------
        // LEFT SIDEBAR: INSTALLED MODS LIST (COMPACT 130PX)
        // -------------------------------------------------------------
        RefreshSubMods();

        // Check if Lyra's Texture Mod is actually installed or loaded
        bool bHasTexMod = (GetModuleHandleA("LyrasTextureMod.dll") != NULL);
        if (!bHasTexMod)
        {
            std::string texModDll = GetGameDirectory() + "mods\\LyrasTextureMod\\LyrasTextureMod.dll";
            if (GetFileAttributesA(texModDll.c_str()) != INVALID_FILE_ATTRIBUTES)
            {
                bHasTexMod = true;
            }
        }

        // If Texture Mod is not installed, avoid staying on or selecting index 2
        if (!bHasTexMod && s_SelectedModIndex == 2)
        {
            s_SelectedModIndex = 0;
        }

        ImGui::BeginChild("ModListSidebar", ImVec2(130.0f, 0.0f), true);
        {
            int totalMods = 2 + (bHasTexMod ? 1 : 0) + (int)s_DiscoveredSubMods.size();
            ImGui::TextColored(kColRose, "MODS (%d)", totalMods);
            ImGui::Separator();
            ImGui::Spacing();

            // Mod 0: Lyras Framework Mod
            bool isLyraSelected = (s_SelectedModIndex == 0);
            if (ImGui::Selectable(" Lyra's Framework##Nav", isLyraSelected, 0, ImVec2(0, 22)))
            {
                s_SelectedModIndex = 0;
            }
            ImGui::TextDisabled("  v1.2 | Core");

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            // Mod 1: Dreamcast Restoration 2.0
            bool isDCSelected = (s_SelectedModIndex == 1);
            if (ImGui::Selectable(" DC Restoration##Nav", isDCSelected, 0, ImVec2(0, 22)))
            {
                s_SelectedModIndex = 1;
            }
            ImGui::TextDisabled("  v2.0 | Brands");

            // Mod 2: Lyra's Texture Mod (ONLY shown if actually installed in mods/ or loaded in process)
            if (bHasTexMod)
            {
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                bool isTexModSelected = (s_SelectedModIndex == 2);
                if (ImGui::Selectable(" Texture Mod##Nav", isTexModSelected, 0, ImVec2(0, 22)))
                {
                    s_SelectedModIndex = 2;
                }
                ImGui::TextDisabled("  v1.0 | HD Textures");
            }

            // Dynamically list any sub-mods loaded from mods/<SubModFolder>/<SubMod>.dll
            for (size_t i = 0; i < s_DiscoveredSubMods.size(); ++i)
            {
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                int modIdx = 3 + (int)i;
                bool isSubSelected = (s_SelectedModIndex == modIdx);
                std::string navLabel = " " + s_DiscoveredSubMods[i].name;
                if (s_DiscoveredSubMods[i].isQuarantined)
                    navLabel += " [!]";
                navLabel += "##NavSub" + std::to_string(i);
                if (ImGui::Selectable(navLabel.c_str(), isSubSelected, 0, ImVec2(0, 22)))
                {
                    s_SelectedModIndex = modIdx;
                }
                const char* subVer = s_DiscoveredSubMods[i].version.empty() ? "1.0" : s_DiscoveredSubMods[i].version.c_str();
                const char* subAuth = s_DiscoveredSubMods[i].author.empty() ? "Community" : s_DiscoveredSubMods[i].author.c_str();
                ImGui::TextDisabled("  v%s | %s", subVer, subAuth);
            }
        }
        ImGui::EndChild();

        ImGui::SameLine();

        // -------------------------------------------------------------
        // RIGHT CONTENT PANEL: SELECTED MOD SETTINGS
        // -------------------------------------------------------------
        ImGui::BeginChild("ModContentPanel", ImVec2(0.0f, 0.0f), false);
        {
            if (s_SelectedModIndex == 0)
            {
                ImGui::TextColored(kColRose, "MOD: Crazy Taxi Mod Framework (v1.2)");
                ImGui::TextDisabled("by Lyra");
                ImGui::Separator();
                ImGui::Spacing();

                if (ImGui::BeginTabBar("LyraModTabs"))
                {
            // -------------------------------------------------------------
            // TAB 1: GAMEPLAY & PHYSICS
            // -------------------------------------------------------------
            if (ImGui::BeginTabItem("Gameplay & Physics"))
            {
                ImGui::Spacing();

                // 1. VEHICLE SPEED MULTIPLIER (CONTINUOUS PHYSICAL DISPLACEMENT BOOST)
                if (ImGui::CollapsingHeader("Speed Multiplier", 0))
                {
                    ImGui::Checkbox("Enable##SpeedMultToggle", &g_Config.enableSpeedMultiplier);
                    if (g_Config.enableSpeedMultiplier)
                    {
                        ImGui::SliderFloat("Boost##SpeedMult", &g_Config.speedMultiplier, 1.00f, 3.00f, "%.2fx");
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("Scales taxi driving displacement and velocity directly in physics loop.");
                        if (ImGui::Button("1.00x##SpdDef")) { g_Config.speedMultiplier = 1.00f; }
                        ImGui::SameLine();
                        if (ImGui::Button("1.25x##Spd125")) { g_Config.speedMultiplier = 1.25f; }
                        ImGui::SameLine();
                        if (ImGui::Button("1.50x##Spd150")) { g_Config.speedMultiplier = 1.50f; }
                        ImGui::SameLine();
                        if (ImGui::Button("2.00x##Spd200")) { g_Config.speedMultiplier = 2.00f; }
                        ImGui::SameLine();
                        if (ImGui::Button("2.50x##Spd250")) { g_Config.speedMultiplier = 2.50f; }

                        float maxSpeedVal = 14.0f * g_Config.speedMultiplier;
                        ImGui::TextColored(kColSage, "Top Speed Limit: %.1f km/h (Scalar: %.2f)", maxSpeedVal * 2.8f, maxSpeedVal);
                        ImGui::TextDisabled("Nitro adds +50%% boost on top of this limit!");
                    }
                }

                ImGui::Spacing();

                // 2. NITRO TAXI (HOLD SHIFT TEMPORARY THRUST BOOST)
                if (ImGui::CollapsingHeader("Nitro Taxi", 0))
                {
                    if (ImGui::Checkbox("Enable (Hold SHIFT)##NitroToggle", &g_Config.nitroEnabled))
                    {
                        SaveConfig();
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", "Hold SHIFT while accelerating to boost (+50% top speed, +70% accel).\n"
                                                "Consumes 5.0 Nitro/s (Tank: 75.0 max).\n"
                                                "Recharge delay: 3.0s after releasing Shift (+1.5 Nitro/s recharge).\n"
                                                "Empty tank penalty: 5.0s recharge delay and locked out until tank reaches 40% (30.0 units).\n"
                                                "Live HUD gauge shown in bottom-right retro cluster.");
                    if (g_Config.nitroEnabled)
                    {
                        ImGui::TextColored(kColSage, "%s", "Hold SHIFT while driving: +50% max speed, +70% accel.");
                        ImGui::TextDisabled("%s", "Tank: 75 max | -5/s drain | +1.5/s recharge (3s delay)");
                        ImGui::TextDisabled("%s", "Penalty: 5s delay & lockout until 40% if fully drained");
                    }
                }

                ImGui::Spacing();

                // 3. STEERING FEEL MODES
                if (ImGui::CollapsingHeader("Steering Mode", 0))
                {
                    if (ImGui::RadioButton("Standard##SteerStd", &g_Config.steeringMode, 0))
                    {
                        SaveConfig();
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", "Original Sega arcade handling.");

                    if (ImGui::RadioButton("4-Wheel Steering##Steer4W", &g_Config.steeringMode, 1))
                    {
                        SaveConfig();
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", "Speed-blended 4WS: Counter-phase at low speed (<50 km/h) for tight turns,\nin-phase at high speed (>50 km/h) for lane-change stability.");

                    if (ImGui::RadioButton("Drunk Driver##SteerDrunk", &g_Config.steeringMode, 2))
                    {
                        SaveConfig();
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", "Wobbly, unpredictable steering input with delayed reaction time.");

                    if (ImGui::RadioButton("Slippery Ice##SteerIce", &g_Config.steeringMode, 3))
                    {
                        SaveConfig();
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", "Low friction (mu=0.1): Drastic lateral sliding, decoupled heading,\noversteering power slides, and wheel spin.");
                }

                ImGui::Spacing();

                // 4. FLY MODE (ZERO COLLISION 3D FLIGHT)
                if (ImGui::CollapsingHeader("Fly Mode", 0))
                {
                    ImGui::Checkbox("Fly Mode##FlyToggle", &g_Config.flyMode);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", "Collision-free flight. SPACE up, SHIFT/CTRL down,\nArrows move & turn. Gamepad sticks also work.");
                    if (g_Config.flyMode)
                    {
                        ImGui::SliderFloat("Flight Speed##FlySpd", &g_Config.flySpeed, 5.0f, 60.0f, "%.1f");
                    }
                }

                ImGui::Spacing();

                // 5. TRAFFIC COLLISION (PASS-THROUGH NPC CARS)
                if (ImGui::CollapsingHeader("Traffic Collision", 0))
                {
                    if (ImGui::Checkbox("No Traffic Collision##DedicatedTrafficColl", &g_Config.disableTrafficCollision))
                    {
                        SaveConfig();
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", "Disable physical collisions with all NPC traffic vehicles.\nWorld, buildings, and obstacles remain solid.");
                    if (g_Config.disableTrafficCollision)
                    {
                        ImGui::TextColored(kColSage, "Traffic collision disabled (pass through NPC cars).");
                    }
                }

                ImGui::Spacing();

                // 6. IN-GAME TIMER CHEAT (UP TO 16 MINUTES / 960s)
                if (ImGui::CollapsingHeader("Round Timer", 0))
                {
                    if (ImGui::Checkbox("Custom Timer", &g_Config.customTimerEnabled))
                    {
                        if (g_Config.customTimerEnabled)
                            SetInGameTimerMinutes(g_Config.customTimerMinutes);
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Extends the round timer up to 16 minutes (fits within arcade 3-digit 999s HUD).");

                    if (g_Config.customTimerEnabled)
                    {
                        if (ImGui::SliderInt("Minutes##TimerMin", &g_Config.customTimerMinutes, 1, 16, "%d min"))
                        {
                            SetInGameTimerMinutes(g_Config.customTimerMinutes);
                        }
                        ImGui::SameLine();
                        if (ImGui::Button("10 min")) { g_Config.customTimerMinutes = 10; SetInGameTimerMinutes(10); }
                        ImGui::SameLine();
                        if (ImGui::Button("16 min")) { g_Config.customTimerMinutes = 16; SetInGameTimerMinutes(16); }

                        if (ImGui::Button("Apply Now"))
                        {
                            SetInGameTimerMinutes(g_Config.customTimerMinutes);
                        }
                        ImGui::SameLine();
                        ImGui::Checkbox("Freeze", &g_Config.freezeTimer);

                        int curSec = GetInGameTimerSeconds();
                        ImGui::TextColored(kColSage, "Active: %02d:%02d (%d sec)", curSec / 60, curSec % 60, curSec);
                    }
                }

                ImGui::Spacing();

                // 7. FARE TIP MULTIPLIER & STUNTS
                if (ImGui::CollapsingHeader("Tip Multiplier", 0))
                {
                    ImGui::SliderFloat("Stunt Tips##Stunt", &g_Config.stuntMultiplier, 1.0f, 100.0f, "%.1fx");
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Multiplies tips from Crazy Stunts, near-misses and drifts.");
                    ImGui::SameLine();
                    if (ImGui::Button("1.0x##Stunt"))
                    {
                        g_Config.stuntMultiplier = 1.0f;
                    }
                }

                ImGui::Spacing();

                // 8. TAXI TELEPORTATION
                if (ImGui::CollapsingHeader("Teleport", 0))
                {
                    float curX = 0, curY = 0, curZ = 0;
                    bool inGame = GetTaxiPosition(curX, curY, curZ);
                    if (inGame)
                    {
                        ImGui::TextColored(kColSage, "Position: %.1f / %.1f / %.1f", curX, curY, curZ);
                        if (s_TeleportX == 0.0f && s_TeleportY == 0.0f && s_TeleportZ == 0.0f)
                        {
                            s_TeleportX = curX;
                            s_TeleportY = curY;
                            s_TeleportZ = curZ;
                        }
                    }
                    else
                    {
                        ImGui::TextColored(kColPeach, "Enter a session first");
                    }

                    ImGui::InputFloat("Target X", &s_TeleportX, 10.0f, 100.0f, "%.2f");
                    ImGui::InputFloat("Target Y (Elevation)", &s_TeleportY, 10.0f, 100.0f, "%.2f");
                    ImGui::InputFloat("Target Z", &s_TeleportZ, 10.0f, 100.0f, "%.2f");

                    if (ImGui::Button("Copy Position"))
                    {
                        if (GetTaxiPosition(curX, curY, curZ))
                        {
                            s_TeleportX = curX;
                            s_TeleportY = curY;
                            s_TeleportZ = curZ;
                            SetStatusMessage("Coordinates copied.");
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Teleport##DoTeleport"))
                    {
                        // UNIQUE IMGUI ID REQUIRED: CollapsingHeader("Teleport") and a plain
                        // Button("Teleport") hash to the same ImGui ID (MSVC merges both literals
                        // into one string constant). With a duplicate ID ImGui routed every click
                        // to the header - 30+ clicks produced zero handler runs (T4 live finding,
                        // proven by patching the shared string in the running game). The "##"
                        // suffix makes the ID unique while the label still displays "Teleport".
                        // clampToGroundY=false: the user types exact coordinates - the step-2
                        // ground probe would otherwise overwrite Y with groundY+1 (verified
                        // live: typed Y=-1117 landed at ground -1184..-1197).
                        // Evaluate the return value: TeleportTaxi refuses during fare
                        // cutscenes (camera state 10/11), the attract demo (state 8) and
                        // outside a session - showing "teleported!" unconditionally was a lie.
                        if (TeleportTaxi(s_TeleportX, s_TeleportY, s_TeleportZ, false))
                        {
                            SetStatusMessage("Taxi teleported!");
                        }
                        else
                        {
                            SetStatusMessage("Teleport blocked (cutscene active or not in a session).");
                        }
                    }

                    ImGui::Spacing();
                    if (ImGui::Button("+15m (Unstuck)"))
                    {
                        // Same refusal rules as above - the return value must decide the
                        // message, otherwise the button reports success while nothing moved.
                        // clampToGroundY=false keeps the elevate semantics: the step-2
                        // ground probe (unbounded below-window) would otherwise clamp the
                        // +15m target straight back to the ground under the taxi.
                        if (GetTaxiPosition(curX, curY, curZ) && TeleportTaxi(curX, curY + 15.0f, curZ, false))
                        {
                            SetStatusMessage("Taxi elevated +15m!");
                        }
                        else
                        {
                            SetStatusMessage("Unstuck blocked (cutscene active or not in a session).");
                        }
                    }
                }

                ImGui::Spacing();

                // 9. SEGA DEVELOPER CHEATS & SECRET MODES
                if (ImGui::CollapsingHeader("Sega Cheats", 0))
                {
                    if (ImGui::Checkbox("Unlock All Vehicles##UnlockVeh", &g_Config.unlockAllVehicles))
                    {
                        if (g_Config.unlockAllVehicles)
                        {
                            SafeWriteByte(ADDR_SEGA_UNLOCK_ALL, 0x3F);
                            SafeWriteInt(ADDR_SEGA_UNLOCK_MASK, 0);
                            SetStatusMessage("All vehicles and bikes unlocked!");
                        }
                        SaveConfig();
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Cabs + Crazy Bikes (Sega unlock flags).");

                    static const char* s_VehicleOptions[8] = {
                        "Axel (Cab)",
                        "B.D. Joe (Cab)",
                        "Gena (Cab)",
                        "Gus (Cab)",
                        "Axel (Bike)",
                        "B.D. Joe (Bike)",
                        "Gena (Bike)",
                        "Gus (Bike)"
                    };
                    int curChar = SafeReadInt(ADDR_SEGA_CHAR_SELECT);
                    if (curChar < 0 || curChar > 7) curChar = 0;
                    if (ImGui::Combo("Vehicle##VehCombo", &curChar, s_VehicleOptions, 8))
                    {
                        SafeWriteByte(ADDR_SEGA_UNLOCK_ALL, 0x3F);
                        SafeWriteInt(ADDR_SEGA_UNLOCK_MASK, 0);
                        SafeWriteInt(ADDR_SEGA_CHAR_SELECT, curChar);
                        SetStatusMessage(std::string("Switched vehicle to: ") + s_VehicleOptions[curChar]);
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Direct vehicle switch, also usable on the character screen.");

                    ImGui::Spacing();

                    if (ImGui::Checkbox("Master Unlock", &g_Config.segaUnlockAll))
                    {
                        if (g_Config.segaUnlockAll)
                        {
                            SafeWriteByte(ADDR_SEGA_UNLOCK_ALL, 0x3F);
                            SafeWriteInt(ADDR_SEGA_UNLOCK_MASK, 0);
                            SetStatusMessage("Sega Master Unlock activated!");
                        }
                        SaveConfig();
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("All bikes & Crazy Box mini-games.");

                    if (ImGui::Checkbox("Another Day", &g_Config.segaAnotherDay))
                    {
                        SafeWriteInt(ADDR_SEGA_ANOTHER_DAY, g_Config.segaAnotherDay ? 1 : 0);
                        SaveConfig();
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Shuffled customers & locations.");

                    if (ImGui::Checkbox("Bike Mode", &g_Config.segaBikeMode))
                    {
                        if (g_Config.segaBikeMode)
                        {
                            SafeWriteByte(ADDR_SEGA_UNLOCK_ALL, 0x3F);
                            SafeWriteInt(ADDR_SEGA_UNLOCK_MASK, 0);
                            SetStatusMessage("Bike Mode unlocked (select bike on character screen)!");
                        }
                        else
                        {
                            SetStatusMessage("Standard Taxi restored.");
                        }
                        SaveConfig();
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Unlock the Crazy Bike / Rickshaw.");

                    if (ImGui::Checkbox("No Guide Arrows", &g_Config.segaNoArrows))
                    {
                        SafeWriteInt(ADDR_SEGA_NO_ARROWS, g_Config.segaNoArrows ? 1 : 0);
                        SaveConfig();
                    }

                    if (ImGui::Checkbox("No Destination Mark", &g_Config.segaNoDestMark))
                    {
                        SafeWriteInt(ADDR_SEGA_NO_DEST_MARK, g_Config.segaNoDestMark ? 1 : 0);
                        SaveConfig();
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("EXPERT mode: no arrows, highest payouts.");

                    if (g_Config.segaNoArrows && g_Config.segaNoDestMark)
                    {
                        ImGui::TextColored(kColCoral, "EXPERT MODE active");
                    }
                }

                ImGui::EndTabItem();
            }

            // -------------------------------------------------------------
            // TAB 2: LIVE 6-DOF CAMERA EDITOR WITH REAL-TIME PREVIEW
            // -------------------------------------------------------------
            if (ImGui::BeginTabItem("Camera Editor", nullptr, (s_ForceLyraTab == 1) ? ImGuiTabItemFlags_SetSelected : 0))
            {
                ImGui::Spacing();

                // 1. CAMERA MODES & PC HOTKEYS (DIRECT CAMERA SWITCHING)
                if (ImGui::CollapsingHeader("Camera Modes & PC Hotkeys", ImGuiTreeNodeFlags_DefaultOpen))
                {
                    ImGui::TextColored(kColSage, "Direct Camera Switching:");
                    if (ImGui::Button("Reset / Chase Cam", ImVec2(150, 28)))
                    {
                        SetCameraMode(0);
                        SetStatusMessage("Switched to Standard Chase Camera");
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Cinematic Cam", ImVec2(150, 28)))
                    {
                        SetCameraMode(1);
                        SetStatusMessage("Switched to Cinematic Camera");
                    }

                    if (ImGui::Button("First-Person / Hood", ImVec2(150, 28)))
                    {
                        SetCameraMode(2);
                        SetStatusMessage("Switched to First-Person Hood Camera");
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Low Bumper Cam", ImVec2(150, 28)))
                    {
                        SetCameraMode(3);
                        SetStatusMessage("Switched to Low Bumper Camera");
                    }

                    ImGui::Spacing();
                    ImGui::TextDisabled("PC Hotkeys: Shift+Alt+F5 (Reset) | F6 (Cinematic) | F7 (First-Person)");
                    ImGui::Spacing();
                }

                ImGui::Spacing();

                float eyeX = 0, eyeY = 0, eyeZ = 0;
                float tgtX = 0, tgtY = 0, tgtZ = 0;
                if (GetCameraEyePosition(eyeX, eyeY, eyeZ) && GetCameraTargetPosition(tgtX, tgtY, tgtZ))
                {
                    ImGui::Text("Eye:   %7.1f  %7.1f  %7.1f", eyeX, eyeY, eyeZ);
                    ImGui::Text("Target:%7.1f  %7.1f  %7.1f", tgtX, tgtY, tgtZ);
                }

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                // 2. Camera Location (XYZ Translation / Movement - 3x Extended Ranges)
                if (ImGui::CollapsingHeader("Location", 0))
                {
                    ImGui::SliderFloat("X (L/R)##CamX", &g_Config.camOffsetX, -250.0f, 250.0f, "%.1f m");
                    ImGui::SliderFloat("Y (Up/Down)##CamY", &g_Config.camOffsetY, -400.0f, 400.0f, "%.1f m");
                    ImGui::SliderFloat("Z (Fwd/Back)##CamZ", &g_Config.camOffsetZ, -450.0f, 450.0f, "%.1f m");

                    if (ImGui::Button("Reset Location"))
                    {
                        g_Config.camOffsetX = 0.0f;
                        g_Config.camOffsetY = 0.0f;
                        g_Config.camOffsetZ = 0.0f;
                        SaveConfig();
                        SetStatusMessage("Camera location reset.");
                    }
                }

                ImGui::Spacing();

                // 3. Camera Rotation (XYZ Orientation)
                if (ImGui::CollapsingHeader("Rotation", 0))
                {
                    ImGui::SliderFloat("Pitch##CamPitch", &g_Config.camRotPitch, -85.0f, 85.0f, "%.1f deg");
                    ImGui::SliderFloat("Yaw##CamYaw", &g_Config.camRotYaw, -180.0f, 180.0f, "%.1f deg");
                    ImGui::SliderFloat("Roll##CamRoll", &g_Config.camRotRoll, -180.0f, 180.0f, "%.1f deg");

                    if (ImGui::Button("Reset Rotation"))
                    {
                        g_Config.camRotPitch = 0.0f;
                        g_Config.camRotYaw   = 0.0f;
                        g_Config.camRotRoll  = 0.0f;
                        SaveConfig();
                        SetStatusMessage("Camera rotation reset.");
                    }
                }

                ImGui::Spacing();

                // 4. Camera Presets & Step Buttons
                if (ImGui::CollapsingHeader("Presets", 0))
                {
                    if (ImGui::Button("Standard Arcade Cam", ImVec2(165, 28)))
                    {
                        g_Config.camOffsetX = 0.0f; g_Config.camOffsetY = 0.0f; g_Config.camOffsetZ = 0.0f;
                        g_Config.camRotPitch = 0.0f; g_Config.camRotYaw = 0.0f; g_Config.camRotRoll = 0.0f;
                        SaveConfig();
                        SetStatusMessage("Standard Arcade camera active.");
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Close Action Cam", ImVec2(150, 28)))
                    {
                        g_Config.camOffsetX = 0.0f; g_Config.camOffsetY = 1.8f; g_Config.camOffsetZ = -4.0f;
                        g_Config.camRotPitch = 2.0f; g_Config.camRotYaw = 0.0f; g_Config.camRotRoll = 0.0f;
                        SaveConfig();
                        SetStatusMessage("Close Action camera active.");
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Wide-Angle Chase", ImVec2(150, 28)))
                    {
                        g_Config.camOffsetX = 0.0f; g_Config.camOffsetY = 6.0f; g_Config.camOffsetZ = 16.0f;
                        g_Config.camRotPitch = -5.0f; g_Config.camRotYaw = 0.0f; g_Config.camRotRoll = 0.0f;
                        SaveConfig();
                        SetStatusMessage("Wide-Angle camera active.");
                    }

                    if (ImGui::Button("Bumper / Low Cam", ImVec2(165, 28)))
                    {
                        g_Config.camOffsetX = 0.0f; g_Config.camOffsetY = -1.2f; g_Config.camOffsetZ = -6.5f;
                        g_Config.camRotPitch = 0.0f; g_Config.camRotYaw = 0.0f; g_Config.camRotRoll = 0.0f;
                        SaveConfig();
                        SetStatusMessage("Bumper / Low camera active.");
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Cockpit / Hood Cam", ImVec2(150, 28)))
                    {
                        g_Config.camOffsetX = 0.0f; g_Config.camOffsetY = 0.8f; g_Config.camOffsetZ = -5.0f;
                        g_Config.camRotPitch = 0.0f; g_Config.camRotYaw = 0.0f; g_Config.camRotRoll = 0.0f;
                        SaveConfig();
                        SetStatusMessage("Cockpit / Hood camera active.");
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Bird's Eye View", ImVec2(150, 28)))
                    {
                        g_Config.camOffsetX = 0.0f; g_Config.camOffsetY = 35.0f; g_Config.camOffsetZ = 12.0f;
                        g_Config.camRotPitch = -55.0f; g_Config.camRotYaw = 0.0f; g_Config.camRotRoll = 0.0f;
                        SaveConfig();
                        SetStatusMessage("Bird's Eye View camera active.");
                    }

                    ImGui::Spacing();
                    if (ImGui::Button("Height +2m")) { g_Config.camOffsetY += 2.0f; SaveConfig(); }
                    ImGui::SameLine();
                    if (ImGui::Button("Height -2m")) { g_Config.camOffsetY -= 2.0f; SaveConfig(); }
                    ImGui::SameLine();
                    if (ImGui::Button("Dist +5m")) { g_Config.camOffsetZ += 5.0f; SaveConfig(); }
                    ImGui::SameLine();
                    if (ImGui::Button("Dist -5m")) { g_Config.camOffsetZ -= 5.0f; SaveConfig(); }

                    if (ImGui::Button("Pitch +5")) { g_Config.camRotPitch += 5.0f; SaveConfig(); }
                    ImGui::SameLine();
                    if (ImGui::Button("Pitch -5")) { g_Config.camRotPitch -= 5.0f; SaveConfig(); }
                    ImGui::SameLine();
                    if (ImGui::Button("Orbit -15")) { g_Config.camRotYaw -= 15.0f; SaveConfig(); }
                    ImGui::SameLine();
                    if (ImGui::Button("Orbit +15")) { g_Config.camRotYaw += 15.0f; SaveConfig(); }
                    ImGui::SameLine();
                    if (ImGui::Button("Rear 180")) { g_Config.camRotYaw = (g_Config.camRotYaw == 180.0f) ? 0.0f : 180.0f; SaveConfig(); }
                }

                ImGui::Spacing();

                // 5. Mouse Look & 360-Degree Camera Orbit
                if (ImGui::CollapsingHeader("Mouse Look", 0))
                {
                    if (ImGui::Checkbox("Enable Mouse Look##MouseLookToggle", &g_Config.mouseLookEnabled))
                    {
                        SaveConfig();
                        SetStatusMessage(g_Config.mouseLookEnabled ? "Mouse Look enabled." : "Mouse Look disabled.");
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Orbit 360 degrees around the taxi with the mouse\nwhile driving or flying. Middle-click recenters.");

                    if (g_Config.mouseLookEnabled)
                    {
                        ImGui::SliderFloat("Sensitivity##MouseLookSens", &g_Config.mouseLookSensitivity, 0.005f, 0.10f, "%.3f");

                        if (ImGui::Button("Recenter##ResetMouseLook", ImVec2(160, 26)))
                        {
                            ResetMouseLook();
                            SetStatusMessage("Mouse Look recentered!");
                        }
                    }
                }

                ImGui::Spacing();

                // 6. Freecam Mode
                if (ImGui::CollapsingHeader("Freecam", 0))
                {
                    if (ImGui::Checkbox("Enable Freecam", &g_Config.freecamEnabled))
                    {
                        SaveConfig();
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Detaches the camera from the taxi for free exploration.\nUncheck to return to the taxi.");

                    if (g_Config.freecamEnabled)
                    {
                        ImGui::SliderFloat("Speed##FreecamSpd", &g_Config.freecamSpeed, 5.0f, 200.0f, "%.1f m/s");
                        ImGui::TextDisabled("Mouse look | WASD move | SPACE up | SHIFT down");
                    }
                }

                ImGui::EndTabItem();
            }

            // -------------------------------------------------------------
            // TAB 3: SETTINGS
            // -------------------------------------------------------------
            if (ImGui::BeginTabItem("Settings"))
            {
                ImGui::Spacing();

                // 1. SPEEDOMETER HUD (DEDICATED STANDALONE SECTION AT THE TOP)
                if (ImGui::CollapsingHeader("Speedometer HUD", ImGuiTreeNodeFlags_DefaultOpen))
                {
                    if (ImGui::Checkbox("Speedometer (km/h)##SettingsSpeedo", &g_Config.showSpeedometer))
                    {
                        SaveConfig();
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Vintage retro km/h speedometer cluster with integrated Nitro gauge and total KM odometer.\nOnly visible during active gameplay.");

                    ImGui::Spacing();
                    ImGui::TextColored(kColSage, "Vintage Retro Instrument Cluster (0 - 200 km/h, 140 km/h Redline):");
                    ImGui::BulletText("Horizontal mechanical linear speed bar with moving orange pointer");
                    ImGui::BulletText("Integrated Nitro booster charge bar below the speedometer");
                    ImGui::BulletText("Dual digital/mechanical odometer: Lifetime KM + Current Trip KM");
                    ImGui::Spacing();

                    if (g_Config.showSpeedometer)
                    {
                        ImGui::TextColored(kColSage, "[ACTIVE] Speedometer HUD is displayed in the bottom-right corner.");
                    }
                    else
                    {
                        ImGui::TextDisabled("[OFF] Speedometer HUD is currently hidden.");
                    }
                }

                ImGui::Spacing();

                // 2. NATIVE ENGINE FIXES
                if (ImGui::CollapsingHeader("Native Engine Fixes", ImGuiTreeNodeFlags_DefaultOpen))
                {
                    ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.4f, 1.0f), "%s", "[OK] Native Engine Fixes integrated (100% no .asi)");
                    if (ImGui::Checkbox("Window & Aspect Ratio Fix", &g_Config.windowAspectRatioFix))
                    {
                        EngineFixes::ApplyWindowFix(g_Config.windowAspectRatioFix);
                    }
                    if (ImGui::Checkbox("Analog Steering Deadzone Calibration", &g_Config.analogDeadzoneFix))
                    {
                        EngineFixes::ApplyAnalogDeadzones(g_Config.analogDeadzoneFix);
                    }
                    if (ImGui::Checkbox("DirectInput Steering Wheel Crash Fix", &g_Config.steeringWheelCrashFix))
                    {
                        EngineFixes::ApplyWheelCrashFix(g_Config.steeringWheelCrashFix);
                    }
                    ImGui::Checkbox("Camera & HUD Debug Hotkeys (Shift+Alt+F5-F8)", &g_Config.debugCameraHotkeys);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Shift+Alt+F5: Reset Cam | F6: Cinematic | F7: First-Person | F8: Native Speedo");

                    ImGui::Spacing();
                }

                ImGui::Spacing();

                // 3. VULKAN (DXVK) TRANSLATION LAYER
                if (ImGui::CollapsingHeader("Vulkan (DXVK) Renderer", 0))
                {
                    static int s_dxvkState = -1; // -1 = uninitialized, 0 = disabled, 1 = enabled
                    static char s_dxvkStatusMsg[128] = "";

                    if (s_dxvkState == -1)
                    {
                        DWORD attr = GetFileAttributesA("d3d9.dll");
                        bool fileExists = (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY));
                        s_dxvkState = (fileExists && g_Config.enableDXVK) ? 1 : (fileExists ? 1 : 0);
                    }

                    bool dxvkActive = (s_dxvkState == 1);
                    if (ImGui::Checkbox("Enable Vulkan (DXVK D3D9)", &dxvkActive))
                    {
                        if (dxvkActive)
                        {
                            g_Config.enableDXVK = true;
                            SaveConfig();
                            DeleteFileA("d3d9.dll.del");
                            if (CopyFileA("dxvk_optional\\d3d9.dll", "d3d9.dll", FALSE))
                            {
                                s_dxvkState = 1;
                                strcpy_s(s_dxvkStatusMsg, "[OK] DXVK activated! Restart game to run on Vulkan.");
                            }
                            else
                            {
                                strcpy_s(s_dxvkStatusMsg, "[!] Error: dxvk_optional\\d3d9.dll not found.");
                            }
                        }
                        else
                        {
                            g_Config.enableDXVK = false;
                            SaveConfig();
                            if (DeleteFileA("d3d9.dll"))
                            {
                                s_dxvkState = 0;
                                strcpy_s(s_dxvkStatusMsg, "[OK] DXVK deactivated! Restart game to use native D3D9.");
                            }
                            else
                            {
                                if (MoveFileExA("d3d9.dll", "d3d9.dll.del", MOVEFILE_REPLACE_EXISTING))
                                {
                                    s_dxvkState = 0;
                                    strcpy_s(s_dxvkStatusMsg, "[OK] DXVK deactivated! Restart game to use native D3D9.");
                                }
                                else
                                {
                                    MoveFileExA("d3d9.dll", NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
                                    s_dxvkState = 0;
                                    strcpy_s(s_dxvkStatusMsg, "[OK] DXVK removal scheduled. Restart game.");
                                }
                            }
                        }
                    }
                    if (ImGui::IsItemHovered())
                    {
                        ImGui::SetTooltip("%s", "Direct3D 9 to Vulkan translation layer.\nEliminates microstutter, stabilizes frametimes, and improves GPU rendering.\nDisabled by default; can be enabled/disabled anytime via menu or ModAPI.");
                    }
                    if (s_dxvkStatusMsg[0] != '\0')
                    {
                        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f), "%s", s_dxvkStatusMsg);
                    }
                    ImGui::Spacing();
                }

                ImGui::Spacing();

                // 4. FFB ARCADE: FORCE FEEDBACK & RUMBLE
                if (ImGui::CollapsingHeader("FFB Arcade: Force Feedback & Rumble", 0))
                {
                    ImGui::Checkbox("Enable FFB Arcade##FFBToggle", &g_Config.ffbEnabled);
                    if (g_Config.ffbEnabled)
                    {
                        ImGui::SliderFloat("FFB Strength", &g_Config.ffbStrength, 0.1f, 2.0f, "%.1fx");
                        ImGui::Checkbox("Ground Surface Rumble (Grass/Sand/Water/Stairs)", &g_Config.ffbGroundEffects);
                        ImGui::Checkbox("Engine Idle/Throttle Rumble", &g_Config.ffbEngineRumble);
                        ImGui::Checkbox("Wall & Car Collision Shocks", &g_Config.ffbCollisionEffects);
                    }
                    ImGui::Spacing();
                }

                ImGui::Spacing();

                // 5. WORLD DISTANCE & STREAMING RADIUS
                if (ImGui::CollapsingHeader("World Distance & LOD", 0))
                {
                    if (ImGui::SliderFloat("Draw Distance##WorldDist", &g_Config.worldDistanceMultiplier, 1.0f, 40.0f, "%.1fx"))
                    {
                        g_Config.renderDistanceMultiplier = g_Config.worldDistanceMultiplier;
                        g_Config.chunkLoadDistanceMultiplier = g_Config.worldDistanceMultiplier;
                        SaveConfig();
                    }
                    if (ImGui::IsItemHovered())
                    {
                        float mult = g_Config.worldDistanceMultiplier;
                        float safeChunkCam = 850.0f * mult; if (safeChunkCam > 4500.0f) safeChunkCam = 4500.0f;
                        float safeFarZ = 15000.0f * mult; if (safeFarZ > 1500000.0f) safeFarZ = 1500000.0f;
                        ImGui::SetTooltip("Camera far-Z: %.0f | Chunk radius: %.0f\nChunk table (1152 slots) stays protected.",
                            safeFarZ, safeChunkCam);
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("1.0x##ResetWorldDist"))
                    {
                        g_Config.worldDistanceMultiplier = 1.0f;
                        g_Config.renderDistanceMultiplier = 1.0f;
                        g_Config.chunkLoadDistanceMultiplier = 1.0f;
                        SaveConfig();
                    }

                    if (ImGui::Checkbox("Persistent Chunk Loader##PersistentChunks", &g_Config.persistentChunkLoader))
                    {
                        SaveConfig();
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Keeps the whole map streamed in; disables Sega despawning.");
                }

                ImGui::Spacing();

                // 6. ORIGINAL SOUNDTRACK (THE OFFSPRING & BAD RELIGION)
                if (ImGui::CollapsingHeader("Original Soundtrack", 0))
                {
                    if (ImGui::Checkbox("Original Soundtrack##OrigSoundtrack", &g_Config.useOriginalSoundtrack))
                    {
                        ApplySoundtrackSetting(g_Config.useOriginalSoundtrack);
                        SaveConfig();
                        SetStatusMessage(g_Config.useOriginalSoundtrack ? "Original soundtrack active." : "Steam soundtrack restored.");
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("The Offspring & Bad Religion (AirChomp mix)\ninstead of the Steam release tracks.");
                }

                ImGui::Spacing();

                // 7. CUSTOM KEYBINDINGS
                if (ImGui::CollapsingHeader("Custom Keybindings", 0))
                {
                    ImGui::Columns(2, "KeybindColumns", false);
                    ImGui::SetColumnWidth(0, 220.0f);

                    auto RenderKeybindRow = [](const char* actionLabel, int actionIndex, int& vkTarget) {
                        ImGui::AlignTextToFramePadding();
                        ImGui::Text("%s:", actionLabel);
                        ImGui::NextColumn();

                        if (s_WaitingForAction == actionIndex)
                        {
                            ImGui::Button("[Press key... (ESC: Cancel)]", ImVec2(240, 24));
                            if (GetTickCount() - s_ListenStartTime > 150)
                            {
                                for (int k = 8; k < 255; ++k)
                                {
                                    if (k == VK_LBUTTON || k == VK_RBUTTON || k == VK_MBUTTON) continue;
                                    if (GetAsyncKeyState(k) & 0x8000)
                                    {
                                        if (k == VK_ESCAPE)
                                        {
                                            s_WaitingForAction = -1;
                                            SetStatusMessage("Keybinding cancelled.");
                                        }
                                        else
                                        {
                                            vkTarget = k;
                                            s_WaitingForAction = -1;
                                            SaveConfig();
                                            SetStatusMessage("Key successfully assigned & saved!");
                                        }
                                        break;
                                    }
                                }
                            }
                        }
                        else
                        {
                            const char* keyName = GetKeyNameString(vkTarget);
                            std::string keyNameStr = (keyName && keyName[0]) ? keyName : "Unknown";
                            std::string label = keyNameStr + " (Assign Key)##kb" + std::to_string(actionIndex);
                            if (ImGui::Button(label.c_str(), ImVec2(240, 24)))
                            {
                                s_WaitingForAction = actionIndex;
                                s_ListenStartTime = GetTickCount();
                            }
                        }
                        ImGui::NextColumn();
                    };

                    RenderKeybindRow("Forward / Gas", 0, g_Config.keyForward);
                    RenderKeybindRow("Backward / Reverse", 1, g_Config.keyBackward);
                    RenderKeybindRow("Steer Left", 2, g_Config.keyLeft);
                    RenderKeybindRow("Steer Right", 3, g_Config.keyRight);
                    RenderKeybindRow("Brake", 4, g_Config.keyBrake);
                    RenderKeybindRow("Shift Drive", 5, g_Config.keyGearDrive);
                    RenderKeybindRow("Shift Reverse", 6, g_Config.keyGearReverse);

                    ImGui::Columns(1);
                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();

                    if (ImGui::Button("Restore Default Keybindings", ImVec2(280, 30)))
                    {
                        g_Config.SetDefaultKeybinds(g_Config.controlMode);
                        SaveConfig();
                        SetStatusMessage("Keybindings restored to defaults!");
                    }
                }

                ImGui::Spacing();

                // 8. INTERFACE & THEME
                if (ImGui::CollapsingHeader("Interface & Theme", ImGuiTreeNodeFlags_DefaultOpen))
                {
                    if (ImGui::Checkbox("Feminine GUI Mode", &g_Config.feminineGuiMode))
                    {
                        SaveConfig();
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Switches the Mod Manager, overlay, and speedometer HUD to a stylish Barbie pink & pastel rose aesthetic.\nDefault: Disabled.");
                }

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                // 9. CONFIGURATION & PERSISTENCE
                ImGui::TextDisabled("Config: mods/Framework_MOD/crazytaxi_mod_config.json");
                ImGui::Spacing();

                if (ImGui::Button("Save Config", ImVec2(180, 32)))
                {
                    if (SaveConfig())
                    {
                        SetStatusMessage("Configuration saved!");
                    }
                    else
                    {
                        SetStatusMessage("Failed to save configuration!");
                    }
                }

                ImGui::SameLine();
                if (ImGui::Button("Reset to Defaults", ImVec2(190, 32)))
                {
                    g_Config.ResetToDefaults();
                    SaveConfig();
                    SetStatusMessage("All settings reset to defaults!");
                }

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                if (ImGui::Button("Return to Game", ImVec2(220, 36)))
                {
                    SetMenuOpenState(false);
                }

                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
    else if (s_SelectedModIndex == 1)
    {
        ImGui::TextColored(kColRose, "MOD: DC Restoration (v2.0)");
        ImGui::TextDisabled("by CookiePLMonster (Silent) & Alexvgz");
        ImGui::Separator();
        ImGui::Spacing();

        // Live status
        bool isActive = DCRestoration::IsActive();
        if (isActive)
        {
            ImGui::TextColored(kColSage, "[ACTIVE] Patch layer operational");
        }
        else
        {
            ImGui::TextColored(kColPeach, "[STANDBY] Hooks armed");
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Master Toggle
        if (ImGui::Checkbox("Dreamcast Restoration##DCMaster", &g_Config.dcRestorationEnabled))
        {
            DCRestoration::UpdatePatches();
            SaveConfig();
            SetStatusMessage(g_Config.dcRestorationEnabled ? "Dreamcast Restoration enabled!" : "Dreamcast Restoration disabled.");
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Globally enables all CT-DC restoration patches.");
        ImGui::Spacing();

        // Toggle 1: Original Brand Names
        if (ImGui::Checkbox("Original Brand Names##DCNames", &g_Config.dcOriginalNames))
        {
            DCRestoration::UpdatePatches();
            SaveConfig();
            SetStatusMessage("Brand destination names updated.");
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Restores Arcade/Dreamcast brand destinations\n(Pizza Hut, KFC, Tower Records, FILA).");
        ImGui::Spacing();

        // Toggle 2: FILA Customer Destination Fix
        if (ImGui::Checkbox("FILA Destination##DCFila", &g_Config.dcFilaDestination))
        {
            DCRestoration::UpdatePatches();
            SaveConfig();
            SetStatusMessage("FILA destination fix updated.");
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Customers request fares to the FILA store.");
        ImGui::Spacing();

        // Toggle 3: Dreamcast 3D Models & Clothes
        if (ImGui::Checkbox("Dreamcast Models##DCModels", &g_Config.dcDreamcastModels))
        {
            DCRestoration::UpdatePatches();
            SaveConfig();
            SetStatusMessage("Dreamcast 3D model patch updated.");
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Loads original Dreamcast 3D model BINs\nand licensed passenger attire.");
        ImGui::Spacing();

        // Toggle 4: Original Voices & Audio Lines
        if (ImGui::Checkbox("Original Voices##DCVoices", &g_Config.dcOriginalVoices))
        {
            DCRestoration::UpdatePatches();
            SaveConfig();
            SetStatusMessage("Passenger voice patch updated.");
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Authentic Dreamcast passenger voice lines.");
        ImGui::Spacing();

        // Toggle 5: Original Dreamcast World Textures & Map Icons
        if (ImGui::Checkbox("Original Textures##DCTextures", &g_Config.dcOriginalTextures))
        {
            DCRestoration::UpdatePatches();
            SaveConfig();
            SetStatusMessage("World textures patch updated.");
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Authentic billboards, world textures and menu thumbnails.");
        ImGui::Spacing();

        // Toggle 6: Destination Logo Z-Bias Fix
        if (ImGui::Checkbox("Logo Z-Bias Fix##DCZBias", &g_Config.dcLogosZBias))
        {
            DCRestoration::UpdatePatches();
            SaveConfig();
            SetStatusMessage("Logo Z-Bias fix updated.");
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Prevents z-fighting flicker on logos and billboards.");
        ImGui::Spacing();

        // Toggle 7: Original Arcade Soundtrack (The Offspring & Bad Religion)
        if (ImGui::Checkbox("Original Soundtrack##DCOrigSoundtrack", &g_Config.useOriginalSoundtrack))
        {
            ApplySoundtrackSetting(g_Config.useOriginalSoundtrack);
            SaveConfig();
            SetStatusMessage(g_Config.useOriginalSoundtrack ? "Original soundtrack active." : "Steam soundtrack restored.");
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("The Offspring & Bad Religion (AirChomp mix).");
        ImGui::Spacing();

        ImGui::Separator();
        ImGui::Spacing();

        // Action Buttons for quick setup
        if (ImGui::Button("Enable All##DCAll", ImVec2(200, 30)))
        {
            g_Config.dcRestorationEnabled = true;
            g_Config.dcOriginalNames = true;
            g_Config.dcFilaDestination = true;
            g_Config.dcDreamcastModels = true;
            g_Config.dcOriginalVoices = true;
            g_Config.dcOriginalTextures = true;
            g_Config.dcLogosZBias = true;
            DCRestoration::UpdatePatches();
            SaveConfig();
            SetStatusMessage("All Dreamcast Restoration features enabled!");
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset to Vanilla##DCVanilla", ImVec2(180, 30)))
        {
            g_Config.dcRestorationEnabled = false;
            g_Config.dcOriginalNames = false;
            g_Config.dcFilaDestination = false;
            g_Config.dcDreamcastModels = false;
            g_Config.dcOriginalVoices = false;
            g_Config.dcOriginalTextures = false;
            g_Config.dcLogosZBias = false;
            DCRestoration::UpdatePatches();
            SaveConfig();
            SetStatusMessage("Dreamcast features reset to vanilla Steam state.");
        }
        ImGui::SameLine();
        if (ImGui::Button("Save##DCSave", ImVec2(120, 30)))
        {
            SaveConfig();
            SetStatusMessage("Configuration saved!");
        }
    } // else if (s_SelectedModIndex == 1)
    else if (s_SelectedModIndex == 2)
    {
        ImGui::TextColored(kColRose, "MOD: Texture Mod (v1.0)");
        ImGui::TextDisabled("by Lyra");
        ImGui::Separator();
        ImGui::Spacing();

        HMODULE hTexMod = GetModuleHandleA("LyrasTextureMod.dll");
        typedef bool (*tGetEnabled)();
        typedef void (*tSetEnabled)(bool);
        typedef void (*tReload)();
        typedef void (*tFlush)();

        tGetEnabled pfnGet = hTexMod ? (tGetEnabled)GetProcAddress(hTexMod, "GetTextureModEnabled") : nullptr;
        tSetEnabled pfnSet = hTexMod ? (tSetEnabled)GetProcAddress(hTexMod, "SetTextureModEnabled") : nullptr;
        tReload pfnReload = hTexMod ? (tReload)GetProcAddress(hTexMod, "ReloadTextureMod") : nullptr;
        tFlush pfnFlush = hTexMod ? (tFlush)GetProcAddress(hTexMod, "FlushTextureCache") : nullptr;

        bool isTexEnabled = pfnGet ? pfnGet() : true;
        if (ImGui::Checkbox("Texture Mod [ON/OFF]##TexModToggle", &isTexEnabled))
        {
            if (pfnSet) pfnSet(isTexEnabled);
            SetStatusMessage(isTexEnabled ? "Texture Mod enabled." : "Texture Mod disabled (vanilla textures).");
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Injects modified textures from mods/LyrasTextureMod/textures/.");
        ImGui::Spacing();

        ImGui::TextColored(isTexEnabled ? kColSage : kColPeach, isTexEnabled ? "Status: Online" : "Status: Offline");
        ImGui::Spacing();

        ImGui::Separator();
        ImGui::Spacing();

        if (ImGui::Button("Reload Textures", ImVec2(180, 30)))
        {
            if (pfnReload) pfnReload();
            SetStatusMessage("Textures reloaded!");
        }
        ImGui::SameLine();
        if (ImGui::Button("Flush Cache", ImVec2(180, 30)))
        {
            if (pfnFlush) pfnFlush();
            SetStatusMessage("Cache flushed (recompiles on next load).");
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Forces modified .png files to recompile into mods/LyrasTextureMod/cache/.");
    } // else if (s_SelectedModIndex == 2)
    else if (s_SelectedModIndex >= 3)
    {
        size_t subIdx = (size_t)(s_SelectedModIndex - 3);
        if (subIdx < s_DiscoveredSubMods.size())
        {
            auto& subMod = s_DiscoveredSubMods[subIdx];
            const char* subName = subMod.name.empty() ? "SubMod" : subMod.name.c_str();
            const char* subVer = subMod.version.empty() ? "1.0" : subMod.version.c_str();
            const char* subAuth = subMod.author.empty() ? "Community" : subMod.author.c_str();
            ImGui::TextColored(kColRose, "MOD: %s (v%s)", subName, subVer);
            ImGui::TextDisabled("by %s", subAuth);
            ImGui::Separator();
            ImGui::Spacing();

            // Toggle checkbox if supported
            if (subMod.pfnIsEnabled && subMod.pfnSetEnabled)
            {
                bool bEnabled = GateCallIsEnabled(subMod);
                std::string chkLabel = subMod.name + " [ON/OFF]##SubToggle" + std::to_string(subIdx);
                if (ImGui::Checkbox(chkLabel.c_str(), &bEnabled))
                {
                    GateCallSetEnabled(subMod, bEnabled);
                    SetStatusMessage(subMod.name + (bEnabled ? " enabled!" : " disabled."));
                }
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();
            }

            // Real-Time SubMod Safety Gate UI Execution
            if (subMod.isQuarantined)
            {
                ImGui::Spacing();
                ImGui::PushStyleColor(ImGuiCol_Text, kColCoral);
                ImGui::Text("REAL-TIME SUBMOD SAFETY GATE: ACTIVE QUARANTINE");
                ImGui::PopStyleColor();
                ImGui::TextWrapped("The safety gate intercepted repeated unhandled exceptions (Code: 0x%08X) from this submod and safely quarantined it in real time to prevent game crashes or process freeze.", subMod.lastExceptionCode);
                ImGui::Spacing();
                if (ImGui::Button("Reset & Retry SubMod Gate", ImVec2(220, 30)))
                {
                    subMod.errorCount = 0;
                    subMod.isQuarantined = false;
                    subMod.consecutiveSuccessFrames = 0;
                }
            }
            else if (subMod.pfnRenderUI)
            {
                if (!GateCallRenderUI(subMod, (void*)ImGui::GetCurrentContext()))
                {
                    ImGui::TextColored(kColCoral, "[SubMod Gate: Exception intercepted in UI callback (Code: 0x%08X)]", subMod.lastExceptionCode);
                }
            }
            else
            {
                ImGui::TextDisabled("This sub-mod provides no custom UI.");
            }
        }
    }

    ImGui::EndChild(); // ModContentPanel
} // block around ModContentPanel

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextDisabled("Shift+F1 Menu | Esc Close");
    } // if (ImGui::Begin(...))
    ImGui::End();

    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(18);
} // RenderModMenuInternal

void RenderModMenu()
{
    __try {
        RenderModMenuInternal();
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        Logger::Error("Render", "Exception caught in RenderModMenuInternal! Closing menu for safety.");
        g_bMenuOpen = false;
        while (ImGui::GetCurrentContext() && ImGui::GetCurrentContext()->CurrentWindowStack.Size > 1)
        {
            ImGuiWindow* window = ImGui::GetCurrentWindow();
            if (window && (window->Flags & ImGuiWindowFlags_ChildWindow))
                ImGui::EndChild();
            else
                ImGui::End();
        }
    }
}

static void SaveSurfaceToBMP(const char* filepath, const BYTE* pBits, int width, int height, int pitch, D3DFORMAT fmt)
{
    FILE* fp = fopen(filepath, "wb");
    if (!fp) return;

    BITMAPFILEHEADER bfh = { 0 };
    BITMAPINFOHEADER bih = { 0 };

    int rowSize = ((width * 3 + 3) & ~3);
    int imageSize = rowSize * height;

    bfh.bfType = 0x4D42; // 'BM'
    bfh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    bfh.bfSize = bfh.bfOffBits + imageSize;

    bih.biSize = sizeof(BITMAPINFOHEADER);
    bih.biWidth = width;
    bih.biHeight = height;
    bih.biPlanes = 1;
    bih.biBitCount = 24;
    bih.biCompression = 0; // BI_RGB
    bih.biSizeImage = imageSize;

    fwrite(&bfh, sizeof(bfh), 1, fp);
    fwrite(&bih, sizeof(bih), 1, fp);

    std::vector<BYTE> rowBuf(rowSize, 0);
    for (int y = height - 1; y >= 0; --y)
    {
        const BYTE* srcRow = pBits + (y * pitch);
        BYTE* dst = rowBuf.data();
        if (fmt == D3DFMT_R5G6B5)
        {
            for (int x = 0; x < width; ++x)
            {
                WORD px = *(const WORD*)(srcRow + x * 2);
                dst[0] = (BYTE)((px & 0x001F) << 3);
                dst[1] = (BYTE)(((px >> 5) & 0x003F) << 2);
                dst[2] = (BYTE)(((px >> 11) & 0x001F) << 3);
                dst += 3;
            }
        }
        else
        {
            for (int x = 0; x < width; ++x)
            {
                dst[0] = srcRow[x * 4 + 0]; // B
                dst[1] = srcRow[x * 4 + 1]; // G
                dst[2] = srcRow[x * 4 + 2]; // R
                dst += 3;
            }
        }
        fwrite(rowBuf.data(), 1, rowSize, fp);
    }

    fclose(fp);
}

static void CaptureD3D9Screenshot(IDirect3DDevice9* pDevice, const char* filepath)
{
    if (!pDevice) return;

    IDirect3DSurface9* pBackBuffer = nullptr;
    if (SUCCEEDED(pDevice->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &pBackBuffer)) && pBackBuffer)
    {
        HMODULE hD3DX = GetModuleHandleA("d3dx9_43.dll");
        if (!hD3DX) hD3DX = LoadLibraryA("d3dx9_43.dll");
        if (hD3DX)
        {
            typedef HRESULT (WINAPI *tD3DXSaveSurfaceToFileA)(LPCSTR, DWORD, LPDIRECT3DSURFACE9, const PALETTEENTRY*, const RECT*);
            tD3DXSaveSurfaceToFileA pSave = (tD3DXSaveSurfaceToFileA)GetProcAddress(hD3DX, "D3DXSaveSurfaceToFileA");
            if (pSave)
            {
                HRESULT hr = pSave(filepath, 0 /* D3DXIFF_BMP */, pBackBuffer, NULL, NULL);
                if (SUCCEEDED(hr))
                {
                    pBackBuffer->Release();
                    return;
                }
            }
        }

        D3DSURFACE_DESC desc;
        pBackBuffer->GetDesc(&desc);

        IDirect3DSurface9* pOffscreen = nullptr;
        if (SUCCEEDED(pDevice->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &pOffscreen, nullptr)) && pOffscreen)
        {
            if (SUCCEEDED(pDevice->GetRenderTargetData(pBackBuffer, pOffscreen)))
            {
                D3DLOCKED_RECT lr;
                if (SUCCEEDED(pOffscreen->LockRect(&lr, nullptr, D3DLOCK_READONLY)))
                {
                    SaveSurfaceToBMP(filepath, (const BYTE*)lr.pBits, desc.Width, desc.Height, lr.Pitch, desc.Format);
                    pOffscreen->UnlockRect();
                }
            }
            pOffscreen->Release();
        }
        pBackBuffer->Release();
    }
}

// ---------------------------------------------------------------------------------
// Direct3D 9 Hooks: EndScene & Reset
// ---------------------------------------------------------------------------------
static HRESULT APIENTRY hkEndScene(IDirect3DDevice9* pDevice)
{
    SyncSegaDevicePointers(pDevice);

    if (!s_ImGuiInitialized && pDevice)
    {
        D3DDEVICE_CREATION_PARAMETERS cp;
        if (SUCCEEDED(pDevice->GetCreationParameters(&cp)))
        {
            ImGui::CreateContext();
            ImGuiIO& io = ImGui::GetIO();
            io.IniFilename = NULL; // Disable imgui.ini

            // Load crisp TrueType font with high-quality oversampling
            ImFontConfig fontConfig;
            fontConfig.OversampleH = 3;
            fontConfig.OversampleV = 2;
            fontConfig.PixelSnapH = true;
            char winDir[MAX_PATH] = { 0 };
            bool fontLoaded = false;
            if (GetWindowsDirectoryA(winDir, MAX_PATH))
            {
                std::string segoePath = std::string(winDir) + "\\Fonts\\segoeui.ttf";
                std::string arialPath = std::string(winDir) + "\\Fonts\\arial.ttf";
                if (GetFileAttributesA(segoePath.c_str()) != INVALID_FILE_ATTRIBUTES)
                {
                    io.Fonts->AddFontFromFileTTF(segoePath.c_str(), 16.0f, &fontConfig);
                    fontLoaded = true;
                }
                else if (GetFileAttributesA(arialPath.c_str()) != INVALID_FILE_ATTRIBUTES)
                {
                    io.Fonts->AddFontFromFileTTF(arialPath.c_str(), 16.0f, &fontConfig);
                    fontLoaded = true;
                }
            }
            if (!fontLoaded)
            {
                io.Fonts->AddFontDefault(&fontConfig);
            }

            // Setup Win32 and DX9 backends
            ImGui_ImplWin32_Init(cp.hFocusWindow);
            ImGui_ImplDX9_Init(pDevice);

            // Hook input on focus window
            InitInputHook(cp.hFocusWindow);

            s_ImGuiInitialized = true;
        }
    }

    if (s_ImGuiInitialized)
    {
        // Guard against rendering on lost device (e.g. during V-Sync mode switches or Alt+Tab)
        if (pDevice->TestCooperativeLevel() != D3D_OK)
        {
            return oEndScene(pDevice);
        }

        // Update physics and features per frame
        UpdateGameFeaturesPerFrame();
        EngineFixes::UpdateHotkeys();
        FFBArcade::UpdatePerFrame();

        ImGui_ImplDX9_NewFrame();
        ImGui_ImplWin32_NewFrame();

        // Synchronize ImGui DisplaySize with native D3D9 backbuffer and scale MousePos
        // so clicking aligns 1:1 with visual elements on screen without vertical offset or downward drift!
        IDirect3DSurface9* pBackBuffer = nullptr;
        if (SUCCEEDED(pDevice->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &pBackBuffer)) && pBackBuffer)
        {
            D3DSURFACE_DESC bbDesc;
            if (SUCCEEDED(pBackBuffer->GetDesc(&bbDesc)))
            {
                HWND hWnd = g_hGameWnd;
                if (!hWnd)
                {
                    D3DDEVICE_CREATION_PARAMETERS cp;
                    if (SUCCEEDED(pDevice->GetCreationParameters(&cp)))
                        hWnd = cp.hFocusWindow;
                }
                RECT cr;
                if (hWnd && GetClientRect(hWnd, &cr))
                {
                    float clientW = (float)(cr.right - cr.left);
                    float clientH = (float)(cr.bottom - cr.top);
                    if (clientW > 0.0f && clientH > 0.0f)
                    {
                        float scaleX = (float)bbDesc.Width / clientW;
                        float scaleY = (float)bbDesc.Height / clientH;

                        ImGuiIO& io = ImGui::GetIO();
                        io.DisplaySize = ImVec2((float)bbDesc.Width, (float)bbDesc.Height);

                        // Obtain physical OS cursor position and map to client rect deterministically.
                        // NEVER multiply io.MousePos in-place cumulatively, as frames without WM_MOUSEMOVE
                        // cause exponential coordinate explosion and severe downward/rightward drift!
                        POINT curPt;
                        if (GetCursorPos(&curPt) && ScreenToClient(hWnd, &curPt))
                        {
                            if (curPt.x >= 0 && curPt.y >= 0 && curPt.x <= (int)clientW && curPt.y <= (int)clientH)
                            {
                                io.MousePos = ImVec2((float)curPt.x * scaleX, (float)curPt.y * scaleY);
                            }
                        }
                    }
                }
            }
            pBackBuffer->Release();
        }

        ImGui::NewFrame();

        // 1. Permanent Coordinate HUD (always drawn!)
        RenderPermanentCoordinateHUD();

        // 2. Analog speedometer with integrated horizontal Nitro bar (only when enabled and in-game)
        RenderSpeedometerHUD();

        // 3. Standalone Nitro Gauge HUD retired (now seamlessly embedded inside the speedometer cluster)
        // RenderNitroHUD();

        // 4. Mod Menu (when toggled open via Shift + F1)
        if (g_bMenuOpen)
        {
            RenderModMenu();
        }

        ImGui::EndFrame();
        ImGui::Render();
        ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());

        static int s_FrameCounter = 0;
        s_FrameCounter++;

        // Live screenshot capture disabled for stability
        // Only execute automated test screenshot sequence if explicitly enabled via CRAZYTAXI_TEST_CAPTURE environment variable!
        static bool s_TestCaptureMode = (getenv("CRAZYTAXI_TEST_CAPTURE") != nullptr);
        if (s_TestCaptureMode)
        {
            if (s_FrameCounter == 60)
            {
                CaptureD3D9Screenshot(pDevice, (GetModDirectory() + "hud_active.bmp").c_str());
            }
            else if (s_FrameCounter == 90)
            {
                s_SelectedModIndex = 0;
                s_ForceLyraTab = 0;
                SetMenuOpenState(true);
            }
            else if (s_FrameCounter == 110)
            {
                CaptureD3D9Screenshot(pDevice, (GetModDirectory() + "mod_menu_open.bmp").c_str());
            }
            else if (s_FrameCounter == 115)
            {
                s_ForceLyraTab = 1; // Switch to Camera tab
            }
            else if (s_FrameCounter == 125)
            {
                CaptureD3D9Screenshot(pDevice, (GetModDirectory() + "mod_menu_camera.bmp").c_str());
            }
            else if (s_FrameCounter == 130)
            {
                s_SelectedModIndex = 1; // Auto-switch to CT-DC page for verification capture
            }
            else if (s_FrameCounter == 145)
            {
                CaptureD3D9Screenshot(pDevice, (GetModDirectory() + "mod_menu_ct_dc.bmp").c_str());
            }
            else if (s_FrameCounter == 155)
            {
                s_SelectedModIndex = 0;
                s_ForceLyraTab = -1;
                SetMenuOpenState(false);
            }
            else if (s_FrameCounter == 170)
            {
                CaptureD3D9Screenshot(pDevice, (GetModDirectory() + "mod_menu_closed.bmp").c_str());
            }
        }
    }

    return oEndScene(pDevice);
}

// ---------------------------------------------------------------------------------
// Managed Texture Emulation for Direct3D9Ex / D3D9On12 (Intel Iris Xe compatibility)
// ---------------------------------------------------------------------------------
#include <unordered_map>

struct ManagedTexInfo
{
    IDirect3DTexture9* pDefTex;
    IDirect3DDevice9* pDevice;
    bool bDirty;
};

static std::unordered_map<IDirect3DTexture9*, ManagedTexInfo> s_ManagedTextures;
static std::unordered_map<IDirect3DSurface9*, IDirect3DTexture9*> s_SurfaceToTexture;
static CRITICAL_SECTION s_TexCS;
static bool s_TexCSInitialized = false;

static HRESULT APIENTRY hkReset(IDirect3DDevice9* pDevice, D3DPRESENT_PARAMETERS* pPresentationParameters)
{
    if (s_ImGuiInitialized)
    {
        ImGui_ImplDX9_InvalidateDeviceObjects();
    }

    // Release any DEFAULT pool proxy textures before oReset so D3DERR_INVALIDCALL is avoided
    if (s_TexCSInitialized)
    {
        EnterCriticalSection(&s_TexCS);
        for (auto& pair : s_ManagedTextures)
        {
            if (pair.second.pDefTex)
            {
                pair.second.pDefTex->Release();
                pair.second.pDefTex = nullptr;
            }
        }
        LeaveCriticalSection(&s_TexCS);
    }

    HRESULT hr = oReset(pDevice, pPresentationParameters);

    if (SUCCEEDED(hr))
    {
        SyncSegaDevicePointers(pDevice);
        if (s_ImGuiInitialized)
        {
            ImGui_ImplDX9_CreateDeviceObjects();
        }
    }

    return hr;
}



static bool s_TextureVTableHooked = false;
static bool s_SurfaceVTableHooked = false;

// Texture VTable hook pointers
typedef HRESULT (APIENTRY *tGetSurfaceLevel)(IDirect3DTexture9*, UINT, IDirect3DSurface9**);
static tGetSurfaceLevel oGetSurfaceLevel = nullptr;

typedef HRESULT (APIENTRY *tTextureUnlockRect)(IDirect3DTexture9*, UINT);
static tTextureUnlockRect oTextureUnlockRect = nullptr;

typedef ULONG (APIENTRY *tTextureRelease)(IDirect3DTexture9*);
static tTextureRelease oTextureRelease = nullptr;

// Surface VTable hook pointers
typedef HRESULT (APIENTRY *tSurfaceUnlockRect)(IDirect3DSurface9*);
static tSurfaceUnlockRect oSurfaceUnlockRect = nullptr;

typedef ULONG (APIENTRY *tSurfaceRelease)(IDirect3DSurface9*);
static tSurfaceRelease oSurfaceRelease = nullptr;

static HRESULT APIENTRY hkGetSurfaceLevel(IDirect3DTexture9* pTexture, UINT Level, IDirect3DSurface9** ppSurfaceLevel);
static HRESULT APIENTRY hkTextureUnlockRect(IDirect3DTexture9* pTexture, UINT Level);
static ULONG APIENTRY hkTextureRelease(IDirect3DTexture9* pTexture);
static HRESULT APIENTRY hkSurfaceUnlockRect(IDirect3DSurface9* pSurface);
static ULONG APIENTRY hkSurfaceRelease(IDirect3DSurface9* pSurface);

static void HookTextureVTable(IDirect3DTexture9* pTexture)
{
    if (s_TextureVTableHooked || !pTexture) return;
    void** pTexVT = *(void***)pTexture;

    if (MH_CreateHook(pTexVT[18], (LPVOID)&hkGetSurfaceLevel, (LPVOID*)&oGetSurfaceLevel) == MH_OK)
    {
        MH_EnableHook(pTexVT[18]);
    }
    if (MH_CreateHook(pTexVT[20], (LPVOID)&hkTextureUnlockRect, (LPVOID*)&oTextureUnlockRect) == MH_OK)
    {
        MH_EnableHook(pTexVT[20]);
    }
    if (MH_CreateHook(pTexVT[2], (LPVOID)&hkTextureRelease, (LPVOID*)&oTextureRelease) == MH_OK)
    {
        MH_EnableHook(pTexVT[2]);
    }

    s_TextureVTableHooked = true;
}

static void HookSurfaceVTable(IDirect3DSurface9* pSurface)
{
    if (s_SurfaceVTableHooked || !pSurface) return;
    void** pSurfVT = *(void***)pSurface;

    if (MH_CreateHook(pSurfVT[14], (LPVOID)&hkSurfaceUnlockRect, (LPVOID*)&oSurfaceUnlockRect) == MH_OK)
    {
        MH_EnableHook(pSurfVT[14]);
    }
    if (MH_CreateHook(pSurfVT[2], (LPVOID)&hkSurfaceRelease, (LPVOID*)&oSurfaceRelease) == MH_OK)
    {
        MH_EnableHook(pSurfVT[2]);
    }

    s_SurfaceVTableHooked = true;
}

static HRESULT APIENTRY hkGetSurfaceLevel(IDirect3DTexture9* pTexture, UINT Level, IDirect3DSurface9** ppSurfaceLevel)
{
    HRESULT hr = oGetSurfaceLevel(pTexture, Level, ppSurfaceLevel);
    if (SUCCEEDED(hr) && ppSurfaceLevel && *ppSurfaceLevel)
    {
        EnterCriticalSection(&s_TexCS);
        s_SurfaceToTexture[*ppSurfaceLevel] = pTexture;
        LeaveCriticalSection(&s_TexCS);

        HookSurfaceVTable(*ppSurfaceLevel);
    }
    return hr;
}

static HRESULT APIENTRY hkTextureUnlockRect(IDirect3DTexture9* pTexture, UINT Level)
{
    EnterCriticalSection(&s_TexCS);
    auto it = s_ManagedTextures.find(pTexture);
    if (it != s_ManagedTextures.end())
    {
        it->second.bDirty = true;
    }
    LeaveCriticalSection(&s_TexCS);

    return oTextureUnlockRect(pTexture, Level);
}

static HRESULT APIENTRY hkSurfaceUnlockRect(IDirect3DSurface9* pSurface)
{
    EnterCriticalSection(&s_TexCS);
    auto itSurf = s_SurfaceToTexture.find(pSurface);
    if (itSurf != s_SurfaceToTexture.end())
    {
        auto itTex = s_ManagedTextures.find(itSurf->second);
        if (itTex != s_ManagedTextures.end())
        {
            itTex->second.bDirty = true;
        }
    }
    LeaveCriticalSection(&s_TexCS);

    return oSurfaceUnlockRect(pSurface);
}

static ULONG APIENTRY hkSurfaceRelease(IDirect3DSurface9* pSurface)
{
    EnterCriticalSection(&s_TexCS);
    s_SurfaceToTexture.erase(pSurface);
    LeaveCriticalSection(&s_TexCS);

    return oSurfaceRelease(pSurface);
}

static ULONG APIENTRY hkTextureRelease(IDirect3DTexture9* pTexture)
{
    EnterCriticalSection(&s_TexCS);
    auto it = s_ManagedTextures.find(pTexture);
    IDirect3DTexture9* pDefTexToRelease = nullptr;

    if (it != s_ManagedTextures.end())
    {
        ULONG ref = oTextureRelease(pTexture);
        if (ref == 0)
        {
            pDefTexToRelease = it->second.pDefTex;
            s_ManagedTextures.erase(it);
        }
        LeaveCriticalSection(&s_TexCS);

        if (pDefTexToRelease)
        {
            pDefTexToRelease->Release();
        }
        return ref;
    }
    LeaveCriticalSection(&s_TexCS);

    return oTextureRelease(pTexture);
}

// ---------------------------------------------------------------------------------
// Device Hooks
// ---------------------------------------------------------------------------------
typedef HRESULT (APIENTRY *tSetViewport)(IDirect3DDevice9* pDevice, CONST D3DVIEWPORT9* pViewport);
static tSetViewport oSetViewport = nullptr;
static DWORD s_ScreenWidth = 0;
static DWORD s_ScreenHeight = 0;

static HRESULT APIENTRY hkSetViewport(IDirect3DDevice9* pDevice, CONST D3DVIEWPORT9* pViewport)
{
    if (pViewport)
    {
        // Track the full screen resolution
        if (pViewport->X == 0 && pViewport->Y == 0 && pViewport->Width >= 320 && pViewport->Height >= 240)
        {
            if (pViewport->Width > s_ScreenWidth)   s_ScreenWidth = pViewport->Width;
            if (pViewport->Height > s_ScreenHeight) s_ScreenHeight = pViewport->Height;
        }

    }
    return oSetViewport(pDevice, pViewport);
}

typedef HRESULT (APIENTRY *tSetTexture)(IDirect3DDevice9* pDevice, DWORD Sampler, IDirect3DBaseTexture9* pTexture);
static tSetTexture oSetTexture = nullptr;

static HRESULT APIENTRY hkSetTexture(IDirect3DDevice9* pDevice, DWORD Sampler, IDirect3DBaseTexture9* pTexture)
{
    return oSetTexture(pDevice, Sampler, pTexture);
}

typedef HRESULT (APIENTRY *tCreateTexture)(
    IDirect3DDevice9* pDevice,
    UINT Width, UINT Height, UINT Levels,
    DWORD Usage, D3DFORMAT Format, D3DPOOL Pool,
    IDirect3DTexture9** ppTexture, HANDLE* pSharedHandle
);
static tCreateTexture oCreateTexture = nullptr;

static HRESULT APIENTRY hkCreateTexture(
    IDirect3DDevice9* pDevice,
    UINT Width, UINT Height, UINT Levels,
    DWORD Usage, D3DFORMAT Format, D3DPOOL Pool,
    IDirect3DTexture9** ppTexture, HANDLE* pSharedHandle
)
{
    // Try native creation first (native standard Direct3D 9 supports D3DPOOL_MANAGED directly without error)
    HRESULT hr = oCreateTexture(pDevice, Width, Height, Levels, Usage, Format, Pool, ppTexture, pSharedHandle);
    if (SUCCEEDED(hr))
    {
        return hr;
    }

    // Fallback only if D3DPOOL_MANAGED was rejected by a D3D9Ex device:
    if (Pool == D3DPOOL_MANAGED)
    {
        DWORD fallbackUsage = Usage | D3DUSAGE_DYNAMIC;
        hr = oCreateTexture(pDevice, Width, Height, Levels, fallbackUsage, Format, D3DPOOL_DEFAULT, ppTexture, pSharedHandle);
    }

    return hr;
}

typedef HRESULT (APIENTRY *tCreateVolumeTexture)(
    IDirect3DDevice9* pDevice,
    UINT Width, UINT Height, UINT Depth, UINT Levels,
    DWORD Usage, D3DFORMAT Format, D3DPOOL Pool,
    IDirect3DVolumeTexture9** ppVolumeTexture, HANDLE* pSharedHandle
);
static tCreateVolumeTexture oCreateVolumeTexture = nullptr;

static HRESULT APIENTRY hkCreateVolumeTexture(
    IDirect3DDevice9* pDevice,
    UINT Width, UINT Height, UINT Depth, UINT Levels,
    DWORD Usage, D3DFORMAT Format, D3DPOOL Pool,
    IDirect3DVolumeTexture9** ppVolumeTexture, HANDLE* pSharedHandle
)
{
    HRESULT hr = oCreateVolumeTexture(pDevice, Width, Height, Depth, Levels, Usage, Format, Pool, ppVolumeTexture, pSharedHandle);
    if (SUCCEEDED(hr)) return hr;
    if (Pool == D3DPOOL_MANAGED)
    {
        Pool = D3DPOOL_DEFAULT;
        Usage |= D3DUSAGE_DYNAMIC;
        hr = oCreateVolumeTexture(pDevice, Width, Height, Depth, Levels, Usage, Format, Pool, ppVolumeTexture, pSharedHandle);
    }
    return hr;
}

typedef HRESULT (APIENTRY *tCreateCubeTexture)(
    IDirect3DDevice9* pDevice,
    UINT EdgeLength, UINT Levels,
    DWORD Usage, D3DFORMAT Format, D3DPOOL Pool,
    IDirect3DCubeTexture9** ppCubeTexture, HANDLE* pSharedHandle
);
static tCreateCubeTexture oCreateCubeTexture = nullptr;

static HRESULT APIENTRY hkCreateCubeTexture(
    IDirect3DDevice9* pDevice,
    UINT EdgeLength, UINT Levels,
    DWORD Usage, D3DFORMAT Format, D3DPOOL Pool,
    IDirect3DCubeTexture9** ppCubeTexture, HANDLE* pSharedHandle
)
{
    HRESULT hr = oCreateCubeTexture(pDevice, EdgeLength, Levels, Usage, Format, Pool, ppCubeTexture, pSharedHandle);
    if (SUCCEEDED(hr)) return hr;
    if (Pool == D3DPOOL_MANAGED)
    {
        Pool = D3DPOOL_DEFAULT;
        Usage |= D3DUSAGE_DYNAMIC;
        hr = oCreateCubeTexture(pDevice, EdgeLength, Levels, Usage, Format, Pool, ppCubeTexture, pSharedHandle);
    }
    return hr;
}

typedef HRESULT (APIENTRY *tCreateVertexBuffer)(
    IDirect3DDevice9* pDevice,
    UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool,
    IDirect3DVertexBuffer9** ppVertexBuffer, HANDLE* pSharedHandle
);
static tCreateVertexBuffer oCreateVertexBuffer = nullptr;

static HRESULT APIENTRY hkCreateVertexBuffer(
    IDirect3DDevice9* pDevice,
    UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool,
    IDirect3DVertexBuffer9** ppVertexBuffer, HANDLE* pSharedHandle
)
{
    HRESULT hr = oCreateVertexBuffer(pDevice, Length, Usage, FVF, Pool, ppVertexBuffer, pSharedHandle);
    if (SUCCEEDED(hr)) return hr;
    if (Pool == D3DPOOL_MANAGED)
    {
        Pool = D3DPOOL_DEFAULT;
        Usage |= (D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY);
        hr = oCreateVertexBuffer(pDevice, Length, Usage, FVF, Pool, ppVertexBuffer, pSharedHandle);
    }
    return hr;
}

typedef HRESULT (APIENTRY *tCreateIndexBuffer)(
    IDirect3DDevice9* pDevice,
    UINT Length, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool,
    IDirect3DIndexBuffer9** ppIndexBuffer, HANDLE* pSharedHandle
);
static tCreateIndexBuffer oCreateIndexBuffer = nullptr;

static HRESULT APIENTRY hkCreateIndexBuffer(
    IDirect3DDevice9* pDevice,
    UINT Length, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool,
    IDirect3DIndexBuffer9** ppIndexBuffer, HANDLE* pSharedHandle
)
{
    HRESULT hr = oCreateIndexBuffer(pDevice, Length, Usage, Format, Pool, ppIndexBuffer, pSharedHandle);
    if (SUCCEEDED(hr)) return hr;
    if (Pool == D3DPOOL_MANAGED)
    {
        Pool = D3DPOOL_DEFAULT;
        Usage |= (D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY);
        hr = oCreateIndexBuffer(pDevice, Length, Usage, Format, Pool, ppIndexBuffer, pSharedHandle);
    }
    return hr;
}

// ---------------------------------------------------------------------------------
// D3DX Hooks (d3dx9_43.dll)
// ---------------------------------------------------------------------------------
typedef HRESULT (WINAPI *tD3DXCreateTexture)(
    LPDIRECT3DDEVICE9 pDevice,
    UINT Width,
    UINT Height,
    UINT MipLevels,
    DWORD Usage,
    D3DFORMAT Format,
    D3DPOOL Pool,
    LPDIRECT3DTEXTURE9 *ppTexture
);
static tD3DXCreateTexture oD3DXCreateTexture = nullptr;

static HRESULT WINAPI hkD3DXCreateTexture(
    LPDIRECT3DDEVICE9 pDevice,
    UINT Width,
    UINT Height,
    UINT MipLevels,
    DWORD Usage,
    D3DFORMAT Format,
    D3DPOOL Pool,
    LPDIRECT3DTEXTURE9 *ppTexture
)
{
    HRESULT hr = oD3DXCreateTexture(pDevice, Width, Height, MipLevels, Usage, Format, Pool, ppTexture);
    if (SUCCEEDED(hr))
    {
        return hr;
    }
    if (Pool == D3DPOOL_MANAGED)
    {
        hr = oD3DXCreateTexture(pDevice, Width, Height, MipLevels, Usage | D3DUSAGE_DYNAMIC, Format, D3DPOOL_DEFAULT, ppTexture);
    }
    return hr;
}

typedef HRESULT (WINAPI *tD3DXCreateTextureFromFileInMemoryEx)(
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
static tD3DXCreateTextureFromFileInMemoryEx oD3DXCreateTextureFromFileInMemoryEx = nullptr;

static HRESULT WINAPI hkD3DXCreateTextureFromFileInMemoryEx(
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
)
{
    HRESULT hr = oD3DXCreateTextureFromFileInMemoryEx(pDevice, pSrcData, SrcDataSize, Width, Height, MipLevels, Usage, Format, Pool, Filter, MipFilter, ColorKey, pSrcInfo, pPalette, ppTexture);
    if (SUCCEEDED(hr))
    {
        return hr;
    }
    if (Pool == D3DPOOL_MANAGED)
    {
        hr = oD3DXCreateTextureFromFileInMemoryEx(pDevice, pSrcData, SrcDataSize, Width, Height, MipLevels, Usage | D3DUSAGE_DYNAMIC, Format, D3DPOOL_DEFAULT, Filter, MipFilter, ColorKey, pSrcInfo, pPalette, ppTexture);
    }
    return hr;
}

typedef HRESULT (WINAPI *tD3DXCreateVolumeTextureFromFileInMemoryEx)(
    LPDIRECT3DDEVICE9 pDevice,
    LPCVOID pSrcData,
    UINT SrcDataSize,
    UINT Width,
    UINT Height,
    UINT Depth,
    UINT MipLevels,
    DWORD Usage,
    D3DFORMAT Format,
    D3DPOOL Pool,
    DWORD Filter,
    DWORD MipFilter,
    D3DCOLOR ColorKey,
    void *pSrcInfo,
    PALETTEENTRY *pPalette,
    LPDIRECT3DVOLUMETEXTURE9 *ppVolumeTexture
);
static tD3DXCreateVolumeTextureFromFileInMemoryEx oD3DXCreateVolumeTextureFromFileInMemoryEx = nullptr;

static HRESULT WINAPI hkD3DXCreateVolumeTextureFromFileInMemoryEx(
    LPDIRECT3DDEVICE9 pDevice,
    LPCVOID pSrcData,
    UINT SrcDataSize,
    UINT Width,
    UINT Height,
    UINT Depth,
    UINT MipLevels,
    DWORD Usage,
    D3DFORMAT Format,
    D3DPOOL Pool,
    DWORD Filter,
    DWORD MipFilter,
    D3DCOLOR ColorKey,
    void *pSrcInfo,
    PALETTEENTRY *pPalette,
    LPDIRECT3DVOLUMETEXTURE9 *ppVolumeTexture
)
{
    HRESULT hr = oD3DXCreateVolumeTextureFromFileInMemoryEx(pDevice, pSrcData, SrcDataSize, Width, Height, Depth, MipLevels, Usage, Format, Pool, Filter, MipFilter, ColorKey, pSrcInfo, pPalette, ppVolumeTexture);
    if (SUCCEEDED(hr))
    {
        return hr;
    }
    if (Pool == D3DPOOL_MANAGED)
    {
        hr = oD3DXCreateVolumeTextureFromFileInMemoryEx(pDevice, pSrcData, SrcDataSize, Width, Height, Depth, MipLevels, Usage | D3DUSAGE_DYNAMIC, Format, D3DPOOL_DEFAULT, Filter, MipFilter, ColorKey, pSrcInfo, pPalette, ppVolumeTexture);
    }
    return hr;
}

typedef HRESULT (WINAPI *tD3DXCreateCubeTextureFromFileInMemoryEx)(
    LPDIRECT3DDEVICE9 pDevice,
    LPCVOID pSrcData,
    UINT SrcDataSize,
    UINT Size,
    UINT MipLevels,
    DWORD Usage,
    D3DFORMAT Format,
    D3DPOOL Pool,
    DWORD Filter,
    DWORD MipFilter,
    D3DCOLOR ColorKey,
    void *pSrcInfo,
    PALETTEENTRY *pPalette,
    LPDIRECT3DCUBETEXTURE9 *ppCubeTexture
);
static tD3DXCreateCubeTextureFromFileInMemoryEx oD3DXCreateCubeTextureFromFileInMemoryEx = nullptr;

static HRESULT WINAPI hkD3DXCreateCubeTextureFromFileInMemoryEx(
    LPDIRECT3DDEVICE9 pDevice,
    LPCVOID pSrcData,
    UINT SrcDataSize,
    UINT Size,
    UINT MipLevels,
    DWORD Usage,
    D3DFORMAT Format,
    D3DPOOL Pool,
    DWORD Filter,
    DWORD MipFilter,
    D3DCOLOR ColorKey,
    void *pSrcInfo,
    PALETTEENTRY *pPalette,
    LPDIRECT3DCUBETEXTURE9 *ppCubeTexture
)
{
    HRESULT hr = oD3DXCreateCubeTextureFromFileInMemoryEx(pDevice, pSrcData, SrcDataSize, Size, MipLevels, Usage, Format, Pool, Filter, MipFilter, ColorKey, pSrcInfo, pPalette, ppCubeTexture);
    if (SUCCEEDED(hr))
    {
        return hr;
    }
    if (Pool == D3DPOOL_MANAGED)
    {
        hr = oD3DXCreateCubeTextureFromFileInMemoryEx(pDevice, pSrcData, SrcDataSize, Size, MipLevels, Usage | D3DUSAGE_DYNAMIC, Format, D3DPOOL_DEFAULT, Filter, MipFilter, ColorKey, pSrcInfo, pPalette, ppCubeTexture);
    }
    return hr;
}

// ---------------------------------------------------------------------------------
// Real Device VTable Hooking
// ---------------------------------------------------------------------------------
static bool IsDXVKRunning()
{
    if (g_Config.enableDXVK)
        return true;
    HMODULE hD3D9 = GetModuleHandleA("d3d9.dll");
    if (hD3D9)
    {
        char szModPath[MAX_PATH] = { 0 };
        GetModuleFileNameA(hD3D9, szModPath, MAX_PATH);
        if (strstr(szModPath, "System32") == nullptr && strstr(szModPath, "SysWOW64") == nullptr)
        {
            return true;
        }
    }
    return false;
}

static void HookDeviceVTable(IDirect3DDevice9* pDevice)
{
    if (s_D3DDeviceHooked || !pDevice) return;

    void** pVTable = *(void***)pDevice;

    if (MH_CreateHook(pVTable[42], (LPVOID)&hkEndScene, (LPVOID*)&oEndScene) == MH_OK)
    {
        MH_EnableHook(pVTable[42]);
    }
    if (MH_CreateHook(pVTable[16], (LPVOID)&hkReset, (LPVOID*)&oReset) == MH_OK)
    {
        MH_EnableHook(pVTable[16]);
    }

    // Hook SetViewport (index 47) for Always Look Destination 70% scaling
    if (MH_CreateHook(pVTable[47], (LPVOID)&hkSetViewport, (LPVOID*)&oSetViewport) == MH_OK)
    {
        MH_EnableHook(pVTable[47]);
    }

    // Under DXVK, Vulkan natively manages all texture pools, vertex buffers, and index buffers.
    // D3DPOOL_MANAGED is fully supported in DXVK without workarounds.
    // Bypassing resource hooks prevents COM vtable conflicts and crashes with DXVK.
    if (!IsDXVKRunning())
    {
        // Hook SetTexture (index 65) to map managed textures to default pool copies
        if (MH_CreateHook(pVTable[65], (LPVOID)&hkSetTexture, (LPVOID*)&oSetTexture) == MH_OK)
        {
            MH_EnableHook(pVTable[65]);
        }

        // Hook resource allocation to fix D3DPOOL_MANAGED on modern Windows 10/11 & Intel Iris Xe
        if (MH_CreateHook(pVTable[23], (LPVOID)&hkCreateTexture, (LPVOID*)&oCreateTexture) == MH_OK)
        {
            MH_EnableHook(pVTable[23]);
        }
        if (MH_CreateHook(pVTable[24], (LPVOID)&hkCreateVolumeTexture, (LPVOID*)&oCreateVolumeTexture) == MH_OK)
        {
            MH_EnableHook(pVTable[24]);
        }
        if (MH_CreateHook(pVTable[25], (LPVOID)&hkCreateCubeTexture, (LPVOID*)&oCreateCubeTexture) == MH_OK)
        {
            MH_EnableHook(pVTable[25]);
        }
        if (MH_CreateHook(pVTable[26], (LPVOID)&hkCreateVertexBuffer, (LPVOID*)&oCreateVertexBuffer) == MH_OK)
        {
            MH_EnableHook(pVTable[26]);
        }
        if (MH_CreateHook(pVTable[27], (LPVOID)&hkCreateIndexBuffer, (LPVOID*)&oCreateIndexBuffer) == MH_OK)
        {
            MH_EnableHook(pVTable[27]);
        }
    }

    s_D3DDeviceHooked = true;
}

static HRESULT APIENTRY hkCreateDevice(
    IDirect3D9* pD3D,
    UINT Adapter,
    D3DDEVTYPE DeviceType,
    HWND hFocusWindow,
    DWORD BehaviorFlags,
    D3DPRESENT_PARAMETERS* pPresentationParameters,
    IDirect3DDevice9** ppReturnedDeviceInterface
)
{
    std::string logPath = GetModDirectory() + "debug_hook_log.txt";
    FILE* fp = fopen(logPath.c_str(), "a");
    if (fp)
    {
        fprintf(fp, "hkCreateDevice ENTER: Adapter=%u, DevType=%d, hFocus=%p, Flags=0x%08X\n",
            Adapter, DeviceType, hFocusWindow, BehaviorFlags);
        if (pPresentationParameters)
        {
            fprintf(fp, "  PP: %ux%u fmt=%d cnt=%u msType=%d msQ=%u swp=%d hDevWnd=%p wnd=%d depth=%d zfmt=%d flags=0x%08X hz=%u interval=0x%08X\n",
                pPresentationParameters->BackBufferWidth,
                pPresentationParameters->BackBufferHeight,
                pPresentationParameters->BackBufferFormat,
                pPresentationParameters->BackBufferCount,
                pPresentationParameters->MultiSampleType,
                pPresentationParameters->MultiSampleQuality,
                pPresentationParameters->SwapEffect,
                pPresentationParameters->hDeviceWindow,
                pPresentationParameters->Windowed,
                pPresentationParameters->EnableAutoDepthStencil,
                pPresentationParameters->AutoDepthStencilFormat,
                pPresentationParameters->Flags,
                pPresentationParameters->FullScreen_RefreshRateInHz,
                pPresentationParameters->PresentationInterval);
        }
        fclose(fp);
    }

    HRESULT hr = oCreateDevice(pD3D, Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPresentationParameters, ppReturnedDeviceInterface);

    std::string logPath2 = GetModDirectory() + "debug_hook_log.txt";
    fp = fopen(logPath2.c_str(), "a");
    if (fp)
    {
        fprintf(fp, "hkCreateDevice EXIT: hr=0x%08X, ppDev=%p, *ppDev=%p\n",
            hr, ppReturnedDeviceInterface, (ppReturnedDeviceInterface ? *ppReturnedDeviceInterface : nullptr));
        fclose(fp);
    }

    if (SUCCEEDED(hr) && ppReturnedDeviceInterface && *ppReturnedDeviceInterface)
    {
        SyncSegaDevicePointers(*ppReturnedDeviceInterface);
        HookDeviceVTable(*ppReturnedDeviceInterface);
    }

    return hr;
}

IDirect3D9* WINAPI hkDirect3DCreate9(UINT SDKVersion)
{
    IDirect3D9* pResult = nullptr;

    // Under DXVK, use standard Direct3DCreate9 directly to initialize Vulkan cleanly
    if (IsDXVKRunning() && oDirect3DCreate9)
    {
        pResult = oDirect3DCreate9(SDKVersion);
    }

    // Prefer Direct3DCreate9Ex for native Windows 10/11 & Intel Iris Xe WDDM compatibility
    if (!pResult)
    {
        HMODULE hD3D9 = GetModuleHandleA("d3d9.dll");
        if (!hD3D9) hD3D9 = LoadLibraryA("d3d9.dll");
        if (hD3D9)
        {
            typedef HRESULT (WINAPI *tDirect3DCreate9Ex)(UINT, IDirect3D9Ex**);
            tDirect3DCreate9Ex pDirect3DCreate9Ex = (tDirect3DCreate9Ex)GetProcAddress(hD3D9, "Direct3DCreate9Ex");
            if (pDirect3DCreate9Ex)
            {
                IDirect3D9Ex* pD3DEx = nullptr;
                if (SUCCEEDED(pDirect3DCreate9Ex(SDKVersion, &pD3DEx)) && pD3DEx)
                {
                    pResult = (IDirect3D9*)pD3DEx;
                }
            }
        }
    }

    // Fallback to standard Direct3DCreate9 if Ex not available
    if (!pResult && oDirect3DCreate9)
    {
        pResult = oDirect3DCreate9(SDKVersion);
    }

    if (pResult)
    {
        void** pVTable = *(void***)pResult;
        if (!oCreateDevice)
        {
            if (MH_CreateHook(pVTable[16], (LPVOID)&hkCreateDevice, (LPVOID*)&oCreateDevice) == MH_OK)
            {
                MH_EnableHook(pVTable[16]);
            }
        }
    }

    return pResult;
}

// ---------------------------------------------------------------------------------
// Setup Render Hook (Direct3DCreate9 + D3DX + IAT Hooking)
// ---------------------------------------------------------------------------------
bool InitRenderHook()
{
    if (!s_TexCSInitialized)
    {
        InitializeCriticalSection(&s_TexCS);
        s_TexCSInitialized = true;
    }

    HMODULE hD3D9 = GetModuleHandleA("d3d9.dll");
    if (!hD3D9) hD3D9 = LoadLibraryA("d3d9.dll");

    if (hD3D9)
    {
        void* pTarget = (void*)GetProcAddress(hD3D9, "Direct3DCreate9");
        if (pTarget)
        {
            MH_CreateHook(pTarget, (LPVOID)&hkDirect3DCreate9, (LPVOID*)&oDirect3DCreate9);
            MH_EnableHook(pTarget);
        }
    }

    // Patch Crazy Taxi IAT entry for Direct3DCreate9 at 0x0053428c
    DWORD oldProtect;
    if (VirtualProtect((LPVOID)0x0053428c, 4, PAGE_EXECUTE_READWRITE, &oldProtect))
    {
        *(uintptr_t*)0x0053428c = (uintptr_t)&hkDirect3DCreate9;
        VirtualProtect((LPVOID)0x0053428c, 4, oldProtect, &oldProtect);
    }

    // Hook D3DX functions in d3dx9_43.dll
    HMODULE hD3DX = GetModuleHandleA("d3dx9_43.dll");
    if (!hD3DX) hD3DX = LoadLibraryA("d3dx9_43.dll");

    if (hD3DX)
    {
        void* pD3DXCreateTex = (void*)GetProcAddress(hD3DX, "D3DXCreateTexture");
        if (pD3DXCreateTex)
        {
            MH_CreateHook(pD3DXCreateTex, (LPVOID)&hkD3DXCreateTexture, (LPVOID*)&oD3DXCreateTexture);
            MH_EnableHook(pD3DXCreateTex);
        }

        void* pD3DXCreateFromFile = (void*)GetProcAddress(hD3DX, "D3DXCreateTextureFromFileInMemoryEx");
        if (pD3DXCreateFromFile)
        {
            MH_CreateHook(pD3DXCreateFromFile, (LPVOID)&hkD3DXCreateTextureFromFileInMemoryEx, (LPVOID*)&oD3DXCreateTextureFromFileInMemoryEx);
            MH_EnableHook(pD3DXCreateFromFile);
        }

        void* pD3DXCreateVolumeFromFile = (void*)GetProcAddress(hD3DX, "D3DXCreateVolumeTextureFromFileInMemoryEx");
        if (pD3DXCreateVolumeFromFile)
        {
            MH_CreateHook(pD3DXCreateVolumeFromFile, (LPVOID)&hkD3DXCreateVolumeTextureFromFileInMemoryEx, (LPVOID*)&oD3DXCreateVolumeTextureFromFileInMemoryEx);
            MH_EnableHook(pD3DXCreateVolumeFromFile);
        }

        void* pD3DXCreateCubeFromFile = (void*)GetProcAddress(hD3DX, "D3DXCreateCubeTextureFromFileInMemoryEx");
        if (pD3DXCreateCubeFromFile)
        {
            MH_CreateHook(pD3DXCreateCubeFromFile, (LPVOID)&hkD3DXCreateCubeTextureFromFileInMemoryEx, (LPVOID*)&oD3DXCreateCubeTextureFromFileInMemoryEx);
            MH_EnableHook(pD3DXCreateCubeFromFile);
        }
    }

    // Also patch Crazy Taxi IAT entries directly for D3DX
    // 0x005342B8 = D3DXCreateTexture
    if (VirtualProtect((LPVOID)0x005342B8, 4, PAGE_EXECUTE_READWRITE, &oldProtect))
    {
        *(uintptr_t*)0x005342B8 = (uintptr_t)&hkD3DXCreateTexture;
        VirtualProtect((LPVOID)0x005342B8, 4, oldProtect, &oldProtect);
    }
    // 0x005342B4 = D3DXCreateTextureFromFileInMemoryEx
    if (VirtualProtect((LPVOID)0x005342B4, 4, PAGE_EXECUTE_READWRITE, &oldProtect))
    {
        *(uintptr_t*)0x005342B4 = (uintptr_t)&hkD3DXCreateTextureFromFileInMemoryEx;
        VirtualProtect((LPVOID)0x005342B4, 4, oldProtect, &oldProtect);
    }
    // 0x005342A0 = D3DXCreateVolumeTextureFromFileInMemoryEx
    if (VirtualProtect((LPVOID)0x005342A0, 4, PAGE_EXECUTE_READWRITE, &oldProtect))
    {
        *(uintptr_t*)0x005342A0 = (uintptr_t)&hkD3DXCreateVolumeTextureFromFileInMemoryEx;
        VirtualProtect((LPVOID)0x005342A0, 4, oldProtect, &oldProtect);
    }
    // 0x005342EC = D3DXCreateCubeTextureFromFileInMemoryEx
    if (VirtualProtect((LPVOID)0x005342EC, 4, PAGE_EXECUTE_READWRITE, &oldProtect))
    {
        *(uintptr_t*)0x005342EC = (uintptr_t)&hkD3DXCreateCubeTextureFromFileInMemoryEx;
        VirtualProtect((LPVOID)0x005342EC, 4, oldProtect, &oldProtect);
    }

    return true;
}

void ShutdownRenderHook()
{
    if (s_ImGuiInitialized)
    {
        ImGui_ImplDX9_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        s_ImGuiInitialized = false;
    }

    if (s_TexCSInitialized)
    {
        DeleteCriticalSection(&s_TexCS);
        s_TexCSInitialized = false;
    }

    ShutdownInputHook();
}

extern "C" {
__declspec(dllexport) void* __cdecl GetFrameworkImGuiContext()
{
    return (void*)ImGui::GetCurrentContext();
}

__declspec(dllexport) IDirect3DDevice9* __cdecl GetFrameworkD3D9Device()
{
    return g_pActiveD3DDevice;
}

__declspec(dllexport) void __cdecl SetFrameworkStatusMessage(const char* msg)
{
    if (msg) SetStatusMessage(msg);
}
}

