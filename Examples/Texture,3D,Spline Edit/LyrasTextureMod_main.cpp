#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <d3d9.h>
#include "TextureEngine.h"
#include "ModelEngine.h"
#include "AssetExtractor.h"
#include "Logger.h"
#include <MinHook.h>
#include <stdio.h>   // sprintf_s for formatted log lines
#include <string.h>  // memcmp for GUID compare-then-write

// Original function pointers
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
static tD3DXCreateTextureFromFileInMemoryEx s_oD3DXCreateTextureFromFileInMemoryEx = nullptr;
static HANDLE s_hInitEvent = nullptr;

// ---------------------------------------------------------------------------------
// HOOK 1: D3DXCreateTextureFromFileInMemoryEx (Menu UI & JPK packages)
// ---------------------------------------------------------------------------------
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
    if (pDevice)
    {
        TextureEngine::Instance().SetD3DDevice(pDevice);
    }

    // If early boot textures are requested before background initialization is done,
    // wait for TextureEngine to finish indexing the manifest so custom bootscreens are replaced.
    // We pump messages so the game window stays responsive during first-time setup.
    if (s_hInitEvent && !TextureEngine::Instance().IsInitialized())
    {
        DWORD startTime = GetTickCount();
        while (WaitForSingleObject(s_hInitEvent, 50) == WAIT_TIMEOUT)
        {
            MSG msg;
            while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE))
            {
                if (msg.message == WM_QUIT)
                {
                    PostQuitMessage((int)msg.wParam);
                    return E_FAIL;
                }
                TranslateMessage(&msg);
                DispatchMessageA(&msg);
            }
            if (GetTickCount() - startTime > 120000)
            {
                Logger::LogWarning("Timeout waiting for TextureEngine initialization in hkD3DXCreateTextureFromFileInMemoryEx!");
                break;
            }
        }
    }

    // If TextureEngine is still uninitialized (e.g. timeout or disabled), safely pass through to vanilla game function
    if (!TextureEngine::Instance().IsInitialized())
    {
        if (s_oD3DXCreateTextureFromFileInMemoryEx)
        {
            return s_oD3DXCreateTextureFromFileInMemoryEx(
                pDevice, pSrcData, SrcDataSize, Width, Height, MipLevels, Usage,
                Format, Pool, Filter, MipFilter, ColorKey, pSrcInfo, pPalette, ppTexture
            );
        }
        return E_FAIL;
    }

    HRESULT hrCustom = TextureEngine::Instance().HandleD3DXCreateTextureFromFileInMemoryEx(
        pDevice, pSrcData, SrcDataSize, Width, Height, MipLevels, Usage,
        Format, Pool, Filter, MipFilter, ColorKey, pSrcInfo, pPalette, ppTexture
    );

    if (hrCustom == D3D_OK)
    {
        return D3D_OK;
    }

    if (s_oD3DXCreateTextureFromFileInMemoryEx)
    {
        return s_oD3DXCreateTextureFromFileInMemoryEx(
            pDevice, pSrcData, SrcDataSize, Width, Height, MipLevels, Usage,
            Format, Pool, Filter, MipFilter, ColorKey, pSrcInfo, pPalette, ppTexture
        );
    }

    return E_FAIL;
}

// ---------------------------------------------------------------------------------
// HOOK 2: LoadModelTexture at 0x00492230
// ---------------------------------------------------------------------------------
static void* s_oLoadModelTexture = (void*)0x00492230;

void __cdecl OnPostLoadModelTexture(void* tablePtr, int tableIndex, uint32_t baseIdx)
{
    TextureEngine::Instance().HandlePostLoadModelTexture(tablePtr, tableIndex, baseIdx);
}

__declspec(naked) void hkLoadModelTexture()
{
    __asm {
        push [esp+4]       // save caller's tableIndex
        push edx           // save caller's tablePtr (edx)
        mov ecx, ds:[0x01B7A6E0] // capture ct_MasterTextureBaseIdx BEFORE call
        push ecx           // save baseIdx

        push [esp+8]       // push tableIndex argument for original call (shifted by 12, index at [esp+8])
        mov edx, [esp+8]   // ensure edx is tablePtr
        call s_oLoadModelTexture
        add esp, 4         // caller cleanup of original arg

        push eax           // save original return value
        push [esp+4]       // push saved baseIdx
        push [esp+16]      // push saved tableIndex
        push [esp+16]      // push saved tablePtr
        call OnPostLoadModelTexture
        add esp, 12        // cleanup OnPostLoadModelTexture args

        pop eax            // restore original return value
        pop ecx            // cleanup saved baseIdx
        pop edx            // restore edx
        pop ecx            // cleanup saved tableIndex
        ret
    }
}

// ---------------------------------------------------------------------------------
// HOOK 2b: LoadSegaTextureContainer at 0x00492780 (Central 2D Sprite & Container Loader)
// ---------------------------------------------------------------------------------
static void* s_oLoadSegaTextureContainer = (void*)0x00492780;

void __cdecl OnPostLoadSegaTextureContainer(void* tablePtr, uint32_t baseIdx)
{
    TextureEngine::Instance().HandlePostLoadSegaTextureContainer(tablePtr, baseIdx);
}

__declspec(naked) void hkLoadSegaTextureContainer()
{
    __asm {
        push [esp+4]       // save caller's arg_0 (palette pointer)
        push eax           // save caller's tablePtr (in eax)
        mov edx, ds:[0x01B7A6E0]
        push edx           // save ct_MasterTextureBaseIdx BEFORE call

        // Setup original call: eax = tablePtr, push arg_0
        push [esp+8]       // push arg_0 (palette)
        mov eax, [esp+8]   // ensure eax is tablePtr
        call s_oLoadSegaTextureContainer
        add esp, 4         // caller cleanup of arg_0

        push eax           // save return value of ct_LoadSegaTextureContainer
        push [esp+4]       // push saved baseIdx
        push [esp+12]      // push saved tablePtr
        call OnPostLoadSegaTextureContainer
        add esp, 8         // cleanup OnPostLoadSegaTextureContainer args

        pop eax            // restore return value
        pop edx            // pop saved baseIdx
        pop ecx            // pop saved tablePtr
        pop edx            // pop saved arg_0
        ret                // caller will do 'add esp, 4'
    }
}

// ---------------------------------------------------------------------------------
// HOOK 3: LoadStageTexture at 0x0048B7A0
// ---------------------------------------------------------------------------------
static void* s_oLoadStageTexture = (void*)0x0048B7A0;

void __cdecl OnPostLoadStageTexture(void* entryPtr, uint32_t id)
{
    TextureEngine::Instance().HandlePostLoadStageTexture(entryPtr, id);
}

__declspec(naked) void hkLoadStageTexture()
{
    __asm {
        push ecx           // save caller's id (ecx)
        push eax           // save caller's entryPtr (eax)
        call s_oLoadStageTexture
        push eax           // save original return value
        push [esp+8]       // push id for OnPostLoadStageTexture
        push [esp+8]       // push entryPtr for OnPostLoadStageTexture
        call OnPostLoadStageTexture
        add esp, 8         // cleanup args
        pop eax            // restore original return value
        pop edx            // cleanup saved entryPtr
        pop ecx            // restore ecx
        ret
    }
}

