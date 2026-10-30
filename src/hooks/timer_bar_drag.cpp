// timer_bar_drag.cpp — Draggable timer bars (cast/action progress bars).
//
// The stock timer bars (nano cast, equip, reload, attack cooldown) are
// small fixed-position RenderWindow_t sprites. This module:
//   1. Hooks CreateTimer to reposition new bars using a saved offset
//   2. Registers a mouse filter so LMB-drag on any bar moves the group
//   3. Persists the offset to AOReloaded.ini via the DValue system
//
// Future extension points:
//   - ScaleAllBars(): call RenderWindow_t::Resize on each bar for
//     configurable dimensions (needs PowerBar_t resize too)
//   - Text overlay: TimerBarBase_c+0x0C already holds a TextLine_t*;
//     hook StartTimerBar to capture the actual nano name from the
//     TimerEntry_t and update the text after creation

#include "hooks/timer_bar_drag.h"
#include "hooks/input_handler.h"
#include "hooks/hook_engine.h"
#include "ao/game_api.h"
#include "ao/gui_build.h"
#include "ao/types.h"
#include "core/logging.h"
#include "core/settings.h"

#include <windows.h>
#include <cstdint>
#include <cstring>

namespace aor {

// ── Constants ───────────────────────────────────────────────────────────

// Native fill-sprite dimensions for GFX 0x1a9, read from Sprite_t::
// GetOriginalSize during TimerBarBase_c construction (confirmed
// empirically: AOReloaded.log `ScaleBar: base=(113,10)`).  These are the
// slider values at which no upscaling/downscaling occurs — pixel-perfect.
// The initial IPoint(0x80, 0x10) in the ctor (128×16) is a transient
// scratch value overwritten by the second Resize; ignore it.
//
// Stock game uses a 20-pixel slot pitch → 10-pixel gaps between visible
// bars.  We prefer a tighter 4-pixel gap at every height; the runtime
// pitch = g_barH + kSlotPadding, so gaps stay constant when the user
// resizes (see RepositionBar).
static constexpr int kDefaultX        = 40;
static constexpr int kDefaultBarW     = 113;  // native fill-sprite width
static constexpr int kDefaultBarH     = 10;   // native fill-sprite height
static constexpr int kSlotPadding     = 4;    // visible gap between bars

// GUI.dll addresses come from GuiAddrs() (ao/gui_build.h): CreateTimer's
// TimerBar_c path (logout, camp) and GameTimerBar_c path (equip, nano,
// attack, reload), RenderWindow_t::Reposition / SetScale / Resize.
// RenderWindow_t::Resize only re-tiles background, doesn't scale content.

// TimerBarBase_c field offsets.
static constexpr int kBarRenderWindow = 0x04;
static constexpr int kBarSlotIndex    = 0x18;

// Future extension offsets (kept for documentation, see plan):
//   kBarPowerBar  = 0x08  — PowerBar_t* for resizing
//   kBarTextLine  = 0x0C  — TextLine_t* for nano name overlay
//   kBarIdentType = 0x10  — timer type (1-5) for type-specific behavior

// TimerSystemModule_t field offsets.
static constexpr int kTSMTimerList    = 0x28;

// ── Preview-bar constants ───────────────────────────────────────────────

// Canonical colours per Timer_* type — sourced from StartTimerBar's dispatcher
// in GUI.dll.  Index is the identity type (1..5).
static constexpr uint32_t kTypeColors[6] = {
    0,          // 0 unused
    0xaaffaa,   // 1 Timer_Attack  (green)
    0xaaffff,   // 2 Timer_Special (cyan)
    0xffaaaa,   // 3 Timer_Nano    (red)
    0xaaaaff,   // 4 Timer_Item    (blue)
    0xffffff,   // 5 Timer_Reload  (white)
};

// Labels double as a colour legend — the user sees which colour maps to which
// in-game bar type.
static const char* const kTypeLabels[6] = {
    nullptr, "Attack", "Special", "Nano", "Item", "Reload"
};

// Preview instance IDs occupy the top of int32 range so they can never
// collide with real AOIDs, which cluster near zero.  Identities are stored
// as two signed ints in-game, so 0x7FFFFFF1..0x7FFFFFF5 are safe.
static constexpr int kPreviewInstanceBase = 0x7FFFFFF0;

// PowerBar_t::AdjustPowerLevel(float) sets fill 0..1.

// TSM::StopTimerbarMessage — verified via Ghidra.
// Walks the timer list, matches by (type, instance) with instance==-1 acting
// as a type-wide wildcard, and calls the private TSM::DeleteTimer on matches.
// DeleteTimer frees slot_flags[slot_index] at TSM+0x34, invokes the bar's
// scalar-deleting destructor at vftable[1] (arg=1 → run dtor + operator
// delete), then erases the std::list node cleanly.  End-to-end zero-leak
// removal — preferred over our manual fallback.

// ── N3InterfaceModule_t API — name resolution ───────────────────────────
// Function pointers are cached in GUI.dll's data section, filled at
// runtime by the game's module system.  Read via ReadCachedPtr.

// GUI.dll data-section RVAs for N3InterfaceModule_t function pointers:
// GuiBuild::iatN3IMGetInstance, iatN3MsgGetName.

using FnN3IMGetInstance = void*(__cdecl*)();
using FnN3MsgGetName = const char*(__thiscall*)(void* n3im,
                                                 const int* identity1,
                                                 const int* identity2);

// Unused API thunks kept for documentation / future use (PRK 2019 build):
//   kN3MsgGetSkillRVA    = 0x1a7728  — N3Msg_GetSkill(Stat_e, int), 2-param local player overload
//   kN3MsgGetSkill4RVA   = 0x1a773c  — N3Msg_GetSkill(Identity_t&, Stat_e, int, Identity_t&), 4-param
//   kN3MsgTemplateIDRVA  = 0x1a771c  — N3Msg_TemplateIDToDynelID(Identity_t const&)
//   kLDBGetTextStrRVA    = 0x1a85c0  — LDBface::GetText(uint, char const*), string key overload
//   kLDBGetTextIdRVA     = 0x1a85dc  — LDBface::GetText(uint, uint), numeric ID overload
//   kStatCurrentNano     = 53        — player stat that holds some ID during nano cast

// ── CastNanoSpell hook — captures nano identity at cast time ────────────
// N3Msg_CastNanoSpell(Identity_t const&, Identity_t const&) is the function
// GUI.dll calls to initiate a nano cast.  We hook it from Interfaces.dll to
// capture the nano's real identity (type + AOID), which we then use for
// name lookup when the timer bar is created moments later.

using FnCastNanoSpell = void(__thiscall*)(void* n3im,
                                          const int* nanoIdentity,
                                          const int* targetIdentity);
static FnCastNanoSpell g_origCastNanoSpell = nullptr;
static int g_lastCastNanoId[2] = { 0, 0 };  // {type, instance} from last cast

static void __fastcall CastNanoSpellDetour(void* ecx_this, void* /*edx*/,
                                            const int* nanoId,
                                            const int* targetId) {
    if (nanoId) {
        g_lastCastNanoId[0] = nanoId[0];
        g_lastCastNanoId[1] = nanoId[1];
        Log("[timerbar] CastNanoSpell: nano={0x%X,%d} target={0x%X,%d}",
            nanoId[0], nanoId[1],
            targetId ? targetId[0] : 0, targetId ? targetId[1] : 0);
    }
    g_origCastNanoSpell(ecx_this, nanoId, targetId);
}

// LDB function pointers — see "Unused API thunks" comments above.

// ── GUI.dll function pointers ───────────────────────────────────────────

static uintptr_t g_guiBase = 0;

// RenderWindow_t::Reposition(IPoint const&) — writes x,y at +0x2c/+0x30,
// then repositions internal RenderSprite_t grid and refreshes HotSpot children.
using FnRWReposition = void(__thiscall*)(void* renderWindow, const int* ipoint);
static FnRWReposition g_rwReposition = nullptr;

// RenderWindow_t::SetScale(float const&, float const&) — stores scale at
// +0x3c/+0x40, propagates to RenderSprite_t tiles.  Updates dest dimensions
// but does NOT rebuild GPU quad vertices — must follow with Resize.
using FnRWSetScale = void(__thiscall*)(void* renderWindow,
                                        const float* scaleX, const float* scaleY);
static FnRWSetScale g_rwSetScale = nullptr;

// RenderWindow_t::Resize(IPoint const&) — re-tiles the grid AND rebuilds
// RenderSprite_t quad vertices using current scale values.
using FnRWResize = void(__thiscall*)(void* renderWindow, const int* ipoint);
static FnRWResize g_rwResize = nullptr;

// TimerSystemModule_t::GetInstance — lazy singleton accessor.
using FnTSMGetInstance = void*(__cdecl*)();
static FnTSMGetInstance g_tsmGetInstance = nullptr;

// WindowController_c::GetInstance — cursor position at WC+0x48.
using FnWCGetInstance = void*(__cdecl*)();
static FnWCGetInstance g_wcGetInstance = nullptr;

// PowerBar_t::AdjustPowerLevel(float progress) — sets the progress fill
// on a timer bar's PowerBar_t* (TimerBarBase_c+0x08).  progress is clamped
// to [0, 1].  Used to force preview bars to full fill.
using FnPBAdjustPowerLevel = void(__thiscall*)(void* powerBar, float progress);
static FnPBAdjustPowerLevel g_pbAdjustPowerLevel = nullptr;

// TSM::StopTimerbarMessage(Identity_t) — dedicated remover.  Identity_t is
// passed by value (8 bytes), which on MSVC x86 __thiscall flattens to two
// int stack arguments: (type, instance).  instance==-1 acts as a wildcard
// removing all bars of the given type.
using FnTSMStopTimerbar = void(__thiscall*)(void* tsm, int type, int instance);
static FnTSMStopTimerbar g_tsmStopTimerbar = nullptr;

// ── CreateTimer hook types ──────────────────────────────────────────────

// Overload 1: TimerBar_c* __thiscall CreateTimer(int, Identity_t const&,
//             char const*, unsigned int).  RET 0x10.  Logout, camp bars.
using FnCreateTimer = void*(__thiscall*)(void* this_tsm, int parent,
                                         const void* identity,
                                         const char* name, uint32_t color);
static FnCreateTimer g_origCreateTimer = nullptr;

// Overload 2: GameTimerBar_c* __thiscall CreateTimer(
//             weak_ptr<TimerEntry_t const>, Identity_t const&,
//             char const*, unsigned int).  RET 0x14.
//   weak_ptr is 8 bytes by value → 2 dwords on stack.
using FnCreateGameTimer = void*(__thiscall*)(void* this_tsm,
                                             uint32_t wp_lo, uint32_t wp_hi,
                                             const void* identity,
                                             const char* name, uint32_t color);
static FnCreateGameTimer g_origCreateGameTimer = nullptr;

// ── State ───────────────────────────────────────────────────────────────

static int  g_posX = kDefaultX;      // absolute X position (AOR_TBarX)
static int  g_posY = kDefaultX;      // absolute Y of first bar (AOR_TBarY); default 40
static int  g_barW = kDefaultBarW;   // bar width (AOR_TBarW)
static int  g_barH = kDefaultBarH;   // bar height (AOR_TBarH)
static bool g_previewEnabled = false; // AOR_TBarPrev — show 5 dummy bars for configuring
static void* g_previewBars[5] = {};   // preview bar pointers (one per type 1..5), or nullptr
static bool g_dragging = false;
static float g_dragStartCursorX = 0;
static float g_dragStartCursorY = 0;
static int   g_dragStartPosX = 0;
static int   g_dragStartPosY = 0;

// Captured from the CreateTimer hook's ECX, or resolved via GetInstance.
static void* g_timerSystemModule = nullptr;

// Forward declarations — called from GetTSM on first resolution.
static void RepositionAllBars();
static void ScaleAllBars();
static void SpawnPreviewBars();
static void DespawnPreviewBars();

// Get the TimerSystemModule_t singleton. Tries cached pointer first,
// then falls back to calling GetInstance().
static void* GetTSM() {
    if (g_timerSystemModule) return g_timerSystemModule;
    if (g_tsmGetInstance) {
        g_timerSystemModule = g_tsmGetInstance();
        if (g_timerSystemModule) {
            Log("[timerbar] resolved TSM singleton via GetInstance: %p",
                g_timerSystemModule);
            // Apply saved position/size to any bars that already exist.
            // These functions call GetTSM() but won't recurse because
            // g_timerSystemModule is already set above.
            if (g_posX != kDefaultX || g_posY != kDefaultX)
                RepositionAllBars();
            // Always scale to force visible size = (g_barW, g_barH) regardless
            // of the bar's natural texture dimensions.  The scale factor is
            // derived from the actual stored base, not from kDefault*.
            ScaleAllBars();
            // Preview mode may have been enabled via settings before the
            // TSM singleton was resolvable.  Spawn the dummies now that it
            // exists.  SpawnPreviewBars is idempotent — it no-ops if the
            // bars are already present.
            if (g_previewEnabled && !g_previewBars[0])
                SpawnPreviewBars();
        }
    }
    return g_timerSystemModule;
}

// ── Position helpers ────────────────────────────────────────────────────

static void RepositionBar(void* barBase) {
    auto* rw = *reinterpret_cast<void**>(
        reinterpret_cast<uintptr_t>(barBase) + kBarRenderWindow);
    int slot = *reinterpret_cast<int*>(
        reinterpret_cast<uintptr_t>(barBase) + kBarSlotIndex);

    // Slot pitch scales with the current bar height so tall bars don't
    // overlap and short bars don't leave gaps.  4px padding matches the
    // stock game value (20 pitch − 16 bar = 4).
    const int slotPitch = g_barH + kSlotPadding;

    int pos[2];
    pos[0] = g_posX;
    pos[1] = g_posY + slot * slotPitch;

    if (rw && g_rwReposition)
        g_rwReposition(rw, pos);
}

// Walk the game's std::list<TimerBarBase_c*> and reposition every bar.
static void RepositionAllBars() {
    void* tsm = GetTSM();
    if (!tsm) return;

    // The list head pointer lives at TSM+0x28.  It points to a heap-
    // allocated sentinel node for a doubly-linked list.
    auto** sentinel = *reinterpret_cast<void***>(
        reinterpret_cast<uintptr_t>(tsm) + kTSMTimerList);
    if (!sentinel) return;

    auto** node = reinterpret_cast<void**>(*sentinel);  // first real node
    while (node != sentinel) {
        void* barBase = node[2];  // data at list-node +0x08
        if (barBase)
            RepositionBar(barBase);
        node = reinterpret_cast<void**>(*node);  // next
    }
}

// ── Resize helpers ──────────────────────────────────────────────────────

// kBarPowerBar = 0x08  — PowerBar_t* (future: nano name overlay, text)

static void ScaleBar(void* barBase) {
    auto* rw = *reinterpret_cast<void**>(
        reinterpret_cast<uintptr_t>(barBase) + kBarRenderWindow);
    if (!rw || !g_rwSetScale || !g_rwResize) return;

    // The bar's natural (texture-derived) base size is stored at +0x34/+0x38.
    // TimerBarBase_c's constructor Resizes the RenderWindow to the fill
    // sprite's GetOriginalSize() — i.e. whatever the GFX asset (0x1a9) is,
    // which is NOT 128×16.  Scaling relative to the hardcoded 128×16
    // "default" produced a visual height of `base_h × g_barH/16` instead of
    // g_barH, making the gap between stacked bars grow with scale.  Scale
    // relative to the actual base so visual_size == (g_barW, g_barH) exactly,
    // which is the contract the slot-pitch formula relies on.
    int baseW = *reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(rw) + 0x34);
    int baseH = *reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(rw) + 0x38);
    if (baseW <= 0 || baseH <= 0) return;

    float sx = static_cast<float>(g_barW) / static_cast<float>(baseW);
    float sy = static_cast<float>(g_barH) / static_cast<float>(baseH);

    // Step 1: Set scale factors on all tiles (updates dest dimensions but
    //         does NOT rebuild GPU quad vertices).
    g_rwSetScale(rw, &sx, &sy);

    // Step 2: Trigger a Resize with the CURRENT stored base.  This calls
    //         RenderSprite_t::Resize on each tile, which reads the scale
    //         values we just set and rebuilds the GPU quad vertices at
    //         (base × scale) = (g_barW, g_barH).
    int currentSize[2] = { baseW, baseH };
    g_rwResize(rw, currentSize);

    Log("[timerbar] ScaleBar: base=(%d,%d) scale=(%.3f,%.3f) → visual=(%d,%d)",
        baseW, baseH, sx, sy, g_barW, g_barH);
}

