// input_handler.cpp — Replacement ActionViewMouseHandler_c state machine.
//
// Hooks 4 GUI.dll callbacks + N3Msg_MovementChanged in Gamecode.dll.
// See input_handler.h for the public interface and design overview.
//
// Stock OnMouseRelease is NOT hooked (unhookable prologue). It delegates
// to EndDrag which we DO hook, so all release logic flows through us.

#include "hooks/input_handler.h"
#include "hooks/hook_engine.h"
#include "ao/game_api.h"
#include "ao/gui_build.h"
#include "core/logging.h"

#include <windows.h>
#include <cstdint>
#include <cmath>
#include <cstring>

namespace aor {

// ── Gamecode.dll API ────────────────────────────────────────────────────

namespace GamecodeAPI {

using FnN3MsgMovementChanged =
    void(__thiscall*)(void* engineAnarchy, int action,
                      float f1, float f2, bool b);

static void* N3Msg_MovementChanged_Addr = nullptr;

// Trampoline to the ORIGINAL N3Msg_MovementChanged. Bypasses our filter.
static FnN3MsgMovementChanged  N3Msg_MovementChanged   = nullptr;

static bool Init() {
    HMODULE gc = GetModuleHandleA("Gamecode.dll");
    if (!gc) {
        Log("[input] Gamecode.dll not loaded");
        return false;
    }
    Log("[input] Gamecode.dll at %p", gc);

    void* p = reinterpret_cast<void*>(GetProcAddress(gc,
        "?N3Msg_MovementChanged@n3EngineClientAnarchy_t@@"
        "QAEXW4MovementAction_e@Movement_n@@MM_N@Z"));
    if (!p) {
        Log("[input] FAILED to resolve N3Msg_MovementChanged");
        return false;
    }
    N3Msg_MovementChanged_Addr = p;
    N3Msg_MovementChanged = reinterpret_cast<FnN3MsgMovementChanged>(p);
    Log("[input] resolved N3Msg_MovementChanged -> %p", p);
    return true;
}

}  // namespace GamecodeAPI

// ── GUI.dll API ─────────────────────────────────────────────────────────

namespace GUIAPI {

using FnGetInstance     = void*(__cdecl*)();
using FnN3MsgLook      = void(__thiscall*)(void* n3im, float dx, float dy);
using FnN3MsgEnd       = void(__thiscall*)(void* n3im);
using FnAFCMSend       = void(__thiscall*)(void* afcm, int cmd, int param);
using FnWCGetInstance   = void*(__cdecl*)();
using FnWCGetDragObj    = void*(__thiscall*)(void* wc);
using FnWCSetMousePos   = void(__thiscall*)(void* wc, const void* point, bool clamp);

static uintptr_t g_guiBase = 0;

static FnWCGetInstance  WC_GetInstance     = nullptr;
static FnWCGetDragObj   WC_GetDragObject   = nullptr;
static FnWCSetMousePos  WC_SetMousePosition = nullptr;

template<typename T>
static T ReadCachedPtr(uint32_t rva) {
    return *reinterpret_cast<T*>(g_guiBase + rva);
}

static void* GetN3IM() {
    auto fn = ReadCachedPtr<FnGetInstance>(GuiAddrs()->iatN3IMGetInstance);
    return fn ? fn() : nullptr;
}

static void CameraMouseLookMovement(float dx, float dy) {
    void* n3im = GetN3IM();
    auto fn = ReadCachedPtr<FnN3MsgLook>(GuiAddrs()->iatN3MsgCameraMouseLookMovement);
    if (n3im && fn) fn(n3im, dx, dy);
}

static void MouseMovement(float dx, float dy) {
    void* n3im = GetN3IM();
    auto fn = ReadCachedPtr<FnN3MsgLook>(GuiAddrs()->iatN3MsgMouseMovement);
    if (n3im && fn) fn(n3im, dx, dy);
}

static void EndCameraMouseLook() {
    void* n3im = GetN3IM();
    auto fn = ReadCachedPtr<FnN3MsgEnd>(GuiAddrs()->iatN3MsgEndCameraMouseLook);
    if (n3im && fn) fn(n3im);
}

static void EndMouseLook() {
    void* n3im = GetN3IM();
    auto fn = ReadCachedPtr<FnN3MsgEnd>(GuiAddrs()->iatN3MsgEndMouseLook);
    if (n3im && fn) fn(n3im);
}

static void AFCMSend(int cmd, int param) {
    auto getInst = ReadCachedPtr<FnGetInstance>(GuiAddrs()->iatAFCMGetInstance);
    auto send    = ReadCachedPtr<FnAFCMSend>(GuiAddrs()->iatAFCMSend);
    if (getInst && send) {
        void* afcm = getInst();
        if (afcm) send(afcm, cmd, param);
    }
}

static bool Init() {
    HMODULE gui = GetModuleHandleA("GUI.dll");
    if (!gui) {
        Log("[input] GUI.dll not loaded");
        return false;
    }
    const GuiBuild* addrs = GuiAddrs();
    if (!addrs) return false;
    g_guiBase = reinterpret_cast<uintptr_t>(gui);
    Log("[input] GUI.dll at %p", gui);

    WC_GetInstance      = reinterpret_cast<FnWCGetInstance>(g_guiBase + addrs->wcGetInstance);
    WC_GetDragObject    = reinterpret_cast<FnWCGetDragObj>(g_guiBase + addrs->wcGetDragObject);
    WC_SetMousePosition = reinterpret_cast<FnWCSetMousePos>(g_guiBase + addrs->wcSetMousePosition);
    return true;
}

}  // namespace GUIAPI

// ── Constants ───────────────────────────────────────────────────────────

constexpr int kActionStartForward = 1;
constexpr int kActionStopForward  = 2;
constexpr int kActionStartReverse = 3;
// kActionFullStop (22) is a periodic heartbeat, not handled by us.

// ── Mouse event filters ─────────────────────────────────────────────────

static constexpr int kMaxMouseFilters = 4;
static MouseEventFilter g_mouseFilters[kMaxMouseFilters] = {};
static int g_mouseFilterCount = 0;

void RegisterMouseFilter(MouseEventFilter filter) {
    if (g_mouseFilterCount < kMaxMouseFilters) {
        g_mouseFilters[g_mouseFilterCount++] = filter;
        Log("[input] mouse filter registered (%d total)", g_mouseFilterCount);
    }
}

void UnregisterMouseFilter(MouseEventFilter filter) {
    for (int i = 0; i < g_mouseFilterCount; ++i) {
        if (g_mouseFilters[i] == filter) {
            for (int j = i; j < g_mouseFilterCount - 1; ++j)
                g_mouseFilters[j] = g_mouseFilters[j + 1];
            g_mouseFilters[--g_mouseFilterCount] = nullptr;
            return;
        }
    }
}

static bool RunMouseFilters(MouseEventType type, const float* pos_or_delta,
                             int button, int flags) {
    for (int i = 0; i < g_mouseFilterCount; ++i) {
        if (g_mouseFilters[i] && g_mouseFilters[i](type, pos_or_delta, button, flags))
            return true;
    }
    return false;
}

// ── State ───────────────────────────────────────────────────────────────

static InputState g_input;

// ── Helpers ─────────────────────────────────────────────────────────────

bool IsCameraEnabled() {
    AOVariant v{};
    if (GameAPI::GetVariant("AOR_CamOn", v) &&
        v.type == static_cast<uint32_t>(VariantType::Bool)) {
        return v.as_bool;
    }
    return true;
}

bool IsMouseRunEnabled() {
    AOVariant v{};
    if (GameAPI::GetVariant("AOR_MouseRun", v) &&
        v.type == static_cast<uint32_t>(VariantType::Bool)) {
        return v.as_bool;
    }
    return true;
}

// ── Autorun state ───────────────────────────────────────────────────────
//
// Autorun is detected at its source by hooking SlotMovementForward in
// GUI.dll and checking the return address to identify calls from
// SlotMovementAutoRun. This gives us a definitive "the user pressed
// the autorun key" signal, separate from the forward key.

static bool g_autorunActive = false;              // autorun toggle state
static bool g_autorunWasActiveBeforeBoth = false;  // for case 6 (pre-existing autorun)

// Check if enhanced autorun (toggle + persist through W release) is enabled.
static bool IsAutorunEnabled() {
    AOVariant v{};
    if (GameAPI::GetVariant("AOR_AutoRun", v) &&
        v.type == static_cast<uint32_t>(VariantType::Bool)) {
        return v.as_bool;
    }
    return true;
}

static AOString MakeString(const char* str) {
    AOString s;
    std::memset(&s, 0, sizeof(s));
    uint32_t len = static_cast<uint32_t>(std::strlen(str));
    if (len < 16) {
        std::memcpy(s.sso_buf, str, len + 1);
        s.length = len;
        s.capacity = 15;
    } else {
        s.heap_ptr = const_cast<char*>(str);
        s.length = len;
        s.capacity = len;
    }
    return s;
}

static float GetMouseSensitivity() {
    if (!GameAPI::GetDValue) return 1.0f;
    static const char kName[] = "MouseTurnSensitivity";
    AOString name = MakeString(kName);
    AOVariant result{};
    GameAPI::GetDValue(&result, name, false);
    if (result.type == static_cast<uint32_t>(VariantType::Float))
        return result.as_float;
    if (result.type == static_cast<uint32_t>(VariantType::Int))
        return static_cast<float>(result.as_int);
    return 1.0f;
}

static void BeginDragVisuals() {
    if (g_input.cursor_hidden) return;
    void* wc = GUIAPI::WC_GetInstance();
    if (wc) {
        auto* cursor = reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(wc) + 0x48);
        g_input.saved_cursor_x = cursor[0];
        g_input.saved_cursor_y = cursor[1];
    }
    g_input.accum_dist = 0.0f;
    g_input.cursor_hidden = true;
    GUIAPI::AFCMSend(0x12, 0x3d);
}