// ---------------------------------------------------------------------------------
// HOOK 4: SegaRenderer::SetTexture at 0x00481200
// ---------------------------------------------------------------------------------
static void* s_oSegaSetTexture = (void*)0x00481200;

void __cdecl OnPreSegaSetTexture(void* pTexObj)
{
    TextureEngine::Instance().HandlePreSetTexture(pTexObj);
}

__declspec(naked) void hkSegaSetTexture()
{
    __asm {
        test eax, eax
        jz _skip_set_texture
        pushad
        push eax           // pass pTextureObject
        call OnPreSegaSetTexture
        add esp, 4
        popad
    _skip_set_texture:
        jmp s_oSegaSetTexture
    }
}

// ---------------------------------------------------------------------------------
// HOOK 5: Input Device Polling & Keycap Arbitration at 0x00494E30 & 0x0048F3F0
// ---------------------------------------------------------------------------------
typedef int (__cdecl *tInputPoll)();
static tInputPoll s_oInputPoll = (tInputPoll)0x00494E30;

typedef int (__cdecl *tSub4F86C0)();
static tSub4F86C0 s_oSub4F86C0 = (tSub4F86C0)0x004F86C0;

static const GUID GUID_KEYBOARD_MOUSE = {
    0x11111111, 0x1111, 0x1111,
    { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11 }
};
static const GUID GUID_CONTROLLER = {
    0x00000000, 0x0000, 0x0000,
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }
};

enum class ActiveInputMode {
    KeyboardMouse,
    Gamepad
};
static ActiveInputMode s_CurrentInputMode = ActiveInputMode::KeyboardMouse;
static POINT s_LastCursorPos = { -1, -1 };
static DWORD s_LastInputModeSwitchTime = 0;

// Mode switches require MODE_SWITCH_CONFIRM_POLLS consecutive polls with the
// same intentional input signal, so a single noisy poll (ADC spike, stale
// GetAsyncKeyState) cannot flip the prompt mode. The 150 ms time debounce in
// hkInputPoll stays in effect on top of this.
static const int MODE_SWITCH_CONFIRM_POLLS = 3;
static int s_GamepadStreak = 0;
static int s_KBMStreak = 0;

// Analog rest-position calibration: rest values are tracked with a slow EMA
// only while the mode is KeyboardMouse, so persistent stick DC offset/drift
// gets calibrated out and cannot pin the mode on Gamepad (irreversible flip).
// Frozen while Gamepad is active so intentional stick movement is never
// calibrated away. Rest starts from the first valid poll and is refined over a
// short seed window (REST_SEED_POLLS) in which axis activity is suppressed, so
// a DLL injected while the stick is held/moved does not freeze that transient
// position as rest. Documented residual case: a stick held perfectly still
// across the whole seed window still seeds as rest; the resulting spurious
// Gamepad flip is self-limiting (~1 s) because the KBM-mode EMA re-calibrates
// rest once the user returns to mouse/keyboard.
static const int REST_SEED_POLLS = 16; // ~0.25 s at 60 Hz polls
static int16_t s_RestX = 0;
static int16_t s_RestY = 0;
static int16_t s_Rest3 = 0;
static int16_t s_Rest4 = 0;
static bool s_RestInitialized = false;
static int s_RestSeedPollsLeft = 0;

// Deadzone threshold in raw axis units, loaded from [General] InputDeadzone in
// InstallHooks. Encoding caveat: this is compared against the raw int16 axis
// fields (+28..+34 of the input state struct). If those carry true int16 range
// (+/-32767) a value of 25 is ~0.08% tilt; if the game stores int8-range
// samples (-128..127) it is ~19.7%, matching the original "~20%" comment. The
// encoding could not be verified (game process not available), so the default
// stays 25 (vanilla behavior) and is INI-tunable instead of being hard-raised.
static int s_GamepadDeadzone = 25;