// Walk the game's timer bar list and resize every bar.
static void ScaleAllBars() {
    void* tsm = GetTSM();
    if (!tsm) return;

    auto** sentinel = *reinterpret_cast<void***>(
        reinterpret_cast<uintptr_t>(tsm) + kTSMTimerList);
    if (!sentinel) return;

    auto** node = reinterpret_cast<void**>(*sentinel);
    while (node != sentinel) {
        void* barBase = node[2];
        if (barBase)
            ScaleBar(barBase);
        node = reinterpret_cast<void**>(*node);
    }
}

// ── Preview bars ────────────────────────────────────────────────────────
//
// The preview mode spawns one dummy bar of each in-game type so the user
// can configure position/size without needing to trigger a real cast.
// Dummies are real TimerBarBase_c / GameTimerBar_c instances joining the
// same std::list as live bars, so RepositionAllBars / ScaleAllBars and
// the mouse drag hit-test handle them automatically.

// Find the std::list node containing the given bar pointer.  Returns
// nullptr if not found.  The list stores the bar at node[2] (offset 0x08
// — past prev/next pointers).
static void** FindListNodeFor(void* tsm, void* bar) {
    auto** sentinel = *reinterpret_cast<void***>(
        reinterpret_cast<uintptr_t>(tsm) + kTSMTimerList);
    if (!sentinel) return nullptr;

    auto** node = reinterpret_cast<void**>(*sentinel);
    while (node != sentinel) {
        if (node[2] == bar) return node;
        node = reinterpret_cast<void**>(*node);
    }
    return nullptr;
}