static void EndDragVisuals() {
    if (!g_input.cursor_hidden) return;
    g_input.cursor_hidden = false;
    GUIAPI::AFCMSend(0x12, 6);
}

// ── Public accessors ────────────────────────────────────────────────────

const InputState& GetInputState() {
    return g_input;
}

// ── Forward movement helpers ────────────────────────────────────────────

// Send StartForward via trampoline (bypasses our filter).
static void DispatchStartForward(void* engine) {
    if (GamecodeAPI::N3Msg_MovementChanged)
        GamecodeAPI::N3Msg_MovementChanged(engine,
            kActionStartForward, 0.0f, 0.0f, true);
}

// Send StopForward via trampoline (bypasses our filter).
static void DispatchStopForward(void* engine) {
    if (GamecodeAPI::N3Msg_MovementChanged)
        GamecodeAPI::N3Msg_MovementChanged(engine,
            kActionStopForward, 0.0f, 0.0f, true);
}

// Engine instance accessor — resolved from N3.dll for movement dispatch.
using FnGetEngineInstance = void*(__cdecl*)();
static FnGetEngineInstance g_getEngineInstance = nullptr;

static void* GetEngine() {
    return g_getEngineInstance ? g_getEngineInstance() : nullptr;
}

// ── SlotMovementForward hook ────────────────────────────────────────────
//
// Hooks the forward key handler in GUI.dll. Both the forward key and the
// autorun key call this function. We detect autorun by checking the return
// address: if the caller is SlotMovementAutoRun, it's autorun.
//
// param_1: false = key down, true = key up.
// Original computes action = param_1 + 1 (false→1=StartForward, true→2=StopForward).