static int __cdecl hkInputPoll()
{
    int result = s_oInputPoll();

    uintptr_t pInputState = *(uintptr_t*)0x01709798;
    bool bIntentionalGamepadInput = false;

    if (pInputState)
    {
        uint32_t buttonsPressed = *(uint32_t*)(pInputState + 8);
        int16_t axisX = *(int16_t*)(pInputState + 28);
        int16_t axisY = *(int16_t*)(pInputState + 30);
        int16_t axis3 = *(int16_t*)(pInputState + 32);
        int16_t axis4 = *(int16_t*)(pInputState + 34);

        // Activity is judged against the calibrated rest position, not absolute
        // zero, so DC offset/drift does not read as intentional input.
        if (!s_RestInitialized)
        {
            s_RestX = axisX; s_RestY = axisY; s_Rest3 = axis3; s_Rest4 = axis4;
            s_RestInitialized = true;
            s_RestSeedPollsLeft = REST_SEED_POLLS - 1;
        }
        else if (s_RestSeedPollsLeft > 0)
        {
            // Seed window right after DLL load: keep tracking the axes with a
            // fast EMA so an injection while the stick is moved does not freeze
            // a transient position as rest. Axis activity is suppressed during
            // the window (buttons still count below).
            --s_RestSeedPollsLeft;
            s_RestX += (int16_t)((axisX - s_RestX) / 4);
            s_RestY += (int16_t)((axisY - s_RestY) / 4);
            s_Rest3 += (int16_t)((axis3 - s_Rest3) / 4);
            s_Rest4 += (int16_t)((axis4 - s_Rest4) / 4);
        }
        else if (s_CurrentInputMode == ActiveInputMode::KeyboardMouse)
        {
            // Slow EMA (~1 s time constant at 60 Hz polls), KBM mode only.
            s_RestX += (int16_t)((axisX - s_RestX) / 64);
            s_RestY += (int16_t)((axisY - s_RestY) / 64);
            s_Rest3 += (int16_t)((axis3 - s_Rest3) / 64);
            s_Rest4 += (int16_t)((axis4 - s_Rest4) / 64);
        }

        // Device presence gate for BOTH gamepad signals (K-INP-1): the game's
        // device index at 0x0134AE6C must be >= 0, i.e. the game itself has
        // enumerated a pad, before axis or button activity may count as
        // gamepad input. This closes the confirmed symptom path where
        // garbage/rest artifacts in the axis fields (+28..+34) flipped the
        // mode to Gamepad on pure keyboard setups (no device enumerated); the
        // resulting all-zero GUID stomp at 0x01BC4530 then made sub_48F3F0
        // render controller prompts on a keyboard. In KeyboardMouse mode this
        // DLL forces the index to -1 after every poll (end of this function),
        // so stale axis garbage cannot reopen the gate unless the game itself
        // re-enumerates a device between polls. Accepted residual: if the
        // game only raises the index after the first pad button press, a
        // stick-only user on a connected, never-pressed pad gets a delayed
        // flip; the button path below still catches it on that first press.
        const int lastDeviceIdx = *(int*)0x0134AE6C;

        // Analog stick deadzone threshold (raw axis units, [General]
        // InputDeadzone) around the calibrated rest position. Suppressed while
        // the seed window is still calibrating (buttons are detected below)
        // and presence-gated like the button path (K-INP-1).
        if (lastDeviceIdx >= 0 && s_RestSeedPollsLeft == 0 &&
            (abs(axisX - s_RestX) > s_GamepadDeadzone || abs(axisY - s_RestY) > s_GamepadDeadzone ||
             abs(axis3 - s_Rest3) > s_GamepadDeadzone || abs(axis4 - s_Rest4) > s_GamepadDeadzone))
        {
            bIntentionalGamepadInput = true;
        }

        // Check if gamepad button was pressed (device index >= 0, K-INP-1)
        if (lastDeviceIdx >= 0 && buttonsPressed != 0)
        {
            bIntentionalGamepadInput = true;
        }
    }

    bool bIntentionalKBMInput = false;
    POINT currentCursorPos;
    if (GetCursorPos(&currentCursorPos))
    {
        if (s_LastCursorPos.x != -1 && s_LastCursorPos.y != -1)
        {
            int dx = abs(currentCursorPos.x - s_LastCursorPos.x);
            int dy = abs(currentCursorPos.y - s_LastCursorPos.y);
            if (dx > 3 || dy > 3)
            {
                bIntentionalKBMInput = true;
            }
        }
        s_LastCursorPos = currentCursorPos;
    }

    // Check mouse clicks
    if ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) ||
        (GetAsyncKeyState(VK_RBUTTON) & 0x8000) ||
        (GetAsyncKeyState(VK_MBUTTON) & 0x8000))
    {
        bIntentionalKBMInput = true;
    }

    // Check keyboard keypresses in one single call without 214 syscalls
    if (!bIntentionalKBMInput)
    {
        BYTE keyState[256];
        if (GetKeyboardState(keyState))
        {
            for (int k = 8; k <= 222; ++k)
            {
                if (k == VK_LBUTTON || k == VK_RBUTTON || k == VK_MBUTTON) continue;
                if (keyState[k] & 0x80)
                {
                    bIntentionalKBMInput = true;
                    break;
                }
            }
        }
    }

    // Streak counting: each signal feeds its own consecutive-poll counter and
    // a poll without that signal resets it, so only sustained input can
    // confirm a switch (noise immunity as before, but without idle-poll
    // streak retention).
    // K-INP-2 (latch): a gamepad signal no longer zeroes or starves the KBM
    // counter. Previously the gamepad branch swallowed every shared poll
    // (s_KBMStreak = 0 plus else-if), so a persistent ungated gamepad signal
    // - enumerated device with axis drift/garbage beyond the deadzone -
    // pinned the mode on Gamepad irreversibly: the KBM counter could never
    // reach MODE_SWITCH_CONFIRM_POLLS and a keyboard user was stuck with
    // controller prompts. Now keyboard input keeps accumulating while a
    // gamepad signal is present, so Gamepad mode always stays reversible.
    // K-INP-3 (keyboard veto, pre-existing direction kept): keyboard input on
    // the same poll resets s_GamepadStreak, so a keyboard user - including
    // one with a connected, drifting pad - is never flipped to Gamepad while
    // actively typing or moving the mouse, and the two counters cannot
    // confirm simultaneously (no prompt flip-flop while both inputs are
    // held). Trade-off: a stick deflection flips the mode only while the
    // keyboard is quiet; a held key delays the flip until release.
    if (bIntentionalGamepadInput)
    {
        ++s_GamepadStreak;
    }
    else
    {
        s_GamepadStreak = 0;
    }

    if (bIntentionalKBMInput)
    {
        ++s_KBMStreak;
        s_GamepadStreak = 0; // K-INP-3: keyboard activity vetoes gamepad confirmation
    }
    else
    {
        s_KBMStreak = 0;
    }

    DWORD now = GetTickCount();
    if (s_GamepadStreak >= MODE_SWITCH_CONFIRM_POLLS &&
        s_CurrentInputMode != ActiveInputMode::Gamepad &&
        (now - s_LastInputModeSwitchTime > 150))
    {
        s_CurrentInputMode = ActiveInputMode::Gamepad;
        s_LastInputModeSwitchTime = now;
        s_GamepadStreak = 0;
        s_KBMStreak = 0; // K-INP-4: no stale counter may confirm a later single-poll flip
    }
    else if (s_KBMStreak >= MODE_SWITCH_CONFIRM_POLLS &&
        s_CurrentInputMode != ActiveInputMode::KeyboardMouse &&
        (now - s_LastInputModeSwitchTime > 150))
    {
        s_CurrentInputMode = ActiveInputMode::KeyboardMouse;
        s_LastInputModeSwitchTime = now;
        s_KBMStreak = 0;
        s_GamepadStreak = 0; // K-INP-4
    }

    // Enforce consistent GUID at 0x01BC4530. Compare-then-write: the game's own
    // writes between polls are still corrected, but unchanged memory is not
    // rewritten on every poll.
    GUID* pActiveGuid = (GUID*)0x01BC4530;
    const GUID& wantedGuid = (s_CurrentInputMode == ActiveInputMode::KeyboardMouse)
        ? GUID_KEYBOARD_MOUSE : GUID_CONTROLLER;
    if (memcmp(pActiveGuid, &wantedGuid, sizeof(GUID)) != 0)
    {
        *pActiveGuid = wantedGuid;
    }
    if (s_CurrentInputMode == ActiveInputMode::KeyboardMouse)
    {
        *(int*)0x0134AE6C = -1;
    }
    else if (*(int*)0x0134AE6C == -1)
    {
        *(int*)0x0134AE6C = 0;
    }

    return result;
}

static int __cdecl hkSub4F86C0()
{
    // Compare-then-write (see hkInputPoll): corrects game-side changes without
    // dirtying the page on every call.
    GUID* pActiveGuid = (GUID*)0x01BC4530;
    const GUID& wantedGuid = (s_CurrentInputMode == ActiveInputMode::KeyboardMouse)
        ? GUID_KEYBOARD_MOUSE : GUID_CONTROLLER;
    if (memcmp(pActiveGuid, &wantedGuid, sizeof(GUID)) != 0)
    {
        *pActiveGuid = wantedGuid;
    }

    return s_oSub4F86C0();
}

// ---------------------------------------------------------------------------------
// Keycap texture offset swap inside the two input branches of sub_48F3F0
// ---------------------------------------------------------------------------------
// Vanilla build ships the keycap texture slot displacement bytes crossed:
//   0x0048F5BA (Keyboard branch): vanilla 0x10 -> mod swaps to 0x0C
//   0x0048F5EA (Gamepad branch):  vanilla 0x0C -> mod swaps to 0x10
// Invariants:
//  1. The swap is part of the mod's fixes and therefore coupled to the mod
//     Enabled state ([General] Enabled / SetTextureModEnabled). Disabled mod
//     must leave vanilla bytes in place - including after runtime toggles -
//     so the pristine bytes are snapshotted once and restored on revert.
//  2. "Success" is only logged when at least one byte actually changed.
//     Bytes holding a value outside {vanilla, swapped} are reported with
//     their actual value (version drift becomes visible in the error log
//     instead of a silent no-op behind the byte guards).
static const uint8_t KEYCAP_VANILLA_KB = 0x10;
static const uint8_t KEYCAP_VANILLA_GP = 0x0C;
static const uint8_t KEYCAP_SWAPPED_KB = 0x0C;
static const uint8_t KEYCAP_SWAPPED_GP = 0x10;
static uint8_t s_KeycapOrigKB = KEYCAP_VANILLA_KB;
static uint8_t s_KeycapOrigGP = KEYCAP_VANILLA_GP;
static bool s_KeycapOrigCaptured = false;