// Fallback destruction path, retained for robustness when
// g_tsmStopTimerbar fails to resolve.  Unlinks the list node, invokes the
// bar's scalar-deleting destructor at vftable[1] (index confirmed by
// decompiling TimerSystemModule_t::DeleteTimer, which uses the same slot),
// which runs ~TimerBarBase_c (freeing RenderWindow_t, PowerBar_t,
// TextLine_t via member dtors) and calls GUI.dll's operator delete on the
// bar itself.
//
// Caveats versus the preferred StopTimerbarMessage path:
//   - The std::list node (12 bytes on 32-bit MSVC STL: next, prev, data)
//     is leaked — its allocator is the game's static CRT and cross-DLL
//     operator delete is unsafe.
//   - The TSM slot_flags[slot_index] byte at TSM+0x34 is not cleared,
//     so the freed slot stays marked occupied until the next frame where
//     FindNextFreePos scans.  Not a correctness issue — at worst the next
//     real bar uses a later slot.
static void ManualRemoveBar(void* tsm, void* bar) {
    void** node = FindListNodeFor(tsm, bar);
    if (!node) {
        Log("[timerbar] preview: ManualRemoveBar — bar %p not in list", bar);
        return;
    }

    // std::list node layout: [0]=next, [1]=prev, [2]=data.
    void** next = reinterpret_cast<void**>(node[0]);
    void** prev = reinterpret_cast<void**>(node[1]);
    if (prev) prev[0] = next;
    if (next) next[1] = prev;

    // Scalar-deleting destructor lives at vftable[1] for TimerBarBase_c
    // (vftable[0] is a SignalTarget_c-inherited virtual).  Verified by
    // decompiling TSM::DeleteTimer, which calls *(vftable+4)(1).
    auto vftbl = *reinterpret_cast<void***>(bar);
    if (vftbl && vftbl[1]) {
        using FnScalarDeletingDtor = void*(__thiscall*)(void*, int);
        auto dtor = reinterpret_cast<FnScalarDeletingDtor>(vftbl[1]);
        dtor(bar, 1);
    }
}