// GuiBuild::slotMovementForward; GuiBuild::autorunCallerRet is the return
// address from SlotMovementAutoRun's CALL.

using FnSlotMovementForward = void(__thiscall*)(void* this_ecx, bool param_1);
static FnSlotMovementForward g_origSlotForward = nullptr;

static void __fastcall SlotMovementForwardDetour(
        void* this_ecx, void* /*edx*/, bool param_1) {
    if (!IsAutorunEnabled()) {
        g_origSlotForward(this_ecx, param_1);
        return;
    }

    bool isAutorun = (__builtin_return_address(0) ==
                      reinterpret_cast<void*>(GUIAPI::g_guiBase + GuiAddrs()->autorunCallerRet));

    if (isAutorun) {
        // Autorun key pressed (key-down only — SlotMovementAutoRun
        // ignores key-up, so we only ever see param_1=false here).
        if (g_autorunActive) {
            // Toggle OFF: send StopForward by calling original with true.
            g_autorunActive = false;
            g_origSlotForward(this_ecx, true);
        } else {
            // Toggle ON: send StartForward by calling original with false.
            g_autorunActive = true;
            g_origSlotForward(this_ecx, false);
        }
        return;
    }

    // Normal forward key (W / up arrow / etc.)
    if (!param_1 && g_autorunActive) {
        // Forward key pressed while autorunning → cancel autorun.
        // The key-down StartForward passes through (redundant, already moving).
        // When the key is released, StopForward will pass through too
        // (autorun is now off) and the character stops.
        g_autorunActive = false;
    }

    g_origSlotForward(this_ecx, param_1);
}