// True once VerifyTargetBuild() passed in InstallHooks; gates the keycap
// revert/apply coupling in SetTextureModEnabled so a runtime toggle never
// touches fixed addresses on an unverified EXE revision.
static bool s_TargetBuildVerified = false;

// Shared path state (declared before InstallHooks: the deadzone INI read in
// InstallHooks needs g_ModDir) and the DllMain fallback marker: set when modDir
// was switched to the per-user writable fallback (read-only game dir); consumed
// for a post-Logger::Init log line below.
static std::string g_ModDir;
static std::string g_GameDir;
static bool g_UsedWritableModDirFallback = false;

// Writes both displacement bytes to the requested state. Returns true when
// both bytes now hold the requested values; false on VirtualProtect failure
// or unexpected byte values (logged with before/after values). *outChanged
// counts bytes actually rewritten.
static bool WriteKeycapBytes(bool apply, int* outChanged)
{
    *outChanged = 0;
    bool ok = true;
    struct Slot { uint8_t* addr; uint8_t orig; uint8_t swapped; const char* what; };
    const Slot slots[2] = {
        { (uint8_t*)0x0048F5BA, s_KeycapOrigKB, KEYCAP_SWAPPED_KB, "0x0048F5BA (Keyboard branch)" },
        { (uint8_t*)0x0048F5EA, s_KeycapOrigGP, KEYCAP_SWAPPED_GP, "0x0048F5EA (Gamepad branch)" },
    };
    for (const Slot& s : slots)
    {
        const uint8_t want = apply ? s.swapped : s.orig;
        const uint8_t cur = *s.addr; // read needs no protection (PAGE_EXECUTE_READ)
        if (cur == want)
        {
            continue;
        }
        if (cur != s.orig && cur != s.swapped)
        {
            char msg[160];
            sprintf_s(msg, "Keycap patch skipped at %s: found byte 0x%02X (expected 0x%02X or 0x%02X) - unknown EXE revision?",
                      s.what, cur, s.orig, s.swapped);
            Logger::LogError(msg);
            ok = false;
            continue;
        }
        DWORD oldProt = 0;
        if (!VirtualProtect((LPVOID)s.addr, 1, PAGE_EXECUTE_READWRITE, &oldProt))
        {
            char msg[96];
            sprintf_s(msg, "Failed to VirtualProtect at %s", s.what);
            Logger::LogError(msg);
            ok = false;
            continue;
        }
        *s.addr = want;
        VirtualProtect((LPVOID)s.addr, 1, oldProt, &oldProt);
        ++(*outChanged);
    }
    return ok;
}

// Applies (apply=true) or reverts (apply=false) the keycap offset swap.
// Returns true if the game bytes now match the requested state.
static bool ApplyKeycapSwap(bool apply)
{
    // Snapshot the pristine bytes once, before the first write of this
    // session, so a later revert restores exactly what the game shipped.
    if (!s_KeycapOrigCaptured)
    {
        const uint8_t curKB = *(uint8_t*)0x0048F5BA;
        const uint8_t curGP = *(uint8_t*)0x0048F5EA;
        if ((curKB != KEYCAP_VANILLA_KB && curKB != KEYCAP_SWAPPED_KB) ||
            (curGP != KEYCAP_VANILLA_GP && curGP != KEYCAP_SWAPPED_GP))
        {
            char msg[160];
            sprintf_s(msg, "Keycap patch unavailable: unexpected bytes at 0x0048F5BA=0x%02X / 0x0048F5EA=0x%02X (unknown EXE revision)",
                      curKB, curGP);
            Logger::LogError(msg);
            return false; // never captured: nothing written, nothing to revert
        }
        s_KeycapOrigKB = curKB;
        s_KeycapOrigGP = curGP;
        s_KeycapOrigCaptured = true;
    }

    int changed = 0;
    const bool ok = WriteKeycapBytes(apply, &changed);
    if (changed > 0)
    {
        Logger::LogInfo(apply
            ? "Keycap texture offsets swapped in sub_48F3F0 (0x10 <-> 0x0C)"
            : "Keycap texture offsets restored to vanilla bytes");
    }
    else if (ok)
    {
        Logger::LogInfo(apply
            ? "Keycap texture offsets already in swapped state"
            : "Keycap texture offsets already in vanilla state");
    }
    // ok==false with changed==0: mismatch already logged above; deliberately
    // no success log here.
    return ok;
}

// ---------------------------------------------------------------------------------
// MODMENU & C EXPORTS
// ---------------------------------------------------------------------------------
extern "C" {

__declspec(dllexport) bool GetTextureModEnabled()
{
    return TextureEngine::Instance().IsEnabled() && !TextureEngine::Instance().IsVanillaRestored();
}

__declspec(dllexport) void SetTextureModEnabled(bool enabled)
{
    TextureEngine::Instance().SetEnabled(enabled);
    // Keep the keycap offset swap coupled to the mod Enabled state across
    // runtime toggles: revert to the snapshotted vanilla bytes on disable,
    // re-apply on enable. No-op unless the fixed-address build check passed.
    if (s_TargetBuildVerified)
    {
        ApplyKeycapSwap(enabled);
    }
}

__declspec(dllexport) void ReloadTextureMod()
{
    TextureEngine::Instance().ReloadTextures();
    ModelEngine::Instance().Reload3DModels();
}

__declspec(dllexport) void Reload3DModels()
{
    ModelEngine::Instance().Reload3DModels();
}

__declspec(dllexport) void Apply3DModels()
{
    ModelEngine::Instance().Reload3DModels();
}

__declspec(dllexport) bool Get3DModEnabled()
{
    return ModelEngine::Instance().IsEnabled();
}

__declspec(dllexport) void Set3DModEnabled(bool enabled)
{
    ModelEngine::Instance().SetEnabled(enabled);
}

__declspec(dllexport) void RestoreOriginal3DModels()
{
    ModelEngine::Instance().RestoreOriginal3DModels();
}

__declspec(dllexport) void FlushTextureCache()
{
    TextureEngine::Instance().FlushCache();
}

__declspec(dllexport) void ScanAndApplyTextures()
{
    TextureEngine::Instance().ScanAndReplaceAllMasterTextures();
}

__declspec(dllexport) void RestoreOriginalTextures()
{
    TextureEngine::Instance().ToggleRestoreOriginalTextures();
}

__declspec(dllexport) bool IsOriginalTexturesRestored()
{
    return TextureEngine::Instance().IsVanillaRestored();
}

__declspec(dllexport) void SetRestoreOriginals(bool restore)
{
    TextureEngine::Instance().SetVanillaRestored(restore);
}

__declspec(dllexport) bool BackupTexturesZip(char* outZipPath, int maxLen, int* outCount)
{
    std::string zipPath;
    int count = 0;
    bool ok = TextureEngine::Instance().CreateTexturesZipBackup(&zipPath, &count);
    if (ok)
    {
        if (outZipPath && maxLen > 0)
        {
            strncpy_s(outZipPath, maxLen, zipPath.c_str(), _TRUNCATE);
        }
        if (outCount) *outCount = count;
        return true;
    }
    return false;
}

__declspec(dllexport) bool ToggleStampTestTextures(char* outStatus, int maxLen)
{
    std::string status;
    bool ok = false;
    if (TextureEngine::Instance().IsTexturesStamped())
    {
        ok = TextureEngine::Instance().UnstampTestTextures(&status);
    }
    else
    {
        ok = TextureEngine::Instance().StampTestTextures(&status);
    }
    if (outStatus && maxLen > 0)
    {
        strncpy_s(outStatus, maxLen, status.c_str(), _TRUNCATE);
    }
    return ok;
}

__declspec(dllexport) bool IsTexturesStamped()
{
    return TextureEngine::Instance().IsTexturesStamped();
}

__declspec(dllexport) bool StampTextures(char* outStatus, int maxLen)
{
    std::string status;
    bool ok = TextureEngine::Instance().StampTestTextures(&status);
    if (outStatus && maxLen > 0)
    {
        strncpy_s(outStatus, maxLen, status.c_str(), _TRUNCATE);
    }
    return ok;
}

__declspec(dllexport) bool UnstampTextures(char* outStatus, int maxLen)
{
    std::string status;
    bool ok = TextureEngine::Instance().UnstampTestTextures(&status);
    if (outStatus && maxLen > 0)
    {
        strncpy_s(outStatus, maxLen, status.c_str(), _TRUNCATE);
    }
    return ok;
}

__declspec(dllexport) void CALLBACK RunNativeStamp(HWND hwnd, HINSTANCE hinst, LPCSTR lpszCmdLine, int nCmdShow)
{
    if (s_hInitEvent) WaitForSingleObject(s_hInitEvent, 5000);
    std::string status;
    TextureEngine::Instance().StampTestTextures(&status);
    MessageBoxA(hwnd, status.c_str(), "LyrasTextureMod - Native Stamp", MB_OK | MB_ICONINFORMATION);
}

__declspec(dllexport) void CALLBACK RunNativeUnstamp(HWND hwnd, HINSTANCE hinst, LPCSTR lpszCmdLine, int nCmdShow)
{
    if (s_hInitEvent) WaitForSingleObject(s_hInitEvent, 5000);
    std::string status;
    TextureEngine::Instance().UnstampTestTextures(&status);
    MessageBoxA(hwnd, status.c_str(), "LyrasTextureMod - Native Unstamp", MB_OK | MB_ICONINFORMATION);
}

} // extern "C"