static void SpawnPreviewBars() {
    void* tsm = GetTSM();
    if (!tsm || !g_origCreateTimer) {
        Log("[timerbar] preview: cannot spawn — tsm=%p origCreateTimer=%p",
            tsm, reinterpret_cast<void*>(g_origCreateTimer));
        return;
    }
    if (g_previewBars[0]) return;  // already spawned — idempotent

    Log("[timerbar] preview: spawning 5 dummy bars (tid=%lu)",
        GetCurrentThreadId());

    for (int t = 1; t <= 5; ++t) {
        int identity[2] = { t, kPreviewInstanceBase + t };

        // Overload 1 (CreateTimer with int parent) is the path used for
        // logout/camp bars — they don't require a TimerEntry_t, which
        // means no per-frame update loop will try to dereference a null
        // weak_ptr.  parent=0 = no owning widget (root).
        void* bar = g_origCreateTimer(tsm, /*parent*/ 0, identity,
                                       kTypeLabels[t], kTypeColors[t]);
        if (!bar) {
            Log("[timerbar] preview: CreateTimer(type=%d) returned null", t);
            continue;
        }

        // Full fill — user chose static "full" bars.  If AdjustPowerLevel
        // failed to resolve, the bar stays at whatever progress the
        // constructor initialised it to.
        void* pb = *reinterpret_cast<void**>(
            reinterpret_cast<uintptr_t>(bar) + 0x08);  // kBarPowerBar
        if (pb && g_pbAdjustPowerLevel)
            g_pbAdjustPowerLevel(pb, 1.0f);

        // Our CreateTimerDetour wasn't re-entered (we called the trampoline
        // directly), so apply the saved position/scale manually.
        RepositionBar(bar);
        ScaleBar(bar);

        g_previewBars[t - 1] = bar;
        Log("[timerbar] preview: spawned type=%d bar=%p label=\"%s\"",
            t, bar, kTypeLabels[t]);
    }
}

