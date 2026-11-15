#include "TextureEngine.h"
#include "ModelEngine.h"
#include "Logger.h"
#include "ZipWriter.h"
#include <gdiplus.h>
#include <fstream>
#include <algorithm>
#include <cstdio>

using namespace Gdiplus;

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

// Standard CRC32 table
static uint32_t s_CrcTable[256];
static bool s_CrcTableInit = false;

static void InitCrcTable()
{
    if (s_CrcTableInit) return;
    for (uint32_t i = 0; i < 256; i++)
    {
        uint32_t c = i;
        for (int j = 0; j < 8; j++)
        {
            if (c & 1)
                c = 0xEDB88320 ^ (c >> 1);
            else
                c >>= 1;
        }
        s_CrcTable[i] = c;
    }
    s_CrcTableInit = true;
}

uint32_t TextureEngine::CalculateCRC32(const void* data, size_t length)
{
    InitCrcTable();
    const uint8_t* p = (const uint8_t*)data;
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < length; i++)
    {
        crc = s_CrcTable[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFF;
}

// ---------------------------------------------------------------------------------
// Static Sega Container Table Mapping (26 Tables identified in Crazy Taxi.exe)
// ---------------------------------------------------------------------------------
struct SegaTableMapEntry
{
    void* tablePtr;
    uint32_t archiveId;
    const char* name;
};

static const SegaTableMapEntry s_SegaTableMap[] = {
    { (void*)0x0135EFD0, 0x8E8E5F62, "sprDC0" },
    { (void*)0x0135F790, 0xB16EA5E6, "sprCENG" },
    { (void*)0x0135F8D0, 0xBE797BD3, "sprCJAP" },
    { (void*)0x0135FA10, 0xBB5127CE, "sprCFRE" },
    { (void*)0x0135FAF0, 0xFC26A882, "sprGAME" },
    { (void*)0x01360890, 0x3CC34CA8, "sprCGER" },
    { (void*)0x01360910, 0x94E7E0CE, "sprCSPA" },
    { (void*)0x01360960, 0x65DF85B6, "sprBOX" },
    { (void*)0x01360B00, 0xA208A39D, "sprMENU" },
    { (void*)0x01360E70, 0x5EA60CE0, "sprMJAP" },
    { (void*)0x01361480, 0x51B1D2D5, "sprMENG" },
    { (void*)0x01361BA0, 0x5436974C, "texDC0" },
    { (void*)0x01363710, 0x5B8E50FD, "sprMFRE" },
    { (void*)0x01363750, 0xDC1C3B9B, "sprMGER" },
    { (void*)0x01363790, 0x743897FD, "sprMSPA" },
    { (void*)0x013637E0, 0x77019AED, "sprCOMM" },
    { (void*)0x01363CA0, 0xFE240B5B, "sprPAL" },
    { (void*)0x01363F00, 0xF3C25D97, "sprPENG" },
    { (void*)0x01364160, 0xF9FDDFBF, "sprPFRE" },
    { (void*)0x013644A0, 0x7E6FB4D9, "sprPGER" },
    { (void*)0x01364500, 0xD64B18BF, "sprPSPA" },
    { (void*)0x01364560, 0x3E0C32B1, "sprGENG" },
    { (void*)0x013645C0, 0x311BEC84, "sprGJAP" },
    { (void*)0x01364620, 0x2331A7DA, "texDC1" },
    { (void*)0x01365B30, 0xBA38F660, "texDC2" },
    { (void*)0x01366F80, 0xCD3FC6F6, "texDC3" },
};

uint32_t TextureEngine::GetArchiveIdFromTablePtr(void* tablePtr)
{
    if (!tablePtr) return 0;
    for (const auto& entry : s_SegaTableMap)
    {
        if (entry.tablePtr == tablePtr)
            return entry.archiveId;
    }
    return 0;
}

uint32_t TextureEngine::GetDefaultBaseIdxForTable(void* tablePtr)
{
    if (!tablePtr) return 0;

    switch ((uintptr_t)tablePtr)
    {
    case 0x0135EFD0: return 0;      // sprDC0
    case 0x0135F790: return 150;    // sprCENG (0x96)
    case 0x0135F8D0: return 125;    // sprCJAP (0x7D)
    case 0x0135FA10: return 175;    // sprCFRE (0xAF)
    case 0x0135FAF0: return 240;    // sprGAME (0xF0)
    case 0x01360890: return 180;    // sprCGER (0xB4)
    case 0x01360910: return 185;    // sprCSPA (0xB9)
    case 0x01360960: return 190;    // sprBOX
    case 0x01360B00: return 520;    // sprMENU (0x208)
    case 0x01360E70: return 580;    // sprMJAP (0x244)
    case 0x01361480: return 680;    // sprMENG (0x2A8)
    case 0x01361BA0: return 1500;   // texDC0 (0x5DC) - Cars, Pedestrians, Chassis
    case 0x01363710: return 800;    // sprMFRE (0x320)
    case 0x01363750: return 850;    // sprMGER (0x352)
    case 0x01363790: return 900;    // sprMSPA (0x384)
    case 0x013637E0: return 950;    // sprCOMM
    case 0x01363CA0: return 1050;   // sprPAL
    case 0x01363F00: return 1100;   // sprPENG
    case 0x01364160: return 1150;   // sprPFRE
    case 0x013644A0: return 1200;   // sprPGER
    case 0x01364500: return 1250;   // sprPSPA
    case 0x01364560: return 1300;   // sprGENG
    case 0x013645C0: return 1350;   // sprGJAP
    case 0x01364620: return 2000;   // texDC1 (0x7D0) - Arcade Stage & Cable Car
    case 0x01365B30: return 2000;   // texDC2 (0x7D0) - Original Stage
    case 0x01366F80: return 2000;   // texDC3 (0x7D0) - Extra Stage
    default:
        break;
    }

    uint32_t activeStageTable = 0;
    __try {
        activeStageTable = *(uint32_t*)0x01B7A838;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (activeStageTable != 0 && (uintptr_t)tablePtr == activeStageTable)
    {
        return 2000;
    }

    return 0;
}

// True for the 26 EXE Sega container tables (identified by archive CRC, or by the
// manifest filename trigger measured to separate the populations exactly: all 2196
// Sega entries contain "texDC"/"spr", 0/379 JPK/NewTexture entries do).
bool TextureEngine::IsSegaContainerRecord(const RuntimeTextureInfo& info)
{
    for (const auto& entry : s_SegaTableMap)
    {
        if (entry.archiveId == info.record.archiveId)
            return true;
    }
    if (info.filename.find("texDC") != std::string::npos || info.filename.find("spr") != std::string::npos)
        return true;
    return false;
}

bool TextureEngine::IsDdsUiRecord(const RuntimeTextureInfo& info)
{
    // Format 1 is DDS (DXT1/3/5/A8R8G8B8) from JPK packages or standalone NewTexture DDS
    if (info.record.origFormat == 1)
        return true;
    if (info.filename.find("FMT_DDS") != std::string::npos)
        return true;
    return false;
}

// ---------------------------------------------------------------------------------
// Sleek Modern Loading / Progress Screen (GDI Double-Buffered)
// ---------------------------------------------------------------------------------
class ProgressScreen
{
public:
    static void Show(HWND hParent, int totalCount, const char* title = nullptr, const char* subtitle = nullptr)
    {
        if (s_hWnd) return;
        s_Total = totalCount;
        s_Current = 0;
        s_CurrentFile = "Initializing texture cache compiler...";
        s_TitleText = (title && title[0]) ? title : "LYRA'S TEXTURE MOD";
        s_SubtitleText = (subtitle && subtitle[0]) ? subtitle : "Compiling modified custom textures into high-speed binary cache (.cache)...";

        HINSTANCE hInst = GetModuleHandleA(NULL);
        if (!s_Registered)
        {
            WNDCLASSEXA wc = { sizeof(WNDCLASSEXA) };
            wc.style = CS_HREDRAW | CS_VREDRAW;
            wc.lpfnWndProc = WndProc;
            wc.hInstance = hInst;
            wc.hCursor = LoadCursor(NULL, IDC_ARROW);
            wc.hbrBackground = NULL;
            wc.lpszClassName = "LyrasTextureMod_ProgressClass";
            RegisterClassExA(&wc);
            s_Registered = true;
        }

        s_hTitleFont = CreateFontA(20, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
        s_hTextFont = CreateFontA(15, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
        s_hSmallFont = CreateFontA(12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");

        int w = 560;
        int h = 180;
        int screenW = GetSystemMetrics(SM_CXSCREEN);
        int screenH = GetSystemMetrics(SM_CYSCREEN);
        int posX = (screenW - w) / 2;
        int posY = (screenH - h) / 2;

        s_hWnd = CreateWindowExA(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
            "LyrasTextureMod_ProgressClass",
            s_TitleText.c_str(),
            WS_POPUP | WS_VISIBLE,
            posX, posY, w, h,
            NULL, NULL, hInst, NULL
        );

        if (s_hWnd)
        {
            SetWindowPos(s_hWnd, HWND_TOPMOST, posX, posY, w, h, SWP_SHOWWINDOW);
            ShowWindow(s_hWnd, SW_SHOW);
            UpdateWindow(s_hWnd);
            PumpMessages();
        }
    }

    static void Update(int current, int total, const std::string& currentFile, const char* customStatus = nullptr)
    {
        s_Current = current;
        s_Total = total;
        s_CurrentFile = currentFile;
        s_CustomStatusText = customStatus ? customStatus : "";

        if (s_hWnd && IsWindow(s_hWnd))
        {
            InvalidateRect(s_hWnd, NULL, FALSE);
            UpdateWindow(s_hWnd);
            PumpMessages();
        }
    }

    static void Hide()
    {
        if (s_hWnd && IsWindow(s_hWnd))
        {
            HWND hWndToHide = s_hWnd;
            s_hWnd = NULL;

            // 1. Instantly hide window from display and unparent
            ShowWindow(hWndToHide, SW_HIDE);
            SetWindowPos(hWndToHide, NULL, 0, 0, 0, 0,
                SWP_HIDEWINDOW | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);

            // 2. Destroy window (or post WM_CLOSE if created on a different thread)
            if (!DestroyWindow(hWndToHide))
            {
                PostMessageA(hWndToHide, WM_CLOSE, 0, 0);
            }

            // 3. Pump all pending messages so destruction completes immediately
            MSG msg;
            for (int i = 0; i < 30 && PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE); ++i)
            {
                TranslateMessage(&msg);
                DispatchMessageA(&msg);
            }

            // 4. Force repaint of the game window to immediately clear any residual dirty rectangle
            // Note: Direct3D games continuously redraw every frame; do NOT send synchronous UpdateWindow(hGame)
            // as that triggers WM_PAINT during rendering frames!
            HWND hGame = FindWindowA("Crazy Taxi", "Crazy Taxi");
            if (!hGame) hGame = GetActiveWindow();
            if (hGame && IsWindow(hGame))
            {
                InvalidateRect(hGame, NULL, FALSE);
            }
        }
        if (s_hTitleFont) { DeleteObject(s_hTitleFont); s_hTitleFont = NULL; }
        if (s_hTextFont)  { DeleteObject(s_hTextFont);  s_hTextFont = NULL; }
        if (s_hSmallFont) { DeleteObject(s_hSmallFont); s_hSmallFont = NULL; }
    }

    static bool IsShowing() { return s_hWnd != NULL; }

private:
    static void PumpMessages()
    {
        if (!s_hWnd || !IsWindow(s_hWnd)) return;
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    }

    static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        switch (msg)
        {
        case WM_ERASEBKGND:
            return 1; // Handled in double buffer

        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);

            RECT clientRect;
            GetClientRect(hWnd, &clientRect);
            int width = clientRect.right - clientRect.left;
            int height = clientRect.bottom - clientRect.top;

            HDC memDC = CreateCompatibleDC(hdc);
            HBITMAP memBmp = CreateCompatibleBitmap(hdc, width, height);
            HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, memBmp);

            // 1. Dark background
            HBRUSH bgBrush = CreateSolidBrush(RGB(20, 22, 28));
            FillRect(memDC, &clientRect, bgBrush);
            DeleteObject(bgBrush);

            // 2. Yellow accent border
            HPEN borderPen = CreatePen(PS_SOLID, 2, RGB(255, 204, 0));
            HPEN oldPen = (HPEN)SelectObject(memDC, borderPen);
            HBRUSH hollowBrush = (HBRUSH)GetStockObject(HOLLOW_BRUSH);
            HBRUSH oldBrush = (HBRUSH)SelectObject(memDC, hollowBrush);
            Rectangle(memDC, 1, 1, width - 1, height - 1);
            SelectObject(memDC, oldPen);
            SelectObject(memDC, oldBrush);
            DeleteObject(borderPen);

            SetBkMode(memDC, TRANSPARENT);

            // 3. Title: s_TitleText
            SelectObject(memDC, s_hTitleFont);
            SetTextColor(memDC, RGB(255, 204, 0));
            RECT titleRect = { 25, 18, width - 25, 45 };
            DrawTextA(memDC, s_TitleText.c_str(), -1, &titleRect, DT_LEFT | DT_SINGLELINE);

            // Subtitle: s_SubtitleText
            SelectObject(memDC, s_hSmallFont);
            SetTextColor(memDC, RGB(160, 165, 175));
            RECT subRect = { 25, 44, width - 25, 62 };
            DrawTextA(memDC, s_SubtitleText.c_str(), -1, &subRect, DT_LEFT | DT_SINGLELINE);

            // 4. Status Text
            SelectObject(memDC, s_hTextFont);
            SetTextColor(memDC, RGB(245, 245, 245));
            char statusBuf[256];
            int pct = s_Total > 0 ? (s_Current * 100 / s_Total) : 0;
            if (!s_CustomStatusText.empty())
            {
                strcpy_s(statusBuf, sizeof(statusBuf), s_CustomStatusText.c_str());
            }
            else
            {
                sprintf_s(statusBuf, sizeof(statusBuf), "Processing: [ %d / %d ]   (%d%%)", s_Current, s_Total, pct);
            }
            RECT statusRect = { 25, 72, width - 25, 95 };
            DrawTextA(memDC, statusBuf, -1, &statusRect, DT_LEFT | DT_SINGLELINE);

            // 5. Current File
            SelectObject(memDC, s_hSmallFont);
            SetTextColor(memDC, RGB(255, 225, 110));
            RECT fileRect = { 25, 98, width - 25, 118 };
            DrawTextA(memDC, s_CurrentFile.c_str(), -1, &fileRect, DT_LEFT | DT_SINGLELINE | DT_PATH_ELLIPSIS);

            // 6. Progress Bar Track
            RECT trackRect = { 25, 126, width - 25, 148 };
            HBRUSH trackBrush = CreateSolidBrush(RGB(36, 40, 50));
            FillRect(memDC, &trackRect, trackBrush);
            DeleteObject(trackBrush);

            HPEN trackBorderPen = CreatePen(PS_SOLID, 1, RGB(60, 66, 82));
            oldPen = (HPEN)SelectObject(memDC, trackBorderPen);
            SelectObject(memDC, hollowBrush);
            Rectangle(memDC, trackRect.left, trackRect.top, trackRect.right, trackRect.bottom);
            SelectObject(memDC, oldPen);
            DeleteObject(trackBorderPen);

            // Progress Bar Fill
            int trackWidth = (trackRect.right - trackRect.left) - 4;
            int fillWidth = s_Total > 0 ? (trackWidth * s_Current / s_Total) : 0;
            if (fillWidth > trackWidth) fillWidth = trackWidth;
            if (fillWidth > 0)
            {
                RECT fillRect = { trackRect.left + 2, trackRect.top + 2, trackRect.left + 2 + fillWidth, trackRect.bottom - 2 };
                HBRUSH fillBrush = CreateSolidBrush(RGB(255, 185, 0)); // Taxi Gold
                FillRect(memDC, &fillRect, fillBrush);
                DeleteObject(fillBrush);
            }

            BitBlt(hdc, 0, 0, width, height, memDC, 0, 0, SRCCOPY);

            SelectObject(memDC, oldBmp);
            DeleteObject(memBmp);
            DeleteDC(memDC);

            EndPaint(hWnd, &ps);
            return 0;
        }

        case WM_CLOSE:
            DestroyWindow(hWnd);
            return 0;

        default:
            return DefWindowProcA(hWnd, msg, wParam, lParam);
        }
    }

    static HWND s_hWnd;
    static int s_Current;
    static int s_Total;
    static std::string s_CurrentFile;
    static std::string s_CustomStatusText;
    static std::string s_TitleText;
    static std::string s_SubtitleText;
    static HFONT s_hTitleFont;
    static HFONT s_hTextFont;
    static HFONT s_hSmallFont;
    static bool s_Registered;
};

HWND ProgressScreen::s_hWnd = NULL;
int ProgressScreen::s_Current = 0;
int ProgressScreen::s_Total = 0;
std::string ProgressScreen::s_CurrentFile = "";
std::string ProgressScreen::s_CustomStatusText = "";
std::string ProgressScreen::s_TitleText = "LYRA'S TEXTURE MOD";
std::string ProgressScreen::s_SubtitleText = "Compiling modified custom textures into high-speed binary cache (.cache)...";
HFONT ProgressScreen::s_hTitleFont = NULL;
HFONT ProgressScreen::s_hTextFont = NULL;
HFONT ProgressScreen::s_hSmallFont = NULL;
bool ProgressScreen::s_Registered = false;

// ---------------------------------------------------------------------------------
// TextureEngine Core Implementation
// ---------------------------------------------------------------------------------
TextureEngine::TextureEngine()
{
    InitializeCriticalSection(&m_CS);
}

TextureEngine::~TextureEngine()
{
    DeleteCriticalSection(&m_CS);
}

TextureEngine& TextureEngine::Instance()
{
    static TextureEngine s_Instance;
    return s_Instance;
}

UINT TextureEngine::NextPowerOfTwo(UINT n, UINT minValue)
{
    if (n <= minValue) return minValue;
    if (n > 2048) return 2048;
    UINT p = 1;
    while (p < n) p <<= 1;
    UINT lower = p >> 1;
    if ((n - lower) < (p - n) && lower >= minValue) return lower;
    return p;
}

static void RecursiveScanDirectory(const std::string& dir, std::unordered_map<std::string, std::string>& outCache)
{
    std::string searchPattern = dir + "\\*";
    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(searchPattern.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;

    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0)
            continue;

        std::string fullPath = dir + "\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            RecursiveScanDirectory(fullPath, outCache);
        } else {
            const char* ext = strrchr(fd.cFileName, '.');
            if (ext && (_stricmp(ext, ".png") == 0 || _stricmp(ext, ".jpg") == 0 || _stricmp(ext, ".jpeg") == 0)) {
                std::string fnLower = fd.cFileName;
                std::transform(fnLower.begin(), fnLower.end(), fnLower.begin(), ::tolower);

                auto it = outCache.find(fnLower);
                if (it == outCache.end()) {
                    outCache[fnLower] = fullPath;
                } else {
                    WIN32_FILE_ATTRIBUTE_DATA fadNew, fadOld;
                    GetFileAttributesExA(fullPath.c_str(), GetFileExInfoStandard, &fadNew);
                    GetFileAttributesExA(it->second.c_str(), GetFileExInfoStandard, &fadOld);
                    uint64_t tNew = ((uint64_t)fadNew.ftLastWriteTime.dwHighDateTime << 32) | fadNew.ftLastWriteTime.dwLowDateTime;
                    uint64_t tOld = ((uint64_t)fadOld.ftLastWriteTime.dwHighDateTime << 32) | fadOld.ftLastWriteTime.dwLowDateTime;
                    bool isJpgNew = (_stricmp(ext, ".jpg") == 0 || _stricmp(ext, ".jpeg") == 0);
                    const char* oldExt = strrchr(it->second.c_str(), '.');
                    bool isJpgOld = oldExt && (_stricmp(oldExt, ".jpg") == 0 || _stricmp(oldExt, ".jpeg") == 0);
                    if ((isJpgNew && !isJpgOld && tNew >= tOld) || tNew > tOld) {
                        outCache[fnLower] = fullPath;
                    }
                }
            }
        }
    } while (FindNextFileA(hFind, &fd));
    FindClose(hFind);
}