// ---------------------------------------------------------------------------------
// FRAMEWORK_MOD GUI INTEGRATION (Shift+F1 Mod Menu Hook)
// ---------------------------------------------------------------------------------
struct ImVec2 { float x, y; ImVec2(float _x = 0.0f, float _y = 0.0f) : x(_x), y(_y) {} };
struct ImVec4 { float x, y, z, w; ImVec4(float _x = 0.0f, float _y = 0.0f, float _z = 0.0f, float _w = 0.0f) : x(_x), y(_y), z(_z), w(_w) {} };

typedef bool (__cdecl *fnImGui_Button)(const char* label, const ImVec2* size);
typedef void (__cdecl *fnImGui_SameLine)(float offset_from_start_x, float spacing);
typedef void (__cdecl *fnImGui_Spacing)();
typedef void (__cdecl *fnImGui_Separator)();
typedef void (__cdecl *fnImGui_Text)(const char* fmt, ...);
typedef void (__cdecl *fnImGui_TextColored)(const ImVec4* col, const char* fmt, ...);
typedef void (__cdecl *fnImGui_TextDisabled)(const char* fmt, ...);
typedef void (__cdecl *fnImGui_SetTooltip)(const char* fmt, ...);
typedef bool (__cdecl *fnImGui_IsItemHovered)(int flags);

static fnImGui_Button       g_pImGuiButton = nullptr;
static fnImGui_SameLine     g_pImGuiSameLine = nullptr;
static fnImGui_Spacing      g_pImGuiSpacing = nullptr;
static fnImGui_Separator    g_pImGuiSeparator = nullptr;
static fnImGui_Text         g_pImGuiText = nullptr;
static fnImGui_TextColored  g_pImGuiTextColored = nullptr;
static fnImGui_TextDisabled g_pImGuiTextDisabled = nullptr;
static fnImGui_SetTooltip   g_pImGuiSetTooltip = nullptr;
static fnImGui_IsItemHovered g_pImGuiIsItemHovered = nullptr;

static std::string s_ModMenuStatus = "";
static uintptr_t s_FrameworkBase = 0;
static uintptr_t s_ReturnToEndChild = 0;

static void DrawTextureModExtraUI_Impl()
{
    if (!g_pImGuiButton) return;

    ImVec2 btnSize(180.0f, 30.0f);
    ImVec4 colHeader(1.0f, 0.8f, 0.0f, 1.0f);   // Taxi Gold
    ImVec4 colSuccess(0.4f, 1.0f, 0.4f, 1.0f);  // Light Green

    g_pImGuiSpacing();
    g_pImGuiSeparator();
    g_pImGuiSpacing();

    // Section Header
    g_pImGuiTextColored(&colHeader, "%s", "Texture Management & Fail-Safe Tools");

    // Row 1: Restore/Apply Toggle & Backup ZIP
    bool isVanilla = TextureEngine::Instance().IsVanillaRestored();
    const char* restoreLabel = isVanilla ? "Apply Custom Textures" : "Restore Original Textures";
    if (g_pImGuiButton(restoreLabel, &btnSize))
    {
        TextureEngine::Instance().ToggleRestoreOriginalTextures();
        s_ModMenuStatus = isVanilla ? "Custom HD textures applied." : "Original textures restored.";
    }
    if (g_pImGuiIsItemHovered(0))
    {
        g_pImGuiSetTooltip("%s", "Instantly toggle between modified HD textures and original game textures without restart.");
    }

    g_pImGuiSameLine(0.0f, -1.0f);

    if (g_pImGuiButton("Backup Textures (ZIP)", &btnSize))
    {
        std::string zipPath;
        int count = 0;
        if (TextureEngine::Instance().CreateTexturesZipBackup(&zipPath, &count))
        {
            const char* fn = strrchr(zipPath.c_str(), '\\');
            char buf[256];
            sprintf_s(buf, sizeof(buf), "Backup created: %s (%d files)", fn ? fn + 1 : zipPath.c_str(), count);
            s_ModMenuStatus = buf;
        }
        else
        {
            s_ModMenuStatus = "Backup failed (no modified textures found).";
        }
    }
    if (g_pImGuiIsItemHovered(0))
    {
        g_pImGuiSetTooltip("%s", "Creates a timestamped .zip archive of all modified textures into mods/LyrasTextureMod/backups/.");
    }

    // Row 2: Native Stamp / Unstamp (Replacing Python stamp_test_textures.py)
    bool isStamped = TextureEngine::Instance().IsTexturesStamped();
    const char* stampLabel = isStamped ? "Unstamp Textures" : "Stamp Test Textures";
    if (g_pImGuiButton(stampLabel, &btnSize))
    {
        std::string status;
        if (isStamped)
        {
            TextureEngine::Instance().UnstampTestTextures(&status);
        }
        else
        {
            TextureEngine::Instance().StampTestTextures(&status);
        }
        s_ModMenuStatus = status;
    }
    if (g_pImGuiIsItemHovered(0))
    {
        g_pImGuiSetTooltip("%s", "Natively stamps or unstamps sequential IDs onto all textures for UV mapping.");
    }

    // Status Message line
    if (!s_ModMenuStatus.empty())
    {
        g_pImGuiSpacing();
        g_pImGuiTextColored(&colSuccess, "%s", s_ModMenuStatus.c_str());
    }
}