static void DespawnPreviewBars() {
    void* tsm = GetTSM();
    if (!tsm) {
        // Nothing to remove — TSM never came up.  Clear the slots so a
        // later spawn doesn't think bars already exist.
        for (int i = 0; i < 5; ++i) g_previewBars[i] = nullptr;
        return;
    }

    Log("[timerbar] preview: despawning (tid=%lu)", GetCurrentThreadId());

    for (int t = 1; t <= 5; ++t) {
        void* bar = g_previewBars[t - 1];
        if (!bar) continue;

        if (g_tsmStopTimerbar) {
            // Identity_t passed by value: (type, instance).
            g_tsmStopTimerbar(tsm, t, kPreviewInstanceBase + t);
        } else {
            ManualRemoveBar(tsm, bar);
        }
        g_previewBars[t - 1] = nullptr;
        Log("[timerbar] preview: despawned type=%d", t);
    }
}

// ── Cursor helpers ──────────────────────────────────────────────────────

static bool GetCursorPos(float& outX, float& outY) {
    if (!g_wcGetInstance) return false;
    void* wc = g_wcGetInstance();
    if (!wc) return false;
    auto* cursor = reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(wc) + 0x48);
    outX = cursor[0];
    outY = cursor[1];
    return true;
}

// ── Hit testing ─────────────────────────────────────────────────────────

// Info returned by a successful hit test — tells the caller where the
// clicked bar actually is on screen so the drag offset can be derived.
struct BarHitInfo {
    int actualX;   // RenderWindow_t +0x2c
    int actualY;   // RenderWindow_t +0x30
    int slot;      // TimerBarBase_c +0x18
};

// Check if (px, py) falls within any active timer bar's bounding rect.
// Reads the ACTUAL position from each bar's RenderWindow_t (+0x2c/+0x30)
// rather than computing from the formula, because bars may have been
// created through paths we don't hook.
//
// The bar's visual size is base_size × scale, NOT the raw base size.
// RenderWindow_t stores the tile-grid base at +0x34/+0x38 and the scale
// factors at +0x3c/+0x40 — RenderSprite_t::Resize multiplies them to
// produce the GPU quad size (confirmed by decompiling RenderWindow_t::
// Resize and SetScale).  When ScaleBar applies a non-unit scale, the base
// stays at the stock 128x16 and only the scale changes; hit-testing
// against the base alone produces a tiny rect in the top-left of a
// visually-larger bar.  Multiply through to the true visual size.
//
// On hit, fills *outHit with the bar's actual position and slot.
static bool HitTestTimerBars(float px, float py, BarHitInfo* outHit) {
    void* tsm = GetTSM();
    if (!tsm) {
        Log("[timerbar] hit test: no TSM singleton");
        return false;
    }

    auto** sentinel = *reinterpret_cast<void***>(
        reinterpret_cast<uintptr_t>(tsm) + kTSMTimerList);
    if (!sentinel) {
        Log("[timerbar] hit test: null sentinel");
        return false;
    }

    int barCount = 0;
    auto** node = reinterpret_cast<void**>(*sentinel);
    while (node != sentinel) {
        void* barBase = node[2];
        if (barBase) {
            void* rw = *reinterpret_cast<void**>(
                reinterpret_cast<uintptr_t>(barBase) + kBarRenderWindow);
            if (rw) {
                int bx = *reinterpret_cast<int*>(
                    reinterpret_cast<uintptr_t>(rw) + 0x2c);
                int by = *reinterpret_cast<int*>(
                    reinterpret_cast<uintptr_t>(rw) + 0x30);
                int baseW = *reinterpret_cast<int*>(
                    reinterpret_cast<uintptr_t>(rw) + 0x34);
                int baseH = *reinterpret_cast<int*>(
                    reinterpret_cast<uintptr_t>(rw) + 0x38);
                float sx = *reinterpret_cast<float*>(
                    reinterpret_cast<uintptr_t>(rw) + 0x3c);
                float sy = *reinterpret_cast<float*>(
                    reinterpret_cast<uintptr_t>(rw) + 0x40);
                // Guard against an un-initialised / zero scale — treat
                // as identity so unscaled bars still hit-test correctly.
                if (!(sx > 0.0f)) sx = 1.0f;
                if (!(sy > 0.0f)) sy = 1.0f;
                int bw = static_cast<int>(baseW * sx);
                int bh = static_cast<int>(baseH * sy);

                if (px >= bx && px < bx + bw &&
                    py >= by && py < by + bh) {
                    if (outHit) {
                        outHit->actualX = bx;
                        outHit->actualY = by;
                        outHit->slot = *reinterpret_cast<int*>(
                            reinterpret_cast<uintptr_t>(barBase) + kBarSlotIndex);
                    }
                    return true;
                }
            }
            barCount++;
        }
        node = reinterpret_cast<void**>(*node);
    }
    if (barCount == 0)
        Log("[timerbar] hit test: no active bars in list");
    return false;
}