// ── N3Msg_MovementChanged filter ────────────────────────────────────────
//
// Now much simpler: suppresses Start/StopForward during BOTH_HELD,
// suppresses StopForward when autorun is active (so W release during
// autorun doesn't cancel it), and cancels autorun on backward movement.

static void __fastcall MovementChangedDetour(
        void* engine, void* /*edx*/, int action, float f1, float f2, bool sync) {

    if (action == kActionStartForward) {
        if (g_input.state == MouseState::BOTH_HELD && IsMouseRunEnabled())
            return;  // mouse owns forward, suppress
    }

    if (action == kActionStopForward) {
        if (g_input.state == MouseState::BOTH_HELD && IsMouseRunEnabled())
            return;  // mouse owns forward, suppress
        if (g_autorunActive)
            return;  // autorun persists through W release
    }

    if (action == kActionStartReverse && g_autorunActive) {
        // Backward cancels autorun. Stop forward first.
        g_autorunActive = false;
        GamecodeAPI::N3Msg_MovementChanged(engine,
            kActionStopForward, 0.0f, 0.0f, true);
    }

    // FullStop (action 22) is a periodic heartbeat from CheckMotionUpdate,
    // NOT a user-initiated stop. Do not let it cancel autorun.

    GamecodeAPI::N3Msg_MovementChanged(engine, action, f1, f2, sync);
}

// ── ActionViewMouseHandler_c detours ────────────────────────────────────

using FnOnMouseDown = void(__thiscall*)(void* this_ecx, const float* pos, int button, int clickFlags);
static FnOnMouseDown g_origOnMouseDown = nullptr;

using FnOnMouseMove = void(__thiscall*)(void* this_ecx, const float* delta);
static FnOnMouseMove g_origOnMouseMove = nullptr;

using FnOnMouseUp = void(__thiscall*)(void* this_ecx, const float* pos, int button);
static FnOnMouseUp g_origOnMouseUp = nullptr;

using FnEndDrag = void(__thiscall*)(void* this_ecx);
static FnEndDrag g_origEndDrag = nullptr;

// ── OnMouseDown ─────────────────────────────────────────────────────────

