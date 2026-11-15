#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <gdiplus.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <cmath>
#include "AssetExtractor.h"
#include "Logger.h"

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

using namespace Gdiplus;

namespace
{

// ---------------------------------------------------------------------------------
// Progress Screen UI for Asset Extraction
// ---------------------------------------------------------------------------------
class ExtractorProgressScreen
{
public:
    static void Show(HWND hParent, int totalSteps, const std::string& title)
    {
        if (s_hWnd) return;
        s_Total = totalSteps;
        s_Current = 0;
        s_Title = title;
        s_StatusText = "Preparing extraction...";

        HINSTANCE hInst = GetModuleHandleA(NULL);
        if (!s_Registered)
        {
            WNDCLASSEXA wc = { sizeof(WNDCLASSEXA) };
            wc.style = CS_HREDRAW | CS_VREDRAW;
            wc.lpfnWndProc = WndProc;
            wc.hInstance = hInst;
            wc.hCursor = LoadCursor(NULL, IDC_ARROW);
            wc.hbrBackground = NULL;
            wc.lpszClassName = "LyrasTextureMod_ExtractorProgressClass";
            RegisterClassExA(&wc);
            s_Registered = true;
        }

        s_hTitleFont = CreateFontA(20, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
        s_hTextFont = CreateFontA(14, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
        s_hSmallFont = CreateFontA(12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");

        int w = 580;
        int h = 190;
        int screenW = GetSystemMetrics(SM_CXSCREEN);
        int screenH = GetSystemMetrics(SM_CYSCREEN);
        int posX = (screenW - w) / 2;
        int posY = (screenH - h) / 2;

        s_hWnd = CreateWindowExA(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
            "LyrasTextureMod_ExtractorProgressClass",
            title.c_str(),
            WS_POPUP | WS_VISIBLE,
            posX, posY, w, h,
            hParent, NULL, hInst, NULL
        );

        if (s_hWnd)
        {
            ShowWindow(s_hWnd, SW_SHOW);
            UpdateWindow(s_hWnd);
            PumpMessages();
        }
    }

    static void Update(int current, int total, const std::string& status)
    {
        s_Current = current;
        s_Total = total;
        s_StatusText = status;

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

            ShowWindow(hWndToHide, SW_HIDE);
            SetWindowPos(hWndToHide, NULL, 0, 0, 0, 0,
                SWP_HIDEWINDOW | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);

            if (!DestroyWindow(hWndToHide))
            {
                PostMessageA(hWndToHide, WM_CLOSE, 0, 0);
            }

            MSG msg;
            for (int i = 0; i < 30 && PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE); ++i)
            {
                TranslateMessage(&msg);
                DispatchMessageA(&msg);
            }

            HWND hGame = FindWindowA("Crazy Taxi", "Crazy Taxi");
            if (!hGame) hGame = GetActiveWindow();
            if (hGame && IsWindow(hGame))
            {
                InvalidateRect(hGame, NULL, TRUE);
                UpdateWindow(hGame);
                RedrawWindow(hGame, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
            }
        }
        if (s_hTitleFont) { DeleteObject(s_hTitleFont); s_hTitleFont = NULL; }
        if (s_hTextFont)  { DeleteObject(s_hTextFont);  s_hTextFont = NULL; }
        if (s_hSmallFont) { DeleteObject(s_hSmallFont); s_hSmallFont = NULL; }
    }

private:
    static void PumpMessages()
    {
        if (!s_hWnd || !IsWindow(s_hWnd)) return;
        MSG msg;
        while (PeekMessageA(&msg, s_hWnd, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    }

    static LRESULT CALLBACK WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
    {
        if (uMsg == WM_PAINT)
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

            HBRUSH bgBrush = CreateSolidBrush(RGB(18, 20, 24));
            FillRect(memDC, &clientRect, bgBrush);
            DeleteObject(bgBrush);

            HPEN borderPen = CreatePen(PS_SOLID, 2, RGB(255, 185, 0));
            HGDIOBJ oldPen = SelectObject(memDC, borderPen);
            HGDIOBJ oldBr = SelectObject(memDC, GetStockObject(NULL_BRUSH));
            Rectangle(memDC, 0, 0, width, height);
            SelectObject(memDC, oldBr);
            SelectObject(memDC, oldPen);
            DeleteObject(borderPen);

            SetBkMode(memDC, TRANSPARENT);

            HFONT oldFont = (HFONT)SelectObject(memDC, s_hTitleFont);
            SetTextColor(memDC, RGB(255, 200, 30));
            RECT titleRect = { 24, 18, width - 24, 45 };
            DrawTextA(memDC, s_Title.c_str(), -1, &titleRect, DT_LEFT | DT_SINGLELINE);

            SelectObject(memDC, s_hTextFont);
            SetTextColor(memDC, RGB(220, 225, 235));
            RECT statusRect = { 24, 52, width - 24, 76 };
            DrawTextA(memDC, s_StatusText.c_str(), -1, &statusRect, DT_LEFT | DT_SINGLELINE | DT_PATH_ELLIPSIS);

            int barX = 24;
            int barY = 88;
            int barW = width - 48;
            int barH = 24;

            HBRUSH barBg = CreateSolidBrush(RGB(32, 36, 44));
            RECT barBgRect = { barX, barY, barX + barW, barY + barH };
            FillRect(memDC, &barBgRect, barBg);
            DeleteObject(barBg);

            float pct = (s_Total > 0) ? (float)s_Current / (float)s_Total : 0.0f;
            if (pct > 1.0f) pct = 1.0f;
            if (pct < 0.0f) pct = 0.0f;

            int fillW = (int)(barW * pct);
            if (fillW > 0)
            {
                HBRUSH barFill = CreateSolidBrush(RGB(255, 175, 0));
                RECT fillRect = { barX, barY, barX + fillW, barY + barH };
                FillRect(memDC, &fillRect, barFill);
                DeleteObject(barFill);
            }

            SelectObject(memDC, s_hSmallFont);
            char pctStr[64];
            sprintf_s(pctStr, "%d / %d (%d%%)", s_Current, s_Total, (int)(pct * 100.0f));
            SetTextColor(memDC, RGB(180, 185, 195));
            RECT countRect = { barX, barY + barH + 8, width - 24, height - 12 };
            DrawTextA(memDC, pctStr, -1, &countRect, DT_LEFT | DT_SINGLELINE);

            BitBlt(hdc, 0, 0, width, height, memDC, 0, 0, SRCCOPY);
            SelectObject(memDC, oldFont);
            SelectObject(memDC, oldBmp);
            DeleteObject(memBmp);
            DeleteDC(memDC);

            EndPaint(hWnd, &ps);
            return 0;
        }
        if (uMsg == WM_CLOSE)
        {
            DestroyWindow(hWnd);
            return 0;
        }
        return DefWindowProcA(hWnd, uMsg, wParam, lParam);
    }

    static HWND s_hWnd;
    static bool s_Registered;
    static int s_Total;
    static int s_Current;
    static std::string s_Title;
    static std::string s_StatusText;
    static HFONT s_hTitleFont;
    static HFONT s_hTextFont;
    static HFONT s_hSmallFont;
};

HWND ExtractorProgressScreen::s_hWnd = NULL;
bool ExtractorProgressScreen::s_Registered = false;
int ExtractorProgressScreen::s_Total = 100;
int ExtractorProgressScreen::s_Current = 0;
std::string ExtractorProgressScreen::s_Title = "LyrasTextureMod";
std::string ExtractorProgressScreen::s_StatusText = "";
HFONT ExtractorProgressScreen::s_hTitleFont = NULL;
HFONT ExtractorProgressScreen::s_hTextFont = NULL;
HFONT ExtractorProgressScreen::s_hSmallFont = NULL;

// ---------------------------------------------------------------------------------
// Standard CRC32
// ---------------------------------------------------------------------------------
static uint32_t s_Crc32Table[256];
static bool s_Crc32TableInit = false;

static void InitCrc32()
{
    if (s_Crc32TableInit) return;
    for (uint32_t i = 0; i < 256; i++)
    {
        uint32_t c = i;
        for (int j = 0; j < 8; j++)
        {
            if (c & 1) c = 0xEDB88320 ^ (c >> 1);
            else c >>= 1;
        }
        s_Crc32Table[i] = c;
    }
    s_Crc32TableInit = true;
}

static uint32_t CalcCRC32(const void* data, size_t length)
{
    InitCrc32();
    const uint8_t* p = (const uint8_t*)data;
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < length; i++)
    {
        crc = s_Crc32Table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFF;
}

static inline uint32_t StringCRC32(const std::string& str)
{
    return CalcCRC32(str.data(), str.size());
}

// ---------------------------------------------------------------------------------
// File & Directory Utilities
// ---------------------------------------------------------------------------------
static bool EnsureDirExists(const std::string& path)
{
    if (path.empty()) return false;
    char full[MAX_PATH];
    strcpy_s(full, path.c_str());
    int res = SHCreateDirectoryExA(NULL, full, NULL);
    return (res == ERROR_SUCCESS || res == ERROR_FILE_EXISTS || res == ERROR_ALREADY_EXISTS);
}

static bool IsDirNonEmpty(const std::string& dirPath)
{
    DWORD attr = GetFileAttributesA(dirPath.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY))
        return false;

    WIN32_FIND_DATAA ffd;
    HANDLE hFind = FindFirstFileA((dirPath + "\\*").c_str(), &ffd);
    if (hFind == INVALID_HANDLE_VALUE) return false;

    bool foundItem = false;
    do
    {
        if (strcmp(ffd.cFileName, ".") != 0 && strcmp(ffd.cFileName, "..") != 0)
        {
            foundItem = true;
            break;
        }
    } while (FindNextFileA(hFind, &ffd));

    FindClose(hFind);
    return foundItem;
}

static std::vector<uint8_t> ReadBinaryFile(const std::string& path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) return {};
    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> buffer((size_t)size);
    if (size > 0)
    {
        file.read((char*)buffer.data(), size);
    }
    return buffer;
}

static bool WriteBinaryFile(const std::string& path, const void* data, size_t size)
{
    std::ofstream file(path, std::ios::binary);
    if (!file.is_open()) return false;
    file.write((const char*)data, size);
    return file.good();
}

static bool WriteStringFile(const std::string& path, const std::string& content)
{
    std::ofstream file(path);
    if (!file.is_open()) return false;
    file.write(content.data(), content.size());
    return file.good();
}

// Save 32-bit ARGB pixels (0xAARRGGBB) as PNG via GDI+
static bool SavePixelsToPng(const std::string& path, const uint32_t* pixels, uint32_t w, uint32_t h)
{
    if (!pixels || w == 0 || h == 0) return false;

    // GDI+ PNG Encoder CLSID: {557CF406-1A04-11D3-9A73-0000F81EF32E}
    static const CLSID clsidPng = { 0x557cf406, 0x1a04, 0x11d3, { 0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e } };

    int wlen = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, NULL, 0);
    std::wstring wpath(wlen, 0);
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &wpath[0], wlen);

    Gdiplus::Bitmap bmp((INT)w, (INT)h, (INT)(w * 4), PixelFormat32bppARGB, (BYTE*)pixels);
    Gdiplus::Status st = bmp.Save(wpath.c_str(), &clsidPng, NULL);
    return (st == Gdiplus::Ok);
}

// Strictly snaps n to closest Power-of-Two (16..1024) per Crazy Taxi Texture Modding Guide
static uint32_t GetClosestPot(uint32_t n)
{
    if (n <= 16) return 16;
    if (n >= 1024) return 1024;
    uint32_t low = 1;
    while ((low << 1) <= n) low <<= 1;
    uint32_t high = low << 1;
    return (n - low < high - n) ? low : high;
}

// Computes target HD resolution adhering to Crazy Taxi Texture Modding Guide.pdf:
// - Strictly Power-of-Two (16..1024)
// - Preserves exact aspect ratio (W/H = 2^k)
// - Upscales small/medium textures by +2 POT levels (4x relative scaling)
// - Capped at max 1024 (1024p) to preserve 32-bit D3D9 VRAM budget
// H4 note (no behavior change here): ARs that are not POT ratios cannot be preserved
// under this rule — 640x480 (4:3) yields 1024x1024 and 640x360 (16:9) yields 1024x512.
// The engine therefore declines aspect-deviating 2D injections at the D3DX hook and
// lets those slots stay vanilla instead of distorting them.
static void CalculateTargetHdResolution(uint32_t origW, uint32_t origH, uint32_t& outW, uint32_t& outH)
{
    if (origW == 0) origW = 16;
    if (origH == 0) origH = 16;

    const uint32_t MAX_DIM = 1024;
    double ratio = (double)origW / (double)origH;
    int k = (int)std::round(std::log2(ratio));

    uint32_t targetW = 0;
    uint32_t targetH = 0;

    if (k >= 0)
    {
        targetW = origW * 4;
        targetW = GetClosestPot(targetW);
        if (targetW > MAX_DIM) targetW = MAX_DIM;
        targetH = targetW >> k;
        if (targetH < 16) targetH = 16;
        if (targetH > MAX_DIM) targetH = MAX_DIM;
    }
    else
    {
        int invK = -k;
        targetH = origH * 4;
        targetH = GetClosestPot(targetH);
        if (targetH > MAX_DIM) targetH = MAX_DIM;
        targetW = targetH >> invK;
        if (targetW < 16) targetW = 16;
        if (targetW > MAX_DIM) targetW = MAX_DIM;
    }

    targetW = GetClosestPot(targetW);
    targetH = GetClosestPot(targetH);

    outW = targetW;
    outH = targetH;
}