void TextureEngine::BuildPathCache()
{
    m_PathCache.clear();
    RecursiveScanDirectory(m_TexturesDir, m_PathCache);
    m_OriginalsPathCache.clear();
    RecursiveScanDirectory(m_OriginalsDir, m_OriginalsPathCache);
    Logger::LogInfo("[INIT] Indexed %u texture files and %u original files across subdirectories.",
        (unsigned int)m_PathCache.size(), (unsigned int)m_OriginalsPathCache.size());
}

// Returns the newest-existing candidate path, or "" when none exists. Used to resolve
// originals/ duplicates: the extractor does not clean the originals/ root, so a legacy
// root copy from an old extraction generation must not shadow the fresh subfolder copy
// (K5-analogon; generations differ by mtime, the name is identical).
static std::string NewestExistingFile(const std::vector<std::string>& candidates)
{
    std::string best;
    uint64_t bestTime = 0;
    for (const auto& p : candidates)
    {
        WIN32_FILE_ATTRIBUTE_DATA fad;
        if (!GetFileAttributesExA(p.c_str(), GetFileExInfoStandard, &fad))
            continue;
        uint64_t t = ((uint64_t)fad.ftLastWriteTime.dwHighDateTime << 32) | fad.ftLastWriteTime.dwLowDateTime;
        if (best.empty() || t > bestTime)
        {
            best = p;
            bestTime = t;
        }
    }
    return best;
}

std::string TextureEngine::ResolveOriginalPath(const std::string& filename)
{
    std::string baseName = filename;
    size_t lastSlash = baseName.find_last_of("\\/");
    if (lastSlash != std::string::npos) baseName = baseName.substr(lastSlash + 1);

    std::string baseLower = baseName;
    std::transform(baseLower.begin(), baseLower.end(), baseLower.begin(), ::tolower);

    // 1. Fast indexed lookup from m_OriginalsPathCache (finds all nested subdirectories!)
    auto itCached = m_OriginalsPathCache.find(baseLower);
    if (itCached != m_OriginalsPathCache.end())
    {
        return itCached->second;
    }

    static const char* subfolders[] = {
        "3D_Objects", "3D_Stages", "2D_Sprites", "2D_UI", "Fonts",
        "2D_UI\\Buttons", "2D_UI\\Loading_Screens", "2D_UI\\Manuals_and_Controls",
        "2D_UI\\Menus_and_Banners", "2D_UI\\HUD_and_Game", "2D_UI\\Logos", "2D_UI\\Fonts"
    };
    auto candidatesFor = [&](const std::string& name) {
        std::vector<std::string> c;
        c.push_back(m_OriginalsDir + "\\" + name);
        for (const char* sub : subfolders)
            c.push_back(m_OriginalsDir + "\\" + sub + "\\" + name);
        return c;
    };

    // 1. Exact name (root + subfolders): newest generation wins, no root priority.
    std::string hit = NewestExistingFile(candidatesFor(baseName));
    if (!hit.empty())
        return hit;

    // 2. Without category prefix
    std::string bare = baseName;
    size_t dblUnderscore = baseName.find("__");
    if (dblUnderscore != std::string::npos && dblUnderscore <= 14) {
        bare = baseName.substr(dblUnderscore + 2);
        std::string bareHit = NewestExistingFile(candidatesFor(bare));
        if (!bareHit.empty())
            return bareHit;
    }

    // 3. Try candidate prefixes (prefix order preserved, newest generation within each)
    const char* prefixes[] = { "3D_OBJECT__", "3D_STAGE__", "2D_SPRITE__", "2D_UI__", "2D_FONT__" };
    for (const char* pfx : prefixes) {
        std::string pfxHit = NewestExistingFile(candidatesFor(std::string(pfx) + bare));
        if (!pfxHit.empty())
            return pfxHit;
    }

    return m_OriginalsDir + "\\" + baseName;
}

std::string TextureEngine::ResolveImagePath(const std::string& manifestFilename)
{
    std::string fname = manifestFilename;
    size_t lastSlash = fname.find_last_of("\\/");
    if (lastSlash != std::string::npos) fname = fname.substr(lastSlash + 1);

    std::string stem = fname;
    size_t lastDot = stem.find_last_of('.');
    if (lastDot != std::string::npos) stem = stem.substr(0, lastDot);

    std::string bareStem = stem;
    std::string ownPrefix;   // category prefix contained in the manifest stem itself ("" if none)
    size_t dblUnderscore = stem.find("__");
    if (dblUnderscore != std::string::npos && dblUnderscore <= 14) {
        std::string prefix = stem.substr(0, dblUnderscore);
        if (prefix == "3D_OBJECT" || prefix == "3D_STAGE" || prefix == "2D_SPRITE" || prefix == "2D_UI" || prefix == "2D_FONT") {
            bareStem = stem.substr(dblUnderscore + 2);
            ownPrefix = prefix + "__";
        }
    }

    std::vector<std::string> prefixes = { "", "3D_OBJECT__", "3D_STAGE__", "2D_SPRITE__", "2D_UI__", "2D_FONT__" };
    std::vector<std::string> extensions = { ".jpg", ".jpeg", ".JPG", ".JPEG", ".png", ".PNG" };

    struct CandidateFile {
        std::string path;
        uint64_t lastWriteTime;
        bool isJpg;
        DWORD fileSize;
        bool isExactStem;   // true = candidate key is exactly the manifest stem (no category-prefix guessing)
    };
    std::vector<CandidateFile> found;

    auto checkInCache = [&](const std::string& key, bool exactStem) {
        std::string lowerKey = key;
        std::transform(lowerKey.begin(), lowerKey.end(), lowerKey.begin(), ::tolower);
        auto it = m_PathCache.find(lowerKey);
        if (it != m_PathCache.end()) {
            WIN32_FILE_ATTRIBUTE_DATA fad;
            if (GetFileAttributesExA(it->second.c_str(), GetFileExInfoStandard, &fad)) {
                for (const auto& f : found) {
                    if (_stricmp(f.path.c_str(), it->second.c_str()) == 0) return;
                }
                uint64_t t = ((uint64_t)fad.ftLastWriteTime.dwHighDateTime << 32) | fad.ftLastWriteTime.dwLowDateTime;
                const char* ext = strrchr(it->second.c_str(), '.');
                bool isJpg = ext && (_stricmp(ext, ".jpg") == 0 || _stricmp(ext, ".jpeg") == 0);
                found.push_back({ it->second, t, isJpg, fad.nFileSizeLow, exactStem });
            }
        }
    };

    // 1. Fast cache lookup
    for (const auto& ext : extensions) {
        checkInCache(stem + ext, true);
        for (const auto& pfx : prefixes) {
            checkInCache(pfx + bareStem + ext, false);
        }
    }

    // 2. Direct disk fallback if not cached yet
    if (found.empty()) {
        const std::vector<std::string> subfolders = { "", "3D_Objects\\", "3D_Stages\\", "2D_Sprites\\", "2D_UI\\", "Fonts\\" };
        for (const auto& sub : subfolders) {
            for (const auto& pfx : prefixes) {
                for (const auto& ext : extensions) {
                    std::string p = m_TexturesDir + "\\" + sub + pfx + bareStem + ext;
                    bool already = false;
                    for (const auto& f : found) {
                        if (_stricmp(f.path.c_str(), p.c_str()) == 0) { already = true; break; }
                    }
                    if (already) continue;

                    WIN32_FILE_ATTRIBUTE_DATA fad;
                    if (GetFileAttributesExA(p.c_str(), GetFileExInfoStandard, &fad)) {
                        uint64_t t = ((uint64_t)fad.ftLastWriteTime.dwHighDateTime << 32) | fad.ftLastWriteTime.dwLowDateTime;
                        bool isJpg = (_stricmp(ext.c_str(), ".jpg") == 0 || _stricmp(ext.c_str(), ".jpeg") == 0);
                        found.push_back({ p, t, isJpg, fad.nFileSizeLow, pfx == ownPrefix });
                    }
                }
            }
        }
    }

    if (found.empty()) {
        return m_TexturesDir + "\\" + manifestFilename;
    }

    // Winner selection (contract K5): the manifest-named file always wins over
    // prefix-variant candidates. Legacy root PNGs (e.g. "2D_SPRITE__sprCENG__...")
    // must never shadow the entry's own file (e.g. "Fonts/2D_FONT__sprCENG__...")
    // just because they carry a newer timestamp. Among candidates of the winning
    // group the historical rule stays intact: newest edit wins, JPG beats PNG on ties.
    auto betterThan = [](const CandidateFile& cand, const CandidateFile& best) {
        if (cand.isJpg && !best.isJpg && cand.lastWriteTime >= best.lastWriteTime) return true;
        return cand.lastWriteTime > best.lastWriteTime;
    };

    for (int pass = 1; pass >= 0; pass--)   // pass 1: exact manifest stem, pass 0: prefix variants
    {
        const CandidateFile* best = nullptr;
        for (const auto& f : found) {
            if (f.isExactStem != (pass == 1)) continue;
            if (!best || betterThan(f, *best)) best = &f;
        }
        if (best) return best->path;
    }
    return found[0].path;
}

// ---------------------------------------------------------------------------------
// Fix support structures (H1/H2/H5) — kept file-local on purpose: the shared types
// (CacheHeader, ManifestEntryRecord, RuntimeTextureInfo) and LTM_CACHE_VERSION live
// in TextureEngine.h, which is outside this fix's file ownership.
// ---------------------------------------------------------------------------------

// Local CRC32 (same polynomial as TextureEngine::CalculateCRC32) so the helpers below
// stay free functions without depending on the member's visibility.
// THREADING: the table init is deliberately unsynchronized — the first call happens
// on the startup thread (ScanModifiedTextures/PrecompileModifiedTextures inside
// TextureEngine::Initialize, before any hook/render thread exists) and every later
// call (hook paths, F9 reload, trailer snapshots) only READS the finished table.
static uint32_t s_LocalCrcTable[256];
static bool s_LocalCrcTableInit = false;

static uint32_t Crc32OfBytes(const void* data, size_t length)
{
    if (!s_LocalCrcTableInit)
    {
        for (uint32_t i = 0; i < 256; i++)
        {
            uint32_t c = i;
            for (int j = 0; j < 8; j++)
                c = (c & 1) ? (0xEDB88320 ^ (c >> 1)) : (c >> 1);
            s_LocalCrcTable[i] = c;
        }
        s_LocalCrcTableInit = true;
    }
    const uint8_t* p = (const uint8_t*)data;
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < length; i++)
        crc = s_LocalCrcTable[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFF;
}

// CRC32 of a whole file. Returns size 0 / crc 0 when unreadable (callers treat that
// as "no verdict" and fall back to the weaker check).
static uint32_t HashFileCrc32(const std::string& path, uint32_t& outSize)
{
    outSize = 0;
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) return 0;
    std::streamoff size = file.tellg();
    if (size <= 0) return 0;
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> buf((size_t)size);
    file.read((char*)buf.data(), size);
    if ((std::streamoff)file.gcount() != size) return 0;
    outSize = (uint32_t)size;
    return Crc32OfBytes(buf.data(), buf.size());
}

// ---------------------------------------------------------------------------------
// H5 support: cache trailer stamp, appended AFTER the pixel block of every cache
// file. LTM_CACHE_VERSION itself lives in TextureEngine.h (outside this fix's file
// ownership), so mapping/behavior changes cannot bump it from here. The trailer is
// the bump-equivalent required for mapping/behavior changes:
//  - new loaders REQUIRE the trailer -> every pre-existing cache file (written
//    without one) is detected as stale at startup and recompiled ("alte cache/*.cache
//    werden beim Start erkannt und neu gebaut"),
//  - old DLLs never read past the pixel block and silently ignore the extra bytes.
// Bump LTM_CACHE_TRAILER_STAMP whenever compiled-pixel behavior changes (which
// source feeds a cache, orientation, snapping, format rules, K2 pairing changes).
// ---------------------------------------------------------------------------------
#pragma pack(push, 1)
struct CacheTrailer
{
    char magic[4];          // "LTMT"
    uint32_t stamp;         // LTM_CACHE_TRAILER_STAMP
    uint32_t sourcePngSize; // byte size of the source image at compile time
    uint32_t sourcePngCrc;  // CRC32 of the source image bytes at compile time (0 = unknown)
};
#pragma pack(pop)
static const uint32_t LTM_CACHE_TRAILER_STAMP = 1;

struct CacheTrailerInfo
{
    bool valid;
    uint32_t sourcePngSize;
    uint32_t sourcePngCrc;
};

// Reads the cache header plus the trailing stamp. Return value: the header carries
// the current magic+version. outTrailer.valid reports whether the CURRENT trailer
// stamp is present (false => cache written by older behavior => stale, recompile).
static bool ReadCacheHeaderAndTrailer(std::istream& file, CacheHeader& outHeader, CacheTrailerInfo& outTrailer)
{
    outTrailer.valid = false;
    outTrailer.sourcePngSize = 0;
    outTrailer.sourcePngCrc = 0;

    CacheHeader header;
    file.read((char*)&header, sizeof(header));
    if (file.gcount() != (std::streamsize)sizeof(header) ||
        memcmp(header.magic, "LTMC", 4) != 0 ||
        header.version != LTM_CACHE_VERSION)
    {
        return false;
    }
    outHeader = header;

    file.clear();
    file.seekg((std::streamoff)(sizeof(CacheHeader) + (uint64_t)header.dataSize), std::ios::beg);
    CacheTrailer trailer;
    ZeroMemory(&trailer, sizeof(trailer));
    file.read((char*)&trailer, sizeof(trailer));
    if (file.gcount() == (std::streamsize)sizeof(trailer) &&
        memcmp(trailer.magic, "LTMT", 4) == 0 &&
        trailer.stamp == LTM_CACHE_TRAILER_STAMP)
    {
        outTrailer.valid = true;
        outTrailer.sourcePngSize = trailer.sourcePngSize;
        outTrailer.sourcePngCrc = trailer.sourcePngCrc;
    }
    return true;
}

// True when the current source image still matches the content snapshot taken at
// compile time: size always, CRC32 whenever the trailer recorded one. Used by BOTH
// freshness checks (PrecompileModifiedTextures and LoadOrCreateCache) — mtime alone
// misses a replaced file that carries an old mtime.
static bool SourceMatchesCacheSnapshot(const CacheTrailerInfo& trailer, const std::string& sourcePath)
{
    if (!trailer.valid) return false;
    uint32_t curSize = 0;
    uint32_t curCrc = HashFileCrc32(sourcePath, curSize);
    if (curSize == 0) return false;
    if (curSize != trailer.sourcePngSize) return false;
    if (trailer.sourcePngCrc != 0 && curCrc != trailer.sourcePngCrc) return false;
    return true;
}

// ---------------------------------------------------------------------------------
// H2 support: extraction baseline table (textures\texture_baselines.bin).
// The extractor records exact size + CRC32 of every textures/ PNG it CREATED itself
// (never for pre-existing files — those may be user edits and must never become the
// baseline, otherwise later edits to the same file would be invisible forever).
// ScanModifiedTextures compares against this table so "untouched default" (the
// +2-POT-level HD upscale) is no longer false-flagged by the legacy rawDataSize
// compare, which used the PRISTINE originals/ size instead.
// ---------------------------------------------------------------------------------
struct BaselineTableEntry
{
    std::string filename;
    uint32_t baselineSize;
    uint32_t baselineCrc;
};
static std::unordered_map<uint32_t, BaselineTableEntry> s_BaselineTable;
static bool s_BaselineTableLoaded = false;

// Idempotent loader. Absent/corrupt sidecar => empty table => legacy fallback rule.
static bool LoadBaselineTable(const std::string& texturesDir)
{
    if (s_BaselineTableLoaded) return true;
    s_BaselineTableLoaded = true;

    std::ifstream file(texturesDir + "\\texture_baselines.bin", std::ios::binary);
    if (!file.is_open())
    {
        Logger::LogInfo("[INIT] No texture_baselines.bin (older extraction) - legacy rawDataSize modification rule stays active. Re-extract to enable content baselines.");
        return false;
    }

#pragma pack(push, 1)
    struct SidecarHeader { char magic[4]; uint32_t version; uint32_t entryCount; };
    struct SidecarEntry { uint32_t nameCrc; uint32_t nameLen; uint32_t baselineSize; uint32_t baselineCrc; };
#pragma pack(pop)

    SidecarHeader header;
    file.read((char*)&header, sizeof(header));
    bool ok = file.gcount() == (std::streamsize)sizeof(header) &&
              memcmp(header.magic, "LTMB", 4) == 0 &&
              header.version == 1;
    if (ok)
    {
        std::string name;
        for (uint32_t i = 0; i < header.entryCount && file.good(); i++)
        {
            SidecarEntry entry;
            file.read((char*)&entry, sizeof(entry));
            if (file.gcount() != (std::streamsize)sizeof(entry) ||
                entry.nameLen == 0 || entry.nameLen > 1024)
            {
                ok = false;
                break;
            }
            name.resize(entry.nameLen);
            file.read(&name[0], entry.nameLen);
            if (file.gcount() != (std::streamsize)entry.nameLen)
            {
                ok = false;
                break;
            }
            BaselineTableEntry out;
            out.filename = name;
            out.baselineSize = entry.baselineSize;
            out.baselineCrc = entry.baselineCrc;
            s_BaselineTable[entry.nameCrc] = out;
        }
    }

    if (!ok)
    {
        Logger::LogError("[INIT] texture_baselines.bin is corrupt - ignored, legacy modification rule stays active.");
        s_BaselineTable.clear();
        return false;
    }

    Logger::LogInfo("[INIT] Loaded %u extraction baselines (content-based modification detection, H2).", (unsigned int)s_BaselineTable.size());
    return true;
}

static const BaselineTableEntry* FindBaselineEntry(const std::string& filename)
{
    uint32_t key = Crc32OfBytes(filename.data(), filename.size());
    auto it = s_BaselineTable.find(key);
    if (it == s_BaselineTable.end()) return nullptr;
    if (it->second.filename != filename) return nullptr; // name-CRC collision => no verdict
    return &it->second;
}

// ---------------------------------------------------------------------------------
// H1 support: CRC collision groups. m_CrcMap (crc32 -> last index, archive-blind;
// its type is owned by TextureEngine.h and cannot change here) keeps its last-wins
// semantics for single-hit lookups; every DUPLICATED crc32 is additionally tracked
// here so FindByCRC_Locked can resolve the group (prefer the isModified member, else
// the last entry). Measured in texture_manifest.json: 360 crc values are shared by
// several entries, 73 2D_UI entries were shadowed by their group's last member
// (65 language-JPK twins such as eng_jpk#31 vs jap_jpk#31, 8 NewTexture files).
// ---------------------------------------------------------------------------------
static std::unordered_map<uint32_t, std::vector<uint32_t>> s_CrcCollisionGroups;