static LONG WINAPI DrawExtraUIExceptionFilter(EXCEPTION_POINTERS* ep)
{
    DWORD code = ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0;
    void* addr = ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionAddress : nullptr;
    ULONG_PTR info1 = (ep->ExceptionRecord && ep->ExceptionRecord->NumberParameters > 1)
        ? ep->ExceptionRecord->ExceptionInformation[1] : 0;
    Logger::LogError("[GUI] Exception 0x%08X at %p (access target %p) in DrawTextureModExtraUI caught and isolated.",
        code, addr, (void*)info1);
    return EXCEPTION_EXECUTE_HANDLER;
}

void __cdecl DrawTextureModExtraUI()
{
    __try
    {
        DrawTextureModExtraUI_Impl();
    }
    __except (DrawExtraUIExceptionFilter(GetExceptionInformation()))
    {
    }
}

__declspec(naked) static void Hook_TextureModPanel_Hovered()
{
    __asm
    {
        add esp, 4                  // clean up SetTooltip argument
        pushad
        pushfd
        call DrawTextureModExtraUI
        popfd
        popad
        jmp ds:[s_ReturnToEndChild] // jmp to Framework_MOD.dll's EndChild call
    }
}

__declspec(naked) static void Hook_TextureModPanel_NotHovered()
{
    __asm
    {
        pushad
        pushfd
        call DrawTextureModExtraUI
        popfd
        popad
        jmp ds:[s_ReturnToEndChild] // jmp to Framework_MOD.dll's EndChild call
    }
}

static bool HookFrameworkModMenu()
{
    HMODULE hFramework = GetModuleHandleA("Framework_MOD.dll");
    if (!hFramework)
        return false;

    uintptr_t base = (uintptr_t)hFramework;
    s_FrameworkBase = base;
    s_ReturnToEndChild = base + 0x689E1;

    g_pImGuiButton = (fnImGui_Button)(base + 0x2F1D);
    g_pImGuiSameLine = (fnImGui_SameLine)(base + 0xA0E2);
    g_pImGuiSpacing = (fnImGui_Spacing)(base + 0x6339);
    g_pImGuiSeparator = (fnImGui_Separator)(base + 0x7752);
    g_pImGuiText = (fnImGui_Text)(base + 0x63E3);
    g_pImGuiTextColored = (fnImGui_TextColored)(base + 0x9DD6);
    g_pImGuiTextDisabled = (fnImGui_TextDisabled)(base + 0xA58D);
    g_pImGuiSetTooltip = (fnImGui_SetTooltip)(base + 0x9D0E);
    g_pImGuiIsItemHovered = (fnImGui_IsItemHovered)(base + 0x19B5);

    uint8_t* pPatchHovered = (uint8_t*)(base + 0x6877C);
    uint8_t* pPatchNotHovered = (uint8_t*)(base + 0x6876C);

    DWORD oldProt;
    // 1. Patch Hovered path at 0x6877C (5 bytes): JMP Hook_TextureModPanel_Hovered
    if (VirtualProtect(pPatchHovered, 5, PAGE_EXECUTE_READWRITE, &oldProt))
    {
        uint32_t rel = (uint32_t)((uintptr_t)&Hook_TextureModPanel_Hovered - ((uintptr_t)pPatchHovered + 5));
        pPatchHovered[0] = 0xE9; // JMP rel32
        *(uint32_t*)(pPatchHovered + 1) = rel;
        VirtualProtect(pPatchHovered, 5, oldProt, &oldProt);
    }

    // 2. Patch NotHovered path at 0x6876C (6 bytes): JZ Hook_TextureModPanel_NotHovered
    // Original instruction: 0F 84 6F 02 00 00 (jz loc_100689E1)
    // Preserves conditional branching cleanly without overwriting the Hovered path!
    if (VirtualProtect(pPatchNotHovered, 6, PAGE_EXECUTE_READWRITE, &oldProt))
    {
        uint32_t rel = (uint32_t)((uintptr_t)&Hook_TextureModPanel_NotHovered - ((uintptr_t)pPatchNotHovered + 6));
        pPatchNotHovered[0] = 0x0F;
        pPatchNotHovered[1] = 0x84; // JZ rel32
        *(uint32_t*)(pPatchNotHovered + 2) = rel;
        VirtualProtect(pPatchNotHovered, 6, oldProt, &oldProt);
    }

    Logger::LogInfo("Successfully hooked Framework_MOD.dll Mod Menu GUI for Texture Mod controls!");
    return true;
}

// Portability gate (build identity): every address below - hooks
// 0x00492230/0x0048B7A0/0x00481200/0x00494E30/0x004F86C0, IAT slot 0x005342B4 and
// keycap displacement bytes 0x0048F5BA/0x0048F5EA (the bytes actually patched) - is
// a code/IAT Virtual Address of exactly one Crazy Taxi.exe revision (preferred base
// 0x00400000). On any other revision (CD build, rebased/patched EXE) these would
// point into unrelated memory and corrupt it. Before installing anything, verify
// (a) every target lies inside the main module's mapped image and (b) the module's
// PE timestamp/image size/base match that one revision. On mismatch the mod stays
// fully passive (no IAT patch, no game hooks, no keycap patch). Full pattern-scan
// portability is out of scope here.
static bool VerifyTargetBuild()
{
    static const uintptr_t kFixedAddrs[] = {
        0x00492780, 0x00492230, 0x0048B7A0, 0x00481200, 0x00494E30, 0x004F86C0,
        0x005342B4, 0x0048F5BA, 0x0048F5EA
    };

    HMODULE hMain = GetModuleHandleA(NULL);
    if (!hMain)
    {
        return false;
    }

    // Parse the PE header in place for ImageBase/SizeOfImage (no psapi dep).
    const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(hMain);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
    {
        return false;
    }
    const IMAGE_NT_HEADERS* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
        reinterpret_cast<const uint8_t*>(hMain) + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
    {
        return false;
    }

    const uintptr_t base = reinterpret_cast<uintptr_t>(hMain);
    const uintptr_t end = base + nt->OptionalHeader.SizeOfImage;
    for (uintptr_t va : kFixedAddrs)
    {
        if (va < base || va + 16 > end)
        {
            return false;
        }
    }

    // Build identity beyond the range check: the exact PE timestamp, image size and
    // preferred base of the one Crazy Taxi.exe revision these addresses were reverse-
    // engineered from (Steam 1.0.0.1, timestamp 0x5433ED5C — confirmed by WER crash
    // reports). A byte-canary at the keycap displacement bytes was tried first, but
    // Framework_MOD.dll's DCRestore layer legitimately rewrites that instruction area
    // before this DLL loads (bytes read 0x40/0x40 in the field instead of the vanilla
    // 0x10/0x0C), so runtime bytes are NOT a stable identity signal. The keycap patch
    // itself keeps its own byte guards and no-ops cleanly when the framework already
    // replaced those instructions. A rebased/ASLR-shifted module fails the ImageBase
    // comparison below, which also protects the hardcoded absolute addresses.
    if (nt->FileHeader.TimeDateStamp != 0x5433ED5Cu ||
        nt->OptionalHeader.SizeOfImage != 0x01BDE000u ||
        nt->OptionalHeader.ImageBase != 0x00400000u)
    {
        return false;
    }
    if (base != 0x00400000u)
    {
        return false; // actual load base differs from the preferred base (relocated)
    }

    return true;
}