// ── Persistence ─────────────────────────────────────────────────────────

static void LoadPosition() {
    AOVariant v{};
    if (GameAPI::GetVariant("AOR_TBarX", v) &&
        v.type == static_cast<uint32_t>(VariantType::Int)) {
        g_posX = v.as_int;
    }
    if (GameAPI::GetVariant("AOR_TBarY", v) &&
        v.type == static_cast<uint32_t>(VariantType::Int)) {
        g_posY = v.as_int;
    }
    if (GameAPI::GetVariant("AOR_TBarW", v) &&
        v.type == static_cast<uint32_t>(VariantType::Int)) {
        g_barW = v.as_int;
    }
    if (GameAPI::GetVariant("AOR_TBarH", v) &&
        v.type == static_cast<uint32_t>(VariantType::Int)) {
        g_barH = v.as_int;
    }
    if (GameAPI::GetVariant("AOR_TBarPrev", v) &&
        v.type == static_cast<uint32_t>(VariantType::Bool)) {
        g_previewEnabled = v.as_bool;
    }
    Log("[timerbar] loaded pos=(%d,%d) size=(%d,%d) preview=%d",
        g_posX, g_posY, g_barW, g_barH, g_previewEnabled ? 1 : 0);
}

static void SavePosition() {
    if (!GameAPI::SetDValue) return;

    AOString nameX = AOString::FromShort("AOR_TBarX");
    AOString nameY = AOString::FromShort("AOR_TBarY");
    AOVariant valX = AOVariant::FromInt(g_posX);
    AOVariant valY = AOVariant::FromInt(g_posY);

    GameAPI::SetDValue(nameX, valX);
    GameAPI::SetDValue(nameY, valY);
    Log("[timerbar] saved position: X=%d Y=%d", g_posX, g_posY);
}

// ── CreateTimer hook ────────────────────────────────────────────────────

static void* __fastcall CreateTimerDetour(void* ecx_this, void* /*edx*/,
                                           int parent, const void* identity,
                                           const char* name, uint32_t color) {
    // Capture the singleton pointer on first call.
    if (!g_timerSystemModule)
        g_timerSystemModule = ecx_this;

    // Call the original — creates the bar at the stock hardcoded position.
    void* bar = g_origCreateTimer(ecx_this, parent, identity, name, color);

    if (bar) {
        RepositionBar(bar);
        ScaleBar(bar);
    }

    return bar;
}

// ── Nano name lookup ────────────────────────────────────────────────────
// Read a cached function pointer from GUI.dll's data section.
template<typename T>
static T ReadCachedPtr(uint32_t rva) {
    return *reinterpret_cast<T*>(g_guiBase + rva);
}

// Try to get the nano name.  wp_lo is the raw TimerEntry_t* from the
// weak_ptr passed to CreateGameTimer.  identity is {type, instance} but
// for nano timers instance is always 0.  The actual nano AOID is
// somewhere in the TimerEntry_t.
static const char* TryGetNanoName(const void* identity) {
    if (!identity) return nullptr;

    auto* id = reinterpret_cast<const int*>(identity);
    if (id[0] != 3) return nullptr;  // only nano timers (type 3)

    // Use the nano identity captured from CastNanoSpell hook.
    if (g_lastCastNanoId[0] == 0 && g_lastCastNanoId[1] == 0) {
        Log("[timerbar] no captured nano identity");
        return nullptr;
    }

    auto getInst = ReadCachedPtr<FnN3IMGetInstance>(GuiAddrs()->iatN3IMGetInstance);
    auto getName = ReadCachedPtr<FnN3MsgGetName>(GuiAddrs()->iatN3MsgGetName);
    if (!getInst || !getName) return nullptr;

    void* n3im = getInst();
    if (!n3im) return nullptr;

    // Try GetName with the captured identity directly.
    int zeroId[2] = { 0, 0 };
    const char* result = getName(n3im, g_lastCastNanoId, zeroId);
    if (result && result[0] && std::strcmp(result, "NoName") != 0) {
        Log("[timerbar] nano name (from CastNanoSpell id): \"%s\"", result);
        return result;
    }

    // Try reversed parameter order.
    result = getName(n3im, zeroId, g_lastCastNanoId);
    if (result && result[0] && std::strcmp(result, "NoName") != 0) {
        Log("[timerbar] nano name (reversed): \"%s\"", result);
        return result;
    }

    Log("[timerbar] GetName failed for nano id={0x%X,%d}",
        g_lastCastNanoId[0], g_lastCastNanoId[1]);
    return nullptr;
}

