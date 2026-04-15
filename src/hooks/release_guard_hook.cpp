// release_guard_hook.cpp — catches access violations during RResource
// release so they don't terminate the process.
//
// See release_guard_hook.h for motivation. The concrete crash we're
// guarding against was observed at radius=100 during zone unload:
//
//   0:000> ECX = 0x0012000c
//   randy31!RResource_t::ReleaseRResource:
//   5cd52f45 8b4124  mov eax,dword ptr [ecx+24h]  ds:...=????????
//
// Earlier version of this file used a range check (this < 0x200000 =
// "clearly bogus") but that was wrong — 32-bit Windows commonly has
// valid mappings below that threshold (AnarchyOnline.exe itself is
// based at 0x00010000, confirmed from stack traces), and the check
// rejected legitimate releases of objects linked into low-address
// static data. The game state ended up inconsistent and crashed later
// in unrelated paths.
//
// The correct guard is structured exception handling: wrap the call to
// the original, catch any access violation, and return silently. We
// only intervene when a release actually faults. Anything that would
// succeed still succeeds.

#include "hooks/release_guard_hook.h"
#include "hooks/hook_engine.h"
#include "core/logging.h"

#include <windows.h>
#include <cstdint>
#include <excpt.h>

namespace aor {

namespace {

// RVA within randy31.dll. RResource_t::ReleaseRResource. Prologue:
// `8B 41 24 85 C0` (mov eax,[ecx+0x24]; test eax,eax) — the hook
// engine's `mov-this24+test` pattern matches exactly.
constexpr uint32_t kReleaseRResourceRVA = 0x46224;

using FnRelease = void(__thiscall*)(void* this_);
FnRelease g_origRelease = nullptr;

// Rate-limited logging. First few AVs are interesting; steady-state
// would spam if a hot path ever hits it, so cap.
volatile LONG g_loggedCount = 0;
constexpr LONG kMaxLogged = 16;

void LogBlockedAV(void* this_) {
    LONG n = InterlockedIncrement(&g_loggedCount);
    if (n <= kMaxLogged) {
        Log("[relguard] caught AV in ReleaseRResource(this=%p) (sample %ld/%ld)",
            this_, n, kMaxLogged);
        if (n == kMaxLogged) {
            Log("[relguard] further AVs will be silent");
        }
    }
}

// SEH has to live in a function that doesn't also use C++ object
// unwinding (MSVC rejects mixing __try/__except with objects that
// have destructors in the same frame). Keep this tiny.
void __fastcall ReleaseRResourceDetour(void* this_, void* /*edx*/) {
    __try {
        g_origRelease(this_);
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
                    ? EXCEPTION_EXECUTE_HANDLER
                    : EXCEPTION_CONTINUE_SEARCH) {
        LogBlockedAV(this_);
    }
}

}  // namespace

bool InitReleaseGuardHook() {
    HMODULE randy = GetModuleHandleA("randy31.dll");
    if (!randy) {
        Log("[relguard] randy31.dll not loaded — skipping");
        return false;
    }

    void* target = reinterpret_cast<void*>(
        reinterpret_cast<uintptr_t>(randy) + kReleaseRResourceRVA);

    auto* bytes = static_cast<uint8_t*>(target);
    Log("[relguard] ReleaseRResource at %p, prologue: %02X %02X %02X %02X %02X",
        target, bytes[0], bytes[1], bytes[2], bytes[3], bytes[4]);

    void* trampoline = nullptr;
    if (!InstallHook(target,
                     reinterpret_cast<void*>(&ReleaseRResourceDetour),
                     &trampoline)) {
        Log("[relguard] hook install failed");
        return false;
    }
    g_origRelease = reinterpret_cast<FnRelease>(trampoline);

    Log("[relguard] release-guard hook installed (SEH-based)");
    return true;
}

}  // namespace aor