static void InstallEarlyIATHook()
{
    if (!VerifyTargetBuild())
    {
        return;
    }
    s_TargetBuildVerified = true;

    HMODULE hD3DX = GetModuleHandleA("d3dx9_43.dll");
    if (!hD3DX) hD3DX = LoadLibraryA("d3dx9_43.dll");
    if (hD3DX)
    {
        s_oD3DXCreateTextureFromFileInMemoryEx = (tD3DXCreateTextureFromFileInMemoryEx)GetProcAddress(hD3DX, "D3DXCreateTextureFromFileInMemoryEx");
    }

    if (s_oD3DXCreateTextureFromFileInMemoryEx)
    {
        DWORD oldProt;
        if (VirtualProtect((LPVOID)0x005342B4, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProt))
        {
            *(uintptr_t*)0x005342B4 = (uintptr_t)&hkD3DXCreateTextureFromFileInMemoryEx;
            VirtualProtect((LPVOID)0x005342B4, sizeof(void*), oldProt, &oldProt);
            Logger::LogInfo("Early IAT patch installed at 0x005342B4 for D3DXCreateTextureFromFileInMemoryEx.");
        }
    }
}

static void InstallHooks()
{
    if (MH_Initialize() != MH_OK)
    {
        Logger::LogError("MinHook failed to initialize!");
        return;
    }

    InstallEarlyIATHook();

    // Portability gate: everything below touches fixed Virtual Addresses of
    // one specific Crazy Taxi.exe revision. Skip it entirely on a mismatch so
    // a different build (CD/rebased/patched EXE) is not corrupted; without the
    // IAT patch this DLL receives no D3DX calls at all and stays fully passive.
    if (!VerifyTargetBuild())
    {
        Logger::LogError("Unknown Crazy Taxi.exe revision detected (fixed-address identity check failed) - skipping IAT patch at 0x005342B4, game hooks 2-7 and keycap patch; the mod stays passive on this EXE.");
        return;
    }
    s_TargetBuildVerified = true;

    // INI-tunable analog deadzone ([General] InputDeadzone, raw axis units).
    // See the s_GamepadDeadzone comment for the encoding caveat.
    s_GamepadDeadzone = GetPrivateProfileIntA("General", "InputDeadzone", 25,
                                              (g_ModDir + "\\LyrasTextureMod.ini").c_str());
    if (s_GamepadDeadzone < 0) s_GamepadDeadzone = 0;
    if (s_GamepadDeadzone > 32767) s_GamepadDeadzone = 32767;

    // Also patch Crazy Taxi IAT directly at 0x005342B4 — only when the original was
    // resolved: never redirect the game's IAT to a hook whose original is missing,
    // the glue would have to answer every game D3DX call with E_FAIL.
    if (s_oD3DXCreateTextureFromFileInMemoryEx)
    {
        DWORD oldProt;
        if (VirtualProtect((LPVOID)0x005342B4, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProt))
        {
            *(uintptr_t*)0x005342B4 = (uintptr_t)&hkD3DXCreateTextureFromFileInMemoryEx;
            VirtualProtect((LPVOID)0x005342B4, sizeof(void*), oldProt, &oldProt);
            Logger::LogInfo("Patched Crazy Taxi IAT entry at 0x005342B4 for D3DXCreateTextureFromFileInMemoryEx");
        }
    }
    else
    {
        Logger::LogError("D3DX original unavailable - skipping IAT patch at 0x005342B4; game hooks 2-7 stay active without texture injection.");
    }

    // 2. Hook LoadModelTexture at 0x00492230
    void* pLoadModelTex = (void*)0x00492230;
    if (MH_CreateHook(pLoadModelTex, (LPVOID)&hkLoadModelTexture, (LPVOID*)&s_oLoadModelTexture) == MH_OK)
    {
        MH_EnableHook(pLoadModelTex);
        Logger::LogInfo("Successfully hooked LoadModelTexture at 0x00492230");
    }
    else
    {
        Logger::LogError("Failed to hook LoadModelTexture at 0x00492230!");
    }

    // 2b. Hook ct_LoadSegaTextureContainer at 0x00492780 (Central 2D Sprite & Container Loader)
    void* pLoadSegaContainer = (void*)0x00492780;
    if (MH_CreateHook(pLoadSegaContainer, (LPVOID)&hkLoadSegaTextureContainer, (LPVOID*)&s_oLoadSegaTextureContainer) == MH_OK)
    {
        MH_EnableHook(pLoadSegaContainer);
        Logger::LogInfo("Successfully hooked ct_LoadSegaTextureContainer at 0x00492780");
    }
    else
    {
        Logger::LogError("Failed to hook ct_LoadSegaTextureContainer at 0x00492780!");
    }

    // 3. Hook LoadStageTexture at 0x0048B7A0
    void* pLoadStageTex = (void*)0x0048B7A0;
    if (MH_CreateHook(pLoadStageTex, (LPVOID)&hkLoadStageTexture, (LPVOID*)&s_oLoadStageTexture) == MH_OK)
    {
        MH_EnableHook(pLoadStageTex);
        Logger::LogInfo("Successfully hooked LoadStageTexture at 0x0048B7A0");
    }
    else
    {
        Logger::LogError("Failed to hook LoadStageTexture at 0x0048B7A0!");
    }

    // 4. Hook SegaRenderer::SetTexture at 0x00481200
    void* pSegaSetTex = (void*)0x00481200;
    if (MH_CreateHook(pSegaSetTex, (LPVOID)&hkSegaSetTexture, (LPVOID*)&s_oSegaSetTexture) == MH_OK)
    {
        MH_EnableHook(pSegaSetTex);
        Logger::LogInfo("Successfully hooked SegaRenderer::SetTexture at 0x00481200");
    }
    else
    {
        Logger::LogError("Failed to hook SegaRenderer::SetTexture at 0x00481200!");
    }

    // 5. Patch inverted keycap texture offsets in sub_48F3F0 - only while the
    // mod is enabled. SetTextureModEnabled keeps the swap in sync on runtime
    // toggles (revert to snapshotted vanilla bytes on disable).
    if (TextureEngine::Instance().IsEnabled())
    {
        ApplyKeycapSwap(true);
    }
    else
    {
        Logger::LogInfo("Keycap texture offset swap skipped (mod disabled via [General] Enabled)");
    }

    // 6+7. Input Polling (0x00494E30) and Start Screen Icon Dispatch (0x004F86C0)
    // hooks are RETIRED (K25, 2026-11-07): their only effect was forcing the
    // input-device GUID (0x01BC4530) and the device slot (0x0134AE6C) from this
    // DLL's own mode logic. Framework_MOD.dll ships its own Analog Deadzone
    // Calibration and input handling that works, and our forcing made keyboard
    // installs render gamepad control prompts (driver select) whenever the int16
    // axis reads exceeded the tiny default deadzone. The game's own device state
    // is now left completely alone; the texture hotkeys (F8/F9/F10) live in the
    // SetTexture hook (HandlePreSetTexture) and are unaffected.
}