static void __fastcall OnMouseDownDetour(void* this_ecx, void* /*edx*/,
                                          const float* pos, int button,
                                          int clickFlags) {
    if (RunMouseFilters(MouseEventType::Down, pos, button, clickFlags))
        return;

    g_origOnMouseDown(this_ecx, pos, button, clickFlags);

    if (!IsCameraEnabled()) return;

    void* wc = GUIAPI::WC_GetInstance();
    if (wc && GUIAPI::WC_GetDragObject(wc)) return;

    auto* handler = reinterpret_cast<uint8_t*>(this_ecx);
    *reinterpret_cast<int*>(handler + 0x08) = 0;
    *reinterpret_cast<int*>(handler + 0x0C) = 0;
    *reinterpret_cast<int*>(handler + 0x10) = static_cast<int>(0xFFFFFFFF);

    bool mouseRunOn = IsMouseRunEnabled();

    if (button == 1) {
        switch (g_input.state) {
        case MouseState::PENDING_RMB:
        case MouseState::DRAGGING_RMB:
            if (mouseRunOn) {
                if (g_input.state == MouseState::PENDING_RMB)
                    BeginDragVisuals();
                g_autorunWasActiveBeforeBoth = g_autorunActive;
                g_input.state = MouseState::BOTH_HELD;
                if (void* eng = GetEngine()) DispatchStartForward(eng);
            }
            break;
        case MouseState::DRAGGING_LMB:
        case MouseState::BOTH_HELD:
            break;
        default:
            g_input.state = MouseState::PENDING_LMB;
            break;
        }
    } else if (button == 2) {
        switch (g_input.state) {
        case MouseState::PENDING_LMB:
        case MouseState::DRAGGING_LMB:
            if (mouseRunOn) {
                if (g_input.state == MouseState::PENDING_LMB)
                    BeginDragVisuals();
                if (g_input.state == MouseState::DRAGGING_LMB)
                    GUIAPI::EndCameraMouseLook();
                g_autorunWasActiveBeforeBoth = g_autorunActive;
                g_input.state = MouseState::BOTH_HELD;
                if (void* eng = GetEngine()) DispatchStartForward(eng);
            }
            break;
        case MouseState::DRAGGING_RMB:
        case MouseState::BOTH_HELD:
            break;
        default:
            g_input.state = MouseState::PENDING_RMB;
            break;
        }
    }
}

// ── OnMouseMove ─────────────────────────────────────────────────────────

static void __fastcall OnMouseMoveDetour(void* this_ecx, void* /*edx*/,
                                          const float* delta) {
    if (RunMouseFilters(MouseEventType::Move, delta, 0, 0))
        return;

    if (!IsCameraEnabled()) {
        g_origOnMouseMove(this_ecx, delta);
        return;
    }

    if (g_input.state == MouseState::PENDING_LMB) {
        BeginDragVisuals();
        g_input.state = MouseState::DRAGGING_LMB;
    } else if (g_input.state == MouseState::PENDING_RMB) {
        BeginDragVisuals();
        g_input.state = MouseState::DRAGGING_RMB;
    }

    if (g_input.state == MouseState::IDLE) return;

    float mag = std::sqrt(delta[0] * delta[0] + delta[1] * delta[1]);
    g_input.accum_dist += std::fabs(mag);

    float sens = GetMouseSensitivity();
    float dx = delta[0] * sens;
    float dy = delta[1] * sens;

    switch (g_input.state) {
    case MouseState::DRAGGING_LMB:
        GUIAPI::CameraMouseLookMovement(dx, dy);
        break;
    case MouseState::DRAGGING_RMB:
    case MouseState::BOTH_HELD:
        GUIAPI::MouseMovement(dx, dy);
        break;
    default:
        break;
    }

    float saved[2] = { g_input.saved_cursor_x, g_input.saved_cursor_y };
    void* wc = GUIAPI::WC_GetInstance();
    if (wc) GUIAPI::WC_SetMousePosition(wc, saved, true);
}

// ── OnMouseUp ───────────────────────────────────────────────────────────

static void __fastcall OnMouseUpDetour(void* this_ecx, void* /*edx*/,
                                        const float* pos, int button) {
    if (RunMouseFilters(MouseEventType::Up, pos, button, 0))
        return;

    if (!IsCameraEnabled()) {
        g_origOnMouseUp(this_ecx, pos, button);
        return;
    }

    float threshold = 0.0f;
    if (GUIAPI::g_guiBase) {
        threshold = *reinterpret_cast<float*>(GUIAPI::g_guiBase + GuiAddrs()->dragThreshold);
    }

    bool wasDragging = (g_input.state == MouseState::DRAGGING_LMB ||
                        g_input.state == MouseState::DRAGGING_RMB ||
                        g_input.state == MouseState::BOTH_HELD);
    if (wasDragging && g_input.accum_dist >= threshold) return;

    auto* handler = reinterpret_cast<uint8_t*>(this_ecx);
    *reinterpret_cast<int*>(handler + 0x08) = 0;
    *reinterpret_cast<float*>(handler + 0x1C) = 0.0f;

    g_origOnMouseUp(this_ecx, pos, button);
}

