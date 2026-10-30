// numpad_fix.cpp — Numpad text input fix.
//
// Hooks InputConfig_t::CheckInput in GUI.dll to intercept numpad keys
// when a text input field has focus. Translates them to characters and
// injects via HandleTextInput, preventing the action binding system from
// consuming them.
//
// See numpad_fix.h for the design overview.

#include "hooks/numpad_fix.h"
#include "hooks/hook_engine.h"
#include "ao/game_api.h"
#include "ao/gui_build.h"
#include "ao/types.h"
#include "core/logging.h"

#include <windows.h>
#include <cstdint>
#include <cstring>

namespace aor {

// ── GUI.dll function pointers ──────────────────────────────────────────

namespace NumpadGUI {

// WindowController_c::GetInstance() — __cdecl, returns singleton.
// GUI.dll RVA 0xb454.
using FnWCGetInstance = void*(__cdecl*)();

// WindowController_c::HandleTextInput(std::string const&) — __thiscall.
// Dispatches a text string to the focused view's text input handler.
// GUI.dll RVA 0x156f92.
// The std::string parameter has the same layout as AOString (MSVC 2010).
using FnHandleTextInput = void(__thiscall*)(void* wc, const AOString* str);

static FnWCGetInstance    WC_GetInstance     = nullptr;
static FnHandleTextInput  WC_HandleTextInput = nullptr;

static bool Init() {
    HMODULE gui = GetModuleHandleA("GUI.dll");
    if (!gui) {
        Log("[numpad] GUI.dll not loaded");
        return false;
    }
    const GuiBuild* addrs = GuiAddrs();
    if (!addrs) return false;
    auto base = reinterpret_cast<uintptr_t>(gui);

    WC_GetInstance     = reinterpret_cast<FnWCGetInstance>(base + addrs->wcGetInstance);
    WC_HandleTextInput = reinterpret_cast<FnHandleTextInput>(base + addrs->wcHandleTextInput);

    Log("[numpad] GUI API resolved: WC_GetInstance=%p, HandleTextInput=%p",
        reinterpret_cast<void*>(WC_GetInstance),
        reinterpret_cast<void*>(WC_HandleTextInput));
    return true;
}

}  // namespace NumpadGUI

// ── Numpad key → character translation ─────────────────────────────────
//
// AO internal key codes for numpad keys, derived from
// BrowserModule_c::TranslateKeyCode (GUI.dll RVA 0x10a751).
//
// AO code → VK code → character:
//   0x42 → VK_DIVIDE   → '/'
//   0x43 → VK_MULTIPLY → '*'
//   0x44 → VK_SUBTRACT → '-'
//   0x45 → VK_ADD      → '+'
//   0x46 → VK_NUMPAD0  → '0'
//   0x47 → VK_NUMPAD1  → '1'
//   ...
//   0x4f → VK_NUMPAD9  → '9'
//   0x50 → VK_DECIMAL  → '.'

static char NumpadKeyToChar(uint32_t aoKeyCode) {
    switch (aoKeyCode) {
    case 0x46: return '0';
    case 0x47: return '1';
    case 0x48: return '2';
    case 0x49: return '3';
    case 0x4a: return '4';
    case 0x4b: return '5';
    case 0x4c: return '6';
    case 0x4d: return '7';
    case 0x4e: return '8';
    case 0x4f: return '9';
    case 0x50: return '.';
    case 0x42: return '/';
    case 0x43: return '*';
    case 0x44: return '-';
    case 0x45: return '+';
    default:   return '\0';
    }
}

// ── Setting check ──────────────────────────────────────────────────────

static bool IsNumpadFixEnabled() {
    AOVariant v{};
    if (GameAPI::GetVariant("AOR_NumpadFix", v) &&
        v.type == static_cast<uint32_t>(VariantType::Bool)) {
        return v.as_bool;
    }
    return true;  // default on
}

// ── CheckInput hook ────────────────────────────────────────────────────
//
// InputConfig_t::CheckInput(InputInfo_t const&)
//   GUI.dll RVA 0x1a4c1
//   __thiscall, RET 4 (1 pointer arg cleaned by callee)
//   Prologue: 55 8B EC 51 53 (push ebp; mov ebp,esp; push ecx; push ebx)
//
// InputInfo_t layout (from disassembly):
//   +0x00: uint32_t  key_and_flags
//          Bits 0-16:  AO internal key code
//          Bit 17:     Ctrl modifier
//          Bit 18:     Alt modifier
//          Bit 19:     Shift modifier
//          Bit 20:     Key-up flag
//   +0x04: uint8_t   is_repeat (0 = initial press, non-zero = repeat)
//
// InputConfig_t layout (relevant fields):
//   +0x0C: uint8_t   afcm_redirect_mode (non-zero = hotkey assignment mode)
//   +0x27: uint8_t   static_mode_3 (static text input mode flag)
//   +0x67: uint8_t   text_input_mode (set by SetTextInputMode when chat focused)

// InputConfig_t::CheckInput: GuiBuild::checkInput.

using FnCheckInput = void(__thiscall*)(void* this_ecx, void* inputInfo);
static FnCheckInput g_origCheckInput = nullptr;

static void __fastcall CheckInputDetour(
        void* this_ecx, void* /*edx*/, void* inputInfo) {

    if (!IsNumpadFixEnabled()) {
        g_origCheckInput(this_ecx, inputInfo);
        return;
    }

    // Parse the InputInfo_t fields.
    auto* info = static_cast<uint8_t*>(inputInfo);
    uint32_t rawKey = *reinterpret_cast<uint32_t*>(info);
    uint32_t aoCode = rawKey & 0x1ffff;
    bool isKeyUp    = (rawKey & 0x100000) != 0;
    bool hasCtrl    = (rawKey & 0x20000)  != 0;
    bool hasAlt     = (rawKey & 0x40000)  != 0;

    // Check text input mode: either static mode 3 or dynamic text input.
    // Matches InputConfig_t::CheckMode(3) logic.
    auto* inputConfig = static_cast<uint8_t*>(this_ecx);
    bool textMode = (inputConfig[0x27] != 0) || (inputConfig[0x67] != 0);

    // Check if the AFCM redirect mode is active (hotkey assignment).
    // If so, don't intercept — let the original handle it.
    bool afcmMode = inputConfig[0x0c] != 0;

    // Try to translate the key to a numpad character.
    char ch = NumpadKeyToChar(aoCode);

    // Intercept conditions: numpad key, text mode active, no AFCM mode,
    // no Ctrl/Alt modifiers (Shift is fine — numpad digits don't change).
    if (ch != '\0' && textMode && !afcmMode && !hasCtrl && !hasAlt) {
        if (!isKeyUp) {
            // Key down or repeat: inject the character as text input.
            void* wc = NumpadGUI::WC_GetInstance();
            if (wc) {
                // Construct an MSVC 2010 std::string on the stack.
                // Same layout as AOString — SSO for single-char string.
                AOString str;
                std::memset(&str, 0, sizeof(str));
                str.sso_buf[0] = ch;
                str.length = 1;
                str.capacity = 15;

                NumpadGUI::WC_HandleTextInput(wc, &str);
            }
        }
        // Key-up in text mode: eat silently (don't trigger action release).
        return;
    }

    // Not a numpad key in text mode — pass through to original.
    g_origCheckInput(this_ecx, inputInfo);
}

// ── Init ───────────────────────────────────────────────────────────────

bool InitNumpadFix() {
    if (!NumpadGUI::Init()) {
        Log("[numpad] GUI API init failed — numpad fix disabled");
        return false;
    }

    void* addr = ResolveRVA("GUI.dll", GuiAddrs()->checkInput);
    if (!addr) {
        Log("[numpad] CheckInput RVA resolve failed");
        return false;
    }

    auto* bytes = static_cast<uint8_t*>(addr);
    Log("[numpad] CheckInput at %p, prologue: %02X %02X %02X %02X %02X",
        addr, bytes[0], bytes[1], bytes[2], bytes[3], bytes[4]);

    void* tramp = nullptr;
    if (!InstallHook(addr, reinterpret_cast<void*>(&CheckInputDetour), &tramp)) {
        Log("[numpad] CheckInput hook failed");
        return false;
    }

    g_origCheckInput = reinterpret_cast<FnCheckInput>(tramp);
    Log("[numpad] numpad text input fix installed");
    return true;
}

}  // namespace aor
