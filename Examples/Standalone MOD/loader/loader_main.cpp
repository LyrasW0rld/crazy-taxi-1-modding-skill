#define DIRECTINPUT_VERSION 0x0800
#include <initguid.h>
#include <windows.h>
#include <dinput.h>
#include <string>
#include <vector>
#include <stdio.h>

typedef void (*tProcessKeyboardBuffer)(BYTE* pBuffer);
static tProcessKeyboardBuffer s_pfnProcessKeyboardBuffer = nullptr;

typedef HRESULT (STDMETHODCALLTYPE *tGetDeviceState)(IDirectInputDevice8A* pThis, DWORD cbData, LPVOID lpvData);
static tGetDeviceState s_oGetDeviceState = nullptr;

typedef HRESULT (STDMETHODCALLTYPE *tCreateDeviceDI)(IDirectInput8A* pThis, REFGUID rguid, LPDIRECTINPUTDEVICE8A* lplpDirectInputDevice, LPUNKNOWN pUnkOuter);
static tCreateDeviceDI s_oCreateDeviceDI = nullptr;

static HRESULT STDMETHODCALLTYPE hkGetDeviceState(IDirectInputDevice8A* pThis, DWORD cbData, LPVOID lpvData)
{
    // Native arcade keyboard passthrough - WASD modification removed
    return s_oGetDeviceState(pThis, cbData, lpvData);
}

static HRESULT STDMETHODCALLTYPE hkCreateDeviceDI(IDirectInput8A* pThis, REFGUID rguid, LPDIRECTINPUTDEVICE8A* lplpDirectInputDevice, LPUNKNOWN pUnkOuter)
{
    HRESULT hr = s_oCreateDeviceDI(pThis, rguid, lplpDirectInputDevice, pUnkOuter);
    if (SUCCEEDED(hr) && lplpDirectInputDevice && *lplpDirectInputDevice)
    {
        void** pDevVT = *(void***)*lplpDirectInputDevice;
        if (pDevVT && pDevVT[9] != (void*)&hkGetDeviceState)
        {
            DWORD oldProt;
            if (VirtualProtect(&pDevVT[9], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProt))
            {
                if (!s_oGetDeviceState)
                    s_oGetDeviceState = (tGetDeviceState)pDevVT[9];
                pDevVT[9] = (void*)&hkGetDeviceState;
                VirtualProtect(&pDevVT[9], sizeof(void*), oldProt, &oldProt);
                OutputDebugStringA("[CrazyTaxi ModLoader] Successfully hooked IDirectInputDevice8::GetDeviceState\n");
            }
        }
    }
    return hr;
}

typedef HRESULT (WINAPI *tDirectInput8Create)(HINSTANCE hinst, DWORD dwVersion, REFIID riidltf, LPVOID *ppvOut, LPUNKNOWN punkOuter);
static tDirectInput8Create s_oDirectInput8Create = nullptr;

extern "C" HRESULT WINAPI DirectInput8Create(
    HINSTANCE hinst,
    DWORD dwVersion,
    REFIID riidltf,
    LPVOID *ppvOut,
    LPUNKNOWN punkOuter
)
{
    if (!s_oDirectInput8Create)
    {
        char sysPath[MAX_PATH];
        GetSystemDirectoryA(sysPath, MAX_PATH);
        strcat_s(sysPath, "\\dinput8.dll");
        HMODULE hOrig = LoadLibraryA(sysPath);
        if (hOrig)
        {
            s_oDirectInput8Create = (tDirectInput8Create)GetProcAddress(hOrig, "DirectInput8Create");
        }
    }

    if (s_oDirectInput8Create)
    {
        HRESULT hr = s_oDirectInput8Create(hinst, dwVersion, riidltf, ppvOut, punkOuter);
        if (SUCCEEDED(hr) && ppvOut && *ppvOut)
        {
            IDirectInput8A* pDI = (IDirectInput8A*)*ppvOut;
            void** pVTable = *(void***)pDI;
            if (pVTable && !s_oCreateDeviceDI)
            {
                DWORD oldProt;
                if (VirtualProtect(&pVTable[3], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProt))
                {
                    s_oCreateDeviceDI = (tCreateDeviceDI)pVTable[3];
                    pVTable[3] = (void*)&hkCreateDeviceDI;
                    VirtualProtect(&pVTable[3], sizeof(void*), oldProt, &oldProt);
                }
            }
        }
        return hr;
    }

    return E_FAIL;
}

static void LogLoader(const std::string& logPath, const std::string& message)
{
    FILE* fp = fopen(logPath.c_str(), "a");
    if (fp)
    {
        fprintf(fp, "%s\n", message.c_str());
        fclose(fp);
    }
}