// ── EndDrag ─────────────────────────────────────────────────────────────

static void __fastcall EndDragDetour(void* this_ecx, void* /*edx*/) {
    if (RunMouseFilters(MouseEventType::EndDrag, nullptr, 0, 0))
        return;

    if (!IsCameraEnabled()) {
        g_origEndDrag(this_ecx);
        return;
    }

    bool lmb = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    bool rmb = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;

    switch (g_input.state) {
    case MouseState::BOTH_HELD: {
        void* eng = GetEngine();
        bool willResumeAutorun = g_autorunActive && !g_autorunWasActiveBeforeBoth;

        if (willResumeAutorun) {
            // Autorun was activated DURING BOTH_HELD (case 5).
            // Seamless handoff — don't stop, let autorun take over.
        } else {
            // End mouse-driven forward movement.
            if (eng) DispatchStopForward(eng);
            if (g_autorunWasActiveBeforeBoth) {
                // Was autorunning BEFORE BOTH_HELD (case 6). Cancel it.
                g_autorunActive = false;
            }
        }

        if (rmb && !lmb) {
            g_input.state = MouseState::DRAGGING_RMB;
        } else if (lmb && !rmb) {
            GUIAPI::EndMouseLook();
            g_input.state = MouseState::DRAGGING_LMB;
        } else {
            GUIAPI::EndMouseLook();
            EndDragVisuals();
            g_input.state = MouseState::IDLE;
        }
        break;
    }

    case MouseState::DRAGGING_LMB:
        if (!lmb) {
            GUIAPI::EndCameraMouseLook();
            EndDragVisuals();
            g_input.state = MouseState::IDLE;
        }
        break;

    case MouseState::DRAGGING_RMB:
        if (!rmb) {
            GUIAPI::EndMouseLook();
            EndDragVisuals();
            g_input.state = MouseState::IDLE;
        }
        break;

    case MouseState::PENDING_LMB:
    case MouseState::PENDING_RMB:
        g_input.state = MouseState::IDLE;
        break;

    case MouseState::IDLE:
        break;
    }

    auto* handler = reinterpret_cast<uint8_t*>(this_ecx);
    *reinterpret_cast<int*>(handler + 0x08) = 0;
    *reinterpret_cast<int*>(handler + 0x0C) = 0;
    *reinterpret_cast<int*>(handler + 0x10) = static_cast<int>(0xFFFFFFFF);
}

// ── Init ────────────────────────────────────────────────────────────────