bool TextureEngine::Initialize(const std::string& modDir)
{
    m_ModDir = modDir;
    m_TexturesDir = m_ModDir + "\\textures";
    m_OriginalsDir = m_ModDir + "\\originals";
    m_CacheDir = m_ModDir + "\\cache";
    m_IniPath = m_ModDir + "\\LyrasTextureMod.ini";
    m_LogPath = m_ModDir + "\\LyrasTextureMod_error.log";

    CreateDirectoryA(m_TexturesDir.c_str(), NULL);
    CreateDirectoryA(m_OriginalsDir.c_str(), NULL);
    CreateDirectoryA(m_CacheDir.c_str(), NULL);

    CreateDirectoryA((m_TexturesDir + "\\3D_Objects").c_str(), NULL);
    CreateDirectoryA((m_TexturesDir + "\\3D_Stages").c_str(), NULL);
    CreateDirectoryA((m_TexturesDir + "\\2D_Sprites").c_str(), NULL);
    CreateDirectoryA((m_TexturesDir + "\\2D_UI").c_str(), NULL);
    CreateDirectoryA((m_TexturesDir + "\\Fonts").c_str(), NULL);

    CreateDirectoryA((m_OriginalsDir + "\\3D_Objects").c_str(), NULL);
    CreateDirectoryA((m_OriginalsDir + "\\3D_Stages").c_str(), NULL);
    CreateDirectoryA((m_OriginalsDir + "\\2D_Sprites").c_str(), NULL);
    CreateDirectoryA((m_OriginalsDir + "\\2D_UI").c_str(), NULL);
    CreateDirectoryA((m_OriginalsDir + "\\Fonts").c_str(), NULL);

    CreateDirectoryA((m_ModDir + "\\3D").c_str(), NULL);
    CreateDirectoryA((m_ModDir + "\\3D\\parts").c_str(), NULL);
    CreateDirectoryA((m_ModDir + "\\3D\\assemblies").c_str(), NULL);

    // 1. Initialize INI & Logger
    IniConfig::Load(m_IniPath, m_Config);
    Logger::Init(m_LogPath, m_Config.LogLevel);
    m_VanillaRestored = m_Config.RestoreOriginals;

    Logger::LogInfo("Initializing LyrasTextureMod TextureEngine...");
    Logger::LogInfo("Mod Directory: %s", m_ModDir.c_str());
    Logger::LogInfo("Textures Directory: %s", m_TexturesDir.c_str());
    Logger::LogInfo("Originals Directory: %s", m_OriginalsDir.c_str());
    Logger::LogInfo("Cache Directory: %s", m_CacheDir.c_str());

    // 2. Initialize GDI+
    GdiplusStartupInput gdiplusStartupInput;
    Status gdiStatus = GdiplusStartup(&m_GdiplusToken, &gdiplusStartupInput, NULL);
    if (gdiStatus != Ok)
    {
        Logger::LogError("Failed to initialize GDI+ (Status: %d). Image decoding may fail!", gdiStatus);
    }
    else
    {
        Logger::LogInfo("GDI+ subsystem initialized successfully for 32-bit PNG/JPG/JPEG processing.");
    }

    // Build directory cache for fast O(1) file resolution
    BuildPathCache();

    // 3. Load Binary Manifest
    if (!LoadManifest())
    {
        Logger::LogError("Failed to load binary texture manifest: %s\\texture_manifest.bin", m_TexturesDir.c_str());
    }

    // 3b. Load extraction baselines for content-based modification detection (H2).
    LoadBaselineTable(m_TexturesDir);

    // 4. Scan for modified PNGs/JPGs
    ScanModifiedTextures();

    // 5. Precompile any modified textures to cache with visual loading screen
    PrecompileModifiedTextures();

    m_Initialized = true;
    Logger::LogInfo("TextureEngine initialized successfully. Mod Enabled: %s, Vanilla Restored: %s",
        m_Config.Enabled ? "TRUE" : "FALSE", m_VanillaRestored ? "TRUE" : "FALSE");

    return true;
}

void TextureEngine::Shutdown()
{
    RestoreAllVanillaTextures();

    EnterCriticalSection(&m_CS);
    for (auto& pair : m_D3DTextureCache)
    {
        if (pair.second)
        {
            pair.second->Release();
        }
    }
    m_D3DTextureCache.clear();

    for (auto& pair : m_OriginalTextures)
    {
        if (pair.second)
        {
            __try { pair.second->Release(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
    }
    m_OriginalTextures.clear();
    m_TexObjToCustom.clear();

    if (m_pMissingTexture)
    {
        m_pMissingTexture->Release();
        m_pMissingTexture = nullptr;
    }
    LeaveCriticalSection(&m_CS);

    if (m_GdiplusToken)
    {
        GdiplusShutdown(m_GdiplusToken);
        m_GdiplusToken = 0;
    }
    m_Initialized = false;
    Logger::LogInfo("TextureEngine shut down cleanly.");
}

void TextureEngine::SetEnabled(bool enabled)
{
    m_Config.Enabled = enabled;
    m_VanillaRestored = !enabled;
    m_Config.RestoreOriginals = !enabled;
    IniConfig::Save(m_IniPath, m_Config);
    Logger::LogInfo("LyrasTextureMod Enabled toggled to: %s", enabled ? "ON" : "OFF");

    if (enabled)
    {
        ScanAndReplaceAllMasterTextures();
        Logger::LogInfo("[TOGGLE] Switched live to CUSTOM HD textures.");
    }
    else
    {
        RestoreAllVanillaTextures();
        Logger::LogInfo("[TOGGLE] Switched live to 100% ORIGINAL VANILLA textures.");
    }
}

void TextureEngine::ToggleRestoreOriginalTextures()
{
    SetVanillaRestored(!m_VanillaRestored);
}

void TextureEngine::SetVanillaRestored(bool restored)
{
    m_VanillaRestored = restored;
    m_Config.RestoreOriginals = restored;
    m_Config.Enabled = !restored;
    IniConfig::Save(m_IniPath, m_Config);

    if (m_VanillaRestored)
    {
        RestoreAllVanillaTextures();
        Logger::LogInfo("[RESTORE] Switched live to 100% ORIGINAL VANILLA textures.");
        MessageBeep(MB_OK);
    }
    else
    {
        ScanAndReplaceAllMasterTextures();
        Logger::LogInfo("[RESTORE] Switched live to CUSTOM HD textures.");
        MessageBeep(MB_ICONINFORMATION);
    }
}

// ---------------------------------------------------------------------------------
// Fail-safe & resilience (Missing Texture Fallback)
// ---------------------------------------------------------------------------------
bool TextureEngine::EnsureMissingTextureExists(const std::string& path)
{
    DWORD attr = GetFileAttributesA(path.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES)
        return true;

    // Generate classic magenta and black checkerboard (64x64, 8x8 blocks of 8x8 px)
    static const CLSID clsidPng = { 0x557cf406, 0x1a04, 0x11d3, { 0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e } };
    UINT w = 64, h = 64;
    std::vector<uint32_t> pixels(w * h);
    for (UINT y = 0; y < h; y++)
    {
        for (UINT x = 0; x < w; x++)
        {
            bool isMagenta = (((x / 8) + (y / 8)) % 2) == 0;
            pixels[y * w + x] = isMagenta ? 0xFFFF00FF : 0xFF000000;
        }
    }

    int wlen = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, NULL, 0);
    std::wstring wpath(wlen, 0);
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &wpath[0], wlen);

    Gdiplus::Bitmap bmp((INT)w, (INT)h, (INT)(w * 4), PixelFormat32bppARGB, (BYTE*)pixels.data());
    return (bmp.Save(wpath.c_str(), &clsidPng, NULL) == Gdiplus::Ok);
}

IDirect3DTexture9* TextureEngine::GetOrCreateMissingTexture()
{
    EnterCriticalSection(&m_CS);
    if (m_pMissingTexture)
    {
        LeaveCriticalSection(&m_CS);
        return m_pMissingTexture;
    }

    IDirect3DDevice9* pDevice = GetD3DDevice();
    if (!pDevice)
    {
        LeaveCriticalSection(&m_CS);
        return nullptr;
    }

    // Create 64x64 D3D texture with magenta/black checkerboard
    UINT w = 64, h = 64;
    IDirect3DTexture9* pTex = nullptr;
    HRESULT hr = pDevice->CreateTexture(w, h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &pTex, NULL);
    if (SUCCEEDED(hr) && pTex)
    {
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(pTex->LockRect(0, &lr, NULL, 0)) && lr.pBits)
        {
            uint32_t* pBits = (uint32_t*)lr.pBits;
            int pitch = lr.Pitch / 4;
            for (UINT y = 0; y < h; y++)
            {
                for (UINT x = 0; x < w; x++)
                {
                    bool isMagenta = (((x / 8) + (y / 8)) % 2) == 0;
                    pBits[y * pitch + x] = isMagenta ? 0xFFFF00FF : 0xFF000000;
                }
            }
            pTex->UnlockRect(0);
            m_pMissingTexture = pTex;
        }
    }

    LeaveCriticalSection(&m_CS);
    return m_pMissingTexture;
}

// ---------------------------------------------------------------------------------
// In-game Tools & Features (100% Native C++, no external tools or Python)
// ---------------------------------------------------------------------------------
bool TextureEngine::CreateTexturesZipBackup(std::string* outZipPath, int* outCount)
{
    std::string backupsDir = m_ModDir + "\\backups";
    CreateDirectoryA(backupsDir.c_str(), NULL);

    SYSTEMTIME st;
    GetLocalTime(&st);
    char filename[128];
    sprintf_s(filename, sizeof(filename), "textures_backup_%04d%02d%02d_%02d%02d%02d.zip",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    std::string fullZipPath = backupsDir + "\\" + filename;
    int count = 0;
    bool ok = ZipWriter::CreateZipFromFolder(m_TexturesDir, fullZipPath, &count);
    if (ok)
    {
        Logger::LogInfo("[BACKUP] Created textures backup: %s (%d files)", fullZipPath.c_str(), count);
        if (outZipPath) *outZipPath = fullZipPath;
        if (outCount) *outCount = count;
        return true;
    }
    else
    {
        Logger::LogError("[BACKUP] Failed to create textures backup at %s", fullZipPath.c_str());
        return false;
    }
}

void TextureEngine::RestartGame()
{
    char exePath[MAX_PATH] = { 0 };
    if (GetModuleFileNameA(NULL, exePath, MAX_PATH) == 0) return;

    // Only restart if running inside Crazy Taxi.exe
    if (strstr(exePath, "Crazy Taxi.exe") == nullptr && strstr(exePath, "Crazy Taxi") == nullptr)
    {
        Logger::LogInfo("[SYSTEM] RestartGame skipped: not running inside Crazy Taxi.exe (%s)", exePath);
        return;
    }

    char workDir[MAX_PATH] = { 0 };
    strcpy_s(workDir, exePath);
    char* lastSlash = strrchr(workDir, '\\');
    if (lastSlash) *lastSlash = '\0';

    STARTUPINFOA si = { sizeof(STARTUPINFOA) };
    PROCESS_INFORMATION pi = { 0 };

    Logger::LogInfo("[SYSTEM] Restarting Crazy Taxi natively from: %s", exePath);

    // Launch fresh game process
    if (CreateProcessA(exePath, NULL, NULL, NULL, FALSE, 0, NULL, workDir, &si, &pi))
    {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        ExitProcess(0);
    }
    else
    {
        ShellExecuteA(NULL, "open", exePath, NULL, workDir, SW_SHOW);
        ExitProcess(0);
    }
}

bool TextureEngine::IsTexturesStamped() const
{
    std::string backupDir = m_TexturesDir + "\\.stamp_backup";
    DWORD attr = GetFileAttributesA(backupDir.c_str());
    return (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY));
}

