// ground_hq_hook.cpp — enlarges AnarchyGround_t per-patch vertex buffers.
//
// See ground_hq_hook.h for motivation. The hook targets an internal helper
// (no exported symbol), resolved by RVA from a Ghidra RE session on
// DisplaySystem.dll. The target is only called by AnarchyGround_t's render
// pass-1 (`FUN_10034658` at RVA 0x34658) — confirmed via xref analysis —
// so scaling is safe and doesn't affect any other subsystem's VBs.

#include "hooks/ground_hq_hook.h"
#include "hooks/hook_engine.h"
#include "ao/game_api.h"
#include "core/logging.h"

#include <windows.h>
#include <climits>
#include <cstdint>
#include <intrin.h>

namespace aor {

namespace {

// RVA of AnarchyGround_t's patch VertexBuffer_c-wrapper constructor inside
// DisplaySystem.dll. Signature: void* __thiscall(void* this, int size).
// Stores `size` at `this+4` (with built-in minimum of 0xcbc), then calls an
// internal helper that actually creates the D3D vertex buffer.
constexpr uint32_t kPatchVBCtorRVA = 0x38606;

// Clamped to [1, 8]. Default 4 is enough for HQ radius up to ~100 based on
// (100/48)² ≈ 4.3 worst-case vertex-count growth. 8 gives margin.
constexpr int kScaleMin     = 1;
constexpr int kScaleMax     = 8;
constexpr int kScaleDefault = 4;

// Read lock-free by the detour hot-path; published by the poller thread.
// Word-sized writes on x86 are atomic, so no fence is required for this use.
volatile int g_vbScale = kScaleDefault;

HANDLE g_pollerThread = nullptr;
HANDLE g_pollerStop   = nullptr;  // manual-reset, signaled on shutdown

using FnPatchVBCtor = void* (__thiscall*)(void* this_, int size);
FnPatchVBCtor g_origPatchVBCtor = nullptr;

// D3D7's 16-bit vertex index limit. Any single VB allocated with
// dwNumVertices > this value is refused by the driver — CreateVertexBuffer
// returns an error which randy31 translates into an unhandled C++ throw.
// Empirically confirmed from instrumented run:
//   size=6867  scaled=27468  -> OK
//   size=19710 scaled=78840  -> throws
// So we never let the scaled value exceed this. Never reduce below the
// raw input: if the game itself asks for > 65535 (unlikely but defensive),
// that's pre-existing client behavior we must not break.
constexpr int kMaxVBCount = 65535;

// Detour: __thiscall → __fastcall(ECX=this, EDX=junk, stack args...).
// The trampoline is typed as the original __thiscall so its RET 4 cleanup
// balances the caller's stack correctly.
void* __fastcall PatchVBCtorDetour(void* this_, void* /*edx*/, int size) {
    int scale = g_vbScale;
    if (scale < kScaleMin) scale = kScaleMin;
    if (scale > kScaleMax) scale = kScaleMax;

    int scaled;
    if (size <= 0) {
        scaled = size;
    } else {
        long long product = static_cast<long long>(size) * scale;
        if (product > kMaxVBCount) product = kMaxVBCount;
        if (product < size)        product = size;  // never shrink
        scaled = static_cast<int>(product);
    }

    return g_origPatchVBCtor(this_, scaled);
}

DWORD WINAPI PollerThreadProc(LPVOID /*param*/) {
    Log("[gndvb] poller started (initial scale=%d)", g_vbScale);
    while (WaitForSingleObject(g_pollerStop, 500) == WAIT_TIMEOUT) {
        AOVariant v{};
        if (!GameAPI::GetVariant("AOR_GndVBScale", v)) continue;
        if (v.type != static_cast<uint32_t>(VariantType::Int)) continue;

        int s = v.as_int;
        if (s < kScaleMin) s = kScaleMin;
        if (s > kScaleMax) s = kScaleMax;
        if (s != g_vbScale) {
            Log("[gndvb] scale changed %d -> %d (applies on next zone)",
                g_vbScale, s);
            g_vbScale = s;
        }
    }
    Log("[gndvb] poller exiting");
    return 0;
}

}  // namespace

bool InitGroundHQHook() {
    HMODULE ds = GetModuleHandleA("DisplaySystem.dll");
    if (!ds) {
        Log("[gndvb] DisplaySystem.dll not loaded — skipping");
        return false;
    }

    void* target = reinterpret_cast<void*>(
        reinterpret_cast<uintptr_t>(ds) + kPatchVBCtorRVA);

    auto* bytes = static_cast<uint8_t*>(target);
    Log("[gndvb] PatchVBCtor at %p, prologue: %02X %02X %02X %02X %02X",
        target, bytes[0], bytes[1], bytes[2], bytes[3], bytes[4]);

    void* trampoline = nullptr;
    if (!InstallHook(target,
                     reinterpret_cast<void*>(&PatchVBCtorDetour),
                     &trampoline)) {
        Log("[gndvb] hook install failed");
        return false;
    }
    g_origPatchVBCtor = reinterpret_cast<FnPatchVBCtor>(trampoline);

    g_pollerStop = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    if (g_pollerStop) {
        g_pollerThread = CreateThread(
            nullptr, 0, PollerThreadProc, nullptr, 0, nullptr);
        if (!g_pollerThread) {
            Log("[gndvb] poller thread create failed: %lu", GetLastError());
        }
    }

    Log("[gndvb] ground-VB scale hook installed (DValue AOR_GndVBScale, "
        "range %d..%d, default %d)",
        kScaleMin, kScaleMax, kScaleDefault);
    return true;
}

}  // namespace aor