// Upscale 32-bit ARGB pixels to HD target resolution using GDI+ High-Quality Bicubic resampling and save as PNG
static bool SaveUpscaledPixelsToPng(const std::string& path, const uint32_t* pixels, uint32_t srcW, uint32_t srcH, uint32_t dstW, uint32_t dstH)
{
    if (!pixels || srcW == 0 || srcH == 0 || dstW == 0 || dstH == 0) return false;
    static const CLSID clsidPng = { 0x557cf406, 0x1a04, 0x11d3, { 0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e } };

    int wlen = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, NULL, 0);
    std::wstring wpath(wlen, 0);
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &wpath[0], wlen);

    if (srcW == dstW && srcH == dstH)
    {
        Gdiplus::Bitmap bmp((INT)srcW, (INT)srcH, (INT)(srcW * 4), PixelFormat32bppARGB, (BYTE*)pixels);
        Gdiplus::Status st = bmp.Save(wpath.c_str(), &clsidPng, NULL);
        return (st == Gdiplus::Ok);
    }

    Gdiplus::Bitmap srcBmp((INT)srcW, (INT)srcH, (INT)(srcW * 4), PixelFormat32bppARGB, (BYTE*)pixels);
    Gdiplus::Bitmap dstBmp((INT)dstW, (INT)dstH, PixelFormat32bppARGB);
    {
        Gdiplus::Graphics g(&dstBmp);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        g.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
        g.Clear(Gdiplus::Color(0, 0, 0, 0));

        Gdiplus::ImageAttributes imgAttr;
        imgAttr.SetWrapMode(Gdiplus::WrapModeTileFlipXY);
        g.DrawImage(&srcBmp, Gdiplus::Rect(0, 0, (INT)dstW, (INT)dstH), 0, 0, (INT)srcW, (INT)srcH, Gdiplus::UnitPixel, &imgAttr);
    }

    Gdiplus::Status st = dstBmp.Save(wpath.c_str(), &clsidPng, NULL);
    return (st == Gdiplus::Ok);
}

// ---------------------------------------------------------------------------------
// Sega PowerVR2 Twiddle and VQ Decoders
// ---------------------------------------------------------------------------------
static inline uint32_t GetTwiddledOffset(uint32_t w, uint32_t h, uint32_t x, uint32_t y)
{
    uint32_t minDim = min(w, h);
    uint32_t blockIdx = 0;
    if (w < h)
    {
        blockIdx = y / minDim;
        y = y % minDim;
    }
    else
    {
        blockIdx = x / minDim;
        x = x % minDim;
    }
    uint32_t interleave = 0;
    for (int i = 0; (1u << i) < minDim; i++)
    {
        interleave |= ((y & (1u << i)) << i) | ((x & (1u << i)) << (i + 1));
    }
    return interleave + (blockIdx * minDim * minDim);
}

static inline uint32_t DecodePvrPixel(uint16_t val, int fmtType)
{
    uint32_t a = 255, r = 0, g = 0, b = 0;
    if (fmtType == 0) // ARGB1555
    {
        a = (val & 0x8000) ? 255 : 0;
        r = ((val >> 10) & 0x1F) * 255 / 31;
        g = ((val >> 5) & 0x1F) * 255 / 31;
        b = (val & 0x1F) * 255 / 31;
    }
    else if (fmtType == 1) // RGB565
    {
        a = 255;
        r = ((val >> 11) & 0x1F) * 255 / 31;
        g = ((val >> 5) & 0x3F) * 255 / 63;
        b = (val & 0x1F) * 255 / 31;
    }
    else if (fmtType == 2) // ARGB4444
    {
        a = ((val >> 12) & 0xF) * 17;
        r = ((val >> 8) & 0xF) * 17;
        g = ((val >> 4) & 0xF) * 17;
        b = (val & 0xF) * 17;
    }
    else // default RGB565
    {
        a = 255;
        r = ((val >> 11) & 0x1F) * 255 / 31;
        g = ((val >> 5) & 0x3F) * 255 / 63;
        b = (val & 0x1F) * 255 / 31;
    }
    return (a << 24) | (r << 16) | (g << 8) | b;
}

static uint32_t GetVqMipSum(uint32_t w, uint32_t h)
{
    uint32_t s = 1;
    uint32_t dim = 2;
    uint32_t maxDim = max(w, h);
    while (dim < maxDim)
    {
        uint32_t bx = max(1u, dim / 2);
        uint32_t by = max(1u, dim / 2);
        s += bx * by;
        dim *= 2;
    }
    return s;
}

static uint32_t GetTwidMipOffset(uint32_t w, uint32_t h)
{
    uint32_t s = 0;
    uint32_t dim = 1;
    uint32_t maxDim = max(w, h);
    while (dim < maxDim)
    {
        s += dim * dim * 2;
        dim *= 2;
    }
    return s;
}

static bool DecodeVQTexture(const uint8_t* chunk, size_t chunkLen, uint32_t w, uint32_t h, int pfmt, bool isMipmap, std::vector<uint32_t>& outPixels)
{
    if (chunkLen < 2048) return false;
    outPixels.resize(w * h, 0);

    struct CodebookEntry { uint32_t c0, c1, c2, c3; };
    CodebookEntry codebook[256];
    const uint16_t* cbRaw = (const uint16_t*)chunk;
    for (int i = 0; i < 256; i++)
    {
        codebook[i].c0 = DecodePvrPixel(cbRaw[i * 4 + 0], pfmt);
        codebook[i].c1 = DecodePvrPixel(cbRaw[i * 4 + 1], pfmt);
        codebook[i].c2 = DecodePvrPixel(cbRaw[i * 4 + 2], pfmt);
        codebook[i].c3 = DecodePvrPixel(cbRaw[i * 4 + 3], pfmt);
    }

    uint32_t nbx = max(1u, w / 2);
    uint32_t nby = max(1u, h / 2);
    size_t needed = (size_t)nbx * nby;
    size_t offset = isMipmap ? (2048 + GetVqMipSum(w, h)) : 2048;
    if (offset + needed > chunkLen) return false;

    const uint8_t* indices = chunk + offset;
    for (uint32_t by = 0; by < nby; by++)
    {
        for (uint32_t bx = 0; bx < nbx; bx++)
        {
            uint32_t bIdx = GetTwiddledOffset(nbx, nby, bx, by);
            if (bIdx < needed)
            {
                uint8_t cbIdx = indices[bIdx];
                const CodebookEntry& entry = codebook[cbIdx];

                uint32_t x0 = bx * 2;
                uint32_t y0 = by * 2;
                if (x0 < w && y0 < h) outPixels[y0 * w + x0] = entry.c0;
                if (x0 < w && y0 + 1 < h) outPixels[(y0 + 1) * w + x0] = entry.c1;
                if (x0 + 1 < w && y0 < h) outPixels[y0 * w + (x0 + 1)] = entry.c2;
                if (x0 + 1 < w && y0 + 1 < h) outPixels[(y0 + 1) * w + (x0 + 1)] = entry.c3;
            }
        }
    }
    return true;
}

static bool DecodeTwiddled16bpp(const uint8_t* chunk, size_t chunkLen, uint32_t w, uint32_t h, int pfmt, bool isMipmap, std::vector<uint32_t>& outPixels)
{
    outPixels.resize(w * h, 0);
    size_t startOff = 0;
    size_t uncomp = (size_t)w * h * 2;
    if (isMipmap)
    {
        size_t mipOff = GetTwidMipOffset(w, h);
        if (chunkLen >= mipOff + uncomp) startOff = mipOff;
    }

    for (uint32_t y = 0; y < h; y++)
    {
        for (uint32_t x = 0; x < w; x++)
        {
            size_t pos = startOff + (size_t)GetTwiddledOffset(w, h, x, y) * 2;
            if (pos + 1 < chunkLen)
            {
                uint16_t val = *(const uint16_t*)(chunk + pos);
                outPixels[y * w + x] = DecodePvrPixel(val, pfmt);
            }
        }
    }
    return true;
}

static bool DecodeLinear16bpp(const uint8_t* chunk, size_t chunkLen, uint32_t w, uint32_t h, int pfmt, std::vector<uint32_t>& outPixels)
{
    outPixels.resize(w * h, 0);
    for (uint32_t y = 0; y < h; y++)
    {
        for (uint32_t x = 0; x < w; x++)
        {
            size_t pos = ((size_t)y * w + x) * 2;
            if (pos + 1 < chunkLen)
            {
                uint16_t val = *(const uint16_t*)(chunk + pos);
                outPixels[y * w + x] = DecodePvrPixel(val, pfmt);
            }
        }
    }
    return true;
}