// ── CreateGameTimer hook (overload 2 — equip, nano, attack, reload) ────

static void* __fastcall CreateGameTimerDetour(void* ecx_this, void* /*edx*/,
                                               uint32_t wp_lo, uint32_t wp_hi,
                                               const void* identity,
                                               const char* name, uint32_t color) {
    if (!g_timerSystemModule)
        g_timerSystemModule = ecx_this;

    // For nano timers, try to replace the generic "Nano program" label
    // with the actual nano program name.
    const char* actualName = name;
    const char* nanoName = TryGetNanoName(identity);
    if (nanoName)
        actualName = nanoName;

    void* bar = g_origCreateGameTimer(ecx_this, wp_lo, wp_hi,
                                       identity, actualName, color);

    if (bar) {
        RepositionBar(bar);
        ScaleBar(bar);
    }

    return bar;
}

// ── Mouse event filter ──────────────────────────────────────────────────

static bool TimerBarMouseFilter(MouseEventType type, const float* pos_or_delta,
                                 int button, int /*flags*/) {
    switch (type) {
    case MouseEventType::Down: {
        // Only handle LMB (button 1).
        if (button != 1) return false;
        if (!pos_or_delta) return false;
        // Drag-to-reposition is a configuration tool, not a live-gameplay
        // interaction: bars only swallow clicks while the preview stack is
        // visible.  When preview is off, all clicks pass through to the
        // game's normal handlers — real cast/reload bars remain purely
        // informational and never intercept input.
        if (!g_previewEnabled) return false;

        float cx, cy;
        if (!GetCursorPos(cx, cy)) return false;

        BarHitInfo hit{};
        if (!HitTestTimerBars(cx, cy, &hit))
            return false;

        // Derive the actual position from where the bar really is, not from
        // the saved DValue.  Bars may have been created at the default
        // position through a path that ran before our hook was ready.  The
        // inverse slot formula must use the same pitch RepositionBar uses.
        g_posX = hit.actualX;
        g_posY = hit.actualY - hit.slot * (g_barH + kSlotPadding);

        // Start drag.
        g_dragging = true;
        g_dragStartCursorX = cx;
        g_dragStartCursorY = cy;
        g_dragStartPosX = g_posX;
        g_dragStartPosY = g_posY;
        Log("[timerbar] drag started at (%.0f, %.0f) pos=(%d,%d)",
            cx, cy, g_posX, g_posY);
        return true;
    }

    case MouseEventType::Move: {
        if (!g_dragging) return false;

        // Read current cursor and compute delta from drag start.
        float cx, cy;
        if (!GetCursorPos(cx, cy)) return false;

        g_posX = g_dragStartPosX + static_cast<int>(cx - g_dragStartCursorX);
        g_posY = g_dragStartPosY + static_cast<int>(cy - g_dragStartCursorY);

        RepositionAllBars();
        return true;
    }

    case MouseEventType::Up: {
        if (!g_dragging) return false;
        // Only end drag on LMB release.
        if (button != 1) return false;

        g_dragging = false;
        SavePosition();
        Log("[timerbar] drag ended, pos: X=%d Y=%d", g_posX, g_posY);
        return true;
    }

    case MouseEventType::EndDrag: {
        if (!g_dragging) return false;

        g_dragging = false;
        SavePosition();
        Log("[timerbar] drag ended (EndDrag), pos: X=%d Y=%d",
            g_posX, g_posY);
        return true;
    }
    }

    return false;
}

// ── Settings callback — live slider updates ─────────────────────────────

static void OnSettingChanged(const char* name, int newValue) {
    if (std::strcmp(name, "AOR_TBarX") == 0) {
        g_posX = newValue;
        RepositionAllBars();
    } else if (std::strcmp(name, "AOR_TBarY") == 0) {
        g_posY = newValue;
        RepositionAllBars();
    } else if (std::strcmp(name, "AOR_TBarW") == 0) {
        g_barW = newValue;
        ScaleAllBars();
    } else if (std::strcmp(name, "AOR_TBarH") == 0) {
        g_barH = newValue;
        ScaleAllBars();
        // Slot pitch is derived from g_barH, so changing height must also
        // re-apply the per-slot Y offset or stacked bars would overlap or
        // gap apart as the user drags the slider.
        RepositionAllBars();
    } else if (std::strcmp(name, "AOR_TBarPrev") == 0) {
        bool enabled = (newValue != 0);
        if (enabled == g_previewEnabled) return;
        g_previewEnabled = enabled;
        if (enabled)
            SpawnPreviewBars();
        else
            DespawnPreviewBars();
    }
}