static void LoadModsFromDirectory(const std::string& modsDir)
{
    std::string logPath = modsDir + "\\loader.log";
    // Overwrite log file for a fresh session
    FILE* fp = fopen(logPath.c_str(), "w");
    if (fp)
    {
        fprintf(fp, "=== Crazy Taxi Mod Loader v1.2 Started ===\n");
        fprintf(fp, "Mods Directory: %s\n\n", modsDir.c_str());
        fclose(fp);
    }

    int modsLoaded = 0;

    // 0. Synchronize DXVK (Vulkan Translation Layer) setting for 100% third-party mod compatibility
    std::string gameRootDir = modsDir;
    size_t lastSlash = gameRootDir.find_last_of("\\/");
    if (lastSlash != std::string::npos)
    {
        gameRootDir = gameRootDir.substr(0, lastSlash);
    }
    std::string pendingDel = gameRootDir + "\\d3d9.dll.del";
    if (GetFileAttributesA(pendingDel.c_str()) != INVALID_FILE_ATTRIBUTES)
    {
        DeleteFileA(pendingDel.c_str());
    }

    std::string cfgPath = modsDir + "\\Framework_MOD\\crazytaxi_mod_config.json";
    bool enableDXVK = false;
    FILE* fCfg = fopen(cfgPath.c_str(), "r");
    if (fCfg)
    {
        char cfgBuf[4096];
        size_t nRead = fread(cfgBuf, 1, sizeof(cfgBuf) - 1, fCfg);
        fclose(fCfg);
        cfgBuf[nRead] = '\0';
        if (strstr(cfgBuf, "\"enableDXVK\": true"))
        {
            enableDXVK = true;
        }
    }

    std::string d3d9Path = gameRootDir + "\\d3d9.dll";
    std::string dxvkSrc  = gameRootDir + "\\dxvk_optional\\d3d9.dll";
    if (!enableDXVK)
    {
        DWORD d3d9Attr = GetFileAttributesA(d3d9Path.c_str());
        if (d3d9Attr != INVALID_FILE_ATTRIBUTES && !(d3d9Attr & FILE_ATTRIBUTE_DIRECTORY))
        {
            if (DeleteFileA(d3d9Path.c_str()))
            {
                LogLoader(logPath, "[DXVK] Deactivated Vulkan layer (native D3D9 active for mod compatibility)");
            }
            else
            {
                MoveFileExA(d3d9Path.c_str(), pendingDel.c_str(), MOVEFILE_REPLACE_EXISTING);
                LogLoader(logPath, "[DXVK] Scheduled d3d9.dll removal for native D3D9 compatibility");
            }
        }
    }
    else
    {
        DWORD d3d9Attr = GetFileAttributesA(d3d9Path.c_str());
        if (d3d9Attr == INVALID_FILE_ATTRIBUTES)
        {
            if (CopyFileA(dxvkSrc.c_str(), d3d9Path.c_str(), FALSE))
            {
                LogLoader(logPath, "[DXVK] Activated Vulkan translation layer from dxvk_optional");
            }
        }
    }

    // 1. First priority: load our primary framework mod if present
    std::string frameworkDll = modsDir + "\\Framework_MOD\\Framework_MOD.dll";
    DWORD dwAttrib = GetFileAttributesA(frameworkDll.c_str());
    if (dwAttrib != INVALID_FILE_ATTRIBUTES && !(dwAttrib & FILE_ATTRIBUTE_DIRECTORY))
    {
        HMODULE hMod = LoadLibraryA(frameworkDll.c_str());
        if (hMod)
        {
            modsLoaded++;
            s_pfnProcessKeyboardBuffer = (tProcessKeyboardBuffer)GetProcAddress(hMod, "ProcessKeyboardBuffer");
            LogLoader(logPath, "[SUCCESS] Loaded Framework_MOD: " + frameworkDll);
            OutputDebugStringA(("[CrazyTaxi ModLoader] Successfully loaded Framework_MOD: " + frameworkDll + "\n").c_str());
        }
        else
        {
            DWORD err = GetLastError();
            char errBuf[128];
            sprintf_s(errBuf, " (Win32 Error: %lu)", err);
            LogLoader(logPath, "[ERROR] Failed to load Framework_MOD: " + frameworkDll + errBuf);
        }
    }
    else
    {
        LogLoader(logPath, "[WARNING] Framework_MOD.dll not found at: " + frameworkDll);
    }

    // Helper lambda to load a plugin DLL or ASI
    auto LoadPluginFile = [&](const std::string& filePath) {
        if (filePath.find("CT-DC.asi") != std::string::npos || filePath.find("SilentPatchCT.asi") != std::string::npos)
        {
            LogLoader(logPath, "[INFO] Skipped standalone ASI (handled natively inside Framework_MOD.dll)");
            return false;
        }

        HMODULE hMod = LoadLibraryA(filePath.c_str());
        if (hMod)
        {
            modsLoaded++;
            LogLoader(logPath, "[SUCCESS] Loaded Plugin Mod: " + filePath);
            OutputDebugStringA(("[CrazyTaxi ModLoader] Loaded plugin: " + filePath + "\n").c_str());
            return true;
        }
        else
        {
            DWORD err = GetLastError();
            char errBuf[128];
            sprintf_s(errBuf, " (Win32 Error: %lu)", err);
            LogLoader(logPath, "[ERROR] Failed to load plugin: " + filePath + errBuf);
            return false;
        }
    };

    // Helper lambda to search and load matching pattern
    auto SearchAndLoad = [&](const std::string& dir, const std::string& filter) {
        std::string searchMask = dir + "\\" + filter;
        WIN32_FIND_DATAA fd;
        HANDLE hFind = FindFirstFileA(searchMask.c_str(), &fd);
        if (hFind != INVALID_HANDLE_VALUE)
        {
            do
            {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                {
                    // Skip if proxy dinput8.dll
                    if (_stricmp(fd.cFileName, "dinput8.dll") == 0 ||
                        _stricmp(fd.cFileName, "steam_api.dll") == 0)
                        continue;

                    LoadPluginFile(dir + "\\" + fd.cFileName);
                }
            } while (FindNextFileA(hFind, &fd));
            FindClose(hFind);
        }
    };

    // 2. Discover and load all subfolder mods: mods\<ModFolder>\*.dll and *.asi
    std::string searchSubdirs = modsDir + "\\*";
    WIN32_FIND_DATAA fdSubdir;
    HANDLE hSubdir = FindFirstFileA(searchSubdirs.c_str(), &fdSubdir);
    if (hSubdir != INVALID_HANDLE_VALUE)
    {
        do
        {
            if ((fdSubdir.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                strcmp(fdSubdir.cFileName, ".") != 0 &&
                strcmp(fdSubdir.cFileName, "..") != 0)
            {
                if (_stricmp(fdSubdir.cFileName, "Framework_MOD") == 0)
                    continue;

                std::string subDirPath = modsDir + "\\" + fdSubdir.cFileName;
                SearchAndLoad(subDirPath, "*.dll");
                SearchAndLoad(subDirPath, "*.asi");
            }
        } while (FindNextFileA(hSubdir, &fdSubdir));
        FindClose(hSubdir);
    }

    // 3. Also load .asi plugins from game root directory (if any third-party plugins are present)
    size_t lastSlashRoot = modsDir.find_last_of("\\/");
    if (lastSlashRoot != std::string::npos)
    {
        std::string rootDirForAsi = modsDir.substr(0, lastSlashRoot);
        SearchAndLoad(rootDirForAsi, "*.asi");
    }

    char summary[128];
    sprintf_s(summary, "\n=== Mod Loading Complete (%d mod(s) active) ===\n", modsLoaded);
    LogLoader(logPath, summary);
}

static bool __cdecl Hooked_SteamAPI_RestartAppIfNecessary(unsigned int unOwnAppID)
{
    return false;
}

static void PatchSteamCheck()
{
    uintptr_t iatAddr = 0x0053432C;
    DWORD oldProtect;
    if (VirtualProtect((LPVOID)iatAddr, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect))
    {
        *(void**)iatAddr = (void*)&Hooked_SteamAPI_RestartAppIfNecessary;
        VirtualProtect((LPVOID)iatAddr, sizeof(void*), oldProtect, &oldProtect);
    }
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpReserved)
{
    if (fdwReason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hinstDLL);

        char exePath[MAX_PATH];
        GetModuleFileNameA(NULL, exePath, MAX_PATH);
        const char* exeName = strrchr(exePath, '\\');
        if (!exeName) exeName = strrchr(exePath, '/');
        if (exeName) exeName++;
        else exeName = exePath;

        // ONLY initialize mod loader for Crazy Taxi.exe!
        // AppLauncher.exe and any other tools must remain completely untouched!
        if (_stricmp(exeName, "Crazy Taxi.exe") != 0)
        {
            return TRUE;
        }

        // Bypass Steam restart requirement so the game can launch directly
        PatchSteamCheck();

        // Dynamically resolve game directory from running executable
        std::string gameDir(exePath);
        size_t lastSlash = gameDir.find_last_of("\\/");
        if (lastSlash != std::string::npos)
        {
            gameDir = gameDir.substr(0, lastSlash);
        }

        std::string modsDir = gameDir + "\\mods";
        std::string frameworkDir = modsDir + "\\Framework_MOD";

        // Create mods directory structure if it does not exist
        CreateDirectoryA(modsDir.c_str(), NULL);
        CreateDirectoryA(frameworkDir.c_str(), NULL);

        // Load installed mods
        LoadModsFromDirectory(modsDir);
    }
    return TRUE;
}