bool TextureEngine::StampTestTextures(std::string* outStatus)
{
    if (IsTexturesStamped())
    {
        if (outStatus) *outStatus = "Textures are already stamped. Please unstamp first.";
        return false;
    }

    std::string backupDir = m_TexturesDir + "\\.stamp_backup";
    CreateDirectoryA(backupDir.c_str(), NULL);

    // 1. Enumerate all PNG files in m_TexturesDir
    struct FileEntry {
        std::string fullPath;
        std::string relPath;
        std::string backupPath;
    };
    std::vector<FileEntry> files;

    auto Enumerate = [&](auto& self, const std::string& curDir, const std::string& relPrefix) -> void
    {
        std::string mask = curDir + "\\*.*";
        WIN32_FIND_DATAA fd;
        HANDLE hFind = FindFirstFileA(mask.c_str(), &fd);
        if (hFind == INVALID_HANDLE_VALUE) return;
        do
        {
            if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
            if (fd.cFileName[0] == '.') continue; // ignore hidden or backup folders

            std::string full = curDir + "\\" + fd.cFileName;
            std::string rel = relPrefix.empty() ? fd.cFileName : (relPrefix + "\\" + fd.cFileName);

            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            {
                self(self, full, rel);
            }
            else
            {
                const char* ext = strrchr(fd.cFileName, '.');
                if (ext && _stricmp(ext, ".png") == 0)
                {
                    std::string bkp = backupDir + "\\" + rel;
                    files.push_back({ full, rel, bkp });
                }
            }
        } while (FindNextFileA(hFind, &fd));
        FindClose(hFind);
    };

    Enumerate(Enumerate, m_TexturesDir, "");

    if (files.empty())
    {
        if (outStatus) *outStatus = "No PNG textures found in textures directory.";
        return false;
    }

    std::sort(files.begin(), files.end(), [](const FileEntry& a, const FileEntry& b) {
        return a.relPath < b.relPath;
    });

    HWND hGame = FindWindowA("Crazy Taxi", "Crazy Taxi");
    if (!hGame) hGame = GetActiveWindow();
    int totalFiles = (int)files.size();
    ProgressScreen::Show(hGame, totalFiles, "LYRA'S TEXTURE MOD - STAMPING TEXTURES", "Please wait... Backing up textures");

    // 2. Backup each file to .stamp_backup
    for (size_t i = 0; i < files.size(); i++)
    {
        const auto& f = files[i];
        size_t lastSlash = f.backupPath.find_last_of("\\/");
        if (lastSlash != std::string::npos)
        {
            std::string dir = f.backupPath.substr(0, lastSlash);
            char temp[MAX_PATH];
            strcpy_s(temp, dir.c_str());
            for (char* p = temp + 3; *p; p++)
            {
                if (*p == '\\' || *p == '/')
                {
                    *p = '\0';
                    CreateDirectoryA(temp, NULL);
                    *p = '\\';
                }
            }
            CreateDirectoryA(temp, NULL);
        }
        CopyFileA(f.fullPath.c_str(), f.backupPath.c_str(), FALSE);

        if (i % 100 == 0 || i + 1 == files.size())
        {
            char msgBuf[128];
            sprintf_s(msgBuf, sizeof(msgBuf), "Backing up textures (%d / %d)... Please wait", (int)(i + 1), totalFiles);
            ProgressScreen::Update((int)(i + 1), totalFiles, msgBuf, "Stage 1/2: Backup");
        }
    }

    // 3. Stamp each texture with GDI+
    static const CLSID clsidPng = { 0x557cf406, 0x1a04, 0x11d3, { 0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e } };

    // Default scale 0.49f (0.70 * 0.70 = 0.49), configurable via INI [General] StampScale
    float stampScale = 0.49f;
    char scaleBuf[32] = { 0 };
    if (GetPrivateProfileStringA("General", "StampScale", "", scaleBuf, sizeof(scaleBuf), m_IniPath.c_str()) > 0)
    {
        float val = (float)atof(scaleBuf);
        if (val > 0.05f && val <= 1.0f) stampScale = val;
    }

    std::ofstream mappingTxt(m_ModDir + "\\texture_mapping.txt");
    std::ofstream mappingCsv(m_ModDir + "\\texture_mapping.csv");
    if (mappingCsv.is_open())
    {
        mappingCsv << "Index,RelativePath,Filename\n";
    }

    // Allocate persistent GDI+ resources outside loop for high performance
    Gdiplus::FontFamily fontFamily(L"Arial");
    Gdiplus::SolidBrush blackBrush(Gdiplus::Color(230, 0, 0, 0));
    Gdiplus::Pen pinkPen(Gdiplus::Color(255, 255, 20, 147), 2.0f);
    Gdiplus::SolidBrush pinkBrush(Gdiplus::Color(255, 255, 20, 147));
    Gdiplus::StringFormat strFormat;
    strFormat.SetAlignment(Gdiplus::StringAlignmentCenter);
    strFormat.SetLineAlignment(Gdiplus::StringAlignmentCenter);

    int stampedCount = 0;
    for (size_t i = 0; i < files.size(); i++)
    {
        int num = (int)(i + 1);
        const auto& f = files[i];

        if (mappingTxt.is_open())
        {
            mappingTxt << "[" << num << "] " << f.relPath << "\n";
        }
        if (mappingCsv.is_open())
        {
            const char* baseFn = strrchr(f.fullPath.c_str(), '\\');
            mappingCsv << num << ",\"" << f.relPath << "\",\"" << (baseFn ? baseFn + 1 : f.relPath.c_str()) << "\"\n";
        }

        int wlen = MultiByteToWideChar(CP_UTF8, 0, f.fullPath.c_str(), -1, NULL, 0);
        std::wstring wpath(wlen, 0);
        MultiByteToWideChar(CP_UTF8, 0, f.fullPath.c_str(), -1, &wpath[0], wlen);

        Gdiplus::Bitmap* pBmp = Gdiplus::Bitmap::FromFile(wpath.c_str());
        if (!pBmp || pBmp->GetLastStatus() != Gdiplus::Ok)
        {
            if (pBmp) delete pBmp;
            continue;
        }

        UINT w = pBmp->GetWidth();
        UINT h = pBmp->GetHeight();
        if (w == 0 || h == 0)
        {
            delete pBmp;
            continue;
        }

        // Create in-memory clone so file handle is released before saving back
        Gdiplus::Bitmap cloneBmp(w, h, PixelFormat32bppARGB);
        {
            Gdiplus::Graphics g(&cloneBmp);
            g.DrawImage(pBmp, 0, 0, w, h);
            delete pBmp;
            pBmp = nullptr;

            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);

            // Stamp box size on all textures
            int boxW = (int)(w * stampScale);
            if (boxW < 18) boxW = (int)w;
            int boxH = (int)(h * stampScale);
            if (boxH < 18) boxH = (int)h;

            int boxX = ((int)w - boxW) / 2;
            int boxY = ((int)h - boxH) / 2;

            // Semi-transparent black background box
            g.FillRectangle(&blackBrush, boxX, boxY, boxW, boxH);

            // Pink border
            g.DrawRectangle(&pinkPen, boxX, boxY, boxW, boxH);

            std::wstring numStr = std::to_wstring(num);
            float fontSize = (float)(boxH * 0.35f);
            if (fontSize < 7.0f) fontSize = 7.0f;

            Gdiplus::Font font(&fontFamily, fontSize, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);

            // 1. Top half: Normal upright number (half size)
            Gdiplus::RectF topRect((float)boxX, (float)boxY, (float)boxW, (float)(boxH / 2));
            g.DrawString(numStr.c_str(), -1, &font, topRect, &strFormat, &pinkBrush);

            // 2. Bottom half: Horizontally mirrored number (for 3D mirrored UVs)
            float centerX = boxX + boxW / 2.0f;
            float bottomCenterY = boxY + boxH * 0.75f;

            Gdiplus::GraphicsState gState = g.Save();
            g.TranslateTransform(centerX, bottomCenterY);
            g.ScaleTransform(-1.0f, 1.0f); // Horizontal flip
            Gdiplus::RectF mirrorRect(-boxW / 2.0f, -(boxH * 0.25f), (float)boxW, (float)(boxH / 2));
            g.DrawString(numStr.c_str(), -1, &font, mirrorRect, &strFormat, &pinkBrush);
            g.Restore(gState);
        }

        cloneBmp.Save(wpath.c_str(), &clsidPng, NULL);
        stampedCount++;

        if (i % 25 == 0 || i + 1 == files.size())
        {
            char msgBuf[128];
            sprintf_s(msgBuf, sizeof(msgBuf), "Stamping textures (%d / %d)... Please wait", (int)(i + 1), totalFiles);
            ProgressScreen::Update((int)(i + 1), totalFiles, msgBuf, "Stage 2/2: Numbering textures");
        }
    }

    if (mappingTxt.is_open()) mappingTxt.close();
    if (mappingCsv.is_open()) mappingCsv.close();

    Logger::LogInfo("[STAMP] Successfully stamped %d textures (mappings saved to texture_mapping.txt/.csv, scale=%.2f).", stampedCount, stampScale);

    // Flush compiled .cache files so fresh stamped textures compile cleanly on restart
    std::string searchMask = m_CacheDir + "\\*.cache";
    WIN32_FIND_DATAA fdCache;
    HANDLE hFindCache = FindFirstFileA(searchMask.c_str(), &fdCache);
    if (hFindCache != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (!(fdCache.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            {
                std::string fullPath = m_CacheDir + "\\" + fdCache.cFileName;
                SetFileAttributesA(fullPath.c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileA(fullPath.c_str());
            }
        } while (FindNextFileA(hFindCache, &fdCache));
        FindClose(hFindCache);
    }

    ProgressScreen::Update(totalFiles, totalFiles, "Stamping complete! Restarting game... Please wait", "Restarting game...");
    Sleep(800);
    RestartGame();

    char statusBuf[256];
    sprintf_s(statusBuf, sizeof(statusBuf), "Stamped %d textures. Game restarting.", stampedCount);
    if (outStatus) *outStatus = statusBuf;
    return true;
}

bool TextureEngine::UnstampTestTextures(std::string* outStatus)
{
    if (!IsTexturesStamped())
    {
        if (outStatus) *outStatus = "No active stamp found (textures are already unstamped).";
        return false;
    }

    std::string backupDir = m_TexturesDir + "\\.stamp_backup";

    struct FileEntry {
        std::string srcPath;
        std::string dstPath;
    };
    std::vector<FileEntry> filesToRestore;

    auto EnumerateBkp = [&](auto& self, const std::string& curBkpDir, const std::string& relPrefix) -> void
    {
        std::string mask = curBkpDir + "\\*.*";
        WIN32_FIND_DATAA fd;
        HANDLE hFind = FindFirstFileA(mask.c_str(), &fd);
        if (hFind == INVALID_HANDLE_VALUE) return;
        do
        {
            if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
            std::string src = curBkpDir + "\\" + fd.cFileName;
            std::string rel = relPrefix.empty() ? fd.cFileName : (relPrefix + "\\" + fd.cFileName);
            std::string dst = m_TexturesDir + "\\" + rel;

            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            {
                self(self, src, rel);
            }
            else
            {
                filesToRestore.push_back({ src, dst });
            }
        } while (FindNextFileA(hFind, &fd));
        FindClose(hFind);
    };

    EnumerateBkp(EnumerateBkp, backupDir, "");

    HWND hGame = FindWindowA("Crazy Taxi", "Crazy Taxi");
    if (!hGame) hGame = GetActiveWindow();
    int totalFiles = (int)filesToRestore.size();
    ProgressScreen::Show(hGame, totalFiles > 0 ? totalFiles : 100, "LYRA'S TEXTURE MOD - RESTORING ORIGINAL TEXTURES", "Please wait... Restoring textures from backup");

    // 1. Restore all files from .stamp_backup to m_TexturesDir
    for (size_t i = 0; i < filesToRestore.size(); i++)
    {
        const auto& f = filesToRestore[i];
        size_t lastSlash = f.dstPath.find_last_of("\\/");
        if (lastSlash != std::string::npos)
        {
            std::string dir = f.dstPath.substr(0, lastSlash);
            char temp[MAX_PATH];
            strcpy_s(temp, dir.c_str());
            for (char* p = temp + 3; *p; p++)
            {
                if (*p == '\\' || *p == '/')
                {
                    *p = '\0';
                    CreateDirectoryA(temp, NULL);
                    *p = '\\';
                }
            }
            CreateDirectoryA(temp, NULL);
        }
        SetFileAttributesA(f.dstPath.c_str(), FILE_ATTRIBUTE_NORMAL);
        CopyFileA(f.srcPath.c_str(), f.dstPath.c_str(), FALSE);

        if (i % 50 == 0 || i + 1 == filesToRestore.size())
        {
            char msgBuf[128];
            sprintf_s(msgBuf, sizeof(msgBuf), "Restoring textures (%d / %d)... Please wait", (int)(i + 1), totalFiles);
            ProgressScreen::Update((int)(i + 1), totalFiles, msgBuf, "Restoring original clean textures");
        }
    }

    // 2. Remove .stamp_backup directory recursively
    auto DeleteRecursive = [&](auto& self, const std::string& curDir) -> void
    {
        std::string mask = curDir + "\\*.*";
        WIN32_FIND_DATAA fd;
        HANDLE hFind = FindFirstFileA(mask.c_str(), &fd);
        if (hFind == INVALID_HANDLE_VALUE) return;
        do
        {
            if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
            std::string path = curDir + "\\" + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            {
                self(self, path);
                SetFileAttributesA(path.c_str(), FILE_ATTRIBUTE_NORMAL);
                RemoveDirectoryA(path.c_str());
            }
            else
            {
                SetFileAttributesA(path.c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileA(path.c_str());
            }
        } while (FindNextFileA(hFind, &fd));
        FindClose(hFind);
        SetFileAttributesA(curDir.c_str(), FILE_ATTRIBUTE_NORMAL);
        RemoveDirectoryA(curDir.c_str());
    };

    DeleteRecursive(DeleteRecursive, backupDir);

    // 3. Remove mapping files if present
    DeleteFileA((m_ModDir + "\\texture_mapping.txt").c_str());
    DeleteFileA((m_ModDir + "\\texture_mapping.csv").c_str());

    // 4. CRITICAL: Flush all compiled .cache files so no stamped texture caches survive
    std::string searchMask = m_CacheDir + "\\*.cache";
    WIN32_FIND_DATAA fdCache;
    HANDLE hFindCache = FindFirstFileA(searchMask.c_str(), &fdCache);
    if (hFindCache != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (!(fdCache.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            {
                std::string fullPath = m_CacheDir + "\\" + fdCache.cFileName;
                SetFileAttributesA(fullPath.c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileA(fullPath.c_str());
            }
        } while (FindNextFileA(hFindCache, &fdCache));
        FindClose(hFindCache);
    }

    Logger::LogInfo("[UNSTAMP] Original textures successfully restored from backup.");

    // 5. Clean native restart of the game
    ProgressScreen::Update(totalFiles > 0 ? totalFiles : 100, totalFiles > 0 ? totalFiles : 100, "Textures restored! Restarting game... Please wait", "Restarting game...");
    Sleep(800);
    RestartGame();

    if (outStatus) *outStatus = "Textures cleanly restored (unstamped) and game restarting.";
    return true;
}

bool TextureEngine::LoadManifest()
{
    std::string binPath = m_TexturesDir + "\\texture_manifest.bin";
    std::ifstream file(binPath, std::ios::binary);
    if (!file.is_open())
    {
        Logger::LogError("Cannot open texture_manifest.bin at: %s", binPath.c_str());
        return false;
    }

    ManifestHeader header;
    file.read((char*)&header, sizeof(header));
    if (memcmp(header.magic, "LTM1", 4) != 0 || header.version != 1)
    {
        Logger::LogError("Invalid header or version in texture_manifest.bin!");
        return false;
    }

    std::vector<ManifestEntryRecord> records(header.entryCount);
    file.read((char*)records.data(), header.entryCount * sizeof(ManifestEntryRecord));

    std::vector<char> stringTable(header.stringTableLen);
    file.read(stringTable.data(), header.stringTableLen);
    file.close();

    EnterCriticalSection(&m_CS);
    m_Textures.clear();
    m_CrcMap.clear();
    s_CrcCollisionGroups.clear();
    m_ArchiveGbixMap.clear();
    m_OffsetMap.clear();
    m_ArchiveIndexMap.clear();

    m_Textures.reserve(header.entryCount);
    for (size_t i = 0; i < records.size(); i++)
    {
        RuntimeTextureInfo info;
        info.record = records[i];
        if (records[i].filenameOffset < header.stringTableLen)
        {
            info.filename = &stringTable[records[i].filenameOffset];
        }
        info.fullSourceImagePath = ResolveImagePath(info.filename);
        info.fullPngPath = info.fullSourceImagePath;
        info.fullCachePath = m_CacheDir + "\\" + info.filename + ".cache";
        info.isModified = false;

        m_Textures.push_back(info);
        size_t idx = m_Textures.size() - 1;

        // H1 (CRC collision fix): m_CrcMap is archive-blind and keeps last-wins
        // semantics (its type is owned by TextureEngine.h and cannot change here).
        // Every duplicated crc32 is additionally recorded in s_CrcCollisionGroups so
        // FindByCRC_Locked can resolve the group. Measured in texture_manifest.json:
        // 360 crc values are shared by multiple entries and 73 2D_UI entries were
        // shadowed by the last member of their group (65 language-JPK twins like
        // eng_jpk#31 vs jap_jpk#31, 8 NewTexture files) — edits to a shadowed member
        // never showed and the injection log named the wrong file.
        auto crcIt = m_CrcMap.find(records[i].crc32);
        if (crcIt != m_CrcMap.end())
        {
            std::vector<uint32_t>& group = s_CrcCollisionGroups[records[i].crc32];
            if (group.empty())
                group.push_back(crcIt->second);
            group.push_back(idx);
        }
        m_CrcMap[records[i].crc32] = idx;
        // K4: GBIX keyed per archive. The archive-blind m_GbixMap[gbix] collided for
        // 210 gbix values (829 cross-archive pairs with identical dimensions measured
        // in the round-trip contract) and could inject a foreign file.
        uint64_t archiveGbixKey = ((uint64_t)records[i].archiveId << 32) | records[i].gbix;
        m_ArchiveGbixMap[archiveGbixKey] = idx;

        uint64_t offsetKey = ((uint64_t)records[i].archiveId << 32) | records[i].dataOffset;
        m_OffsetMap[offsetKey] = idx;

        uint64_t archiveIndexKey = ((uint64_t)records[i].archiveId << 32) | records[i].chunkIndex;
        m_ArchiveIndexMap[archiveIndexKey] = idx;
    }
    LeaveCriticalSection(&m_CS);

    if (!s_CrcCollisionGroups.empty())
    {
        Logger::LogInfo("Manifest CRC collision groups: %u (H1: resolved by preferring the isModified member, else last entry).",
            (unsigned int)s_CrcCollisionGroups.size());
    }

    Logger::LogInfo("Loaded texture manifest successfully. %u textures indexed into O(1) hash maps.", header.entryCount);
    return true;
}

void TextureEngine::ScanModifiedTextures()
{
    EnterCriticalSection(&m_CS);
    int modifiedCount = 0;
    m_ModifiedArchiveIndexSet.clear();
    m_ModifiedOffsetSet.clear();
    m_ModifiedGbixSet.clear();

    for (auto& tex : m_Textures)
    {
        tex.isModified = false;
        tex.fullSourceImagePath = ResolveImagePath(tex.filename);
        tex.fullPngPath = tex.fullSourceImagePath;
        WIN32_FILE_ATTRIBUTE_DATA fileData;
        if (GetFileAttributesExA(tex.fullSourceImagePath.c_str(), GetFileExInfoStandard, &fileData))
        {
            uint64_t curTime = ((uint64_t)fileData.ftLastWriteTime.dwHighDateTime << 32) | fileData.ftLastWriteTime.dwLowDateTime;
            tex.sourceImageLastWriteTime = curTime;

            // NOTE (K7): cache existence is intentionally NOT a modification signal.
            // A slot whose textures/ PNG is byte-size-identical to the vanilla extraction
            // is not user-modified and must never be replaced (true no-op), regardless of
            // leftover cache files from previous sessions.
            bool isMod = false;
            // 1. Is it a user-provided JPG or JPEG?
            size_t dotPos = tex.fullSourceImagePath.find_last_of('.');
            if (dotPos != std::string::npos)
            {
                std::string ext = tex.fullSourceImagePath.substr(dotPos);
                if (_stricmp(ext.c_str(), ".jpg") == 0 || _stricmp(ext.c_str(), ".jpeg") == 0)
                {
                    isMod = true;
                }
            }

            // 2. Check the textures/ PNG against the extraction baseline (H2).
            // record.rawDataSize is the size of the PRISTINE originals/ PNG (Jpng: the
            // payload bytes), while the default textures/ file is the +2-POT-level HD
            // upscale — comparing against it false-flagged ~1870/1926 untouched files
            // as "user-customized" and made the K8 gate below ineffective. The
            // extractor records size+CRC32 of every textures/ file IT created in
            // textures\texture_baselines.bin; a file that still matches that baseline
            // is untouched. Records without a baseline entry (older installs, or
            // pre-existing files the extractor must not baseline) keep the legacy
            // rawDataSize fallback.
            if (!isMod)
            {
                const BaselineTableEntry* baseline = FindBaselineEntry(tex.filename);
                if (baseline)
                {
                    if (fileData.nFileSizeLow != baseline->baselineSize)
                    {
                        isMod = true;
                    }
                    else if (baseline->baselineCrc != 0)
                    {
                        uint32_t curSize = 0;
                        uint32_t curCrc = HashFileCrc32(tex.fullSourceImagePath, curSize);
                        if (curCrc != 0 && curCrc != baseline->baselineCrc)
                            isMod = true;
                    }
                }
                else if (tex.record.rawDataSize != 0 && fileData.nFileSizeLow != tex.record.rawDataSize)
                {
                    isMod = true;
                }
            }

            if (isMod)
            {
                tex.isModified = true;
                modifiedCount++;
                m_ModifiedArchiveIndexSet.insert(((uint64_t)tex.record.archiveId << 32) | (uint64_t)tex.record.chunkIndex);
                m_ModifiedOffsetSet.insert(((uint64_t)tex.record.archiveId << 32) | (uint64_t)tex.record.dataOffset);
                if (tex.record.gbix != 0)
                {
                    m_ModifiedGbixSet.insert(((uint64_t)tex.record.archiveId << 32) | (uint64_t)tex.record.gbix);
                }
            }
        }
    }
    LeaveCriticalSection(&m_CS);

    Logger::LogInfo("Scanned texture modifications: %d user-customized textures active.", modifiedCount);
}

// True when the cache file carries the current LTMC format version INCLUDING the
// trailing stamp (H5). Caches written by older engine behavior (pre-contract
// orientation/snap/keying, or any mapping/behavior change made without touching the
// header-owned LTM_CACHE_VERSION) are invalid and must be recompiled, otherwise stale
// pixel behavior survives behavior changes (req: cache consistency).
static bool HasCurrentCacheVersion(const std::string& cachePath, CacheTrailerInfo* outTrailer = nullptr)
{
    std::ifstream file(cachePath, std::ios::binary);
    if (!file.is_open()) return false;
    CacheHeader header;
    CacheTrailerInfo trailer;
    if (!ReadCacheHeaderAndTrailer(file, header, trailer)) return false;
    if (!trailer.valid) return false; // pre-trailer cache => stale
    if (outTrailer) *outTrailer = trailer;
    return true;
}

// Resolves INI [General] ShowProgressScreen=auto|on|off (default auto). The key is
// read directly from the INI here because ModConfig/IniConfig live outside this
// file's ownership.
//   off  -> never show the GDI progress screen (silent compile).
//   on   -> always show it while compiling (explicit user override).
//   auto -> always show it during compilation to prevent UI freezing and give visual feedback.
static bool ShouldShowProgressScreen(const std::string& iniPath)
{
    // Never show modal progress screen or pump messages if D3D is already initialized and active
    // (in-game reload/flush/unstamp) to prevent re-entrant WM_PAINT message pumping during ImGui render frames.
    if (TextureEngine::Instance().IsInitialized() && TextureEngine::Instance().GetD3DDevice() != nullptr)
        return false;

    char value[16] = { 0 };
    GetPrivateProfileStringA("General", "ShowProgressScreen", "auto",
        value, sizeof(value), iniPath.c_str());
    if (_stricmp(value, "off") == 0)
        return false;
    return true;
}

void TextureEngine::PrecompileModifiedTextures(const char* title, const char* subtitle)
{
    std::vector<const RuntimeTextureInfo*> toCompile;
    {
        EnterCriticalSection(&m_CS);
        for (const auto& tex : m_Textures)
        {
            if (tex.isModified)
            {
                WIN32_FILE_ATTRIBUTE_DATA cacheData;
                bool cacheExists = GetFileAttributesExA(tex.fullCachePath.c_str(), GetFileExInfoStandard, &cacheData);
                bool needsCompile = true;
                CacheTrailerInfo trailer;
                if (cacheExists && HasCurrentCacheVersion(tex.fullCachePath, &trailer))
                {
                    uint64_t cacheTime = ((uint64_t)cacheData.ftLastWriteTime.dwHighDateTime << 32) | cacheData.ftLastWriteTime.dwLowDateTime;
                    // H5: mtime freshness is kept, but is no longer sufficient on its
                    // own — the source must also still match the size+CRC32 snapshot
                    // recorded at compile time (same check as LoadOrCreateCache).
                    if (cacheTime >= tex.sourceImageLastWriteTime &&
                        SourceMatchesCacheSnapshot(trailer, tex.fullSourceImagePath))
                    {
                        needsCompile = false;
                    }
                }
                if (needsCompile)
                {
                    toCompile.push_back(&tex);
                }
            }
        }
        LeaveCriticalSection(&m_CS);
    }

    if (toCompile.empty())
    {
        Logger::LogInfo("All modified textures are already compiled in cache. Startup is instant.");
        return;
    }

    const bool showScreen = ShouldShowProgressScreen(m_IniPath);
    Logger::LogInfo("Found %zu modified textures requiring compilation. Progress screen: %s",
        toCompile.size(), showScreen ? "visible" : "hidden (silent compile)");

    HWND hGame = FindWindowA("Crazy Taxi", "Crazy Taxi");
    if (!hGame) hGame = GetActiveWindow();

    bool openedLocally = false;
    if (showScreen && !ProgressScreen::IsShowing())
    {
        ProgressScreen::Show(hGame, (int)toCompile.size(), title, subtitle);
        openedLocally = true;
    }

    // The compile loop itself pumps progress window messages so it never freezes
    int done = 0;
    for (const auto* pTex : toCompile)
    {
        if (showScreen)
            ProgressScreen::Update(done, (int)toCompile.size(), pTex->filename);
        CompileImageToCache(*pTex, pTex->fullSourceImagePath, pTex->fullCachePath);
        done++;
        if (showScreen)
            ProgressScreen::Update(done, (int)toCompile.size(), pTex->filename);
    }

    if (openedLocally)
        ProgressScreen::Hide();
    Logger::LogInfo("All %zu modified textures compiled successfully into smart cache!", toCompile.size());
}

// SEH-isolated helpers to avoid MSVC C2712 unwinding restrictions
static void SafeReleaseD3DTexture(IDirect3DTexture9* pTex)
{
    if (!pTex) return;
    __try {
        pTex->Release();
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static bool IsValidTextureObject(void* pTexObj)
{
    if (!pTexObj || (uintptr_t)pTexObj < 0x10000 || (uintptr_t)pTexObj > 0x7FFE0000)
        return false;
    __try {
        return (*(uintptr_t*)pTexObj == 0x0059EB10);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void TextureEngine::ReloadTextures(bool isFlush)
{
    const bool showScreen = ShouldShowProgressScreen(m_IniPath);
    HWND hGame = FindWindowA("Crazy Taxi", "Crazy Taxi");
    if (!hGame) hGame = GetActiveWindow();

    if (showScreen)
    {
        ProgressScreen::Show(
            hGame, 100,
            isFlush ? "LYRA'S TEXTURE MOD - FLUSHING CACHE" : "LYRA'S TEXTURE MOD - RELOADING TEXTURES",
            isFlush ? "Flushing binary cache and recompiling all textures..." : "Scanning modified textures and rebuilding cache..."
        );
        ProgressScreen::Update(5, 100, "Clearing Direct3D texture cache...", "Stage 1/5: Clearing D3D cache");
    }

    Logger::LogInfo(isFlush ? "Flushing cache and recompiling all textures..." : "Reloading all textures and rebuilding smart cache...");

    EnterCriticalSection(&m_CS);
    for (auto& pair : m_D3DTextureCache)
    {
        SafeReleaseD3DTexture(pair.second);
    }
    m_D3DTextureCache.clear();
    m_TexObjToCustom.clear();
    LeaveCriticalSection(&m_CS);

    if (showScreen)
        ProgressScreen::Update(15, 100, "Scanning texture folders and manifests...", "Stage 2/5: Scanning folders");

    BuildPathCache();
    LoadManifest();
    ScanModifiedTextures();

    if (showScreen)
        ProgressScreen::Update(30, 100, "Compiling textures into binary cache (.cache)...", "Stage 3/5: Compiling cache");

    if (isFlush)
    {
        PrecompileModifiedTextures(
            "LYRA'S TEXTURE MOD - FLUSHING CACHE",
            "Rebuilding all textures from scratch into binary cache (.cache)..."
        );
    }
    else
    {
        PrecompileModifiedTextures(
            "LYRA'S TEXTURE MOD - RELOADING TEXTURES",
            "Compiling updated textures into high-speed binary cache (.cache)..."
        );
    }

    if (showScreen)
        ProgressScreen::Update(75, 100, "Refreshing Direct3D 9 texture objects in engine...", "Stage 4/5: Re-linking D3D slots");

    // If disabled or vanilla textures are restored, do NOT re-install custom textures into game slots!
    if (!m_Config.Enabled || m_VanillaRestored)
    {
        Logger::LogInfo("Texture reload completed (mod is disabled / vanilla restored; skipping live custom texture re-installation).");
        if (showScreen)
        {
            ProgressScreen::Update(100, 100, "Texture reload complete!", "Completed!");
            ProgressScreen::Hide();
        }
        return;
    }

    // K26: re-install pass — swap every recorded slot to its fresh custom. The
    // objects themselves are the game's live texture objects; InstallCustomTexture
    // classifies each (C1 skip / C2 swap / C3 re-apply / C4 evict-recycled).
    std::vector<std::pair<void*, uint64_t>> snapshot;
    EnterCriticalSection(&m_CS);
    snapshot.reserve(m_TexObjToRecordKey.size());
    for (const auto& pair : m_TexObjToRecordKey)
        snapshot.push_back(pair);
    LeaveCriticalSection(&m_CS);

    int refreshed = 0;
    int reverted = 0;
    for (auto& entry : snapshot)
    {
        void* pTexObj = entry.first;
        const RuntimeTextureInfo* info = FindInfoByCacheKey(entry.second);
        if (!info || !info->isModified)
        {
            // Slot is no longer modified (e.g. restored by Unstamp or user deleted custom texture).
            // Revert slot back to vanilla texture!
            IDirect3DTexture9* pStoredOriginal = nullptr;
            EnterCriticalSection(&m_CS);
            auto origIt = m_OriginalTextures.find(pTexObj);
            if (origIt != m_OriginalTextures.end())
            {
                pStoredOriginal = origIt->second;
            }
            LeaveCriticalSection(&m_CS);

            if (pStoredOriginal)
            {
                RestoreOriginalForTexObj(pTexObj, pStoredOriginal);
            }

            EnterCriticalSection(&m_CS);
            m_TexObjToRecordKey.erase(pTexObj);
            m_TexObjToCustom.erase(pTexObj);
            LeaveCriticalSection(&m_CS);
            reverted++;
            continue;
        }
        IDirect3DTexture9* pCustom = GetOrCreateD3DTexture(*info);
        if (!pCustom)
            continue;
        if (InstallCustomTexture(pTexObj, pCustom, /*evictOriginalOnForeign=*/true))
            refreshed++;
    }
    Logger::LogInfo("Texture reload completed; %d installed slot(s) refreshed, %d slot(s) reverted to vanilla.", refreshed, reverted);

    if (showScreen)
        ProgressScreen::Update(90, 100, "Scanning partition tables and updating 3D models...", "Stage 5/5: Updating 3D models");

    // Also scan all partitions in the master table for any updated slots
    ScanAndReplaceAllMasterTextures();
    ModelEngine::Instance().Reload3DModels();

    if (showScreen)
    {
        ProgressScreen::Update(100, 100, "All textures reloaded successfully!", "Completed!");
        Sleep(60);
        ProgressScreen::Hide();
    }
}

void TextureEngine::FlushCache()
{
    Logger::LogInfo("Flushing compiled binary texture cache...");
    const bool showScreen = ShouldShowProgressScreen(m_IniPath);
    HWND hGame = FindWindowA("Crazy Taxi", "Crazy Taxi");
    if (!hGame) hGame = GetActiveWindow();

    if (showScreen)
    {
        ProgressScreen::Show(hGame, 100, "LYRA'S TEXTURE MOD - FLUSHING CACHE", "Flushing cache and recompiling all textures...");
        ProgressScreen::Update(5, 100, "Deleting binary cache files (.cache)...", "Deleting cache files");
    }

    std::string searchMask = m_CacheDir + "\\*.cache";
    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(searchMask.c_str(), &fd);
    if (hFind != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            {
                std::string fullPath = m_CacheDir + "\\" + fd.cFileName;
                DeleteFileA(fullPath.c_str());
            }
        } while (FindNextFileA(hFind, &fd));
        FindClose(hFind);
    }
    Logger::LogInfo("Binary cache flushed successfully.");
    ReloadTextures(/*isFlush=*/true);
}

void TextureEngine::ApplySmartFormatRules(
    const RuntimeTextureInfo& info,
    std::vector<uint8_t>& pixels,
    UINT width,
    UINT height,
    bool sourceHasAlpha
)
{
    std::string fn = info.filename;
    bool isRGB565 = (fn.find("RGB565") != std::string::npos);
    bool isARGB1555 = (fn.find("1555") != std::string::npos);
    bool isARGB4444 = (fn.find("4444") != std::string::npos || fn.find("DXT5") != std::string::npos || fn.find("PNG") != std::string::npos);

    // Rule 1: Strictly Opaque (RGB565)
    if (isRGB565)
    {
        for (size_t i = 0; i < pixels.size(); i += 4)
        {
            pixels[i + 3] = 0xFF; // Strictly 100% opaque
        }
        return;
    }

    // Helper: Magenta / Fuchsia color-keying, exactly RGB(255, 0, 255) (contract K3).
    // The former wide mask (R>=240 && G<=20 && B>=240) killed opaque vanilla texels in
    // 21/1323 texDC chunks; the narrowed mask matches INDEX.md 1.3 and only keys the
    // actual MS-Paint magenta fill color.
    auto applyColorKey = [&pixels]() {
        for (size_t i = 0; i < pixels.size(); i += 4)
        {
            uint8_t b = pixels[i];
            uint8_t g = pixels[i + 1];
            uint8_t r = pixels[i + 2];
            if (r == 255 && g == 0 && b == 255)
            {
                pixels[i + 3] = 0x00;
            }
        }
    };

    // Helper: Load pristine original texture and resample its alpha mask to width x height
    auto tryRestoreOriginalAlphaMask = [&](bool threshold1Bit) -> bool {
        std::string origPath = ResolveOriginalPath(info.filename);
        DWORD attr = GetFileAttributesA(origPath.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES)
            return false;

        int wlen = MultiByteToWideChar(CP_UTF8, 0, origPath.c_str(), -1, NULL, 0);
        std::wstring wpath(wlen, 0);
        MultiByteToWideChar(CP_UTF8, 0, origPath.c_str(), -1, &wpath[0], wlen);

        Bitmap origBmp(wpath.c_str());
        if (origBmp.GetLastStatus() != Ok)
            return false;

        Bitmap resampledOrig(width, height, PixelFormat32bppARGB);
        Graphics g(&resampledOrig);
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(PixelOffsetModeHighQuality);
        g.DrawImage(&origBmp, 0, 0, (INT)width, (INT)height);

        Rect rc(0, 0, width, height);
        BitmapData origData;
        if (resampledOrig.LockBits(&rc, ImageLockModeRead, PixelFormat32bppARGB, &origData) != Ok)
            return false;

        uint8_t* pSrc = (uint8_t*)origData.Scan0;
        bool hasAnyAlpha = false;

        for (UINT y = 0; y < height; y++)
        {
            uint8_t* pSrcRow = pSrc + (y * origData.Stride);
            for (UINT x = 0; x < width; x++)
            {
                uint8_t origAlpha = pSrcRow[x * 4 + 3];
                size_t dstIdx = (y * width + x) * 4;

                if (origAlpha < 255)
                    hasAnyAlpha = true;

                if (threshold1Bit)
                {
                    pixels[dstIdx + 3] = (origAlpha >= 128) ? 0xFF : 0x00;
                }
                else
                {
                    pixels[dstIdx + 3] = origAlpha;
                }
            }
        }

        resampledOrig.UnlockBits(&origData);

        if (hasAnyAlpha)
        {
            Logger::LogInfo(
                "[%s] Smart Alpha Guardian: Restored and upscaled alpha mask from pristine original (%s)!",
                info.filename.c_str(), threshold1Bit ? "1-Bit Threshold" : "Smooth Bicubic"
            );
        }

        return hasAnyAlpha;
    };

    // Rule 2: 1-Bit Punch-Through Alpha (ARGB1555)
    if (isARGB1555)
    {
        applyColorKey();

        if (sourceHasAlpha)
        {
            for (size_t i = 0; i < pixels.size(); i += 4)
            {
                uint8_t a = pixels[i + 3];
                pixels[i + 3] = (a >= 128) ? 0xFF : 0x00;
            }
        }
        else
        {
            if (!tryRestoreOriginalAlphaMask(true /* threshold 1-bit */))
            {
                for (size_t i = 0; i < pixels.size(); i += 4)
                {
                    pixels[i + 3] = 0xFF;
                }
            }
        }
        return;
    }

    // Rule 3: Smooth Alpha Gradients (ARGB4444, DXT5, PNG)
    if (isARGB4444)
    {
        applyColorKey();

        if (sourceHasAlpha)
        {
            return;
        }
        else
        {
            tryRestoreOriginalAlphaMask(false /* smooth bicubic alpha */);
        }
        return;
    }

    applyColorKey();
}

bool TextureEngine::CompileImageToCache(const RuntimeTextureInfo& info, const std::string& imagePath, const std::string& cachePath)
{
    // Fail-safe: Detect 0-byte corrupted texture files early
    WIN32_FILE_ATTRIBUTE_DATA fileAttr;
    bool isCorruptOrZero = false;
    if (GetFileAttributesExA(imagePath.c_str(), GetFileExInfoStandard, &fileAttr))
    {
        if (fileAttr.nFileSizeLow == 0 && fileAttr.nFileSizeHigh == 0)
        {
            isCorruptOrZero = true;
        }
    }

    std::string effectivePath = imagePath;
    if (isCorruptOrZero)
    {
        Logger::LogError("[FAIL-SAFE] 0-byte texture file detected for '%s' (%s)! Substituting Missing Texture fallback.",
            info.filename.c_str(), imagePath.c_str());
        effectivePath = m_ModDir + "\\missing_texture.png";
        EnsureMissingTextureExists(effectivePath);
    }

    int wlen = MultiByteToWideChar(CP_UTF8, 0, effectivePath.c_str(), -1, NULL, 0);
    std::wstring wpath(wlen, 0);
    MultiByteToWideChar(CP_UTF8, 0, effectivePath.c_str(), -1, &wpath[0], wlen);

    Bitmap* bitmap = new Bitmap(wpath.c_str());
    if (!bitmap || bitmap->GetLastStatus() != Ok)
    {
        Logger::LogError("[FAIL-SAFE] Failed to load image '%s' (Status: %d)! Substituting Missing Texture fallback.",
            effectivePath.c_str(), bitmap ? bitmap->GetLastStatus() : -1);
        if (bitmap) delete bitmap;

        effectivePath = m_ModDir + "\\missing_texture.png";
        EnsureMissingTextureExists(effectivePath);
        int wlenM = MultiByteToWideChar(CP_UTF8, 0, effectivePath.c_str(), -1, NULL, 0);
        std::wstring wpathM(wlenM, 0);
        MultiByteToWideChar(CP_UTF8, 0, effectivePath.c_str(), -1, &wpathM[0], wlenM);
        bitmap = new Bitmap(wpathM.c_str());
        if (!bitmap || bitmap->GetLastStatus() != Ok)
        {
            if (bitmap) delete bitmap;
            return false;
        }
    }

    UINT srcW = bitmap->GetWidth();
    UINT srcH = bitmap->GetHeight();
    UINT origW = info.record.origWidth;
    UINT origH = info.record.origHeight;

    if (origW == 0) origW = srcW;
    if (origH == 0) origH = srcH;

    UINT minDim = 8;
    UINT targetW = srcW;
    UINT targetH = srcH;

    // For Sega 3D/VRAM container textures, snap to Power-of-Two (minDim = 8) to match Sega PVR VRAM hardware layout.
    // For 2D UI / JPK / standalone DDS textures, preserve the EXACT source dimensions so aspect ratio is 100% bit-exact
    // and D3D9 non-power-of-two UI surfaces do not get distorted or rejected by the aspect ratio guardian!
    if (IsSegaContainerRecord(info))
    {
        targetW = NextPowerOfTwo(srcW, minDim);
        targetH = NextPowerOfTwo(srcH, minDim);

        double origAspect = (double)origW / (double)origH;
        double newAspect = (double)targetW / (double)targetH;

        if (m_Config.ForceHDAspectAlignment && fabs(newAspect - origAspect) > 0.04)
        {
            UINT maxDim = max(targetW, targetH);
            if (origW >= origH)
            {
                targetW = maxDim;
                targetH = NextPowerOfTwo((UINT)((double)maxDim / origAspect + 0.5), minDim);
            }
            else
            {
                targetH = maxDim;
                targetW = NextPowerOfTwo((UINT)((double)maxDim * origAspect + 0.5), minDim);
            }

            if (targetW > 2048) targetW = 2048;
            if (targetH > 2048) targetH = 2048;
            if (targetW < minDim) targetW = minDim;
            if (targetH < minDim) targetH = minDim;

            Logger::LogInfo(
                "[%s] HD Alignment Guardian: Input %ux%u (AR: %.2f) resampled to %ux%u (AR: %.2f) to preserve 3D model UV alignment!",
                info.filename.c_str(), srcW, srcH, (double)srcW / (double)srcH, targetW, targetH, origAspect
            );
        }
        else if (!IsPowerOfTwo(srcW) || !IsPowerOfTwo(srcH))
        {
            Logger::LogInfo(
                "[%s] HD Dimension Snapper: Snapped non-Power-of-Two %ux%u to valid D3D9 dimensions %ux%u",
                info.filename.c_str(), srcW, srcH, targetW, targetH
            );
        }
    }

    Bitmap* pProcessedBmp = bitmap;
    Bitmap* pResampledBmp = nullptr;
    if (targetW != srcW || targetH != srcH)
    {
        pResampledBmp = new Bitmap(targetW, targetH, PixelFormat32bppARGB);
        Graphics g(pResampledBmp);
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(PixelOffsetModeHighQuality);
        g.SetSmoothingMode(SmoothingModeHighQuality);
        g.DrawImage(bitmap, 0, 0, (INT)targetW, (INT)targetH);
        pProcessedBmp = pResampledBmp;
    }

    Rect rect(0, 0, targetW, targetH);
    BitmapData bmpData;
    Status lockStatus = pProcessedBmp->LockBits(&rect, ImageLockModeRead, PixelFormat32bppARGB, &bmpData);
    if (lockStatus != Ok)
    {
        Logger::LogError("[%s] GDI+ LockBits failed with status %d!", info.filename.c_str(), lockStatus);
        if (pResampledBmp) delete pResampledBmp;
        delete bitmap;
        return false;
    }

    size_t dataSize = targetW * targetH * 4;
    std::vector<uint8_t> pixelBuffer(dataSize);

    uint8_t* pSrc = (uint8_t*)bmpData.Scan0;
    bool sourceHasAlpha = false;

    for (UINT y = 0; y < targetH; y++)
    {
        uint8_t* pSrcRow = pSrc + (y * bmpData.Stride);
        uint8_t* pDstRow = pixelBuffer.data() + (y * targetW * 4);
        memcpy(pDstRow, pSrcRow, targetW * 4);

        for (UINT x = 0; x < targetW; x++)
        {
            if (pSrcRow[x * 4 + 3] < 250)
            {
                sourceHasAlpha = true;
            }
        }
    }

    pProcessedBmp->UnlockBits(&bmpData);
    if (pResampledBmp) delete pResampledBmp;
    delete bitmap;

    // Apply Smart Format & Alpha Channel Rules
    ApplySmartFormatRules(info, pixelBuffer, targetW, targetH, sourceHasAlpha);

    // In-game orientation contract:
    // Sega container textures (texDC*, spr*) and 2D_UI DDS textures (Jdds / standalone DDS)
    // are stored upright on disk for modders, so we invert rows vertically back to the scanline
    // order expected by the original engine and Direct3D 9.
    if (IsSegaContainerRecord(info) || IsDdsUiRecord(info))
    {
        std::vector<uint8_t> flipped(dataSize);
        size_t stride = targetW * 4;
        for (UINT y = 0; y < targetH; y++)
        {
            memcpy(flipped.data() + (size_t)y * stride,
                   pixelBuffer.data() + (size_t)(targetH - 1 - y) * stride,
                   stride);
        }
        pixelBuffer = std::move(flipped);
    }

    CacheHeader header;
    memcpy(header.magic, "LTMC", 4);
    header.version = LTM_CACHE_VERSION;
    header.width = targetW;
    header.height = targetH;
    header.d3dFormat = D3DFMT_A8R8G8B8;
    header.dataSize = (uint32_t)dataSize;
    header.sourcePngTime = info.sourceImageLastWriteTime;

    // H5: snapshot the exact source image this cache was compiled from (size +
    // CRC32). Stored in the trailer below and verified by both freshness checks
    // (PrecompileModifiedTextures and LoadOrCreateCache) so a replaced source with a
    // stale mtime cannot keep serving old compiled pixels.
    CacheTrailer trailer;
    ZeroMemory(&trailer, sizeof(trailer));
    memcpy(trailer.magic, "LTMT", 4);
    trailer.stamp = LTM_CACHE_TRAILER_STAMP;
    {
        uint32_t srcSize = 0;
        uint32_t srcCrc = HashFileCrc32(imagePath, srcSize);
        trailer.sourcePngSize = srcSize;
        trailer.sourcePngCrc = srcCrc; // 0 = unknown (read failed) => size-only compare
    }

    std::ofstream cacheFile(cachePath, std::ios::binary);
    if (!cacheFile.is_open())
    {
        Logger::LogError("[%s] Failed to write binary cache file: %s", info.filename.c_str(), cachePath.c_str());
        return false;
    }

    cacheFile.write((const char*)&header, sizeof(header));
    cacheFile.write((const char*)pixelBuffer.data(), dataSize);
    cacheFile.write((const char*)&trailer, sizeof(trailer)); // H5: trailing stamp
    cacheFile.close();

    Logger::LogInfo(
        "[%s] Smart Cache compiled successfully! %ux%u (D3DFMT_A8R8G8B8, %zu bytes) -> %s",
        info.filename.c_str(), targetW, targetH, dataSize, cachePath.c_str()
    );

    return true;
}


bool TextureEngine::LoadOrCreateCache(const RuntimeTextureInfo& info, std::vector<uint8_t>& outPixels, uint32_t& outWidth, uint32_t& outHeight)
{
    std::ifstream cacheFile(info.fullCachePath, std::ios::binary);
    if (cacheFile.is_open())
    {
        CacheHeader header;
        CacheTrailerInfo trailer;
        // H5: the cache is only usable when it carries the current format (header
        // magic/version AND the trailing stamp); anything older is stale and gets
        // recompiled below ("alte cache/*.cache werden beim Start erkannt").
        if (ReadCacheHeaderAndTrailer(cacheFile, header, trailer) && trailer.valid)
        {
            WIN32_FILE_ATTRIBUTE_DATA fileData;
            if (GetFileAttributesExA(info.fullSourceImagePath.c_str(), GetFileExInfoStandard, &fileData))
            {
                uint64_t curTime = ((uint64_t)fileData.ftLastWriteTime.dwHighDateTime << 32) | fileData.ftLastWriteTime.dwLowDateTime;
                uint32_t curSize = fileData.nFileSizeLow;
                // Fast path: if timestamp and file size match trailer snapshot, cache is 100% fresh!
                // Eliminates redundant synchronous PNG disk reads & CRC32 calculations during gameplay.
                bool cacheFresh = (curTime <= header.sourcePngTime && curSize == trailer.sourcePngSize);
                if (!cacheFresh && SourceMatchesCacheSnapshot(trailer, info.fullSourceImagePath))
                {
                    cacheFresh = true;
                }

                if (cacheFresh)
                {
                    // BLOCKER (review round 2): ReadCacheHeaderAndTrailer leaves the
                    // stream at EOF — it seeks PAST the pixel block to read the
                    // trailer. Rewind to the pixel data and demand the FULL read,
                    // otherwise this hit path returned zero-initialized pixels of the
                    // correct size (the size guard in GetOrCreateD3DTexture passed and
                    // a black/transparent texture was uploaded). A short/corrupt read
                    // falls through to the recompile below instead.
                    outWidth = header.width;
                    outHeight = header.height;
                    outPixels.resize(header.dataSize);
                    cacheFile.clear();
                    cacheFile.seekg((std::streamoff)sizeof(CacheHeader), std::ios::beg);
                    cacheFile.read((char*)outPixels.data(), header.dataSize);
                    if (cacheFile.gcount() == (std::streamsize)header.dataSize)
                    {
                        cacheFile.close();
                        return true;
                    }
                    outPixels.clear();
                }
            }
        }
        cacheFile.close();
    }

    if (!CompileImageToCache(info, info.fullSourceImagePath, info.fullCachePath))
    {
        return false;
    }

    std::ifstream freshCache(info.fullCachePath, std::ios::binary);
    if (freshCache.is_open())
    {
        CacheHeader header;
        freshCache.read((char*)&header, sizeof(header));
        outWidth = header.width;
        outHeight = header.height;
        outPixels.resize(header.dataSize);
        freshCache.read((char*)outPixels.data(), header.dataSize);
        freshCache.close();
        return true;
    }

    return false;
}

const RuntimeTextureInfo* TextureEngine::FindByCRC_Locked(uint32_t crc, uint32_t wantW, uint32_t wantH) const
{
    auto it = m_CrcMap.find(crc);
    if (it == m_CrcMap.end() || it->second >= m_Textures.size())
        return nullptr;

    // H1: collision-aware resolution. All members of a CRC group share byte-identical
    // payloads (the CRC is built over the payload bytes), so the D3DX hook cannot
    // distinguish them by content or by pSrcInfo decode dimensions. Resolution rule:
    // 1. When the caller passes plausible game-requested Width/Height (not
    //    D3DX_DEFAULT/0), prefer members whose vanilla record dims match: the request
    //    names the SLOT, and cross-container group members (e.g. a texDC hair texture
    //    vs. a 2D_UI character portrait) usually differ in dims. Without this filter
    //    the "prefer isModified" step below let a false-positive-modified foreign
    //    member paint its art into this slot (character-select showed a hair texture
    //    instead of the 2D character).
    // 2. Among the surviving candidates prefer the isModified member (last one wins if
    //    several are modified, keeping the decision deterministic), otherwise keep the
    //    legacy last-entry mapping. This makes edits to previously shadowed members
    //    visible and fixes the wrong filename in the injection log; per-slot differing
    //    art within one dims-matching group stays impossible at this hook by
    //    construction.
    auto groupIt = s_CrcCollisionGroups.find(crc);
    if (groupIt != s_CrcCollisionGroups.end())
    {
        // D3DX_DEFAULT is 0xFFFFFFFF and "use the source dims" is 0 — neither carries
        // slot information, so only plausible dims may filter.
        const bool plausibleDims = wantW > 0 && wantH > 0 && wantW <= 4096 && wantH <= 4096;
        std::vector<uint32_t> candidates;
        bool dimsFilterApplied = false;
        if (plausibleDims)
        {
            for (uint32_t memberIdx : groupIt->second)
            {
                if (memberIdx < m_Textures.size() &&
                    m_Textures[memberIdx].record.origWidth == wantW &&
                    m_Textures[memberIdx].record.origHeight == wantH)
                {
                    candidates.push_back(memberIdx);
                }
            }
            dimsFilterApplied = !candidates.empty();
        }
        if (!dimsFilterApplied)
        {
            // No member matches the request (default dims, header lies): fall back to
            // the whole group so the legacy behavior survives for slots whose manifest
            // dims cannot describe the request.
            candidates = groupIt->second;
        }

        const RuntimeTextureInfo* chosen = nullptr;
        for (uint32_t memberIdx : candidates)
        {
            if (memberIdx < m_Textures.size() && m_Textures[memberIdx].isModified)
                chosen = &m_Textures[memberIdx];
        }
        if (!chosen)
        {
            if (dimsFilterApplied)
            {
                // All dims-matching members are untouched: deterministic last match,
                // NOT the map's last-entry (which may be a dims-mismatched member).
                chosen = &m_Textures[candidates.back()];
            }
            else
            {
                chosen = &m_Textures[it->second];
            }
        }
        return chosen;
    }

    return &m_Textures[it->second];
}

const RuntimeTextureInfo* TextureEngine::FindByArchiveAndGBIX_Locked(uint32_t archiveId, uint32_t gbix) const
{
    // K4: GBIX lookups are archive-scoped. The old archive-blind FindByGBIX(gbix)
    // returned an arbitrary entry on gbix collisions (210 colliding values, 829
    // cross-archive pairs with identical dimensions).
    uint64_t key = ((uint64_t)archiveId << 32) | gbix;
    auto it = m_ArchiveGbixMap.find(key);
    if (it != m_ArchiveGbixMap.end() && it->second < m_Textures.size())
    {
        return &m_Textures[it->second];
    }
    return nullptr;
}

const RuntimeTextureInfo* TextureEngine::FindByOffset_Locked(uint32_t archiveId, uint32_t offset) const
{
    uint64_t key = ((uint64_t)archiveId << 32) | offset;
    auto it = m_OffsetMap.find(key);
    if (it != m_OffsetMap.end() && it->second < m_Textures.size())
    {
        return &m_Textures[it->second];
    }
    return nullptr;
}

const RuntimeTextureInfo* TextureEngine::FindByArchiveAndIndex_Locked(uint32_t archiveId, uint32_t chunkIndex) const
{
    uint64_t key = ((uint64_t)archiveId << 32) | chunkIndex;
    auto it = m_ArchiveIndexMap.find(key);
    if (it != m_ArchiveIndexMap.end() && it->second < m_Textures.size())
    {
        return &m_Textures[it->second];
    }
    return nullptr;
}

const RuntimeTextureInfo* TextureEngine::FindByCRC(uint32_t crc, uint32_t wantW, uint32_t wantH)
{
    EnterCriticalSection(&m_CS);
    const auto* res = FindByCRC_Locked(crc, wantW, wantH);
    LeaveCriticalSection(&m_CS);
    return res;
}

// K24: cross-container GBIX guard. Sega containers legitimately reuse GBIX values
// across archives (sprGAME#0156 portrait and texDC0#0254 pants both carry
// GBIX 0x000F3100), and the game binds them through one live texture object per
// GBIX — a texDC0-side custom can surface as the other container's art. Called
// only on the install path for isModified records, so the linear scan is fine.
bool TextureEngine::IsGbixContested(uint32_t archiveId, uint32_t gbix)
{
    // Container-scoped lookups prevent cross-container misattribution; GBIX sharing
    // across different archives is normal engine design and does not cause collisions.
    return false;
}

const RuntimeTextureInfo* TextureEngine::FindByArchiveAndGBIX(uint32_t archiveId, uint32_t gbix)
{
    EnterCriticalSection(&m_CS);
    const auto* res = FindByArchiveAndGBIX_Locked(archiveId, gbix);
    LeaveCriticalSection(&m_CS);
    return res;
}

const RuntimeTextureInfo* TextureEngine::FindByOffset(uint32_t archiveId, uint32_t offset)
{
    EnterCriticalSection(&m_CS);
    const auto* res = FindByOffset_Locked(archiveId, offset);
    LeaveCriticalSection(&m_CS);
    return res;
}

const RuntimeTextureInfo* TextureEngine::FindByArchiveAndIndex(uint32_t archiveId, uint32_t chunkIndex)
{
    EnterCriticalSection(&m_CS);
    const auto* res = FindByArchiveAndIndex_Locked(archiveId, chunkIndex);
    LeaveCriticalSection(&m_CS);
    return res;
}

const RuntimeTextureInfo* TextureEngine::FindByArchiveAndOffset(uint32_t archiveId, uint32_t offset)
{
    return FindByOffset(archiveId, offset);
}

IDirect3DDevice9* TextureEngine::GetD3DDevice()
{
    if (m_pDevice)
        return m_pDevice;

    __try
    {
        uintptr_t pSegaRenderer = *(uintptr_t*)0x01B7AC00;
        if (pSegaRenderer)
        {
            IDirect3DDevice9* pDev = *(IDirect3DDevice9**)(pSegaRenderer + 0x18);
            if (pDev)
            {
                m_pDevice = pDev;
                return pDev;
            }
        }
        uintptr_t pContainer = *(uintptr_t*)0x01B7AB04;
        if (pContainer)
        {
            IDirect3DDevice9* pDev = *(IDirect3DDevice9**)(pContainer + 8);
            if (pDev)
            {
                m_pDevice = pDev;
                return pDev;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}

    return nullptr;
}

IDirect3DTexture9* TextureEngine::GetOrCreateD3DTexture(const RuntimeTextureInfo& info)
{
    EnterCriticalSection(&m_CS);
    uint64_t cacheKey = info.record.chunkIndex | ((uint64_t)info.record.archiveId << 32);
    auto it = m_D3DTextureCache.find(cacheKey);
    if (it != m_D3DTextureCache.end() && it->second != nullptr)
    {
        IDirect3DTexture9* pExisting = it->second;
        LeaveCriticalSection(&m_CS);
        return pExisting;
    }

    std::vector<uint8_t> pixels;
    uint32_t texW = 0, texH = 0;
    if (!LoadOrCreateCache(info, pixels, texW, texH))
    {
        IDirect3DTexture9* pMissing = GetOrCreateMissingTexture();
        LeaveCriticalSection(&m_CS);
        return pMissing;
    }

    IDirect3DDevice9* pDevice = GetD3DDevice();
    if (!pDevice)
    {
        Logger::LogError("[%s] Cannot create custom texture: IDirect3DDevice9 is null!", info.filename.c_str());
        LeaveCriticalSection(&m_CS);
        return nullptr;
    }

    IDirect3DTexture9* pNewTex = nullptr;
    DWORD usage = 0;
    UINT mipLevels = 1;

    IDirect3D9* pD3D = nullptr;
    pDevice->GetDirect3D(&pD3D);
    D3DCAPS9 caps;
    ZeroMemory(&caps, sizeof(caps));
    pDevice->GetDeviceCaps(&caps);

    bool autogenSupported = (caps.Caps2 & D3DCAPS2_CANAUTOGENMIPMAP) != 0;
    if (pD3D)
    {
        D3DDISPLAYMODE mode;
        pDevice->GetDisplayMode(0, &mode);
        if (FAILED(pD3D->CheckDeviceFormat(caps.AdapterOrdinal, caps.DeviceType, mode.Format,
            D3DUSAGE_AUTOGENMIPMAP, D3DRTYPE_TEXTURE, D3DFMT_A8R8G8B8)))
        {
            autogenSupported = false;
        }
        pD3D->Release();
    }

    bool wantMipmaps = m_Config.EnableMipmaps && (texW > 64 || texH > 64);
    if (wantMipmaps)
    {
        if (autogenSupported)
        {
            usage = D3DUSAGE_AUTOGENMIPMAP;
            mipLevels = 0; // Hardware autogen
        }
        else
        {
            usage = 0;
            mipLevels = 0; // Allocate complete mip chain in MANAGED pool for CPU box-filter
        }
    }

    HRESULT hr = pDevice->CreateTexture(
        texW, texH, mipLevels, usage, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &pNewTex, NULL
    );

    // If hardware autogen failed, fallback to software mip chain in managed pool
    if (FAILED(hr) && mipLevels == 0 && usage != 0)
    {
        usage = 0;
        mipLevels = 0;
        hr = pDevice->CreateTexture(
            texW, texH, mipLevels, usage, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &pNewTex, NULL
        );
    }

    if (FAILED(hr))
    {
        usage = 0;
        mipLevels = 1;
        hr = pDevice->CreateTexture(
            texW, texH, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &pNewTex, NULL
        );
    }

    if (FAILED(hr))
    {
        hr = pDevice->CreateTexture(
            texW, texH, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &pNewTex, NULL
        );
    }

    if (FAILED(hr) || !pNewTex)
    {
        Logger::LogError("[%s] IDirect3DDevice9::CreateTexture failed! HRESULT: 0x%08X", info.filename.c_str(), hr);
        IDirect3DTexture9* pMissing = GetOrCreateMissingTexture();
        LeaveCriticalSection(&m_CS);
        return pMissing;
    }

    // Corrupt/short cache must not cause an out-of-bounds read in the flip/copy below.
    if (pixels.size() < (size_t)texW * (size_t)texH * 4)
    {
        Logger::LogError("[%s] Cached pixel buffer too small (%zu bytes for %ux%u)! Rejecting texture, substituting Missing Texture.",
            info.filename.c_str(), pixels.size(), texW, texH);
        pNewTex->Release();
        IDirect3DTexture9* pMissing = GetOrCreateMissingTexture();
        LeaveCriticalSection(&m_CS);
        return pMissing;
    }

    // K2 (round-trip contract step 7): exactly ONE side flips — as a PAIRED unit.
    // Current SOURCE pairing (both sides in this codebase):
    //   AssetExtractor.cpp flips Sega containers AND DDS-UI (Jdds chunks and
    //   standalone DDS) to UPRIGHT on disk, and this compile path flips those same
    //   record classes back (IsSegaContainerRecord || IsDdsUiRecord below) before
    //   upload — double flip = net game orientation, human-readable PNGs.
    //   Jpng payloads are stored upright and match neither flip trigger.
    // WARNING: the installed PNG GENERATION may differ from this source (an older
    // install can still carry RAW/unflipped PNGs). Running this flip against RAW
    // PNGs mirrors Sega + DDS-UI textures vertically (= "wrong UI elements").
    //   upright PNGs (current extractor) -> compile flip here ACTIVE
    //   RAW PNGs (old generation)        -> compile flip here must be removed
    // PAIRING INVARIANT (breaking it caused three 100%-vertical-mirror blockers):
    // change extractor AND engine ONLY together, never exactly one side alone; a
    // flip relocation must stay BEFORE the mip pyramid build below (mips derive from
    // the flipped buffer) and requires a cache invalidation — bump
    // LTM_CACHE_TRAILER_STAMP (H5) so old caches cannot survive the pairing change.
    D3DLOCKED_RECT lr;
    hr = pNewTex->LockRect(0, &lr, NULL, 0);
    if (SUCCEEDED(hr) && lr.pBits)
    {
        for (UINT y = 0; y < texH; y++)
        {
            uint8_t* pDst = (uint8_t*)lr.pBits + (y * lr.Pitch);
            const uint8_t* pSrc = pixels.data() + (y * texW * 4);
            memcpy(pDst, pSrc, texW * 4);
        }
        pNewTex->UnlockRect(0);

        // Generate complete mipmap pyramid
        if (wantMipmaps)
        {
            if (usage & D3DUSAGE_AUTOGENMIPMAP)
            {
                pNewTex->GenerateMipSubLevels();
            }
            else
            {
                // Box-filtered mipmap pyramid downsampling on CPU
                DWORD levelCount = pNewTex->GetLevelCount();
                if (levelCount > 1)
                {
                    std::vector<uint8_t> prevBuffer = pixels;
                    UINT prevW = texW;
                    UINT prevH = texH;

                    for (DWORD lvl = 1; lvl < levelCount; lvl++)
                    {
                        UINT curW = max(1u, prevW / 2);
                        UINT curH = max(1u, prevH / 2);
                        std::vector<uint8_t> curBuffer(curW * curH * 4);

                        for (UINT y = 0; y < curH; y++)
                        {
                            for (UINT x = 0; x < curW; x++)
                            {
                                UINT srcX0 = min(x * 2, prevW - 1);
                                UINT srcX1 = min(x * 2 + 1, prevW - 1);
                                UINT srcY0 = min(y * 2, prevH - 1);
                                UINT srcY1 = min(y * 2 + 1, prevH - 1);

                                const uint8_t* p00 = prevBuffer.data() + (srcY0 * prevW + srcX0) * 4;
                                const uint8_t* p10 = prevBuffer.data() + (srcY0 * prevW + srcX1) * 4;
                                const uint8_t* p01 = prevBuffer.data() + (srcY1 * prevW + srcX0) * 4;
                                const uint8_t* p11 = prevBuffer.data() + (srcY1 * prevW + srcX1) * 4;

                                uint8_t* pDst = curBuffer.data() + (y * curW + x) * 4;
                                pDst[0] = (uint8_t)(((UINT)p00[0] + p10[0] + p01[0] + p11[0] + 2) / 4); // B
                                pDst[1] = (uint8_t)(((UINT)p00[1] + p10[1] + p01[1] + p11[1] + 2) / 4); // G
                                pDst[2] = (uint8_t)(((UINT)p00[2] + p10[2] + p01[2] + p11[2] + 2) / 4); // R
                                pDst[3] = (uint8_t)(((UINT)p00[3] + p10[3] + p01[3] + p11[3] + 2) / 4); // A
                            }
                        }

                        D3DLOCKED_RECT lvlRect;
                        if (SUCCEEDED(pNewTex->LockRect(lvl, &lvlRect, NULL, 0)) && lvlRect.pBits)
                        {
                            for (UINT y = 0; y < curH; y++)
                            {
                                uint8_t* pDstRow = (uint8_t*)lvlRect.pBits + (y * lvlRect.Pitch);
                                const uint8_t* pSrcRow = curBuffer.data() + (y * curW * 4);
                                memcpy(pDstRow, pSrcRow, curW * 4);
                            }
                            pNewTex->UnlockRect(lvl);
                        }

                        prevBuffer = std::move(curBuffer);
                        prevW = curW;
                        prevH = curH;
                    }
                }
            }
        }

        m_D3DTextureCache[cacheKey] = pNewTex;
        Logger::LogInfo(
            "[%s] In-game 3D custom texture created (%ux%u, %s mipmaps)",
            info.filename.c_str(), texW, texH,
            (usage & D3DUSAGE_AUTOGENMIPMAP) ? "hardware autogen" : (pNewTex->GetLevelCount() > 1 ? "software box-filter" : "no")
        );
        LeaveCriticalSection(&m_CS);
        return pNewTex;
    }

    pNewTex->Release();
    LeaveCriticalSection(&m_CS);
    return nullptr;
}

void TextureEngine::PreloadAllModifiedD3DTextures()
{
    IDirect3DDevice9* pDevice = GetD3DDevice();
    if (!pDevice)
        return;

    std::vector<RuntimeTextureInfo> toPreload;
    EnterCriticalSection(&m_CS);
    for (const auto& tex : m_Textures)
    {
        if (tex.isModified)
        {
            uint64_t cacheKey = tex.record.chunkIndex | ((uint64_t)tex.record.archiveId << 32);
            auto it = m_D3DTextureCache.find(cacheKey);
            if (it == m_D3DTextureCache.end() || it->second == nullptr)
            {
                toPreload.push_back(tex);
            }
        }
    }
    LeaveCriticalSection(&m_CS);

    if (toPreload.empty())
        return;

    Logger::LogInfo("Preloading %zu modified Direct3D 9 textures into memory cache...", toPreload.size());
    for (const auto& tex : toPreload)
    {
        GetOrCreateD3DTexture(tex);
    }
    Logger::LogInfo("Preloaded all modified textures. Chunk streaming will experience zero disk stalls.");
}

// ---------------------------------------------------------------------------------
// Hook 1: D3DXCreateTextureFromFileInMemoryEx (Menu UI & JPK packages)
// ---------------------------------------------------------------------------------
HRESULT TextureEngine::HandleD3DXCreateTextureFromFileInMemoryEx(
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
    if (pDevice)
    {
        m_pDevice = pDevice;
    }

    // H3 (repro note, deliberately no behavior change): this is one of FOUR runtime
    // hooks gated on m_Config.Enabled (D3DX here, HandlePostLoadModelTexture,
    // HandlePostLoadStageTexture, HandlePreSetTexture/master scan). With Enabled=false
    // the mod injects nothing, while Initialize still runs ScanModifiedTextures and
    // PrecompileModifiedTextures — cache-compile log lines in a disabled session are
    // expected, not a contradiction. A repro needs Enabled=true set BEFORE process
    // start (SetEnabled persists to the INI immediately) and a ReloadTextures (F9) or
    // FlushTextureCache for textures that were already loaded while disabled.
    if (!m_Initialized || !m_Config.Enabled || m_VanillaRestored || !pSrcData || SrcDataSize == 0 || !pDevice || !ppTexture)
        return E_FAIL;

    uint32_t crc = CalculateCRC32(pSrcData, SrcDataSize);
    // Pass the game-requested dims: inside a CRC collision group they name the slot
    // (see FindByCRC_Locked — fixes foreign member art, e.g. hair in char-select).
    const RuntimeTextureInfo* info = FindByCRC(crc, Width, Height);
    if (!info)
    {
        return E_FAIL;
    }

    // K8: CRC match only proves the game is loading the vanilla payload — it does NOT
    // prove the user modified that texture. Without this gate, unmodified UI/JPK
    // textures were re-created as 32-bit PoT-snapped replacements (game original
    // touched). Returning E_FAIL lets the hook glue fall through to the original
    // D3DX call, leaving the game original completely untouched (true no-op).
    if (!info->isModified)
    {
        return E_FAIL;
    }

    IDirect3DTexture9* pNewTex = GetOrCreateD3DTexture(*info);
    if (pNewTex)
    {
        // Standalone boot logos, splash screens, and loading screens (e.g. Logos subfolder,
        // Loading_Screens subfolder, or NewTexture standalone UI entries) are rendered on
        // full-screen quads with normalized [0..1] UVs. They do not share an atlas, so
        // custom resolutions (such as 1024x512 or 1024x1024) must NOT be blocked by
        // the atlas-oriented 2% aspect ratio guard.
        D3DSURFACE_DESC injectDesc = {};
        bool hasDesc = SUCCEEDED(pNewTex->GetLevelDesc(0, &injectDesc));

        bool isStandaloneLogo = (info->filename.find("NewTexture") != std::string::npos ||
                                 info->fullSourceImagePath.find("Logos") != std::string::npos ||
                                 info->fullSourceImagePath.find("Loading_Screens") != std::string::npos ||
                                 info->filename.find("2D_UI") != std::string::npos ||
                                 info->fullSourceImagePath.find("2D_UI") != std::string::npos);

        if (!isStandaloneLogo && hasDesc &&
            info->record.origWidth > 0 && info->record.origHeight > 0 &&
            injectDesc.Width > 0 && injectDesc.Height > 0)
        {
            double injectAspect = (double)injectDesc.Width / (double)injectDesc.Height;
            double recordAspect = (double)info->record.origWidth / (double)info->record.origHeight;
            if (fabs(injectAspect - recordAspect) > 0.02 * recordAspect)
            {
                Logger::LogWarning(
                    "[%s] Injection %ux%u deviates in aspect from vanilla %ux%u — declining, vanilla texture stays active.",
                    info->filename.c_str(), injectDesc.Width, injectDesc.Height,
                    info->record.origWidth, info->record.origHeight);
                return E_FAIL;
            }
        }

        pNewTex->AddRef();
        *ppTexture = pNewTex;
        if (hasDesc)
        {
            Logger::LogInfo("[%s] Injected custom high-res UI texture (%ux%u) into DirectX successfully!",
                info->filename.c_str(), injectDesc.Width, injectDesc.Height);
        }
        else
        {
            Logger::LogInfo("[%s] Injected custom high-res UI texture into DirectX successfully!", info->filename.c_str());
        }
        return D3D_OK;
    }

    return E_FAIL;
}

bool TextureEngine::HandleSegaCreateTexture(
    void* pRenderer,
    void* pTexStruct,
    UINT width,
    UINT height,
    DWORD formatIndex,
    int* pOutFlag,
    DWORD* pOutFlags
)
{
    return false;
}

// ---------------------------------------------------------------------------------
// Hook 2: Model Texture Loading at 0x00492230
// ---------------------------------------------------------------------------------
void TextureEngine::HandlePostLoadModelTexture(void* tablePtr, int tableIndex, uint32_t baseIdx)
{
    if (!m_Initialized || !m_Config.Enabled || m_VanillaRestored || !tablePtr || tableIndex < 0)
        return;

    // Fast-path: if no modified textures exist, return immediately in 1ns!
    if (m_ModifiedArchiveIndexSet.empty() && m_ModifiedOffsetSet.empty())
        return;

    char* pEntry = (char*)tablePtr + (tableIndex * 16);
    uint32_t pData = 0;
    __try {
        pData = *(uint32_t*)(pEntry + 8);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }

    if (pData < 0x0CB40000 || pData > 0x0F000000)
        return;

    uint32_t relOffset = pData - 0x0CB40000;

    uint32_t archiveId = GetArchiveIdFromTablePtr(tablePtr);
    if (archiveId == 0)
    {
        archiveId = 0x5436974C; // texDC0 default
    }

    uint64_t keyIndex = ((uint64_t)archiveId << 32) | (uint32_t)tableIndex;
    uint64_t keyOffset = ((uint64_t)archiveId << 32) | relOffset;

    // Fast O(1) filter: if neither index, offset, nor GBIX matches any modified texture, bail out instantly in <5ns!
    if (m_ModifiedArchiveIndexSet.find(keyIndex) == m_ModifiedArchiveIndexSet.end() &&
        m_ModifiedOffsetSet.find(keyOffset) == m_ModifiedOffsetSet.end() &&
        m_ModifiedGbixSet.find(keyOffset) == m_ModifiedGbixSet.end())
    {
        return;
    }

    const RuntimeTextureInfo* info = FindByArchiveAndIndex(archiveId, (uint32_t)tableIndex);
    if (!info)
    {
        info = FindByArchiveAndOffset(archiveId, relOffset);
    }
    if (!info)
    {
        // K4: archive-scoped GBIX fallback — never hit a foreign archive's entry.
        info = FindByArchiveAndGBIX(archiveId, relOffset);
    }

    if (!info || !info->isModified)
        return;

    // Resolve baseIdx:
    // 1. If passed baseIdx is 0, read from game memory ct_MasterTextureBaseIdx
    if (baseIdx == 0)
    {
        __try {
            baseIdx = *(uint32_t*)0x01B7A6E0;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    // 2. Strong table-specific fallback (only if baseIdx was still 0):
    if (baseIdx == 0)
    {
        if (archiveId == 0x2331A7DA || archiveId == 0xBA38F660 || archiveId == 0xCD3FC6F6)
        {
            baseIdx = 2000; // Enforce 2000 for stage meshes (including Cable Car / Tram in texDC1)
        }
        else if (archiveId == 0x5436974C)
        {
            baseIdx = 1500; // Enforce 1500 for traffic cars / taxi chassis / pedestrians
        }
        else
        {
            baseIdx = GetDefaultBaseIdxForTable(tablePtr);
        }
    }

    uint32_t masterId = (uint32_t)tableIndex + baseIdx;
    if (masterId >= 16384)
        return;

    char* masterEntry = (char*)0x01B09938 + (masterId * 24);
    void* pTexObj = nullptr;
    float fW = 0.0f, fH = 0.0f;
    __try {
        pTexObj = *(void**)masterEntry;
        fW = *(float*)(masterEntry + 0x08);
        fH = *(float*)(masterEntry + 0x0C);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}

    if (!pTexObj)
        return;

    // Validate that pTexObj is a legitimate ct_TextureObject with vtable 0x0059EB10.
    // Crazy Taxi master table 0x01B09938 can contain non-texture pointers (e.g. flat model strips,
    // shadow volume buffers) if indices overlap. Overwriting those crashes submit_flat_model_strips (0x00409927)!
    __try {
        if (*(uintptr_t*)pTexObj != 0x0059EB10)
        {
            return;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }

    uint32_t origW = (uint32_t)(fW + 0.5f);
    uint32_t origH = (uint32_t)(fH + 0.5f);
    if (origW > 0 && origH > 0 && info->record.origWidth > 0 && info->record.origHeight > 0)
    {
        if (info->record.origWidth != origW || info->record.origHeight != origH)
        {
            Logger::LogWarning("[MODEL_HOOK] Dimension mismatch on masterId %u: expected %ux%u, got %ux%u (%s). Skipping replacement.",
                masterId, origW, origH, info->record.origWidth, info->record.origHeight, info->filename.c_str());
            return;
        }
    }

    IDirect3DTexture9* pCustomTex = GetOrCreateD3DTexture(*info);
    if (!pCustomTex)
        return;

    // K9: classify BEFORE writing (eviction before the vanilla fill, F2) — a stale
    // map entry on a recycled texObj must never survive into the fresh record, and
    // the install takes the slot-content AddRef (context C).
    if (InstallCustomTexture(pTexObj, pCustomTex, /*evictOriginalOnForeign=*/true))
    {
        // K26: persist the object->record binding for reload re-installs + verifier.
        EnterCriticalSection(&m_CS);
        m_TexObjToRecordKey[pTexObj] = ((uint64_t)info->record.archiveId << 32) | (uint64_t)info->record.chunkIndex;
        LeaveCriticalSection(&m_CS);
        Logger::LogInfo("[IN-GAME] Replaced Model Texture (archive 0x%08X #%d, slot %u, GBIX 0x%08X: %s) with custom texture!",
            archiveId, tableIndex, masterId, relOffset, info->filename.c_str());
    }
}

// ---------------------------------------------------------------------------------
// Hook 2b: Central Sega Container & Sprite Table Loading at 0x00492780
// ---------------------------------------------------------------------------------
void TextureEngine::HandlePostLoadSegaTextureContainer(void* tablePtr, uint32_t baseIdx)
{
    if (!m_Initialized || !m_Config.Enabled || m_VanillaRestored || !tablePtr)
        return;

    uint32_t archiveId = GetArchiveIdFromTablePtr(tablePtr);
    if (archiveId == 0)
        return;

    if (baseIdx == 0)
    {
        baseIdx = GetDefaultBaseIdxForTable(tablePtr);
    }

    // Fast-path: if no modified textures exist, return immediately
    if (m_ModifiedArchiveIndexSet.empty() && m_ModifiedOffsetSet.empty())
        return;

    // Table entries are 16 bytes: { int16 width, int16 height, int32 format, int32 pData, int32 pTex }
    // Sentinel is pData == 0xFFFFFFFF
    for (uint32_t i = 0; i < 500; i++)
    {
        char* pEntry = (char*)tablePtr + (i * 16);
        uint32_t pData = 0;
        __try {
            pData = *(uint32_t*)(pEntry + 8);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            break;
        }

        if (pData == 0xFFFFFFFF)
            break; // Sentinel reached

        uint32_t masterId = baseIdx + i;
        if (masterId >= 16384)
            break;

        char* masterEntry = (char*)0x01B09938 + (masterId * 24);
        void* pTexObj = nullptr;
        float fW = 0.0f, fH = 0.0f;
        __try {
            pTexObj = *(void**)masterEntry;
            fW = *(float*)(masterEntry + 0x08);
            fH = *(float*)(masterEntry + 0x0C);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            continue;
        }

        if (!pTexObj)
            continue;

        // Validate ct_TextureObject vtable 0x0059EB10
        __try {
            if (*(uintptr_t*)pTexObj != 0x0059EB10)
            {
                continue;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            continue;
        }

        uint32_t relOffset = (pData >= 0x0CB40000 && pData <= 0x0F000000) ? (pData - 0x0CB40000) : 0;
        uint64_t keyIndex = ((uint64_t)archiveId << 32) | i;
        uint64_t keyOffset = ((uint64_t)archiveId << 32) | relOffset;

        if (m_ModifiedArchiveIndexSet.find(keyIndex) == m_ModifiedArchiveIndexSet.end() &&
            m_ModifiedOffsetSet.find(keyOffset) == m_ModifiedOffsetSet.end() &&
            m_ModifiedGbixSet.find(keyOffset) == m_ModifiedGbixSet.end())
        {
            continue;
        }

        const RuntimeTextureInfo* info = FindByArchiveAndIndex(archiveId, i);
        if (!info && relOffset != 0)
        {
            info = FindByArchiveAndOffset(archiveId, relOffset);
        }
        if (!info && relOffset != 0)
        {
            info = FindByArchiveAndGBIX(archiveId, relOffset);
        }

        if (!info || !info->isModified)
            continue;

        uint32_t origW = (uint32_t)(fW + 0.5f);
        uint32_t origH = (uint32_t)(fH + 0.5f);
        if (origW > 0 && origH > 0 && info->record.origWidth > 0 && info->record.origHeight > 0)
        {
            if (info->record.origWidth != origW || info->record.origHeight != origH)
            {
                Logger::LogWarning("[SEGA_CONTAINER] Dimension mismatch on masterId %u: expected %ux%u, got %ux%u (%s). Skipping.",
                    masterId, origW, origH, info->record.origWidth, info->record.origHeight, info->filename.c_str());
                continue;
            }
        }

        IDirect3DTexture9* pCustomTex = GetOrCreateD3DTexture(*info);
        if (!pCustomTex)
            continue;

        if (InstallCustomTexture(pTexObj, pCustomTex, /*evictOriginalOnForeign=*/true))
        {
            EnterCriticalSection(&m_CS);
            m_TexObjToRecordKey[pTexObj] = ((uint64_t)info->record.archiveId << 32) | (uint64_t)info->record.chunkIndex;
            LeaveCriticalSection(&m_CS);
            Logger::LogInfo("[IN-GAME] Replaced Sega Container Texture (archive 0x%08X #%u, slot %u: %s)",
                archiveId, i, masterId, info->filename.c_str());
        }
    }
}

// ---------------------------------------------------------------------------------
// Hook 3: Stage Texture Loading at 0x0048B7A0
// ---------------------------------------------------------------------------------
void TextureEngine::HandlePostLoadStageTexture(void* entryPtr, uint32_t id)
{
    if (!m_Initialized || !m_Config.Enabled || m_VanillaRestored || !entryPtr)
        return;

    // Fast-path: if no modified textures exist, return immediately in 1ns!
    if (m_ModifiedArchiveIndexSet.empty() && m_ModifiedOffsetSet.empty())
        return;

    uint32_t activeStageTable = 0;
    __try {
        activeStageTable = *(uint32_t*)0x01B7A838;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}

    // K6: when the active stage table pointer is null/unmapped we must NOT guess an
    // archive (the old texDC1 default matched texDC2/texDC3 slots by chunk index when
    // dimensions coincided). Skip instead of guessing.
    uint32_t archiveId = GetArchiveIdFromTablePtr((void*)activeStageTable);
    if (archiveId == 0)
    {
        return;
    }

    uint32_t pData = 0;
    __try {
        pData = *(uint32_t*)((char*)entryPtr + 8);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }

    if (pData < 0x0CB40000 || pData > 0x0F000000)
        return;

    uint32_t relOffset = pData - 0x0CB40000;
    uint32_t chunkIndex = (id >= 0x7D0) ? (id - 0x7D0) : id;
    uint64_t keyIndex = ((uint64_t)archiveId << 32) | (uint32_t)chunkIndex;
    uint64_t keyOffset = ((uint64_t)archiveId << 32) | relOffset;

    // Fast O(1) filter: if neither index, offset, nor GBIX matches any modified texture, bail out instantly in <5ns!
    if (m_ModifiedArchiveIndexSet.find(keyIndex) == m_ModifiedArchiveIndexSet.end() &&
        m_ModifiedOffsetSet.find(keyOffset) == m_ModifiedOffsetSet.end() &&
        m_ModifiedGbixSet.find(keyOffset) == m_ModifiedGbixSet.end())
    {
        return;
    }

    const RuntimeTextureInfo* info = FindByArchiveAndIndex(archiveId, chunkIndex);
    if (!info)
    {
        info = FindByArchiveAndOffset(archiveId, relOffset);
    }
    if (!info)
    {
        // K4: archive-scoped GBIX fallback — never hit a foreign archive's entry.
        info = FindByArchiveAndGBIX(archiveId, relOffset);
    }

    if (!info || !info->isModified)
        return;

    char* masterEntry = (char*)0x01B09938 + (id * 24);
    void* pTexObj = nullptr;
    float fW = 0.0f, fH = 0.0f;
    __try {
        pTexObj = *(void**)masterEntry;
        fW = *(float*)(masterEntry + 0x08);
        fH = *(float*)(masterEntry + 0x0C);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}

    if (!pTexObj)
        return;

    // Validate ct_TextureObject vtable 0x0059EB10
    __try {
        if (*(uintptr_t*)pTexObj != 0x0059EB10)
        {
            return;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }

    uint32_t origW = (uint32_t)(fW + 0.5f);
    uint32_t origH = (uint32_t)(fH + 0.5f);
    if (origW > 0 && origH > 0 && info->record.origWidth > 0 && info->record.origHeight > 0)
    {
        if (info->record.origWidth != origW || info->record.origHeight != origH)
        {
            Logger::LogWarning("[STAGE_HOOK] Dimension mismatch on stage id 0x%X: expected %ux%u, got %ux%u (%s). Skipping replacement.",
                id, origW, origH, info->record.origWidth, info->record.origHeight, info->filename.c_str());
            return;
        }
    }

    IDirect3DTexture9* pCustomTex = GetOrCreateD3DTexture(*info);
    if (!pCustomTex)
        return;

    // K9: classify BEFORE writing (eviction before the vanilla fill, F2) — same
    // contract as the model hook above.
    if (InstallCustomTexture(pTexObj, pCustomTex, /*evictOriginalOnForeign=*/true))
    {
        // K26: persist the object->record binding for reload re-installs + verifier.
        EnterCriticalSection(&m_CS);
        m_TexObjToRecordKey[pTexObj] = ((uint64_t)info->record.archiveId << 32) | (uint64_t)info->record.chunkIndex;
        LeaveCriticalSection(&m_CS);
        Logger::LogInfo("[IN-GAME] Replaced Stage Texture (id 0x%X, chunk #%d, GBIX 0x%08X: %s) with custom texture!",
            id, chunkIndex, relOffset, info->filename.c_str());
    }
}

// ---------------------------------------------------------------------------------
// K9: recycling-aware slot classification (fixes F1/F2/F3/F5/F7 and context C).
// Shared by Hook 2 (model), Hook 3 (stage), Hook 4 (apply/restore) and the master
// scan. Every +0x0C write and every map mutation goes through these helpers.
//
// Reference discipline (context C premise: the game releases the slot content when
// the tex struct dies or rebinds the slot):
//   - every slot write of a custom texture takes ONE AddRef modeling the slot
//     content (AddRef-per-Install). N slots sharing one cached texture -> refcount
//     1 (cache) + N (slots). The D3DX UI path at HandleD3DXCreateTextureFromFile-
//     InMemoryEx keeps its own separate, already-balanced AddRef.
//   - restoring a LIVE object releases that slot ref; object death releases
//     nothing on our side (the game owns the slot-content release). If the game
//     never releases, one MANAGED-pool reference leaks per slot (bounded, process
//     lifetime) — the deliberately chosen safe direction versus a double Release.
//   - stored vanilla pointers are a raw borrow (no pin); the classification below
//     therefore never writes a stored vanilla into an object it was not recorded
//     from.
// Locking: critical sections are recursive per-thread, so callers may arrive with
// or without m_CS held (the scan enters it recursively). Hook 4's per-frame paths
// stay lockfree and only enter these helpers on a mismatch, so the render thread
// can never block behind a full scan holding m_CS.
// SEH: raw game-memory accesses are guarded; a dangling texObj key is classified
// as foreign (evict, never write) instead of faulting the render thread.
// Cache note: none of this changes extraction, compile or the cache file format
// (the K2 flip pairing is untouched), so LTM_CACHE_VERSION / trailer stamp stay.
// ---------------------------------------------------------------------------------

// SEH-isolated +0x0C read. Returns nullptr both for a genuine null slot and for an
// unreadable (dangling) texObj — callers treat null as "not verifiable". Must stay
// free of locals that need unwinding (__try restriction C2712).
static IDirect3DTexture9* ReadSlotTextureSafe(void* pTexObj)
{
    if (!pTexObj || (uintptr_t)pTexObj < 0x10000 || (uintptr_t)pTexObj > 0x7FFE0000)
        return nullptr;

    __try
    {
        return *(IDirect3DTexture9**)((char*)pTexObj + 0x0C);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }
}

// Central install / re-apply classification (hooks 2/3/4 + scan, rules S1-S4/R1-R3):
//   C1  pCur == pCustomTex                    -> already installed: no-op (its slot
//                                                ref was taken by the install that
//                                                wrote it; no second AddRef).
//   C2  pCur == m_TexObjToCustom[key]         -> our previous custom still occupies
//                                                the slot but the cache now resolves
//                                                a different texture: swap content,
//                                                release the old slot ref, AddRef
//                                                the new one.
//   C3  pCur == m_OriginalTextures[key]       -> vanilla slot (first install or
//                                                re-apply after F10 restore): write
//                                                custom, AddRef.
//   C4  key present, pCur is neither          -> recycled/foreign object (F1/F3):
//                                                NEVER write; evict the mapping. No
//                                                Release (object-death discipline).
//                                                evictOriginalOnForeign selects the
//                                                documented asymmetry: hook/apply
//                                                paths (true=true) evict both entries
//                                                — an active SetTexture on a mapped
//                                                address is strong recycling evidence,
//                                                and evicting both lets the periodic
//                                                scan re-install fresh (<=5 s self
//                                                heal, original re-recorded from the
//                                                live object). The scan passes false:
//                                                a foreign pCur there may be a live
//                                                game-side texture swap, so the TRUE
//                                                vanilla record must survive; erasing
//                                                it would let the next pass hit C5
//                                                and record the current (possibly
//                                                swapped or stale) slot content as
//                                                "vanilla" for later restore.
//   C5  key absent in both maps               -> fresh install: record pCur as the
//                                                object's vanilla original, write
//                                                custom, AddRef. Runs after the C4
//                                                decision so the eviction-before-fill
//                                                order (F2) captures the recycled
//                                                object's own texture, never the dead
//                                                object's.
// Returns true when the slot content changed (callers use it to log/count).
bool TextureEngine::InstallCustomTexture(void* pTexObj, IDirect3DTexture9* pCustomTex, bool evictOriginalOnForeign)
{
    if (!pTexObj || !pCustomTex)
        return false;

    // Validate ct_TextureObject vtable 0x0059EB10
    __try {
        if (*(uintptr_t*)pTexObj != 0x0059EB10)
            return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }

    bool changed = false;
    EnterCriticalSection(&m_CS);
    __try
    {
        __try
        {
            auto customIt = m_TexObjToCustom.find(pTexObj);
            IDirect3DTexture9* pPrevCustom =
                (customIt != m_TexObjToCustom.end()) ? customIt->second : nullptr;
            auto origIt = m_OriginalTextures.find(pTexObj);
            IDirect3DTexture9* pStoredOriginal =
                (origIt != m_OriginalTextures.end()) ? origIt->second : nullptr;

            IDirect3DTexture9* pCur = ReadSlotTextureSafe(pTexObj);

            if (pCur == pCustomTex)
            {
                changed = false; // C1
            }
            else if (pPrevCustom != nullptr && pCur == pPrevCustom)
            {
                // C2: remap overwrite. Write first (raw access), then swap refs, then
                // publish the map entry — a fault during the write leaves the maps
                // untouched. The m_TexObjToCustom[k] => m_OriginalTextures[k]
                // invariant is maintained by every write site, so the original record
                // is intentionally left untouched here.
                *(IDirect3DTexture9**)((char*)pTexObj + 0x0C) = pCustomTex;
                pPrevCustom->Release(); // slot ref of the replaced custom
                pCustomTex->AddRef();   // new slot ref
                m_TexObjToCustom[pTexObj] = pCustomTex;
                changed = true;
            }
            else if (pCur == pStoredOriginal)
            {
                // C3: re-apply onto the recorded vanilla slot. Null == null is
                // included on purpose (never-bound slot, parity with the former
                // fill-if-absent + unconditional write). A dangling key reads as
                // null here and the write faults into the guarded __except before
                // any map mutation.
                *(IDirect3DTexture9**)((char*)pTexObj + 0x0C) = pCustomTex;
                pCustomTex->AddRef(); // slot ref
                m_TexObjToCustom[pTexObj] = pCustomTex;
                changed = true;
            }
            else if (customIt == m_TexObjToCustom.end() && origIt == m_OriginalTextures.end())
            {
                // C5: fresh install (fill-if-absent with the LIVE object's content).
                if (pCur) pCur->AddRef();
                m_OriginalTextures[pTexObj] = pCur;
                *(IDirect3DTexture9**)((char*)pTexObj + 0x0C) = pCustomTex;
                pCustomTex->AddRef(); // slot ref
                m_TexObjToCustom[pTexObj] = pCustomTex;
                changed = true;
            }
            else
            {
                // C4: When pCur is neither pCustomTex, nor pPrevCustom, nor pStoredOriginal,
                // the game's streaming engine or D3D renderer has re-allocated or re-decoded
                // a new vanilla texture into this slot (e.g. streaming background city chunks,
                // cable car actors, or traffic vehicles).
                // Rather than abandoning the slot and leaving it vanilla forever,
                // update m_OriginalTextures to the new live vanilla pointer,
                // write our custom texture into the slot, and AddRef it!
                auto oldOrigIt = m_OriginalTextures.find(pTexObj);
                if (oldOrigIt != m_OriginalTextures.end() && oldOrigIt->second)
                {
                    __try { oldOrigIt->second->Release(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
                }
                if (pCur) pCur->AddRef();
                m_OriginalTextures[pTexObj] = pCur;
                *(IDirect3DTexture9**)((char*)pTexObj + 0x0C) = pCustomTex;
                pCustomTex->AddRef();
                m_TexObjToCustom[pTexObj] = pCustomTex;
                changed = true;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            // texObj died between classification and write: keep map state as-is
            // (the slot write precedes every map mutation in each branch).
        }
    }
    __finally
    {
        LeaveCriticalSection(&m_CS);
    }
    return changed;
}

// K9 mirror rule for the restore side (F7/F10), shared by the Hook-4 restore branch
// and RestoreAllVanillaTextures:
//   R1  pCur == pStoredOriginal              -> already vanilla: skip. Entries stay
//                                               so toggle-back re-apply keeps working.
//   R2  pCur == m_TexObjToCustom[key]        -> our custom still occupies the slot:
//                                               write the stored vanilla back and
//                                               release the custom slot ref (paired
//                                               with the install AddRef). Entries
//                                               stay for the next enable.
//   R3  key present, pCur is neither         -> recycled/foreign object: evict BOTH
//                                               entries, never write — the recorded
//                                               vanilla belongs to a (possibly dead)
//                                               object and writing it into a live
//                                               slot is the stale-vanilla UAF (F7).
// R2 requires the custom-map key to exist: after F9 only m_OriginalTextures remains,
// so an absent custom key degrades to R3 (conservative). Unreadable slots fall
// through to R3 via the null slot read.
void TextureEngine::RestoreOriginalForTexObj(void* pTexObj, IDirect3DTexture9* pStoredOriginal)
{
    if (!pTexObj || !pStoredOriginal)
        return;

    EnterCriticalSection(&m_CS);
    __try
    {
        __try
        {
            // Validate pStoredOriginal via guarded AddRef/Release before writing into slot
            pStoredOriginal->AddRef();
            pStoredOriginal->Release();

            IDirect3DTexture9* pCur = ReadSlotTextureSafe(pTexObj);
            if (pCur != pStoredOriginal) // R1 when equal: nothing to do
            {
                auto customIt = m_TexObjToCustom.find(pTexObj);
                IDirect3DTexture9* pPrevCustom =
                    (customIt != m_TexObjToCustom.end()) ? customIt->second : nullptr;
                if (pPrevCustom != nullptr && pCur == pPrevCustom)
                {
                    // R2: restore + release the custom slot ref.
                    *(IDirect3DTexture9**)((char*)pTexObj + 0x0C) = pStoredOriginal;
                    pPrevCustom->Release();
                }
                else
                {
                    // R3: foreign/recycled — no write, no Release. K26: the record
                    // binding died with the object (F10 re-enable re-installs fresh
                    // via hooks 2/3 loads, not via this stale key).
                    auto oldOrigIt = m_OriginalTextures.find(pTexObj);
                    if (oldOrigIt != m_OriginalTextures.end() && oldOrigIt->second)
                    {
                        __try { oldOrigIt->second->Release(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
                    }
                    m_TexObjToCustom.erase(pTexObj);
                    m_OriginalTextures.erase(pTexObj);
                    m_TexObjToRecordKey.erase(pTexObj);
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            // Dangling key or invalid COM pointer: map state stays consistent
            auto oldOrigIt = m_OriginalTextures.find(pTexObj);
            if (oldOrigIt != m_OriginalTextures.end() && oldOrigIt->second)
            {
                __try { oldOrigIt->second->Release(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
            }
            m_OriginalTextures.erase(pTexObj);
            m_TexObjToCustom.erase(pTexObj);
            m_TexObjToRecordKey.erase(pTexObj);
        }
    }
    __finally
    {
        LeaveCriticalSection(&m_CS);
    }
}

// ---------------------------------------------------------------------------------
// Hook 4: Pre-SetTexture at 0x00481200
// ---------------------------------------------------------------------------------
void TextureEngine::HandlePreSetTexture(void* pTexObj)
{
    if (!m_Initialized || !pTexObj) return;

    // Validate ct_TextureObject vtable 0x0059EB10
    __try {
        if (*(uintptr_t*)pTexObj != 0x0059EB10)
            return;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }

    // Fast Active Guardian: If this texture object has an assigned custom texture,
    // verify and enforce that pTexObj+0x0C points to it right before Direct3D renders!
    if (m_Config.Enabled && !m_VanillaRestored)
    {
        IDirect3DTexture9* pCustomTex = nullptr;
        EnterCriticalSection(&m_CS);
        auto customIt = m_TexObjToCustom.find(pTexObj);
        if (customIt != m_TexObjToCustom.end())
        {
            pCustomTex = customIt->second;
        }
        LeaveCriticalSection(&m_CS);

        if (pCustomTex != nullptr)
        {
            IDirect3DTexture9** ppSlot = (IDirect3DTexture9**)((char*)pTexObj + 0x0C);
            __try {
                if (*ppSlot != pCustomTex)
                {
                    *ppSlot = pCustomTex;
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
    }

    DWORD now = GetTickCount();

    // Fast hotkey check for live Restore Original Textures (F10) & Quick Reload (F9)
    // Throttled to at most once per 50ms so GetAsyncKeyState syscalls don't run 5,000 times per frame!
    static DWORD s_lastHotkeyPoll = 0;
    static DWORD s_lastF10Press = 0;
    if (now - s_lastHotkeyPoll >= 50)
    {
        s_lastHotkeyPoll = now;
        if (now - s_lastF10Press > 300)
        {
            if (m_Config.ToggleOriginalsKey > 0 && (GetAsyncKeyState(m_Config.ToggleOriginalsKey) & 0x8000))
            {
                s_lastF10Press = now;
                ToggleRestoreOriginalTextures();
            }
            if (m_Config.QuickReloadKey > 0 && (GetAsyncKeyState(m_Config.QuickReloadKey) & 0x8000))
            {
                s_lastF10Press = now;
                ReloadTextures();
                ModelEngine::Instance().Reload3DModels();
                MessageBeep(MB_ICONASTERISK);
            }
            // F8 hotkey for live 3D models reload
            if (GetAsyncKeyState(VK_F8) & 0x8000)
            {
                s_lastF10Press = now;
                ModelEngine::Instance().Reload3DModels();
                MessageBeep(MB_OK);
            }
        }
    }

    // Preload modified D3D textures on first render frame when D3D device is ready
    static bool s_d3dPreloadDone = false;
    if (!s_d3dPreloadDone)
    {
        if (GetD3DDevice())
        {
            s_d3dPreloadDone = true;
            PreloadAllModifiedD3DTextures();
        }
    }

    // K26: periodic verifier over our own installed bindings (5s throttle). Heals
    // game-side +0x0C resets on objects the game no longer passes through
    // SetTexture, and evicts bindings whose object was recycled.
    static DWORD s_lastSlotVerify = 0;
    if (now - s_lastSlotVerify > 5000)
    {
        s_lastSlotVerify = now;
        VerifyInstalledSlots();
    }
}

// ---------------------------------------------------------------------------------
// Master Texture Table Scanning & Synchronization
// ---------------------------------------------------------------------------------
void TextureEngine::ScanAndReplaceAllMasterTextures()
{
    if (!m_Initialized || !m_Config.Enabled || m_VanillaRestored)
        return;

    // Fast-path: if no modified textures exist, return immediately
    if (m_ModifiedArchiveIndexSet.empty() && m_ModifiedOffsetSet.empty() && m_ModifiedGbixSet.empty())
        return;

    // Ensure all modified D3D textures are compiled into memory cache upfront
    PreloadAllModifiedD3DTextures();

    int installedCount = 0;

    uint32_t activeStageTable = 0;
    __try {
        activeStageTable = *(uint32_t*)0x01B7A838;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}

    // Synchronize ALL 26 Sega Container Tables (sprDC0..sprGJAP, texDC0..texDC3)
    for (const auto& tableEntry : s_SegaTableMap)
    {
        uint32_t archiveId = tableEntry.archiveId;
        uint32_t baseIdx = GetDefaultBaseIdxForTable(tableEntry.tablePtr);

        // If this is stage table texDC1/2/3, ensure it only scans if it's the active stage table or active stage table is 0
        if (archiveId == 0x2331A7DA || archiveId == 0xBA38F660 || archiveId == 0xCD3FC6F6)
        {
            if (activeStageTable != 0 && (uintptr_t)tableEntry.tablePtr != activeStageTable)
            {
                continue; // Not the currently active stage
            }
        }

        void* tablePtr = tableEntry.tablePtr;
        for (uint32_t i = 0; i < 1000; i++)
        {
            char* pEntry = (char*)tablePtr + (i * 16);
            uint32_t pData = 0;
            __try {
                pData = *(uint32_t*)(pEntry + 8);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                break;
            }
            if (pData == 0xFFFFFFFF)
                break; // Sentinel reached

            uint32_t masterId = baseIdx + i;
            if (masterId >= 16384)
                break;

            char* masterEntry = (char*)0x01B09938 + (masterId * 24);
            void* pTexObj = nullptr;
            float fW = 0.0f, fH = 0.0f;
            uint32_t masterData = 0;

            __try {
                pTexObj = *(void**)masterEntry;
                fW = *(float*)(masterEntry + 0x08);
                fH = *(float*)(masterEntry + 0x0C);
                masterData = *(uint32_t*)(masterEntry + 0x10);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                continue;
            }

            if (!pTexObj)
                continue;

            // Validate ct_TextureObject vtable 0x0059EB10
            __try {
                if (*(uintptr_t*)pTexObj != 0x0059EB10)
                    continue;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                continue;
            }

            uint32_t origW = (uint32_t)(fW + 0.5f);
            uint32_t origH = (uint32_t)(fH + 0.5f);
            if (origW == 0 || origH == 0)
                continue;

            const RuntimeTextureInfo* info = FindByArchiveAndIndex(archiveId, i);
            if (!info && masterData >= 0x0CB40000 && masterData <= 0x0F000000)
            {
                info = FindByArchiveAndOffset(archiveId, masterData - 0x0CB40000);
            }
            if (!info && pData >= 0x0CB40000 && pData <= 0x0F000000)
            {
                info = FindByArchiveAndOffset(archiveId, pData - 0x0CB40000);
            }

            if (!info || !info->isModified)
            {
                EnterCriticalSection(&m_CS);
                auto customIt = m_TexObjToCustom.find(pTexObj);
                bool hasCustom = (customIt != m_TexObjToCustom.end());
                auto origIt = m_OriginalTextures.find(pTexObj);
                IDirect3DTexture9* pVanilla = (origIt != m_OriginalTextures.end()) ? origIt->second : nullptr;
                LeaveCriticalSection(&m_CS);

                if (hasCustom && pVanilla)
                {
                    RestoreOriginalForTexObj(pTexObj, pVanilla);
                    EnterCriticalSection(&m_CS);
                    m_TexObjToRecordKey.erase(pTexObj);
                    m_TexObjToCustom.erase(pTexObj);
                    LeaveCriticalSection(&m_CS);
                }
                continue;
            }

            if (info->record.origWidth > 0 && info->record.origHeight > 0)
            {
                if (info->record.origWidth != origW || info->record.origHeight != origH)
                    continue;
            }

            IDirect3DTexture9* pCustomTex = GetOrCreateD3DTexture(*info);
            if (!pCustomTex)
                continue;

            if (InstallCustomTexture(pTexObj, pCustomTex, /*evictOriginalOnForeign=*/true))
            {
                EnterCriticalSection(&m_CS);
                m_TexObjToRecordKey[pTexObj] = ((uint64_t)info->record.archiveId << 32) | (uint64_t)info->record.chunkIndex;
                LeaveCriticalSection(&m_CS);
                Logger::LogInfo("[MASTER_SCAN] Synchronized %s #%u (master slot %u, GBIX 0x%08X: %s)",
                    tableEntry.name, i, masterId, info->record.gbix, info->filename.c_str());
                installedCount++;
            }
        }
    }

    if (installedCount > 0)
    {
        Logger::LogInfo("[MASTER_SCAN] Completed master table scan: %d custom texture(s) synchronized.", installedCount);
    }
}

void TextureEngine::RestoreAllVanillaTextures()
{
    // K9 (F7/F10): classify per key instead of the former blind write. The
    // classification may erase from m_OriginalTextures (R3 foreign/recycled), so the
    // loop iterates a stable key snapshot.
    std::vector<void*> keys;
    EnterCriticalSection(&m_CS);
    keys.reserve(m_OriginalTextures.size());
    for (auto& pair : m_OriginalTextures)
    {
        keys.push_back(pair.first);
    }
    LeaveCriticalSection(&m_CS);

    for (void* pTexObj : keys)
    {
        IDirect3DTexture9* pVanillaTex = nullptr;
        EnterCriticalSection(&m_CS);
        auto it = m_OriginalTextures.find(pTexObj);
        if (it != m_OriginalTextures.end())
        {
            pVanillaTex = it->second;
        }
        LeaveCriticalSection(&m_CS);
        if (pTexObj && pVanillaTex)
        {
            // Per-key classification (R1 skip / R2 restore+slot-ref-release / R3
            // evict) with SEH-guarded raw accesses; locked (recursive) internally.
            RestoreOriginalForTexObj(pTexObj, pVanillaTex);
        }
    }
    Logger::LogInfo("Restored all active textures to native vanilla textures.");
}

// K26: cache-key -> record lookup for the reload re-install pass and the verifier.
// Linear scan is fine: only called for the handful of installed bindings.
const RuntimeTextureInfo* TextureEngine::FindInfoByCacheKey(uint64_t cacheKey)
{
    EnterCriticalSection(&m_CS);
    for (const auto& tex : m_Textures)
    {
        uint64_t key = ((uint64_t)tex.record.archiveId << 32) | (uint64_t)tex.record.chunkIndex;
        if (key == cacheKey)
        {
            const RuntimeTextureInfo* p = &tex;
            LeaveCriticalSection(&m_CS);
            return p;
        }
    }
    LeaveCriticalSection(&m_CS);
    return nullptr;
}

// K26: periodic verifier over OUR OWN installed bindings only (never a full
// master-table walk). Heals game-side +0x0C resets (re-apply via C3) and evicts
// recycled objects (C4) with the same classified machinery as every other write.
void TextureEngine::VerifyInstalledSlots()
{
    if (!m_Config.Enabled || m_VanillaRestored)
        return;

    std::vector<std::pair<void*, uint64_t>> snapshot;
    EnterCriticalSection(&m_CS);
    if (m_TexObjToRecordKey.empty())
    {
        LeaveCriticalSection(&m_CS);
        return;
    }
    snapshot.reserve(m_TexObjToRecordKey.size());
    for (const auto& pair : m_TexObjToRecordKey)
        snapshot.push_back(pair);
    LeaveCriticalSection(&m_CS);

    for (auto& entry : snapshot)
    {
        void* pTexObj = entry.first;
        if (!IsValidTextureObject(pTexObj))
        {
            EnterCriticalSection(&m_CS);
            m_TexObjToRecordKey.erase(pTexObj);
            m_TexObjToCustom.erase(pTexObj);
            m_OriginalTextures.erase(pTexObj);
            LeaveCriticalSection(&m_CS);
            continue;
        }

        IDirect3DTexture9* pExpectedCustom = nullptr;
        IDirect3DTexture9* pStoredOriginal = nullptr;
        EnterCriticalSection(&m_CS);
        auto cIt = m_TexObjToCustom.find(entry.first);
        if (cIt != m_TexObjToCustom.end())
            pExpectedCustom = cIt->second;
        auto oIt = m_OriginalTextures.find(entry.first);
        if (oIt != m_OriginalTextures.end())
            pStoredOriginal = oIt->second;
        LeaveCriticalSection(&m_CS);
        if (!pExpectedCustom)
            continue;

        IDirect3DTexture9* pCur = ReadSlotTextureSafe(entry.first);
        if (pCur == pExpectedCustom)
            continue; // still ours — nothing to do
        if (pStoredOriginal != nullptr && pCur == pStoredOriginal)
        {
            // Game-side reset back to the recorded vanilla: heal (C3 re-apply).
            if (InstallCustomTexture(entry.first, pExpectedCustom, /*evictOriginalOnForeign=*/false))
                Logger::LogInfo("[K26] Re-applied custom after game-side slot reset.");
        }
        else
        {
            // Foreign/recycled pointer: classified install evicts instead of writing.
            InstallCustomTexture(entry.first, pExpectedCustom, /*evictOriginalOnForeign=*/true);
        }
    }
}
