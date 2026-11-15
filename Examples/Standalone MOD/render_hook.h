#pragma once
#include <d3d9.h>

bool InitRenderHook();
void ShutdownRenderHook();

void RenderPermanentCoordinateHUD();
void RenderModMenu();

extern "C" {
__declspec(dllexport) void* __cdecl GetFrameworkImGuiContext();
__declspec(dllexport) IDirect3DDevice9* __cdecl GetFrameworkD3D9Device();
__declspec(dllexport) void __cdecl SetFrameworkStatusMessage(const char* msg);
}