static std::string GetCurrentModDirectory(HMODULE hModule)
{
    char dllPath[MAX_PATH];
    DWORD n = GetModuleFileNameA(hModule, dllPath, MAX_PATH);
    if (n == 0)
    {
        dllPath[0] = '\0'; // keep the failure visible instead of a bogus path
    }
    else
    {
        // GetModuleFileNameA does not guarantee null-termination when the ANSI
        // buffer is exhausted; force it so strrchr below cannot run off the end.
        dllPath[MAX_PATH - 1] = '\0';
    }
    char* lastSlash = strrchr(dllPath, '\\');
    if (lastSlash) *lastSlash = '\0';
    return std::string(dllPath);
}

static std::string GetGameDirectory(const std::string& modDir)
{
    // The mod DLL is located at <gameDir>\mods\LyrasTextureMod\LyrasTextureMod.dll
    // Deriving relative to modDir guarantees the exact game directory:
    char fullMod[MAX_PATH];
    strcpy_s(fullMod, modDir.c_str());
    char* s1 = strrchr(fullMod, '\\');
    if (s1) *s1 = '\0'; // strip \LyrasTextureMod -> <gameDir>\mods
    char* s2 = strrchr(fullMod, '\\');
    if (s2) *s2 = '\0'; // strip \mods -> <gameDir>

    // Portability: Steam and CD layouts differ. Accept any known game-root
    // marker instead of requiring 'data_little' specifically; unknown layouts
    // fall through to the EXE directory below.
    static const char* const kGameRootMarkers[] = {
        "data_little", "NewTexture", "Crazy Taxi.exe", "polDC0.bin"
    };
    for (const char* marker : kGameRootMarkers)
    {
        DWORD attr = GetFileAttributesA((std::string(fullMod) + "\\" + marker).c_str());
        if (attr != INVALID_FILE_ATTRIBUTES)
        {
            return std::string(fullMod);
        }
    }

    char exePath[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, exePath, MAX_PATH);
    if (n == 0)
    {
        return std::string(fullMod); // best effort: unvalidated candidate
    }
    exePath[MAX_PATH - 1] = '\0'; // no guaranteed termination on ANSI truncation
    char* lastSlash = strrchr(exePath, '\\');
    if (lastSlash) *lastSlash = '\0';
    return std::string(exePath);
}

static DWORD WINAPI ModInitThread(LPVOID lpParam)
{
    // Automatic First-Time Asset Extraction:
    // Verifies 3D, splines, originals, textures exist and contain content.
    // If not, extracts textures, 3D models and splines directly from gamefiles in C++.
    AssetExtractor::CheckAndRunAutoExtraction(g_ModDir, g_GameDir);

    // Install hooks as early as possible so early game boot loader calls are intercepted
    InstallHooks();

    // Initialize TextureEngine immediately so manifest maps are ready for early boot calls
    TextureEngine::Instance().Initialize(g_ModDir);
    ModelEngine::Instance().Initialize(g_ModDir, g_GameDir);

    // Signal that TextureEngine manifest indexing is ready! Early boot D3DX calls waiting on s_hInitEvent proceed now.
    if (s_hInitEvent)
    {
        SetEvent(s_hInitEvent);
    }

    // Hook Framework_MOD.dll Mod Menu GUI (retries if Framework_MOD loads slightly later)
    for (int retry = 0; retry < 10; retry++)
    {
        if (HookFrameworkModMenu())
            break;
        Sleep(100);
    }

    // Synchronize textures loaded during startup into the game's master table
    TextureEngine::Instance().ScanAndReplaceAllMasterTextures();

    Logger::LogInfo("LyrasTextureMod initialization complete.");
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
    if (fdwReason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hinstDLL);

        std::string modDir = GetCurrentModDirectory(hinstDLL);
        std::string gameDir = GetGameDirectory(modDir);

        DWORD attr = GetFileAttributesA((modDir + "\\textures").c_str());
        if (attr == INVALID_FILE_ATTRIBUTES &&
            GetFileAttributesA((modDir + "\\3D").c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            if (GetFileAttributesA((modDir + "\\mods\\LyrasTextureMod").c_str()) != INVALID_FILE_ATTRIBUTES)
            {
                modDir += "\\mods\\LyrasTextureMod";
            }
            else if (GetFileAttributesA((gameDir + "\\mods\\LyrasTextureMod").c_str()) != INVALID_FILE_ATTRIBUTES)
            {
                modDir = gameDir + "\\mods\\LyrasTextureMod";
            }
        }

        // Portability (CD media / Program Files installs / UAC): first-time
        // asset extraction, INI and logs all live in modDir, which must be
        // writable. If the resolved modDir rejects a write probe, fall back to
        // a per-user writable directory so extraction still works; the game
        // directory itself stays read-only.
        {
            std::string probePath = modDir + "\\writetest.tmp";
            HANDLE hProbe = CreateFileA(probePath.c_str(), GENERIC_WRITE, 0, nullptr,
                                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (hProbe != INVALID_HANDLE_VALUE)
            {
                CloseHandle(hProbe);
                DeleteFileA(probePath.c_str());
            }
            else
            {
                char userDir[MAX_PATH];
                std::string fallback;
                if (GetEnvironmentVariableA("LOCALAPPDATA", userDir, MAX_PATH) > 0)
                {
                    fallback = std::string(userDir) + "\\LyrasTextureMod";
                }
                else
                {
                    // Pre-Win Vista fallback: per-user temp directory.
                    if (GetTempPathA(MAX_PATH, userDir) > 0)
                    {
                        fallback = std::string(userDir) + "LyrasTextureMod";
                    }
                }
                if (!fallback.empty())
                {
                    CreateDirectoryA(fallback.c_str(), nullptr);
                    if (GetFileAttributesA(fallback.c_str()) != INVALID_FILE_ATTRIBUTES)
                    {
                        modDir = fallback;
                        g_UsedWritableModDirFallback = true;
                    }
                }
            }
        }

        // Initialize logging early
        Logger::Init(modDir + "\\LyrasTextureMod_error.log", 2);
        if (g_UsedWritableModDirFallback)
        {
            Logger::LogInfo("Game-directory mod folder is not writable (CD media/UAC?) - using per-user fallback: %s", modDir.c_str());
        }

        g_ModDir = modDir;
        g_GameDir = gameDir;

        // Create synchronization event for early boot D3DX interception
        s_hInitEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);

        // Install early IAT redirection immediately so early boot D3DX calls cannot slip through unhooked
        InstallEarlyIATHook();

        // Spawn Mod Initialization Thread so DllMain returns immediately (releasing loader lock)
        HANDLE hThread = CreateThread(nullptr, 0, ModInitThread, nullptr, 0, nullptr);
        if (hThread) CloseHandle(hThread);
    }
    else if (fdwReason == DLL_PROCESS_DETACH)
    {
        MH_DisableHook(MH_ALL_HOOKS);
        MH_Uninitialize();
        if (s_hInitEvent)
        {
            CloseHandle(s_hInitEvent);
            s_hInitEvent = nullptr;
        }
        ModelEngine::Instance().Shutdown();
        TextureEngine::Instance().Shutdown();
        Logger::Shutdown();
    }

    return TRUE;
}
