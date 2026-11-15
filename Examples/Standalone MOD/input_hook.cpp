#include "input_hook.h"
#include "../deps/imgui/imgui.h"
#include <windows.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

bool g_bMenuOpen = false;
HWND g_hGameWnd = NULL;
static WNDPROC s_oWndProc = NULL;

void SetMenuOpenState(bool bOpen)
{
    g_bMenuOpen = bOpen;
    if (!ImGui::GetCurrentContext())
        return;

    ImGuiIO& io = ImGui::GetIO();
    io.ClearInputKeys();

    if (g_bMenuOpen)
    {
        // Force Windows hardware mouse cursor visible (leave io.MouseDrawCursor = false to prevent double-cursor and font atlas issues)
        io.MouseDrawCursor = false;
        ShowCursor(TRUE);
        ClipCursor(NULL);
    }
    else
    {
        // Hide mouse cursor completely and return focus to game
        io.MouseDrawCursor = false;
        ShowCursor(FALSE);
        if (g_hGameWnd)
        {
            SetFocus(g_hGameWnd);
        }
    }
}

void ToggleMenuOpenState()
{
    SetMenuOpenState(!g_bMenuOpen);
}

LRESULT CALLBACK ModWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    // Toggle Mod Menu on Shift + F1
    if (uMsg == WM_KEYDOWN && wParam == VK_F1)
    {
        if ((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0)
        {
            // Ignore typematic key repeats (bit 30 of lParam is 1 if the key was already down)
            if ((lParam & 0x40000000) == 0)
            {
                ToggleMenuOpenState();
            }
            return 0; // Always consume F1 when Shift is held
        }
    }

    if (g_bMenuOpen)
    {
        // Forward message to ImGui
        if (ImGui_ImplWin32_WndProcHandler(hWnd, uMsg, wParam, lParam))
        {
            return true;
        }

        ImGuiIO& io = ImGui::GetIO();

        // Block mouse events from reaching game when ImGui wants mouse or menu is open
        if (uMsg >= WM_MOUSEFIRST && uMsg <= WM_MOUSELAST)
        {
            return 0;
        }

        // Block keyboard events when ImGui wants keyboard input (e.g. typing coords)
        if (io.WantCaptureKeyboard && (uMsg >= WM_KEYFIRST && uMsg <= WM_KEYLAST))
        {
            return 0;
        }
    }

    if (s_oWndProc)
    {
        return CallWindowProcA(s_oWndProc, hWnd, uMsg, wParam, lParam);
    }

    return DefWindowProcA(hWnd, uMsg, wParam, lParam);
}

void InitInputHook(HWND hWnd)
{
    if (hWnd && !s_oWndProc)
    {
        g_hGameWnd = hWnd;
        s_oWndProc = (WNDPROC)SetWindowLongPtrA(hWnd, GWLP_WNDPROC, (LONG_PTR)ModWndProc);
    }
}

void ShutdownInputHook()
{
    if (g_hGameWnd && s_oWndProc)
    {
        SetWindowLongPtrA(g_hGameWnd, GWLP_WNDPROC, (LONG_PTR)s_oWndProc);
        s_oWndProc = NULL;
    }
}