// ── Init ────────────────────────────────────────────────────────────────

bool InitTimerBarDrag() {
    HMODULE gui = GetModuleHandleA("GUI.dll");
    if (!gui) {
        Log("[timerbar] GUI.dll not loaded");
        return false;
    }
    g_guiBase = reinterpret_cast<uintptr_t>(gui);
    const GuiBuild* addrs = GuiAddrs();
    if (!addrs) return false;

    // Resolve GUI.dll function pointers.
    // N3Msg_GetName and N3InterfaceModule_t::GetInstance are cached
    // function pointers at known GUI.dll data section RVAs — resolved
    // lazily by ReadCachedPtr in TryGetNanoName.
    g_rwReposition = reinterpret_cast<FnRWReposition>(
        g_guiBase + addrs->rwReposition);
    g_rwSetScale = reinterpret_cast<FnRWSetScale>(
        g_guiBase + addrs->rwSetScale);
    g_rwResize = reinterpret_cast<FnRWResize>(
        g_guiBase + addrs->rwResize);
    g_tsmGetInstance = reinterpret_cast<FnTSMGetInstance>(
        g_guiBase + addrs->tsmGetInstance);
    g_wcGetInstance = reinterpret_cast<FnWCGetInstance>(
        g_guiBase + addrs->wcGetInstance);
    g_pbAdjustPowerLevel = reinterpret_cast<FnPBAdjustPowerLevel>(
        g_guiBase + addrs->pbAdjustPowerLevel);
    g_tsmStopTimerbar = reinterpret_cast<FnTSMStopTimerbar>(
        g_guiBase + addrs->tsmStopTimerbar);

    // Load saved position from DValues.
    LoadPosition();

    // Register for live slider updates from the options panel.
    RegisterSettingCallback(&OnSettingChanged);

    // Hook CreateTimer.
    void* target = reinterpret_cast<void*>(g_guiBase + addrs->createTimer);
    auto* bytes = static_cast<uint8_t*>(target);
    Log("[timerbar] CreateTimer at %p, prologue: %02X %02X %02X %02X %02X",
        target, bytes[0], bytes[1], bytes[2], bytes[3], bytes[4]);

    void* trampoline = nullptr;
    if (!InstallHook(target, reinterpret_cast<void*>(&CreateTimerDetour),
                     &trampoline)) {
        Log("[timerbar] CreateTimer hook failed");
        return false;
    }
    g_origCreateTimer = reinterpret_cast<FnCreateTimer>(trampoline);
    Log("[timerbar] CreateTimer hook installed");

    // Hook CreateGameTimer (overload 2 — equip, nano, attack, reload).
    {
        void* target2 = reinterpret_cast<void*>(g_guiBase + addrs->createGameTimer);
        auto* bytes2 = static_cast<uint8_t*>(target2);
        Log("[timerbar] CreateGameTimer at %p, prologue: %02X %02X %02X %02X %02X",
            target2, bytes2[0], bytes2[1], bytes2[2], bytes2[3], bytes2[4]);

        void* tramp2 = nullptr;
        if (!InstallHook(target2, reinterpret_cast<void*>(&CreateGameTimerDetour),
                         &tramp2)) {
            Log("[timerbar] CreateGameTimer hook failed");
            // Non-fatal: some bar types won't be repositioned at creation.
        } else {
            g_origCreateGameTimer = reinterpret_cast<FnCreateGameTimer>(tramp2);
            Log("[timerbar] CreateGameTimer hook installed");
        }
    }

    // Hook CastNanoSpell from Interfaces.dll for nano name capture.
    {
        HMODULE iface = GetModuleHandleA("Interfaces.dll");
        if (iface) {
            void* castAddr = reinterpret_cast<void*>(GetProcAddress(iface,
                "?N3Msg_CastNanoSpell@N3InterfaceModule_t@@QBEXABVIdentity_t@@0@Z"));
            Log("[timerbar] CastNanoSpell at %p", castAddr);
            if (castAddr) {
                auto* bytes3 = static_cast<uint8_t*>(castAddr);
                Log("[timerbar] CastNanoSpell prologue: %02X %02X %02X %02X %02X",
                    bytes3[0], bytes3[1], bytes3[2], bytes3[3], bytes3[4]);
                void* tramp3 = nullptr;
                if (InstallHook(castAddr, reinterpret_cast<void*>(&CastNanoSpellDetour),
                                &tramp3)) {
                    g_origCastNanoSpell = reinterpret_cast<FnCastNanoSpell>(tramp3);
                    Log("[timerbar] CastNanoSpell hook installed");
                } else {
                    Log("[timerbar] CastNanoSpell hook failed");
                }
            }
        } else {
            Log("[timerbar] Interfaces.dll not loaded");
        }
    }

    // Register mouse filter with the input handler.
    RegisterMouseFilter(&TimerBarMouseFilter);
    Log("[timerbar] mouse filter registered");

    return true;
}

}  // namespace aor