bool InitInputHandler() {
    // Resolve engine instance accessor from N3.dll (for movement dispatch).
    HMODULE n3 = GetModuleHandleA("N3.dll");
    if (n3) {
        g_getEngineInstance = reinterpret_cast<FnGetEngineInstance>(
            GetProcAddress(n3, "?GetInstance@n3EngineClient_t@@SAPAV1@XZ"));
        Log("[input] GetEngineInstance -> %p", g_getEngineInstance);
    }

    if (!GamecodeAPI::Init()) {
        Log("[input] Gamecode API init failed — mouse-run disabled");
    }

    if (!GUIAPI::Init()) {
        Log("[input] GUI API init failed — input handler disabled");
        return false;
    }

    // Hook SlotMovementForward (GUI.dll) — autorun detection.
    {
        void* addr = ResolveRVA("GUI.dll", GuiAddrs()->slotMovementForward);
        if (!addr) { Log("[input] SlotMovementForward RVA invalid"); return false; }
        auto* bytes = static_cast<uint8_t*>(addr);
        Log("[input] SlotMovementForward at %p, prologue: %02X %02X %02X %02X %02X",
            addr, bytes[0], bytes[1], bytes[2], bytes[3], bytes[4]);
        void* tramp = nullptr;
        if (!InstallHook(addr, reinterpret_cast<void*>(&SlotMovementForwardDetour), &tramp)) {
            Log("[input] failed to hook SlotMovementForward — autorun disabled");
        } else {
            g_origSlotForward = reinterpret_cast<FnSlotMovementForward>(tramp);
            Log("[input] SlotMovementForward hook installed — autorun active");
        }
    }

    // Hook N3Msg_MovementChanged (non-critical).
    if (GamecodeAPI::N3Msg_MovementChanged_Addr) {
        auto* bytes = static_cast<uint8_t*>(GamecodeAPI::N3Msg_MovementChanged_Addr);
        Log("[input] MovementChanged at %p, prologue: %02X %02X %02X %02X %02X",
            GamecodeAPI::N3Msg_MovementChanged_Addr,
            bytes[0], bytes[1], bytes[2], bytes[3], bytes[4]);

        void* tramp = nullptr;
        if (InstallHook(GamecodeAPI::N3Msg_MovementChanged_Addr,
                        reinterpret_cast<void*>(&MovementChangedDetour),
                        &tramp)) {
            GamecodeAPI::N3Msg_MovementChanged =
                reinterpret_cast<GamecodeAPI::FnN3MsgMovementChanged>(tramp);
            Log("[input] MovementChanged hook installed");
        } else {
            Log("[input] MovementChanged hook failed — forward filter disabled");
        }
    }

    // Hook OnMouseDown.
    {
        void* addr = ResolveRVA("GUI.dll", GuiAddrs()->onMouseDown);
        if (!addr) { Log("[input] OnMouseDown RVA invalid"); return false; }
        void* tramp = nullptr;
        if (!InstallHook(addr, reinterpret_cast<void*>(&OnMouseDownDetour), &tramp)) {
            Log("[input] failed to hook OnMouseDown"); return false;
        }
        g_origOnMouseDown = reinterpret_cast<FnOnMouseDown>(tramp);
        Log("[input] OnMouseDown hook installed");
    }

    // Hook OnMouseMove.
    {
        void* addr = ResolveRVA("GUI.dll", GuiAddrs()->onMouseMove);
        if (!addr) { Log("[input] OnMouseMove RVA invalid"); return false; }
        void* tramp = nullptr;
        if (!InstallHook(addr, reinterpret_cast<void*>(&OnMouseMoveDetour), &tramp)) {
            Log("[input] failed to hook OnMouseMove"); return false;
        }
        g_origOnMouseMove = reinterpret_cast<FnOnMouseMove>(tramp);
        Log("[input] OnMouseMove hook installed");
    }

    // Hook OnMouseUp.
    {
        void* addr = ResolveRVA("GUI.dll", GuiAddrs()->onMouseUp);
        if (!addr) { Log("[input] OnMouseUp RVA invalid"); return false; }
        void* tramp = nullptr;
        if (!InstallHook(addr, reinterpret_cast<void*>(&OnMouseUpDetour), &tramp)) {
            Log("[input] failed to hook OnMouseUp"); return false;
        }
        g_origOnMouseUp = reinterpret_cast<FnOnMouseUp>(tramp);
        Log("[input] OnMouseUp hook installed");
    }

    // Hook EndDrag.
    {
        void* addr = ResolveRVA("GUI.dll", GuiAddrs()->endDrag);
        if (!addr) { Log("[input] EndDrag RVA invalid"); return false; }
        void* tramp = nullptr;
        if (!InstallHook(addr, reinterpret_cast<void*>(&EndDragDetour), &tramp)) {
            Log("[input] failed to hook EndDrag"); return false;
        }
        g_origEndDrag = reinterpret_cast<FnEndDrag>(tramp);
        Log("[input] EndDrag hook installed");
    }

    Log("[input] All input hooks installed");
    return true;
}

}  // namespace aor