static bool Decode8bpp(const uint8_t* chunk, size_t chunkLen, uint32_t w, uint32_t h, std::vector<uint32_t>& outPixels)
{
    outPixels.resize(w * h, 0);
    for (uint32_t y = 0; y < h; y++)
    {
        for (uint32_t x = 0; x < w; x++)
        {
            size_t pos = GetTwiddledOffset(w, h, x, y);
            if (pos < chunkLen)
            {
                uint8_t b = chunk[pos];
                uint8_t v = (b <= 15) ? (b & 0xF) * 17 : b;
                outPixels[y * w + x] = (v << 24) | 0x00FFFFFF;
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------------------------
// Pure C++ DXT1, DXT3, DXT5 Decoders for DirectDraw Surface (DDS) Chunks
// ---------------------------------------------------------------------------------
static inline void DecodeDxtColorBlock(const uint8_t* block, uint32_t colors[4])
{
    uint16_t c0 = *(const uint16_t*)(block + 0);
    uint16_t c1 = *(const uint16_t*)(block + 2);

    uint32_t r0 = ((c0 >> 11) & 0x1F) * 255 / 31;
    uint32_t g0 = ((c0 >> 5) & 0x3F) * 255 / 63;
    uint32_t b0 = (c0 & 0x1F) * 255 / 31;

    uint32_t r1 = ((c1 >> 11) & 0x1F) * 255 / 31;
    uint32_t g1 = ((c1 >> 5) & 0x3F) * 255 / 63;
    uint32_t b1 = (c1 & 0x1F) * 255 / 31;

    colors[0] = 0xFF000000 | (r0 << 16) | (g0 << 8) | b0;
    colors[1] = 0xFF000000 | (r1 << 16) | (g1 << 8) | b1;

    if (c0 > c1)
    {
        uint32_t r2 = (2 * r0 + r1) / 3;
        uint32_t g2 = (2 * g0 + g1) / 3;
        uint32_t b2 = (2 * b0 + b1) / 3;
        colors[2] = 0xFF000000 | (r2 << 16) | (g2 << 8) | b2;

        uint32_t r3 = (r0 + 2 * r1) / 3;
        uint32_t g3 = (g0 + 2 * g1) / 3;
        uint32_t b3 = (b0 + 2 * b1) / 3;
        colors[3] = 0xFF000000 | (r3 << 16) | (g3 << 8) | b3;
    }
    else
    {
        uint32_t r2 = (r0 + r1) / 2;
        uint32_t g2 = (g0 + g1) / 2;
        uint32_t b2 = (b0 + b1) / 2;
        colors[2] = 0xFF000000 | (r2 << 16) | (g2 << 8) | b2;
        colors[3] = 0x00000000; // transparent
    }
}

static bool DecodeDDS(const uint8_t* data, size_t size, uint32_t& outW, uint32_t& outH, std::vector<uint32_t>& outPixels)
{
    if (size < 128) return false;
    if (memcmp(data, "DDS ", 4) != 0) return false;

    outH = *(const uint32_t*)(data + 12);
    outW = *(const uint32_t*)(data + 16);
    if (outW == 0 || outH == 0) return false;

    uint32_t fourCC = *(const uint32_t*)(data + 84);
    const uint8_t* src = data + 128;
    outPixels.resize(outW * outH, 0);

    uint32_t nbx = (outW + 3) / 4;
    uint32_t nby = (outH + 3) / 4;

    if (fourCC == 0x31545844) // DXT1
    {
        for (uint32_t by = 0; by < nby; by++)
        {
            for (uint32_t bx = 0; bx < nbx; bx++)
            {
                if ((size_t)(src - data) + 8 > size) break;
                uint32_t colors[4];
                DecodeDxtColorBlock(src, colors);
                uint32_t bits = *(const uint32_t*)(src + 4);
                src += 8;

                for (uint32_t y = 0; y < 4; y++)
                {
                    for (uint32_t x = 0; x < 4; x++)
                    {
                        uint32_t px = bx * 4 + x;
                        uint32_t py = by * 4 + y;
                        if (px < outW && py < outH)
                        {
                            uint32_t idx = (bits >> (2 * (y * 4 + x))) & 3;
                            outPixels[py * outW + px] = colors[idx];
                        }
                    }
                }
            }
        }
        return true;
    }
    else if (fourCC == 0x33545844) // DXT3
    {
        for (uint32_t by = 0; by < nby; by++)
        {
            for (uint32_t bx = 0; bx < nbx; bx++)
            {
                if ((size_t)(src - data) + 16 > size) break;
                uint64_t alphaRaw = *(const uint64_t*)src;
                src += 8;
                uint32_t colors[4];
                DecodeDxtColorBlock(src, colors);
                uint32_t bits = *(const uint32_t*)(src + 4);
                src += 8;

                for (uint32_t y = 0; y < 4; y++)
                {
                    for (uint32_t x = 0; x < 4; x++)
                    {
                        uint32_t px = bx * 4 + x;
                        uint32_t py = by * 4 + y;
                        if (px < outW && py < outH)
                        {
                            uint32_t idx = (bits >> (2 * (y * 4 + x))) & 3;
                            uint32_t a = (uint32_t)((alphaRaw >> (4 * (y * 4 + x))) & 0xF) * 17;
                            outPixels[py * outW + px] = (a << 24) | (colors[idx] & 0x00FFFFFF);
                        }
                    }
                }
            }
        }
        return true;
    }
    else if (fourCC == 0x35545844) // DXT5
    {
        for (uint32_t by = 0; by < nby; by++)
        {
            for (uint32_t bx = 0; bx < nbx; bx++)
            {
                if ((size_t)(src - data) + 16 > size) break;
                uint8_t a0 = src[0];
                uint8_t a1 = src[1];
                uint8_t alphas[8];
                alphas[0] = a0;
                alphas[1] = a1;
                if (a0 > a1)
                {
                    for (int i = 1; i <= 6; i++) alphas[i + 1] = ((7 - i) * a0 + i * a1) / 7;
                }
                else
                {
                    for (int i = 1; i <= 4; i++) alphas[i + 1] = ((5 - i) * a0 + i * a1) / 5;
                    alphas[6] = 0;
                    alphas[7] = 255;
                }

                uint64_t aIndices = 0;
                memcpy(&aIndices, src + 2, 6);
                src += 8;

                uint32_t colors[4];
                DecodeDxtColorBlock(src, colors);
                uint32_t bits = *(const uint32_t*)(src + 4);
                src += 8;

                for (uint32_t y = 0; y < 4; y++)
                {
                    for (uint32_t x = 0; x < 4; x++)
                    {
                        uint32_t px = bx * 4 + x;
                        uint32_t py = by * 4 + y;
                        if (px < outW && py < outH)
                        {
                            uint32_t cIdx = (bits >> (2 * (y * 4 + x))) & 3;
                            uint32_t aIdx = (uint32_t)((aIndices >> (3 * (y * 4 + x))) & 7);
                            outPixels[py * outW + px] = ((uint32_t)alphas[aIdx] << 24) | (colors[cIdx] & 0x00FFFFFF);
                        }
                    }
                }
            }
        }
        return true;
    }
    else // Uncompressed formats (16-bit or 32-bit)
    {
        uint32_t rgbBitCount = *(const uint32_t*)(data + 88);
        if (rgbBitCount == 16)
        {
            size_t expected = (size_t)outW * outH * 2;
            if (src + expected <= data + size)
            {
                uint32_t rMask = *(const uint32_t*)(data + 92);
                uint32_t gMask = *(const uint32_t*)(data + 96);
                uint32_t bMask = *(const uint32_t*)(data + 100);
                uint32_t aMask = *(const uint32_t*)(data + 104);
                const uint16_t* src16 = (const uint16_t*)src;

                for (size_t i = 0; i < (size_t)outW * outH; i++)
                {
                    uint16_t p = src16[i];
                    uint32_t a = 255, r = 0, g = 0, b = 0;
                    if (aMask == 0xF000 && rMask == 0x0F00 && gMask == 0x00F0 && bMask == 0x000F) // ARGB4444
                    {
                        a = ((p >> 12) & 0x0F) * 17;
                        r = ((p >> 8) & 0x0F) * 17;
                        g = ((p >> 4) & 0x0F) * 17;
                        b = (p & 0x0F) * 17;
                    }
                    else if (rMask == 0xF800 && gMask == 0x07E0 && bMask == 0x001F) // RGB565
                    {
                        a = 255;
                        r = ((p >> 11) & 0x1F) * 255 / 31;
                        g = ((p >> 5) & 0x3F) * 255 / 63;
                        b = (p & 0x1F) * 255 / 31;
                    }
                    else if (aMask == 0x8000 && rMask == 0x7C00 && gMask == 0x03E0 && bMask == 0x001F) // ARGB1555
                    {
                        a = (p & 0x8000) ? 255 : 0;
                        r = ((p >> 10) & 0x1F) * 255 / 31;
                        g = ((p >> 5) & 0x1F) * 255 / 31;
                        b = (p & 0x1F) * 255 / 31;
                    }
                    else
                    {
                        // Generic 16-bit 565 / 4444 fallback
                        r = ((p >> 11) & 0x1F) * 255 / 31;
                        g = ((p >> 5) & 0x3F) * 255 / 63;
                        b = (p & 0x1F) * 255 / 31;
                    }
                    outPixels[i] = (a << 24) | (r << 16) | (g << 8) | b;
                }
                return true;
            }
        }
        else // 32-bit BGRA / ARGB
        {
            size_t expected = (size_t)outW * outH * 4;
            if (src + expected <= data + size)
            {
                memcpy(outPixels.data(), src, expected);
                return true;
            }
        }
    }
    return false;
}

} // anonymous namespace

namespace
{

struct SegaTableInfo
{
    uint32_t tableVA;
    const char* name;
    uint32_t archiveId;
};

static const SegaTableInfo s_SegaTables[] = {
    { 0x0135EFD0, "sprDC0",  0x8E8E5F62 },
    { 0x0135F790, "sprCENG", 0xB16EA5E6 },
    { 0x0135F8D0, "sprCJAP", 0xBE797BD3 },
    { 0x0135FA10, "sprCFRE", 0xBB5127CE },
    { 0x0135FAF0, "sprGAME", 0xFC26A882 },
    { 0x01360890, "sprCGER", 0x3CC34CA8 },
    { 0x01360910, "sprCSPA", 0x94E7E0CE },
    { 0x01360960, "sprBOX",  0x65DF85B6 },
    { 0x01360B00, "sprMENU", 0xA208A39D },
    { 0x01360E70, "sprMJAP", 0x5EA60CE0 },
    { 0x01361480, "sprMENG", 0x51B1D2D5 },
    { 0x01361BA0, "texDC0",  0x5436974C },
    { 0x01363710, "sprMFRE", 0x5B8E50FD },
    { 0x01363750, "sprMGER", 0xDC1C3B9B },
    { 0x01363790, "sprMSPA", 0x743897FD },
    { 0x013637E0, "sprCOMM", 0x77019AED },
    { 0x01363CA0, "sprPAL",  0xFE240B5B },
    { 0x01363F00, "sprPENG", 0xF3C25D97 },
    { 0x01364160, "sprPFRE", 0xF9FDDFBF },
    { 0x013644A0, "sprPGER", 0x7E6FB4D9 },
    { 0x01364500, "sprPSPA", 0xD64B18BF },
    { 0x01364560, "sprGENG", 0x3E0C32B1 },
    { 0x013645C0, "sprGJAP", 0x311BEC84 },
    { 0x01364620, "texDC1",  0x2331A7DA },
    { 0x01365B30, "texDC2",  0xBA38F660 },
    { 0x01366F80, "texDC3",  0xCD3FC6F6 }
};

static bool IsFontArchive(const std::string& name)
{
    static const char* fontNames[] = {
        "sprCENG", "sprCFRE", "sprCGER", "sprCJAP", "sprCSPA",
        "sprMENG", "sprMFRE", "sprMGER", "sprMJAP", "sprMSPA",
        "sprPENG", "sprPFRE", "sprPGER", "sprPSPA",
        "sprGENG", "sprGJAP"
    };
    for (const char* fn : fontNames)
    {
        if (name == fn) return true;
    }
    return false;
}

static void GetCategoryInfo(const std::string& archiveName, std::string& outCat, std::string& outSubfolder)
{
    if (IsFontArchive(archiveName))
    {
        outCat = "2D_FONT";
        outSubfolder = "Fonts";
    }
    else if (archiveName == "texDC0")
    {
        outCat = "3D_OBJECT";
        outSubfolder = "3D_Objects";
    }
    else if (archiveName == "texDC1" || archiveName == "texDC2" || archiveName == "texDC3")
    {
        outCat = "3D_STAGE";
        outSubfolder = "3D_Stages";
    }
    else if (archiveName.rfind("spr", 0) == 0)
    {
        outCat = "2D_SPRITE";
        outSubfolder = "2D_Sprites";
    }
    else
    {
        outCat = "2D_UI";
        outSubfolder = "2D_UI";
    }
}

struct ManifestItem
{
    std::string category;
    std::string subfolder;
    std::string relativePath;
    std::string archive;
    uint32_t chunkIndex;
    uint32_t gbix;
    std::string gbixHex;
    std::string origFormat;
    uint32_t origFormatId;
    uint32_t width;
    uint32_t height;
    uint32_t crc32;
    std::string crc32Hex;
    uint32_t dataOffset;
    uint32_t dataSize;
    std::string filename;
    uint32_t extractedPngSize;
    // H2: baseline of the textures/ PNG the extractor itself created
    // (0 = none: the file already existed and may be a user edit).
    uint32_t texBaselineSize = 0;
    uint32_t texBaselineCrc = 0;
};

#pragma pack(push, 1)
struct ManifestHeaderBin
{
    char magic[4];        // "LTM1"
    uint32_t version;     // 1
    uint32_t entryCount;
    uint32_t stringTableLen;
};

struct ManifestEntryRecordBin
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
#pragma pack(pop)

// H2: baseline sidecar written next to texture_manifest.bin. It records the exact
// size + CRC32 of every textures/ PNG the extractor ITSELF created, so the engine's
// ScanModifiedTextures can distinguish untouched HD defaults from user edits by
// content instead of comparing against the pristine originals/ size (which made
// ~1870/1926 default files count as "user-customized"). Deliberately a separate
// file, NOT part of the LTM1 bin manifest: the bin record layout is mirrored by a
// struct in TextureEngine.h that this fix does not own, so old engines ignore the
// sidecar and the engine keeps the legacy rawDataSize rule as fallback when it is
// absent. Layout: BaselineSidecarHeader, then entryCount BaselineSidecarEntry
// structs, then all filenames concatenated in entry order (no NUL terminators;
// entry i's name starts at the sum of nameLen[0..i-1]). Entries are keyed by
// CRC32 of the manifest filename; the name itself is stored so the engine can
// verify the key.
#pragma pack(push, 1)
struct BaselineSidecarHeader { char magic[4]; uint32_t version; uint32_t entryCount; }; // "LTMB", 1
struct BaselineSidecarEntry { uint32_t nameCrc; uint32_t nameLen; uint32_t baselineSize; uint32_t baselineCrc; };
#pragma pack(pop)

// Save vanilla texture and preserve user modification if already present
static void SaveExtractedTexture(
    const std::string& texturesDir,
    const std::string& originalsDir,
    const std::string& subfolder,
    const std::string& filename,
    const uint32_t* pixels,
    uint32_t w,
    uint32_t h,
    uint32_t& outSavedSize,
    uint32_t& outTexBaselineSize,
    uint32_t& outTexBaselineCrc)
{
    std::string texSubDir = texturesDir + "\\" + subfolder;
    std::string origSubDir = originalsDir + "\\" + subfolder;
    EnsureDirExists(texSubDir);
    EnsureDirExists(origSubDir);

    std::string origPath = origSubDir + "\\" + filename;
    std::string texPath = texSubDir + "\\" + filename;

    // 1. Always write pristine unscaled 1:1 vanilla copy to originals/
    SavePixelsToPng(origPath, pixels, w, h);

    // 2. Only write to textures/ if it doesn't already exist (preserve user customizations!)
    // Automatically upscales to HD dimensions (up to 1024p max, +2 POT levels) per Modding Guide
    //
    // H2 baseline rule: record size+CRC32 of the textures/ file ONLY when THIS call
    // created it (the existence guard below doubles as the baseline guard). Never
    // baseline a pre-existing file — it may be a surviving user edit, and freezing it
    // as the baseline would make later edits to the same file permanently invisible.
    // Never use the payload/originals size as the baseline either: the default
    // textures/ file is the HD upscale whose encoded size differs from the pristine
    // original — exactly the mismatch that made unmodified files count as modified.
    if (GetFileAttributesA(texPath.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        uint32_t targetW = w;
        uint32_t targetH = h;
        CalculateTargetHdResolution(w, h, targetW, targetH);
        if (SaveUpscaledPixelsToPng(texPath, pixels, w, h, targetW, targetH))
        {
            std::vector<uint8_t> texBytes = ReadBinaryFile(texPath);
            outTexBaselineSize = (uint32_t)texBytes.size();
            outTexBaselineCrc = CalcCRC32(texBytes.data(), texBytes.size());
        }
    }

    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (GetFileAttributesExA(origPath.c_str(), GetFileExInfoStandard, &fad))
    {
        outSavedSize = fad.nFileSizeLow;
    }
    else
    {
        outSavedSize = w * h * 4;
    }
}

struct SegaTableRawEntry
{
    uint32_t w, h, fmt, relOff;
};

static size_t RvaToFileOffset(const uint8_t* pExe, size_t exeSize, uint32_t rva)
{
    if (!pExe || exeSize < 0x200) return 0;
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)pExe;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || (size_t)dos->e_lfanew + sizeof(IMAGE_NT_HEADERS32) > exeSize) return 0;
    const IMAGE_NT_HEADERS32* nt = (const IMAGE_NT_HEADERS32*)(pExe + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;

    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++)
    {
        if (rva >= sec->VirtualAddress && rva < sec->VirtualAddress + sec->Misc.VirtualSize)
        {
            return sec->PointerToRawData + (rva - sec->VirtualAddress);
        }
    }
    return 0;
}

static int SafeReadSegaTable(
    uint32_t tableVA,
    SegaTableRawEntry* outEntries,
    int maxEntries,
    const std::vector<uint8_t>& exeFallbackBytes = {})
{
    int count = 0;

    // 1. Try reading from active process memory
    __try
    {
        const char* curr = (const char*)tableVA;
        while (count < maxEntries)
        {
            uint32_t dims = *(const uint32_t*)curr;
            uint32_t fmt = *(const uint32_t*)(curr + 4);
            uint32_t pData = *(const uint32_t*)(curr + 8);
            uint32_t pTex = *(const uint32_t*)(curr + 12);

            if (dims == 0xFFFFFFFF && fmt == 0xFFFFFFFF) break;
            if (pData < 0x0CB40000 || pData > 0x0E000000) break;

            outEntries[count].w = dims & 0xFFFF;
            outEntries[count].h = (dims >> 16) & 0xFFFF;
            outEntries[count].fmt = fmt;
            outEntries[count].relOff = pData - 0x0CB40000;
            count++;
            curr += 16;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        count = 0;
    }

    // 2. If memory read returned empty/failed, fallback to reading mapped PE on disk
    if (count == 0 && !exeFallbackBytes.empty() && tableVA >= 0x00400000)
    {
        uint32_t rva = tableVA - 0x00400000;
        size_t fileOff = RvaToFileOffset(exeFallbackBytes.data(), exeFallbackBytes.size(), rva);
        if (fileOff > 0 && fileOff + 16 <= exeFallbackBytes.size())
        {
            const char* curr = (const char*)(exeFallbackBytes.data() + fileOff);
            size_t maxLen = exeFallbackBytes.size() - fileOff;
            size_t readPos = 0;
            while (count < maxEntries && readPos + 16 <= maxLen)
            {
                uint32_t dims = *(const uint32_t*)(curr + readPos);
                uint32_t fmt = *(const uint32_t*)(curr + readPos + 4);
                uint32_t pData = *(const uint32_t*)(curr + readPos + 8);
                uint32_t pTex = *(const uint32_t*)(curr + readPos + 12);

                if (dims == 0xFFFFFFFF && fmt == 0xFFFFFFFF) break;
                if (pData < 0x0CB40000 || pData > 0x0E000000) break;

                outEntries[count].w = dims & 0xFFFF;
                outEntries[count].h = (dims >> 16) & 0xFFFF;
                outEntries[count].fmt = fmt;
                outEntries[count].relOff = pData - 0x0CB40000;
                count++;
                readPos += 16;
            }
        }
    }

    return count;
}

// ---------------------------------------------------------------------------------
// Phase 1: Textures Extraction
// ---------------------------------------------------------------------------------
static std::string Get2DUISubcategory(
    const std::string& archiveName,
    uint32_t chunkIdx,
    const std::string& origFilename,
    uint32_t w,
    uint32_t h)
{
    std::string fnLower = origFilename;
    std::transform(fnLower.begin(), fnLower.end(), fnLower.begin(), ::tolower);

    // 1. Fonts
    if (fnLower.find("font") != std::string::npos)
        return "Fonts";

    // 2. Logos & Legal / Publisher Screens
    if (fnLower.find("sega") != std::string::npos ||
        fnLower.find("criware") != std::string::npos ||
        fnLower.find("rp-t") != std::string::npos)
    {
        return "Logos";
    }

    // 3. Loading Screens & Progress Elements
    if (fnLower.find("load_screen") != std::string::npos ||
        fnLower.find("load_bar") != std::string::npos ||
        fnLower.find("load_dot") != std::string::npos)
    {
        return "Loading_Screens";
    }

    // 4. Instruction Manuals & Controls
    if (fnLower.find("manual") != std::string::npos)
        return "Manuals_and_Controls";

    // 5. JPK Archive Specific Rules
    if (archiveName == "common_jpk")
    {
        if (chunkIdx == 0) return "Buttons";           // 64x32 menu arrow/button
        if (chunkIdx == 1) return "Buttons";           // 512x32 options bar
        if (chunkIdx == 2 || chunkIdx == 3 || chunkIdx == 6 || chunkIdx == 7)
            return "Loading_Screens";                 // Background / load screens
        if (chunkIdx == 4 || chunkIdx == 5)
            return "HUD_and_Game";                    // Controller / pedals
        if (chunkIdx >= 8 && chunkIdx <= 27)
            return "HUD_and_Game";                    // Minimap & game icons
        if (chunkIdx >= 28 && chunkIdx <= 44)
            return "HUD_and_Game";                    // 1024x1024 Store billboards
        if (chunkIdx >= 45)
            return "Buttons";                         // 128x32 UI arrows
    }
    else if (archiveName.find("_jpk") != std::string::npos)
    {
        // Language JPKs (eng, fre, ger, ita, jap, spa)
        if ((chunkIdx >= 5 && chunkIdx <= 11) || (chunkIdx >= 39 && chunkIdx <= 42))
            return "Buttons";                         // 64x32 / 96x32 Arcade, Options, Exit, etc.
        if (chunkIdx >= 22 && chunkIdx <= 29)
            return "Buttons";                         // 512x32 Option menu bars
        if (chunkIdx == 31)
            return "Loading_Screens";                 // 640x480 Loading screen
        if (chunkIdx >= 12 && chunkIdx <= 16)
            return "Manuals_and_Controls";            // 1024x512 Instruction slides
        if (chunkIdx == 3 || chunkIdx == 4 || chunkIdx == 34 || chunkIdx == 35)
            return "Menus_and_Banners";               // 128x256 UI side strips
        if (chunkIdx <= 2 || (chunkIdx >= 17 && chunkIdx <= 21) || chunkIdx == 30 || (chunkIdx >= 32 && chunkIdx <= 33) || (chunkIdx >= 36 && chunkIdx <= 38))
            return "Menus_and_Banners";               // 640x128 Header banners
        if (chunkIdx >= 43)
            return "Menus_and_Banners";               // Results, Game Over & Records
    }

    // 6. Generic dimension fallbacks
    if (h == 32 && (w == 64 || w == 96 || w == 512))
        return "Buttons";
    if (w == 640 && (h == 480 || h == 360))
        return "Loading_Screens";
    if (w == 1024 && h == 512)
        return "Manuals_and_Controls";

    return "Menus_and_Banners";
}

static bool DoExtractTextures(
    const std::string& gameDir,
    const std::string& modDir,
    std::vector<ManifestItem>& outManifestItems)
{
    std::string texturesDir = modDir + "\\textures";
    std::string originalsDir = modDir + "\\originals";
    EnsureDirExists(texturesDir);
    EnsureDirExists(originalsDir);

    const char* subdirs[] = { "3D_Objects", "3D_Stages", "2D_Sprites", "2D_UI", "Fonts" };
    for (const char* sd : subdirs)
    {
        EnsureDirExists(texturesDir + "\\" + sd);
        EnsureDirExists(originalsDir + "\\" + sd);
    }

    const char* uiSubdirs[] = { "Buttons", "Loading_Screens", "Manuals_and_Controls", "Menus_and_Banners", "HUD_and_Game", "Logos", "Fonts" };
    for (const char* uisd : uiSubdirs)
    {
        EnsureDirExists(texturesDir + "\\2D_UI\\" + uisd);
        EnsureDirExists(originalsDir + "\\2D_UI\\" + uisd);
    }

    // 1. Extract JPK packages from NewTexture\LITTLE_ENDIAN\*.jpk
    std::string jpkDir = gameDir + "\\NewTexture\\LITTLE_ENDIAN";
    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA((jpkDir + "\\*.jpk").c_str(), &fd);
    if (hFind != INVALID_HANDLE_VALUE)
    {
        do
        {
            std::string jpkFileName = fd.cFileName;
            std::string jpkPath = jpkDir + "\\" + jpkFileName;
            std::string baseName = jpkFileName.substr(0, jpkFileName.find_last_of('.'));
            std::string archiveName = baseName + "_jpk";
            std::string cat, subfolder;
            GetCategoryInfo(archiveName, cat, subfolder);

            std::vector<uint8_t> data = ReadBinaryFile(jpkPath);
            if (data.empty()) continue;

            ExtractorProgressScreen::Update(0, 100, "Extracting UI: " + jpkFileName);

            size_t pos = 0;
            uint32_t chunkIdx = 0;
            while (pos + 24 <= data.size())
            {
                uint32_t magic = *(const uint32_t*)(data.data() + pos);
                if (magic != 0x7364644A /* Jdds */ && magic != 0x676E704A /* Jpng */)
                    break;

                uint32_t sz = *(const uint32_t*)(data.data() + pos + 4);
                uint32_t texId = *(const uint32_t*)(data.data() + pos + 8);
                uint32_t w = *(const uint32_t*)(data.data() + pos + 12);
                uint32_t h = *(const uint32_t*)(data.data() + pos + 16);

                if (pos + 24 + sz > data.size()) break;

                const uint8_t* payload = data.data() + pos + 24;
                pos += 24 + sz;

                try
                {
                    uint32_t crc = CalcCRC32(payload, sz);
                    char crcHex[16], gbixHex[16];
                    sprintf_s(crcHex, "%08X", crc);
                    sprintf_s(gbixHex, "%08X", texId);

                    std::string origFmt = (magic == 0x676E704A) ? "PNG" : "DDS";
                    if (magic == 0x7364644A && sz >= 128)
                    {
                        uint32_t fcc = *(const uint32_t*)(payload + 84);
                        if (fcc == 0x31545844) origFmt = "DDS_DXT1";
                        else if (fcc == 0x33545844) origFmt = "DDS_DXT3";
                        else if (fcc == 0x35545844) origFmt = "DDS_DXT5";
                        else origFmt = "DDS_A8R8G8B8";
                    }

                    std::string uiSub = Get2DUISubcategory(archiveName, chunkIdx, "", w, h);
                    std::string targetSubfolder = "2D_UI\\" + uiSub;
                    std::string manifestSubfolder = "2D_UI/" + uiSub;

                    char fnBuf[256];
                    sprintf_s(fnBuf, "%s__%s__%04u__GBIX_%s__FMT_%s_%ux%u__HASH_%s.png",
                        cat.c_str(), archiveName.c_str(), chunkIdx, gbixHex, origFmt.c_str(), w, h, crcHex);
                    std::string filename = fnBuf;

                    uint32_t savedSize = 0;
                    // H2: baseline of the textures/ default file, set only when this run
                    // creates it (see SaveExtractedTexture for the exact guard rationale).
                    uint32_t texBaselineSize = 0;
                    uint32_t texBaselineCrc = 0;
                    if (magic == 0x676E704A /* Jpng: raw PNG */)
                    {
                        std::string texSubDir = texturesDir + "\\" + targetSubfolder;
                        std::string origSubDir = originalsDir + "\\" + targetSubfolder;
                        EnsureDirExists(texSubDir);
                        EnsureDirExists(origSubDir);

                        std::string origPath = origSubDir + "\\" + filename;
                        std::string texPath = texSubDir + "\\" + filename;

                        // 1. Pristine unscaled 1:1 vanilla copy saved directly to originals/
                        WriteBinaryFile(origPath, payload, sz);

                        // 2. Automatically upscale to HD dimensions (up to 1024p) in textures/
                        if (GetFileAttributesA(texPath.c_str()) == INVALID_FILE_ATTRIBUTES)
                        {
                            int wlen = MultiByteToWideChar(CP_UTF8, 0, origPath.c_str(), -1, NULL, 0);
                            std::wstring worig(wlen, 0);
                            MultiByteToWideChar(CP_UTF8, 0, origPath.c_str(), -1, &worig[0], wlen);

                            Bitmap bmp(worig.c_str());
                            if (bmp.GetLastStatus() == Ok)
                            {
                                uint32_t srcW = bmp.GetWidth();
                                uint32_t srcH = bmp.GetHeight();
                                uint32_t targetW = srcW, targetH = srcH;
                                CalculateTargetHdResolution(srcW, srcH, targetW, targetH);

                                std::vector<uint32_t> pixels(srcW * srcH);
                                Rect rect(0, 0, srcW, srcH);
                                BitmapData bd;
                                if (bmp.LockBits(&rect, ImageLockModeRead, PixelFormat32bppARGB, &bd) == Ok)
                                {
                                    for (uint32_t y = 0; y < srcH; y++)
                                    {
                                        memcpy(&pixels[y * srcW], (const uint8_t*)bd.Scan0 + (y * bd.Stride), srcW * 4);
                                    }
                                    bmp.UnlockBits(&bd);
                                    if (SaveUpscaledPixelsToPng(texPath, pixels.data(), srcW, srcH, targetW, targetH))
                                    {
                                        std::vector<uint8_t> texBytes = ReadBinaryFile(texPath);
                                        texBaselineSize = (uint32_t)texBytes.size();
                                        texBaselineCrc = CalcCRC32(texBytes.data(), texBytes.size());
                                    }
                                }
                            }
                        }
                        savedSize = sz;
                    }
                    else // Jdds: decode DDS to PNG
                    {
                        uint32_t decW = 0, decH = 0;
                        std::vector<uint32_t> pixels;
                        if (DecodeDDS(payload, sz, decW, decH, pixels))
                        {
                            // Invert vertically so UI elements are upright
                            std::vector<uint32_t> uprightPixels(decW * decH);
                            for (uint32_t y = 0; y < decH; y++)
                            {
                                memcpy(&uprightPixels[y * decW], &pixels[(decH - 1 - y) * decW], decW * sizeof(uint32_t));
                            }
                            SaveExtractedTexture(texturesDir, originalsDir, targetSubfolder, filename, uprightPixels.data(), decW, decH, savedSize, texBaselineSize, texBaselineCrc);
                        }
                    }

                    ManifestItem item;
                    item.category = cat;
                    item.subfolder = manifestSubfolder;
                    item.relativePath = manifestSubfolder + "/" + filename;
                    item.archive = archiveName;
                    item.chunkIndex = chunkIdx;
                    item.gbix = texId;
                    item.gbixHex = gbixHex;
                    item.origFormat = origFmt;
                    item.origFormatId = (magic == 0x7364644A) ? 1 : 2;
                    item.width = w;
                    item.height = h;
                    item.crc32 = crc;
                    item.crc32Hex = crcHex;
                    item.dataOffset = (uint32_t)(pos - sz);
                    item.dataSize = sz;
                    item.filename = filename;
                    item.extractedPngSize = savedSize;
                    item.texBaselineSize = texBaselineSize;
                    item.texBaselineCrc = texBaselineCrc;
                    outManifestItems.push_back(item);
                }
                catch (const std::exception& e)
                {
                    Logger::LogWarning("[EXTRACTOR] Skipped corrupted JPK entry %d in %s: %s", chunkIdx, jpkFileName.c_str(), e.what());
                }
                catch (...)
                {
                    Logger::LogWarning("[EXTRACTOR] Skipped corrupted JPK entry %d in %s (unknown exception)", chunkIdx, jpkFileName.c_str());
                }

                chunkIdx++;
            }
        } while (FindNextFileA(hFind, &fd));
        FindClose(hFind);
    }

    // 2. Extract standalone DDS & PNG in NewTexture\*.dds, NewTexture\*.png
    std::string newTexDir = gameDir + "\\NewTexture";
    hFind = FindFirstFileA((newTexDir + "\\*.*").c_str(), &fd);
    if (hFind != INVALID_HANDLE_VALUE)
    {
        uint32_t chunkIdx = 0;
        do
        {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            std::string fn = fd.cFileName;
            std::string ext = fn.substr(fn.find_last_of('.'));
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext != ".dds" && ext != ".png") continue;

            std::string p = newTexDir + "\\" + fn;
            std::vector<uint8_t> raw = ReadBinaryFile(p);
            if (raw.empty()) continue;

            uint32_t crc = CalcCRC32(raw.data(), raw.size());
            char crcHex[16], gbixHex[16];
            sprintf_s(crcHex, "%08X", crc);
            sprintf_s(gbixHex, "%08X", chunkIdx);

            uint32_t w = 0, h = 0;
            std::vector<uint32_t> pixels;
            std::string origFmt = (ext == ".dds") ? "DDS" : "PNG";

            if (ext == ".dds")
            {
                if (!DecodeDDS(raw.data(), raw.size(), w, h, pixels)) continue;
                // Vertical flip for standalone DDS so they are right-side up on disk:
                std::vector<uint32_t> uprightPixels(w * h);
                for (uint32_t y = 0; y < h; y++)
                {
                    memcpy(&uprightPixels[y * w], &pixels[(h - 1 - y) * w], w * sizeof(uint32_t));
                }
                pixels = std::move(uprightPixels);
            }
            else
            {
                int wlen = MultiByteToWideChar(CP_UTF8, 0, p.c_str(), -1, NULL, 0);
                std::wstring wp(wlen, 0);
                MultiByteToWideChar(CP_UTF8, 0, p.c_str(), -1, &wp[0], wlen);
                Gdiplus::Bitmap bmp(wp.c_str());
                if (bmp.GetLastStatus() != Gdiplus::Ok) continue;
                w = bmp.GetWidth();
                h = bmp.GetHeight();
                pixels.resize(w * h);
                Gdiplus::Rect r(0, 0, w, h);
                Gdiplus::BitmapData bd;
                bmp.LockBits(&r, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &bd);
                for (uint32_t y = 0; y < h; y++)
                {
                    memcpy(&pixels[y * w], (const uint8_t*)bd.Scan0 + (y * bd.Stride), w * 4);
                }
                bmp.UnlockBits(&bd);
            }

            std::string uiSub = Get2DUISubcategory("NewTexture", chunkIdx, fn, w, h);
            std::string targetSubfolder = "2D_UI\\" + uiSub;
            std::string manifestSubfolder = "2D_UI/" + uiSub;

            char fnBuf[256];
            sprintf_s(fnBuf, "2D_UI__NewTexture__%04u__GBIX_%s__FMT_%s_%ux%u__HASH_%s.png",
                chunkIdx, gbixHex, origFmt.c_str(), w, h, crcHex);
            std::string filename = fnBuf;

            uint32_t savedSize = 0;
            // H2: baseline locals — filled only when SaveExtractedTexture creates the file.
            uint32_t texBaselineSize = 0;
            uint32_t texBaselineCrc = 0;
            SaveExtractedTexture(texturesDir, originalsDir, targetSubfolder, filename, pixels.data(), w, h, savedSize, texBaselineSize, texBaselineCrc);

            ManifestItem item;
            item.category = "2D_UI";
            item.subfolder = manifestSubfolder;
            item.relativePath = manifestSubfolder + "/" + filename;
            item.archive = "NewTexture";
            item.chunkIndex = chunkIdx;
            item.gbix = chunkIdx;
            item.gbixHex = gbixHex;
            item.origFormat = origFmt;
            item.origFormatId = (ext == ".dds") ? 1 : 3;
            item.width = w;
            item.height = h;
            item.crc32 = crc;
            item.crc32Hex = crcHex;
            item.dataOffset = 0;
            item.dataSize = (uint32_t)raw.size();
            item.filename = filename;
            item.extractedPngSize = savedSize;
            item.texBaselineSize = texBaselineSize;
            item.texBaselineCrc = texBaselineCrc;
            outManifestItems.push_back(item);

            chunkIdx++;
        } while (FindNextFileA(hFind, &fd));
        FindClose(hFind);
    }

    // 3. Extract Sega Containers (26 Tables in Crazy Taxi.exe + data_little\*.bin)
    std::string exePath = gameDir + "\\Crazy Taxi.exe";
    std::vector<uint8_t> exeFallbackBytes = ReadBinaryFile(exePath);

    int totalSega = sizeof(s_SegaTables) / sizeof(s_SegaTables[0]);
    for (int ti = 0; ti < totalSega; ti++)
    {
        const SegaTableInfo& tbl = s_SegaTables[ti];
        std::string cat, subfolder;
        GetCategoryInfo(tbl.name, cat, subfolder);

        ExtractorProgressScreen::Update(ti + 1, totalSega, "Extracting Sega Textures: " + std::string(tbl.name) + ".bin");

        std::string binPath = gameDir + "\\data_little\\" + std::string(tbl.name) + ".bin";
        std::vector<uint8_t> binBytes = ReadBinaryFile(binPath);
        if (binBytes.empty()) continue;

        // Read table directly from process memory if within mapped range, fallback to exe file
        SegaTableRawEntry rawEntries[2048];
        int numEntries = SafeReadSegaTable(tbl.tableVA, rawEntries, 2048, exeFallbackBytes);
        if (numEntries <= 0)
        {
            Logger::LogError("Failed or empty Sega Table %s at VA 0x%08X", tbl.name, tbl.tableVA);
            continue;
        }

        for (int chunkIdx = 0; chunkIdx < numEntries; chunkIdx++)
        {
            try
            {
                uint32_t w = rawEntries[chunkIdx].w;
                uint32_t h = rawEntries[chunkIdx].h;
                uint32_t fmt = rawEntries[chunkIdx].fmt;
                uint32_t relOff = rawEntries[chunkIdx].relOff;

                if (w == 0 || h == 0 || w > 2048 || h > 2048)
                    continue;

                uint32_t nextOff = (chunkIdx + 1 < numEntries) ?
                    rawEntries[chunkIdx + 1].relOff : (uint32_t)binBytes.size();

                if (nextOff <= relOff)
                {
                    nextOff = min((uint32_t)binBytes.size(), relOff + w * h * 2);
                }
                if (relOff >= binBytes.size() || nextOff > binBytes.size() || relOff >= nextOff)
                    continue;

                size_t chunkLen = nextOff - relOff;
                const uint8_t* chunkData = binBytes.data() + relOff;

                uint32_t crc = CalcCRC32(chunkData, chunkLen);
                char crcHex[16], gbixHex[16];
                sprintf_s(crcHex, "%08X", crc);
                sprintf_s(gbixHex, "%08X", relOff);

                int pfmtEnum = fmt & 0xFF;
                std::string pfmtName = "RGB565";
                if (pfmtEnum == 0) pfmtName = "ARGB1555";
                else if (pfmtEnum == 2) pfmtName = "ARGB4444";
                else pfmtEnum = 1;

                uint32_t dfmt = (fmt >> 8) & 0xFF;
                if (dfmt == 0x05 || dfmt == 0x07)
                {
                    // Palette chunk without palette data - skip as per engine specification
                    continue;
                }

                size_t uncomp = (size_t)w * h * 2;
                bool isVq = (dfmt == 0x03 || dfmt == 0x04 || dfmt == 0x10 || dfmt == 0x11) ||
                            (chunkLen < uncomp && chunkLen >= 2048 && w >= 4 && h >= 4);
                bool isVqMm = (dfmt == 0x04 || dfmt == 0x11) || (isVq && chunkLen > 2048 + (w / 2) * (h / 2));
                bool isTwidMm = (dfmt == 0x02);
                bool isLinear = (dfmt == 0x00 || dfmt == 0x09 || dfmt == 0x0B);

                std::string layoutName = isVq ? "VQ" : (isLinear ? "LIN" : "TWID");
                std::string origFmt = layoutName + "_" + pfmtName;

                char fnBuf[256];
                sprintf_s(fnBuf, "%s__%s__%04u__GBIX_%s__FMT_%s_%ux%u__HASH_%s.png",
                    cat.c_str(), tbl.name, chunkIdx, gbixHex, origFmt.c_str(), w, h, crcHex);
                std::string filename = fnBuf;

                std::vector<uint32_t> rawPixels;
                bool decoded = false;
                if (isVq)
                {
                    decoded = DecodeVQTexture(chunkData, chunkLen, w, h, pfmtEnum, isVqMm, rawPixels);
                }
                else if (chunkLen >= uncomp)
                {
                    if (isLinear)
                        decoded = DecodeLinear16bpp(chunkData, chunkLen, w, h, pfmtEnum, rawPixels);
                    else
                        decoded = DecodeTwiddled16bpp(chunkData, chunkLen, w, h, pfmtEnum, isTwidMm, rawPixels);
                }
                else
                {
                    decoded = Decode8bpp(chunkData, chunkLen, w, h, rawPixels);
                }

                if (decoded && rawPixels.size() == w * h)
                {
                    // CRITICAL ORIENTATION CONTRACT FIX:
                    // Flip vertically so that extracted PNG files on disk are right-side-up (human-readable)!
                    std::vector<uint32_t> uprightPixels(w * h);
                    for (uint32_t y = 0; y < h; y++)
                    {
                        memcpy(&uprightPixels[y * w], &rawPixels[(h - 1 - y) * w], w * sizeof(uint32_t));
                    }

                    uint32_t savedSize = 0;
                    uint32_t texBaselineSize = 0;
                    uint32_t texBaselineCrc = 0;
                    SaveExtractedTexture(texturesDir, originalsDir, subfolder, filename, uprightPixels.data(), w, h, savedSize, texBaselineSize, texBaselineCrc);

                    ManifestItem item;
                    item.category = cat;
                    item.subfolder = subfolder;
                    item.relativePath = subfolder + "/" + filename;
                    item.archive = tbl.name;
                    item.chunkIndex = chunkIdx;
                    item.gbix = relOff;
                    item.gbixHex = gbixHex;
                    item.origFormat = origFmt;
                    item.origFormatId = 4;
                    item.width = w;
                    item.height = h;
                    item.crc32 = crc;
                    item.crc32Hex = crcHex;
                    item.dataOffset = relOff;
                    item.dataSize = (uint32_t)chunkLen;
                    item.filename = filename;
                    item.extractedPngSize = savedSize;
                    item.texBaselineSize = texBaselineSize;
                    item.texBaselineCrc = texBaselineCrc;
                    outManifestItems.push_back(item);
                }
            }
            catch (const std::exception& e)
            {
                Logger::LogWarning("[EXTRACTOR] Skipped corrupted Sega chunk %d in %s: %s", chunkIdx, tbl.name, e.what());
            }
            catch (...)
            {
                Logger::LogWarning("[EXTRACTOR] Skipped corrupted Sega chunk %d in %s (unknown exception)", chunkIdx, tbl.name);
            }
        }
    }

    // 4. Generate Binary Manifest (texture_manifest.bin) & Debug JSON (texture_manifest.json)
    std::string stringTable;
    std::vector<ManifestEntryRecordBin> records;
    records.reserve(outManifestItems.size());

    for (const auto& item : outManifestItems)
    {
        uint32_t strOff = (uint32_t)stringTable.size();
        stringTable.append(item.filename);
        stringTable.push_back('\0');

        ManifestEntryRecordBin rec;
        rec.crc32 = item.crc32;
        rec.gbix = item.gbix;
        rec.archiveId = StringCRC32(item.archive);
        rec.chunkIndex = item.chunkIndex;
        rec.dataOffset = item.dataOffset;
        rec.origWidth = item.width;
        rec.origHeight = item.height;
        rec.origFormat = item.origFormatId;
        rec.filenameOffset = strOff;
        rec.rawDataSize = item.extractedPngSize;
        records.push_back(rec);
    }

    ManifestHeaderBin header;
    memcpy(header.magic, "LTM1", 4);
    header.version = 1;
    header.entryCount = (uint32_t)records.size();
    header.stringTableLen = (uint32_t)stringTable.size();

    std::vector<uint8_t> binManifestData;
    binManifestData.resize(sizeof(header) + records.size() * sizeof(ManifestEntryRecordBin) + stringTable.size());
    uint8_t* pDest = binManifestData.data();
    memcpy(pDest, &header, sizeof(header));
    pDest += sizeof(header);
    if (!records.empty())
    {
        memcpy(pDest, records.data(), records.size() * sizeof(ManifestEntryRecordBin));
        pDest += records.size() * sizeof(ManifestEntryRecordBin);
    }
    if (!stringTable.empty())
    {
        memcpy(pDest, stringTable.data(), stringTable.size());
    }

    WriteBinaryFile(texturesDir + "\\texture_manifest.bin", binManifestData.data(), binManifestData.size());
    WriteBinaryFile(modDir + "\\texture_manifest.bin", binManifestData.data(), binManifestData.size());

    // H2: write the baseline sidecar consumed by TextureEngine::ScanModifiedTextures.
    // Only entries whose textures/ file was CREATED by this run carry a baseline;
    // pre-existing files (possible user edits) intentionally get none. The engine
    // falls back to the legacy rawDataSize rule for records missing from this table.
    {
        struct BaselineItem {
            BaselineSidecarEntry entry;
            std::string filename;
        };
        std::vector<BaselineItem> baseItems;
        size_t totalBytes = sizeof(BaselineSidecarHeader);

        for (const auto& it : outManifestItems)
        {
            if (it.texBaselineSize == 0)
                continue; // file pre-existed -> no baseline (may be a user edit)
            BaselineItem bi;
            bi.entry.nameCrc = CalcCRC32(it.filename.data(), it.filename.size());
            bi.entry.nameLen = (uint32_t)it.filename.size();
            bi.entry.baselineSize = it.texBaselineSize;
            bi.entry.baselineCrc = it.texBaselineCrc;
            bi.filename = it.filename;
            totalBytes += sizeof(BaselineSidecarEntry) + bi.filename.size();
            baseItems.push_back(bi);
        }

        BaselineSidecarHeader bh;
        memcpy(bh.magic, "LTMB", 4);
        bh.version = 1;
        bh.entryCount = (uint32_t)baseItems.size();

        std::vector<uint8_t> baseData;
        baseData.resize(totalBytes);
        uint8_t* pBase = baseData.data();
        memcpy(pBase, &bh, sizeof(bh));
        pBase += sizeof(bh);
        for (const auto& bi : baseItems)
        {
            memcpy(pBase, &bi.entry, sizeof(BaselineSidecarEntry));
            pBase += sizeof(BaselineSidecarEntry);
            if (!bi.filename.empty())
            {
                memcpy(pBase, bi.filename.data(), bi.filename.size());
                pBase += bi.filename.size();
            }
        }
        WriteBinaryFile(texturesDir + "\\texture_baselines.bin", baseData.data(), baseData.size());
        Logger::LogInfo("Baseline sidecar written: %zu entries (textures/ defaults created by this extraction).", baseItems.size());
    }

    // Generate JSON manifest
    std::ostringstream json;
    json << "{\n  \"version\": 2,\n  \"total_textures\": " << outManifestItems.size() << ",\n  \"textures\": [\n";
    for (size_t i = 0; i < outManifestItems.size(); i++)
    {
        const auto& it = outManifestItems[i];
        json << "    {\n"
             << "      \"category\": \"" << it.category << "\",\n"
             << "      \"subfolder\": \"" << it.subfolder << "\",\n"
             << "      \"relative_path\": \"" << it.relativePath << "\",\n"
             << "      \"archive\": \"" << it.archive << "\",\n"
             << "      \"chunk_index\": " << it.chunkIndex << ",\n"
             << "      \"gbix\": " << it.gbix << ",\n"
             << "      \"gbix_hex\": \"" << it.gbixHex << "\",\n"
             << "      \"orig_format\": \"" << it.origFormat << "\",\n"
             << "      \"orig_format_id\": " << it.origFormatId << ",\n"
             << "      \"width\": " << it.width << ",\n"
             << "      \"height\": " << it.height << ",\n"
             << "      \"crc32\": " << it.crc32 << ",\n"
             << "      \"crc32_hex\": \"" << it.crc32Hex << "\",\n"
             << "      \"data_offset\": " << it.dataOffset << ",\n"
             << "      \"data_size\": " << it.dataSize << ",\n"
             << "      \"filename\": \"" << it.filename << "\",\n"
             << "      \"extracted_png_size\": " << it.extractedPngSize << ",\n"
             << "      \"textures_baseline_size\": " << it.texBaselineSize << ",\n"
             << "      \"textures_baseline_crc\": " << it.texBaselineCrc << "\n"
             << "    }" << ((i + 1 < outManifestItems.size()) ? ",\n" : "\n");
    }
    json << "  ]\n}\n";

    std::string jsonStr = json.str();
    WriteStringFile(texturesDir + "\\texture_manifest.json", jsonStr);
    WriteStringFile(modDir + "\\texture_manifest.json", jsonStr);

    Logger::LogInfo("Extracted %zu textures into %s", outManifestItems.size(), texturesDir.c_str());
    return true;
}

// ---------------------------------------------------------------------------------
// Phase 2: 3D Models & Multi-Part Assembly Extraction
// ---------------------------------------------------------------------------------
struct Vertex32
{
    float x, y, z;
    float nx, ny, nz;
    float u, v;
};

struct Triangle
{
    int v0, v1, v2;
};

struct Submesh
{
    int submeshIndex;
    int textureIndex;
    std::vector<Vertex32> vertices;
    std::vector<Triangle> triangles;
};

static std::vector<Submesh> ParseModelMesh(const uint8_t* binBytes, size_t binLen, size_t meshOffset)
{
    std::vector<Submesh> submeshes;
    size_t pos = meshOffset + 0x18; // skip 0x18 chunk header

    while (pos + 0x50 <= binLen)
    {
        int32_t first = *(const int32_t*)(binBytes + pos);
        if (first == 0) break; // terminator
        if (first > 0) break;  // invalid record start

        uint32_t texIdx = *(const uint32_t*)(binBytes + pos + 0x20);
        uint32_t blobSize = *(const uint32_t*)(binBytes + pos + 0x4C);

        if (blobSize == 0)
        {
            pos += 0x50;
            continue;
        }
        if (blobSize > 0x200000 || pos + 0x50 + blobSize > binLen) break;

        size_t cur = pos + 0x50;
        size_t blobEnd = cur + blobSize;

        std::vector<Vertex32> verts;
        std::unordered_map<size_t, int> addr2idx;
        std::vector<Triangle> tris;

        while (cur + 8 <= blobEnd)
        {
            uint32_t fmt = *(const uint32_t*)(binBytes + cur);
            uint32_t count = *(const uint32_t*)(binBytes + cur + 4);
            cur += 8;

            bool isTriList = (fmt & 8) != 0;
            uint32_t entries = isTriList ? (count * 3) : count;
            int secStart = (int)verts.size();

            for (uint32_t i = 0; i < entries; i++)
            {
                if (cur >= blobEnd) break;
                if (binBytes[cur] & 1)
                {
                    // inline 32-byte vertex
                    if (cur + 32 > blobEnd) break;
                    const float* flt = (const float*)(binBytes + cur);
                    Vertex32 v;
                    v.x = flt[0]; v.y = flt[1]; v.z = flt[2];
                    v.nx = flt[3]; v.ny = flt[4]; v.nz = flt[5];
                    v.u = flt[6]; v.v = flt[7];
                    addr2idx[cur] = (int)verts.size();
                    verts.push_back(v);
                    cur += 32;
                }
                else
                {
                    // 8-byte reference
                    if (cur + 8 > blobEnd) break;
                    int32_t rel = *(const int32_t*)(binBytes + cur + 4);
                    size_t tgt = cur + 8 + rel;
                    auto it = addr2idx.find(tgt);
                    if (it != addr2idx.end())
                    {
                        verts.push_back(verts[it->second]);
                    }
                    else if (!verts.empty())
                    {
                        verts.push_back(verts.back());
                    }
                    cur += 8;
                }
            }

            if (isTriList)
            {
                for (int t = secStart; t + 2 < (int)verts.size(); t += 3)
                {
                    tris.push_back({ t, t + 1, t + 2 });
                }
            }
            else
            {
                for (int t = secStart; t + 2 < (int)verts.size(); t++)
                {
                    if ((t - secStart) % 2 == 0)
                        tris.push_back({ t, t + 1, t + 2 });
                    else
                        tris.push_back({ t + 1, t, t + 2 });
                }
            }
        }

        if (!verts.empty() && !tris.empty())
        {
            Submesh s;
            s.submeshIndex = (int)submeshes.size();
            s.textureIndex = (texIdx == 0xFFFFFFFF) ? -1 : (int)texIdx;
            s.vertices = std::move(verts);
            s.triangles = std::move(tris);
            submeshes.push_back(std::move(s));
        }

        pos += 0x50 + ((blobSize >> 2) << 2);
    }

    return submeshes;
}

struct PairHash
{
    size_t operator()(const std::pair<std::string, int>& p) const
    {
        return std::hash<std::string>()(p.first) ^ (std::hash<int>()(p.second) << 1);
    }
};

static void ExportSubmeshesToObj(
    const std::vector<Submesh>& submeshes,
    const std::string& outObjPath,
    const std::string& outMtlPath,
    const std::string& objName,
    const std::unordered_map<std::pair<std::string, int>, std::string, struct PairHash>& texMap,
    const std::string& archiveName = "texDC0",
    float offX = 0.0f, float offY = 0.0f, float offZ = 0.0f)
{
    size_t totalTris = 0;
    size_t totalVerts = 0;
    for (const auto& sm : submeshes)
    {
        totalTris += sm.triangles.size();
        totalVerts += sm.vertices.size();
    }

    if (totalTris == 0 || totalVerts == 0)
    {
        DeleteFileA(outObjPath.c_str());
        DeleteFileA(outMtlPath.c_str());
        return;
    }

    std::ofstream obj(outObjPath);
    if (!obj.is_open()) return;

    std::string mtlFilename = outMtlPath.substr(outMtlPath.find_last_of("\\/") + 1);
    obj << "# Crazy Taxi OBJ Exporter - LyrasTextureMod\n";
    obj << "mtllib " << mtlFilename << "\n\n";

    std::unordered_map<std::string, std::string> writtenMaterials;
    int vOffset = 1;

    for (size_t si = 0; si < submeshes.size(); si++)
    {
        const auto& sm = submeshes[si];
        std::string groupName = objName + "_Submesh_" + std::to_string(si);
        obj << "g " << groupName << "\n";

        char matName[64];
        if (sm.textureIndex >= 0)
            sprintf_s(matName, "mat_%s_%d", archiveName.c_str(), sm.textureIndex);
        else
            sprintf_s(matName, "mat_untextured");

        obj << "usemtl " << matName << "\n";

        if (writtenMaterials.find(matName) == writtenMaterials.end())
        {
            std::string texFile = "";
            auto it = texMap.find({ archiveName, sm.textureIndex });
            if (it != texMap.end()) texFile = it->second;
            writtenMaterials[matName] = texFile;
        }

        for (const auto& v : sm.vertices)
        {
            obj << "v " << std::fixed << std::setprecision(4)
                << (v.x + offX) << " " << (v.y + offY) << " " << (v.z + offZ) << "\n";
            obj << "vn " << std::fixed << std::setprecision(4)
                << v.nx << " " << v.ny << " " << v.nz << "\n";
            obj << "vt " << std::fixed << std::setprecision(4)
                << v.u << " " << v.v << "\n";
        }

        for (const auto& tri : sm.triangles)
        {
            int i0 = vOffset + tri.v0;
            int i1 = vOffset + tri.v1;
            int i2 = vOffset + tri.v2;
            obj << "f " << i0 << "/" << i0 << "/" << i0 << " "
                        << i1 << "/" << i1 << "/" << i1 << " "
                        << i2 << "/" << i2 << "/" << i2 << "\n";
        }

        vOffset += (int)sm.vertices.size();
        obj << "\n";
    }

    std::ofstream mtl(outMtlPath);
    if (mtl.is_open())
    {
        mtl << "# Crazy Taxi Material Library - LyrasTextureMod\n\n";
        for (const auto& pair : writtenMaterials)
        {
            mtl << "newmtl " << pair.first << "\n";
            mtl << "Ka 1.000 1.000 1.000\n";
            mtl << "Kd 1.000 1.000 1.000\n";
            mtl << "Ks 0.000 0.000 0.000\n";
            mtl << "d 1.0\n";
            mtl << "illum 1\n";
            if (!pair.second.empty())
            {
                mtl << "map_Kd " << pair.second << "\n";
            }
            mtl << "\n";
        }
    }
}

struct AssembledPart
{
    std::string name;
    std::vector<Submesh> submeshes;
    float offX, offY, offZ;
};

static void ExportAssembledObj(
    const std::vector<AssembledPart>& parts,
    const std::string& outObjPath,
    const std::string& outMtlPath,
    const std::string& modelName,
    const std::unordered_map<std::pair<std::string, int>, std::string, struct PairHash>& texMap,
    const std::string& archiveName = "texDC0")
{
    size_t totalTris = 0;
    size_t totalVerts = 0;
    for (const auto& p : parts)
    {
        for (const auto& sm : p.submeshes)
        {
            totalTris += sm.triangles.size();
            totalVerts += sm.vertices.size();
        }
    }

    if (totalTris == 0 || totalVerts == 0)
    {
        DeleteFileA(outObjPath.c_str());
        DeleteFileA(outMtlPath.c_str());
        return;
    }

    std::ofstream obj(outObjPath);
    if (!obj.is_open()) return;

    std::string mtlFilename = outMtlPath.substr(outMtlPath.find_last_of("\\/") + 1);
    obj << "# Crazy Taxi Assembled Model - LyrasTextureMod\n";
    obj << "mtllib " << mtlFilename << "\n\n";

    std::unordered_map<std::string, std::string> writtenMaterials;
    int vOffset = 1;

    for (const auto& part : parts)
    {
        for (size_t si = 0; si < part.submeshes.size(); si++)
        {
            const auto& sm = part.submeshes[si];
            std::string groupName = part.name + "_" + std::to_string(si);
            obj << "g " << groupName << "\n";

            char matName[64];
            if (sm.textureIndex >= 0)
                sprintf_s(matName, "mat_%s_%d", archiveName.c_str(), sm.textureIndex);
            else
                sprintf_s(matName, "mat_untextured");

            obj << "usemtl " << matName << "\n";

            if (writtenMaterials.find(matName) == writtenMaterials.end())
            {
                std::string texFile = "";
                auto it = texMap.find({ archiveName, sm.textureIndex });
                if (it != texMap.end()) texFile = it->second;
                writtenMaterials[matName] = texFile;
            }

            for (const auto& v : sm.vertices)
            {
                obj << "v " << std::fixed << std::setprecision(4)
                    << (v.x + part.offX) << " " << (v.y + part.offY) << " " << (v.z + part.offZ) << "\n";
                obj << "vn " << std::fixed << std::setprecision(4)
                    << v.nx << " " << v.ny << " " << v.nz << "\n";
                obj << "vt " << std::fixed << std::setprecision(4)
                    << v.u << " " << v.v << "\n";
            }

            for (const auto& tri : sm.triangles)
            {
                int i0 = vOffset + tri.v0;
                int i1 = vOffset + tri.v1;
                int i2 = vOffset + tri.v2;
                obj << "f " << i0 << "/" << i0 << "/" << i0 << " "
                            << i1 << "/" << i1 << "/" << i1 << " "
                            << i2 << "/" << i2 << "/" << i2 << "\n";
            }

            vOffset += (int)sm.vertices.size();
            obj << "\n";
        }
    }

    std::ofstream mtl(outMtlPath);
    if (mtl.is_open())
    {
        mtl << "# Crazy Taxi Material Library - LyrasTextureMod\n\n";
        for (const auto& pair : writtenMaterials)
        {
            mtl << "newmtl " << pair.first << "\n";
            mtl << "Ka 1.000 1.000 1.000\n";
            mtl << "Kd 1.000 1.000 1.000\n";
            mtl << "Ks 0.000 0.000 0.000\n";
            mtl << "d 1.0\n";
            mtl << "illum 1\n";
            if (!pair.second.empty())
            {
                mtl << "map_Kd " << pair.second << "\n";
            }
            mtl << "\n";
        }
    }
}

static bool DoExtract3DModels(
    const std::string& gameDir,
    const std::string& modDir,
    const std::vector<ManifestItem>& manifestItems)
{
    std::string modelsDir = modDir + "\\3D";
    std::string partsDir = modelsDir + "\\parts";
    std::string assembledDir = modelsDir + "\\assembled";
    std::string stagesDir = modelsDir + "\\stages";
    std::string charsDir = modelsDir + "\\characters";

    EnsureDirExists(modelsDir);
    EnsureDirExists(partsDir);
    EnsureDirExists(assembledDir);
    EnsureDirExists(stagesDir);
    EnsureDirExists(charsDir);

    std::string poldc0Path = gameDir + "\\data_little\\polDC0.bin";
    std::vector<uint8_t> poldc0 = ReadBinaryFile(poldc0Path);
    if (poldc0.empty()) return false;

    // Build texture map (archive, chunk_idx) -> relativePath
    std::unordered_map<std::pair<std::string, int>, std::string, PairHash> texMap;
    for (const auto& item : manifestItems)
    {
        texMap[{ item.archive, (int)item.chunkIndex }] = "../../textures/" + item.relativePath;
    }

    // Structure for reverse texture-to-model index
    std::unordered_map<std::string, std::vector<std::tuple<std::string, std::string, std::string>>> texToModels;

    auto RegisterTex = [&](const std::string& texFn, const std::string& modelName, const std::string& modelFile, const std::string& partLabel) {
        if (texFn.empty()) return;
        texToModels[texFn].push_back({ modelName, modelFile, partLabel });
    };

    // 1. Player Taxis (Axel, BDJoe, Gena, Gus)
    struct CabDef
    {
        const char* name;
        size_t body;
        size_t steering;
        size_t driver;
        size_t mirrorL;
        size_t mirrorR;
        size_t wheelFL, wheelFR, wheelRL, wheelRR;
        float fl[3], fr[3], rl[3], rr[3];
    };

    static const CabDef cabs[] = {
        { "Axel_Taxi",  0x000038E8, 0x00021220, 0x00027C10, 0x00020F20, 0x000210A0, 0x000115F0, 0x00012540, 0x000134F0, 0x00014440,
          { 9.42f, 3.0f, 13.02f }, { -9.44f, 3.0f, 13.05f }, { 8.76f, 3.0f, -19.51f }, { -8.77f, 3.0f, -19.38f } },
        { "BDJoe_Taxi", 0x00030CE8, 0x0004E628, 0x00049180, 0x0004E328, 0x0004E4A8, 0x000115F0, 0x00012540, 0x000134F0, 0x00014440,
          { 9.18f, 3.0f, 12.86f }, { -9.17f, 3.0f, 12.64f }, { 9.10f, 3.0f, -20.55f }, { -9.07f, 3.0f, -20.72f } },
        { "Gena_Taxi",  0x0006F3C0, 0x00091D18, 0x0008A680, 0x00091A18, 0x00091B98, 0x0007FB50, 0x000809C8, 0x000818A0, 0x00082718,
          { 8.83f, 3.0f, 13.55f }, { -8.84f, 3.0f, 13.55f }, { 8.47f, 3.0f, -17.55f }, { -8.47f, 3.0f, -17.55f } },
        { "Gus_Taxi",   0x00051568, 0x0006D880, 0x00068330, 0x0006D580, 0x0006D700, 0x0005FA70, 0x000608E8, 0x000617C0, 0x00062638,
          { 8.85f, 3.0f, 14.30f }, { -8.85f, 3.0f, 14.30f }, { 8.48f, 3.0f, -19.72f }, { -8.48f, 3.0f, -19.72f } }
    };

    for (const auto& c : cabs)
    {
        ExtractorProgressScreen::Update(0, 100, "Extracting 3D: " + std::string(c.name));
        std::vector<AssembledPart> cabParts;

        auto ExtractPart = [&](size_t offset, const std::string& partName, float ox = 0, float oy = 0, float oz = 0) {
            try
            {
                std::vector<Submesh> subs = ParseModelMesh(poldc0.data(), poldc0.size(), offset);
                if (!subs.empty())
                {
                    ExportSubmeshesToObj(subs, partsDir + "\\" + std::string(c.name) + "_" + partName + ".obj",
                                         partsDir + "\\" + std::string(c.name) + "_" + partName + ".mtl",
                                         std::string(c.name) + "_" + partName, texMap, "texDC0");
                    cabParts.push_back({ partName, subs, ox, oy, oz });

                    for (const auto& sm : subs)
                    {
                        if (sm.textureIndex >= 0)
                        {
                            auto it = texMap.find({ "texDC0", sm.textureIndex });
                            if (it != texMap.end())
                            {
                                std::string fn = it->second.substr(it->second.find_last_of('/') + 1);
                                RegisterTex(fn, std::string(c.name) + "_Complete", "assembled/" + std::string(c.name) + "_Complete.obj", partName);
                            }
                        }
                    }
                }
            }
            catch (const std::exception& e)
            {
                Logger::LogWarning("[EXTRACTOR] Skipped corrupted 3D part '%s' in %s: %s", partName.c_str(), c.name, e.what());
            }
            catch (...)
            {
                Logger::LogWarning("[EXTRACTOR] Skipped corrupted 3D part '%s' in %s (unknown exception)", partName.c_str(), c.name);
            }
        };

        ExtractPart(c.body, "Body");
        ExtractPart(c.steering, "SteeringWheel");
        ExtractPart(c.driver, "Driver");
        ExtractPart(c.mirrorL, "Mirror_L");
        ExtractPart(c.mirrorR, "Mirror_R");
        ExtractPart(c.wheelFL, "Wheel_FL", c.fl[0], c.fl[1], c.fl[2]);
        ExtractPart(c.wheelFR, "Wheel_FR", c.fr[0], c.fr[1], c.fr[2]);
        ExtractPart(c.wheelRL, "Wheel_RL", c.rl[0], c.rl[1], c.rl[2]);
        ExtractPart(c.wheelRR, "Wheel_RR", c.rr[0], c.rr[1], c.rr[2]);

        std::string compName = std::string(c.name) + "_Complete";
        ExportAssembledObj(cabParts, assembledDir + "\\" + compName + ".obj",
                           assembledDir + "\\" + compName + ".mtl",
                           compName, texMap, "texDC0");

        // Write Assembly JSON
        std::ostringstream aj;
        aj << "{\n  \"name\": \"" << c.name << "\",\n  \"complete_model\": \"assembled/" << compName << ".obj\",\n  \"parts\": [\n";
        for (size_t pi = 0; pi < cabParts.size(); pi++)
        {
            const auto& p = cabParts[pi];
            aj << "    {\n      \"part_name\": \"" << p.name << "\",\n      \"file\": \"parts/" << c.name << "_" << p.name << ".obj\",\n"
               << "      \"offset\": [" << p.offX << ", " << p.offY << ", " << p.offZ << "]\n    }" << ((pi + 1 < cabParts.size()) ? ",\n" : "\n");
        }
        aj << "  ]\n}\n";
        WriteStringFile(modelsDir + "\\" + std::string(c.name) + "_assembly.json", aj.str());
    }

    // 2. Traffic Vehicles (12 authentic vehicles)
    struct TrafficDef
    {
        const char* name;
        size_t body;
        size_t wheels;
        float wf[3], wr[3];
    };

    static const TrafficDef traffic[] = {
        { "Traffic_Sedan_Blue",              0x0B1318, 0x0B36F0, { 0.0f, 3.27f, 16.0f }, { 0.0f, 3.27f, -16.0f } },
        { "Traffic_StationWagon",            0x0AD140, 0x0AF8F0, { 0.0f, 4.10f, 16.0f }, { 0.0f, 4.10f, -16.0f } },
        { "Traffic_Police_Car",              0x0B5080, 0x0B71E8, { 0.0f, 3.30f, 16.0f }, { 0.0f, 3.30f, -16.0f } },
        { "Traffic_Van",                     0x0A5A48, 0x0A4238, { 0.0f, 3.50f, 12.0f }, { 0.0f, 3.50f, -12.0f } },
        { "Traffic_Pickup_Truck",            0x0B8088, 0x0BA248, { 0.0f, 3.50f, 16.0f }, { 0.0f, 3.50f, -16.0f } },
        { "Traffic_City_Bus",                0x099AA8, 0x0980B0, { 0.0f, 5.70f, 28.0f }, { 0.0f, 5.70f, -28.0f } },
        { "Traffic_Sedan_Red",               0x0CF170, 0x0D1458, { 0.0f, 3.30f, 16.0f }, { 0.0f, 3.30f, -16.0f } },
        { "Traffic_Ambulance",               0x0BBF28, 0x0BE818, { 0.0f, 3.50f, 18.0f }, { 0.0f, 3.50f, -18.0f } },
        { "Traffic_Delivery_Truck",          0x0A1AB0, 0x0A4238, { 0.0f, 6.30f, 20.0f }, { 0.0f, 6.30f, -20.0f } },
        { "Traffic_Garbage_Truck",           0x0A9628, 0x0ABA38, { 0.0f, 4.00f, 18.0f }, { 0.0f, 4.00f, -18.0f } },
        { "Traffic_Highway_Coach",           0x096848, 0x0980B0, { 0.0f, 5.70f, 35.0f }, { 0.0f, 5.70f, -35.0f } },
        { "Traffic_Cable_Car_Assembled",     0x161328, 0x000000, { 0.0f, 0.0f, 0.0f },   { 0.0f, 0.0f, 0.0f } },
        { "Traffic_Cable_Car",               0x165DA0, 0x000000, { 0.0f, 0.0f, 0.0f },   { 0.0f, 0.0f, 0.0f } }
    };

    for (const auto& t : traffic)
    {
        ExtractorProgressScreen::Update(0, 100, "Extracting Traffic: " + std::string(t.name));
        std::vector<AssembledPart> tparts;

        std::vector<Submesh> bodySubs = ParseModelMesh(poldc0.data(), poldc0.size(), t.body);
        if (!bodySubs.empty())
        {
            ExportSubmeshesToObj(bodySubs, partsDir + "\\" + std::string(t.name) + "_Body.obj",
                                 partsDir + "\\" + std::string(t.name) + "_Body.mtl",
                                 std::string(t.name) + "_Body", texMap, "texDC0");
            tparts.push_back({ "Body", bodySubs, 0, 0, 0 });
            for (const auto& sm : bodySubs)
            {
                if (sm.textureIndex >= 0)
                {
                    auto it = texMap.find({ "texDC0", sm.textureIndex });
                    if (it != texMap.end())
                    {
                        std::string fn = it->second.substr(it->second.find_last_of('/') + 1);
                        RegisterTex(fn, std::string(t.name) + "_Complete", "assembled/" + std::string(t.name) + "_Complete.obj", "Body");
                    }
                }
            }
        }

        if (t.wheels != 0)
        {
            std::vector<Submesh> wheelSubs = ParseModelMesh(poldc0.data(), poldc0.size(), t.wheels);
            if (!wheelSubs.empty())
            {
                ExportSubmeshesToObj(wheelSubs, partsDir + "\\" + std::string(t.name) + "_Wheels.obj",
                                     partsDir + "\\" + std::string(t.name) + "_Wheels.mtl",
                                     std::string(t.name) + "_Wheels", texMap, "texDC0");
                tparts.push_back({ "Wheels_Front", wheelSubs, t.wf[0], t.wf[1], t.wf[2] });
                tparts.push_back({ "Wheels_Rear", wheelSubs, t.wr[0], t.wr[1], t.wr[2] });
            }
        }

        if (!tparts.empty())
        {
            std::string compName = std::string(t.name) + "_Complete";
            ExportAssembledObj(tparts, assembledDir + "\\" + compName + ".obj",
                               assembledDir + "\\" + compName + ".mtl",
                               compName, texMap, "texDC0");

            std::ostringstream aj;
            aj << "{\n  \"name\": \"" << t.name << "\",\n  \"complete_model\": \"assembled/" << compName << ".obj\",\n  \"parts\": [\n";
            for (size_t pi = 0; pi < tparts.size(); pi++)
            {
                const auto& p = tparts[pi];
                aj << "    {\n      \"part_name\": \"" << p.name << "\",\n      \"offset\": [" << p.offX << ", " << p.offY << ", " << p.offZ << "]\n    }"
                   << ((pi + 1 < tparts.size()) ? ",\n" : "\n");
            }
            aj << "  ]\n}\n";
            WriteStringFile(modelsDir + "\\" + std::string(t.name) + "_assembly.json", aj.str());
        }
    }

    // 3. Characters & Seated Drivers
    static const struct { const char* name; size_t off; } drivers[] = {
        { "Axel_Driver",  0x027C10 },
        { "BDJoe_Driver", 0x049180 },
        { "Gus_Driver",   0x068330 },
        { "Gena_Driver",  0x08A680 }
    };
    for (const auto& d : drivers)
    {
        std::vector<Submesh> subs = ParseModelMesh(poldc0.data(), poldc0.size(), d.off);
        if (!subs.empty())
        {
            ExportSubmeshesToObj(subs, charsDir + "\\" + std::string(d.name) + ".obj",
                                 charsDir + "\\" + std::string(d.name) + ".mtl",
                                 d.name, texMap, "texDC0");
            for (const auto& sm : subs)
            {
                if (sm.textureIndex >= 0)
                {
                    auto it = texMap.find({ "texDC0", sm.textureIndex });
                    if (it != texMap.end())
                    {
                        std::string fn = it->second.substr(it->second.find_last_of('/') + 1);
                        RegisterTex(fn, d.name, "characters/" + std::string(d.name) + ".obj", "Driver");
                    }
                }
            }
        }
    }

    // Individual Character Body Parts (Axel1..15, BDJoe1..15, Gena1..15, Gus1..8)
    struct CharacterPartList
    {
        const char* prefix;
        std::vector<size_t> offsets;
    };

    static const CharacterPartList charParts[] = {
        { "Axel",  { 0x1d9a18, 0x1db320, 0x1db6b8, 0x1db990, 0x1dcef8, 0x1dd290, 0x1dd568, 0x1ddeb8, 0x1defa8, 0x1df6b8, 0x1dffb0, 0x1e05b0, 0x1e0c60, 0x1e1558, 0x1e1bd0 } },
        { "BDJoe", { 0x1e8a80, 0x1ebc48, 0x1ec108, 0x1ec818, 0x1ed2b0, 0x1ee508, 0x1eec18, 0x1ef6b0, 0x1f0088, 0x1f0ac0, 0x1f1228, 0x1f1a88, 0x1f24b0, 0x1f2c00, 0x1f3448 } },
        { "Gena",  { 0x1061f8, 0x10c220, 0x10e180, 0x108170, 0x10d1d0, 0x10e6d8, 0x108760, 0x10a468, 0x107600, 0x10b580, 0x109a88, 0x107bb8, 0x10bbd0, 0x109f78, 0x108d50 } },
        { "Gus",   { 0x1fa510, 0x1fc0d8, 0x1fc460, 0x1fd310, 0x1fea80, 0x1fee08, 0x1ff390, 0x1ffd60 } }
    };

    // Remove legacy glitched merged character files if present
    const char* oldMerged[] = { "Axel_Character", "BDJoe_Character", "Gena_Character", "Gus_Character" };
    for (const char* om : oldMerged)
    {
        DeleteFileA((charsDir + "\\" + om + ".obj").c_str());
        DeleteFileA((charsDir + "\\" + om + ".mtl").c_str());
    }

    for (const auto& cp : charParts)
    {
        ExtractorProgressScreen::Update(0, 100, "Extracting Character Parts: " + std::string(cp.prefix));
        for (size_t pi = 0; pi < cp.offsets.size(); pi++)
        {
            std::string partName = std::string(cp.prefix) + std::to_string(pi + 1);
            std::vector<Submesh> subs = ParseModelMesh(poldc0.data(), poldc0.size(), cp.offsets[pi]);
            if (!subs.empty())
            {
                ExportSubmeshesToObj(subs, charsDir + "\\" + partName + ".obj",
                                     charsDir + "\\" + partName + ".mtl",
                                     partName, texMap, "texDC0");

                for (const auto& sm : subs)
                {
                    if (sm.textureIndex >= 0)
                    {
                        auto it = texMap.find({ "texDC0", sm.textureIndex });
                        if (it != texMap.end())
                        {
                            std::string fn = it->second.substr(it->second.find_last_of('/') + 1);
                            RegisterTex(fn, partName, "characters/" + partName + ".obj", partName);
                        }
                    }
                }
            }
        }
    }

    // 4. Props
    static const struct { const char* name; size_t off; } props[] = {
        { "Prop_City_Bench",    0x098958 },
        { "Prop_Fire_Hydrant",  0x098B60 },
        { "Prop_Street_Lamp",   0x098D68 },
        { "Prop_Traffic_Sign",  0x098F70 }
    };
    for (const auto& pr : props)
    {
        std::vector<Submesh> subs = ParseModelMesh(poldc0.data(), poldc0.size(), pr.off);
        if (!subs.empty())
        {
            ExportSubmeshesToObj(subs, charsDir + "\\" + std::string(pr.name) + ".obj",
                                 charsDir + "\\" + std::string(pr.name) + ".mtl",
                                 pr.name, texMap, "texDC0");
            for (const auto& sm : subs)
            {
                if (sm.textureIndex >= 0)
                {
                    auto it = texMap.find({ "texDC0", sm.textureIndex });
                    if (it != texMap.end())
                    {
                        std::string fn = it->second.substr(it->second.find_last_of('/') + 1);
                        RegisterTex(fn, pr.name, "characters/" + std::string(pr.name) + ".obj", "Prop");
                    }
                }
            }
        }
    }

    // 5. Stage Meshes (Stages 1, 2, 3)
    static const struct { const char* fn; const char* arch; const char* stgName; } stageFiles[] = {
        { "polDC1.bin", "texDC1", "Stage1_Arcade" },
        { "polDC2.bin", "texDC2", "Stage2_Original" },
        { "polDC3.bin", "texDC3", "Stage3_CrazyBox" }
    };

    for (const auto& sf : stageFiles)
    {
        ExtractorProgressScreen::Update(0, 100, "Extracting Stage: " + std::string(sf.stgName));
        std::string sp = gameDir + "\\data_little\\" + sf.fn;
        std::vector<uint8_t> sBytes = ReadBinaryFile(sp);
        if (sBytes.empty()) continue;

        std::string sOutDir = stagesDir + "\\" + sf.stgName;
        EnsureDirExists(sOutDir);

        size_t off = 0;
        int meshIdx = 0;
        size_t n = sBytes.size();

        while (off + 0x68 <= n)
        {
            uint32_t f0 = *(const uint32_t*)(sBytes.data() + off);
            uint32_t f4 = *(const uint32_t*)(sBytes.data() + off + 4);
            bool jumped = false;
            if (f0 == 1 && (f4 == 1 || f4 == 3))
            {
                int32_t first = *(const int32_t*)(sBytes.data() + off + 0x18);
                uint32_t blob = *(const uint32_t*)(sBytes.data() + off + 0x4C + 0x18);
                if (first < 0 && blob > 0 && blob <= 0x200000 && off + 0x68 + blob <= n)
                {
                    std::vector<Submesh> subs = ParseModelMesh(sBytes.data(), n, off);
                    size_t totalTris = 0;
                    for (const auto& sm : subs) totalTris += sm.triangles.size();

                    if (!subs.empty() && totalTris >= 4)
                    {
                        char mname[64];
                        sprintf_s(mname, "%s_Mesh_%03d", sf.stgName, meshIdx++);
                        ExportSubmeshesToObj(subs, sOutDir + "\\" + mname + ".obj",
                                             sOutDir + "\\" + mname + ".mtl",
                                             mname, texMap, sf.arch);

                        for (const auto& sm : subs)
                        {
                            if (sm.textureIndex >= 0)
                            {
                                auto it = texMap.find({ sf.arch, sm.textureIndex });
                                if (it != texMap.end())
                                {
                                    std::string fn = it->second.substr(it->second.find_last_of('/') + 1);
                                    RegisterTex(fn, mname, "stages/" + std::string(sf.stgName) + "/" + mname + ".obj", "Stage");
                                }
                            }
                        }
                    }

                    // Compute chunk size to advance past the chunk rather than stepping 4 bytes
                    size_t cpos = off + 0x18;
                    while (cpos + 0x50 <= n)
                    {
                        int32_t cf = *(const int32_t*)(sBytes.data() + cpos);
                        if (cf == 0 || cf > 0) break;
                        uint32_t cb = *(const uint32_t*)(sBytes.data() + cpos + 0x4C);
                        if (cb > 0x200000 || cpos + 0x50 + cb > n) break;
                        cpos += 0x50 + ((cb >> 2) << 2);
                    }
                    if (cpos > off + 0x18)
                    {
                        off = (cpos + 7) & ~7;
                        jumped = true;
                    }
                }
            }
            if (!jumped)
            {
                off += 4;
            }
        }
    }

    // Write 3D_texture_to_model_index.json
    std::ostringstream t2m;
    t2m << "{\n";
    size_t ti = 0;
    for (const auto& pair : texToModels)
    {
        t2m << "  \"" << pair.first << "\": {\n"
            << "    \"texture_filename\": \"" << pair.first << "\",\n";

        std::string primaryModel = "";
        std::string primaryFile = "";
        std::string primaryCategory = "3D_OBJECT";
        if (!pair.second.empty())
        {
            primaryModel = std::get<0>(pair.second[0]);
            primaryFile = std::get<1>(pair.second[0]);
            primaryCategory = (primaryFile.find("stages/") != std::string::npos) ? "3D_STAGE" : "3D_OBJECT";
        }

        t2m << "    \"model_name\": \"" << primaryModel << "\",\n"
            << "    \"model_file\": \"" << primaryFile << "\",\n"
            << "    \"category\": \"" << primaryCategory << "\",\n"
            << "    \"models\": [\n";

        for (size_t mi = 0; mi < pair.second.size(); mi++)
        {
            auto [mName, mFile, pLabel] = pair.second[mi];
            t2m << "      {\n"
                << "        \"model_name\": \"" << mName << "\",\n"
                << "        \"model_file\": \"" << mFile << "\",\n"
                << "        \"part_label\": \"" << pLabel << "\",\n"
                << "        \"category\": \"" << ((mFile.find("stages/") != std::string::npos) ? "3D_STAGE" : "3D_OBJECT") << "\"\n"
                << "      }" << ((mi + 1 < pair.second.size()) ? ",\n" : "\n");
        }

        t2m << "    ]\n  }" << ((++ti < texToModels.size()) ? ",\n" : "\n");
    }
    t2m << "}\n";

    std::string t2mStr = t2m.str();
    WriteStringFile(modelsDir + "\\3D_texture_to_model_index.json", t2mStr);
    WriteStringFile(modDir + "\\3D_texture_to_model_index.json", t2mStr);

    // Write 3D\extracted_manifest.json recording vanilla file sizes and write timestamps
    std::ostringstream em;
    em << "{\n";
    std::vector<std::string> subdirs = { "parts", "characters", "assembled" };
    bool firstEntry = true;
    for (const auto& sd : subdirs)
    {
        std::string sdir = modelsDir + "\\" + sd;
        WIN32_FIND_DATAA wfd;
        HANDLE hFind = FindFirstFileA((sdir + "\\*.obj").c_str(), &wfd);
        if (hFind != INVALID_HANDLE_VALUE)
        {
            do
            {
                if (!(wfd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                {
                    if (!firstEntry) em << ",\n";
                    firstEntry = false;
                    std::string key = sd + "/" + wfd.cFileName;
                    em << "  \"" << key << "\": {\n"
                       << "    \"size\": " << wfd.nFileSizeLow << ",\n"
                       << "    \"time_low\": " << wfd.ftLastWriteTime.dwLowDateTime << ",\n"
                       << "    \"time_high\": " << wfd.ftLastWriteTime.dwHighDateTime << "\n"
                       << "  }";
                }
            } while (FindNextFileA(hFind, &wfd));
            FindClose(hFind);
        }
    }
    em << "\n}\n";
    WriteStringFile(modelsDir + "\\extracted_manifest.json", em.str());

    Logger::LogInfo("3D Model extraction complete! Exported to %s", modelsDir.c_str());
    return true;
}

// ---------------------------------------------------------------------------------
// Phase 3: Spline Routes Extraction
// ---------------------------------------------------------------------------------
struct SplineDef
{
    int id;
    const char* name;
    size_t offset;
    int maxWaypoints;
    float defaultSpeed;
};

static const SplineDef s_SplineRoutes[] = {
    { 10, "Route_10_P_Cable",       0x096848, 35, 12.5f },
    { 11, "Route_11_P_RailRoad_A",  0x09E518, 19, 25.0f },
    { 12, "Route_12_P_RailRoad_B",  0x0A0698, 25, 25.0f },
    { 13, "Route_13_P_RailRoad_C",  0x0A1AB0, 21, 25.0f }
};

static bool DoExtractSplines(const std::string& gameDir, const std::string& modDir)
{
    std::string splinesDir = modDir + "\\splines";
    EnsureDirExists(splinesDir);

    std::string poldc0Path = gameDir + "\\data_little\\polDC0.bin";
    std::vector<uint8_t> poldc0 = ReadBinaryFile(poldc0Path);
    if (poldc0.empty()) return false;

    for (const auto& r : s_SplineRoutes)
    {
        if (r.offset + (size_t)r.maxWaypoints * 32 > poldc0.size()) continue;

        std::ostringstream json;
        json << "{\n  \"route_id\": " << r.id << ",\n  \"route_name\": \"" << r.name << "\",\n  \"waypoints\": [\n";

        for (int i = 0; i < r.maxWaypoints; i++)
        {
            size_t off = r.offset + i * 32;
            uint32_t flags = *(const uint32_t*)(poldc0.data() + off);
            uint32_t segId = *(const uint32_t*)(poldc0.data() + off + 4);
            float x = *(const float*)(poldc0.data() + off + 12);
            float y = *(const float*)(poldc0.data() + off + 16);
            float z = *(const float*)(poldc0.data() + off + 20);
            float speed = *(const float*)(poldc0.data() + off + 24);
            uint32_t nextLink = *(const uint32_t*)(poldc0.data() + off + 28);

            json << "    {\n"
                 << "      \"index\": " << i << ",\n"
                 << "      \"flags\": " << flags << ",\n"
                 << "      \"segment_id\": " << segId << ",\n"
                 << "      \"x\": " << std::fixed << std::setprecision(4) << x << ",\n"
                 << "      \"y\": " << std::fixed << std::setprecision(4) << y << ",\n"
                 << "      \"z\": " << std::fixed << std::setprecision(4) << z << ",\n"
                 << "      \"speed\": " << std::fixed << std::setprecision(4) << speed << ",\n"
                 << "      \"next_link\": " << nextLink << "\n"
                 << "    }" << ((i + 1 < r.maxWaypoints) ? ",\n" : "\n");
        }
        json << "  ]\n}\n";

        WriteStringFile(splinesDir + "\\" + r.name + ".json", json.str());
    }

    Logger::LogInfo("Spline routes extracted to %s", splinesDir.c_str());
    return true;
}

} // anonymous namespace

// ---------------------------------------------------------------------------------
// AssetExtractor Public API
// ---------------------------------------------------------------------------------
bool AssetExtractor::s_IsExtracting = false;

bool AssetExtractor::IsExtracting()
{
    return s_IsExtracting;
}

bool AssetExtractor::CheckAndRunAutoExtraction(const std::string& modDir, const std::string& gameDir)
{
    std::string dir3D = modDir + "\\3D";
    std::string dirSplines = modDir + "\\splines";
    std::string dirOriginals = modDir + "\\originals";
    std::string dirOriginal = modDir + "\\original";
    std::string dirTextures = modDir + "\\textures";

    bool has3D = IsDirNonEmpty(dir3D);
    bool hasSplines = IsDirNonEmpty(dirSplines);
    bool hasOriginals = IsDirNonEmpty(dirOriginals) || IsDirNonEmpty(dirOriginal);
    bool hasTextures = IsDirNonEmpty(dirTextures);

    if (has3D && hasSplines && hasOriginals && hasTextures)
    {
        Logger::LogInfo("Asset directories (3D, splines, originals, textures) exist and are populated. No extraction required.");
        return true;
    }

    Logger::LogInfo("Missing or empty asset folders detected! Initiating automatic first-time asset extraction...");
    return ExtractAllAssets(modDir, gameDir);
}

bool AssetExtractor::ExtractAllAssets(const std::string& modDir, const std::string& gameDir)
{
    s_IsExtracting = true;
    Logger::LogInfo("Starting Native Asset Extraction Pipeline. ModDir: %s, GameDir: %s", modDir.c_str(), gameDir.c_str());

    ULONG_PTR gdiplusToken = 0;
    GdiplusStartupInput gdiplusStartupInput;
    GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);

    ExtractorProgressScreen::Show(NULL, 100, "Lyra's Texture Mod - First-Time Setup");
    ExtractorProgressScreen::Update(5, 100, "Extracting textures and user interface...");

    std::vector<ManifestItem> manifestItems;
    bool texOk = DoExtractTextures(gameDir, modDir, manifestItems);

    ExtractorProgressScreen::Update(60, 100, "Extracting 3D models, vehicles, and stages...");
    bool modelsOk = DoExtract3DModels(gameDir, modDir, manifestItems);

    ExtractorProgressScreen::Update(90, 100, "Extracting spline routes...");
    bool splinesOk = DoExtractSplines(gameDir, modDir);

    ExtractorProgressScreen::Update(100, 100, "Extraction complete!");
    Sleep(500);
    ExtractorProgressScreen::Hide();

    GdiplusShutdown(gdiplusToken);

    s_IsExtracting = false;
    Logger::LogInfo("Asset extraction finished. Results: Textures=%d, Models=%d, Splines=%d", texOk, modelsOk, splinesOk);
    return texOk && modelsOk && splinesOk;
}
