#pragma once
#include <windows.h>

extern bool g_bMenuOpen;
extern HWND g_hGameWnd;

void InitInputHook(HWND hWnd);
void ShutdownInputHook();

void SetMenuOpenState(bool bOpen);
void ToggleMenuOpenState();

LRESULT CALLBACK ModWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
