// dllmain.cpp — AOReloaded entry point.
//
// We masquerade as version.dll (DLL search order hijacking). The game
// loads us early, before most game DLLs are mapped. We therefore do
// only safe, minimal work in DLL_PROCESS_ATTACH:
//
//   1. Init logging.
//   2. Suppress thread attach/detach notifications.
//   3. Spawn a deferred init thread that waits for the game's DLLs
//      to be loaded, then resolves the game API and applies mods.
//
// The version.dll forwarding is handled lazily on first export call,
// which is always after DllMain has returned. No LoadLibrary from
// DllMain.

#include "core/logging.h"
#include "ao/game_api.h"
#include "ao/dvalue_dump.h"
#include "hooks/camera_hook.h"
#include "hooks/ground_hq_hook.h"
#include "hooks/release_guard_hook.h"

#include <windows.h>

namespace {

bool g_initialised = false;

bool IsAnarchyOnlineProcess() {
    wchar_t path[MAX_PATH] = {};
    DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return false;
    const wchar_t* filename = path;
    for (const wchar_t* p = path; *p; ++p) {
        if (*p == L'\\' || *p == L'/') filename = p + 1;
    }
    return _wcsicmp(filename, L"AnarchyOnline.exe") == 0;
}

// Deferred initialisation — runs on a background thread after the game's
// DLLs are loaded. We poll for Utils.dll since that's where the core
// game API lives.
DWORD WINAPI DeferredInit(LPVOID /*param*/) {
    aor::Log("[init] deferred init thread started");

    // Wait for Utils.dll to appear. The game loads it during startup.
    // Timeout after 30 seconds to avoid hanging forever on broken installs.
    for (int i = 0; i < 300; ++i) {
        if (GetModuleHandleA("Utils.dll")) break;
        Sleep(100);
    }

    if (!GetModuleHandleA("Utils.dll")) {
        aor::Log("[init] Utils.dll never loaded — aborting init");
        return 1;
    }

    aor::Log("[init] Utils.dll detected, resolving game API...");
    if (!aor::GameAPI::Init()) {
        aor::Log("[init] game API init failed (partial resolve)");
    }

    // Register our custom DValues early — before the game loads the
    // options panel XML so the widgets can bind to them.
    aor::GameAPI::RegisterBool("AOR_TestToggle", true);
    aor::GameAPI::RegisterInt("AOR_TestSlider", 50);

    // Camera system DValues.
    // Names must be ≤ 15 chars (AOString SSO limit).
    aor::GameAPI::RegisterBool("AOR_CamOn", true);
    aor::GameAPI::RegisterInt("AOR_CYawSpd", 2);

    // Ground high-quality VB scale (1..8, default 4). Raises the effective
    // DisplayGroundFullQualityRadius ceiling by enlarging the per-patch
    // terrain vertex buffers. Changes take effect on next zone load.
    aor::GameAPI::RegisterInt("AOR_GndVBScale", 4);

    // Wait for game world.
    aor::Log("[init] waiting for game world...");
    for (int i = 0; i < 600; ++i) {
        if (aor::GameAPI::Exists("camera_mode")) break;
        Sleep(100);
    }

    if (aor::GameAPI::Exists("camera_mode")) {
        aor::Log("[init] game world detected!");

        // Install camera hooks now that GUI.dll and N3.dll are loaded.
        if (!aor::InitCameraHooks()) {
            aor::Log("[init] camera hooks failed — running without camera mod");
        }

        // Install ground-VB scale hook. DisplaySystem.dll is loaded by the
        // time we reach the game world.
        if (!aor::InitGroundHQHook()) {
            aor::Log("[init] ground HQ hook failed — HQ radius > 48 unsafe");
        }

        // Install defensive guard on randy31's ReleaseRResource. Protects
        // against the zone-teardown use-after-free that surfaces at high
        // DisplayGroundFullQualityRadius (and potentially other paths).
        if (!aor::InitReleaseGuardHook()) {
            aor::Log("[init] release guard hook failed — zone-change CTD risk");
        }
    } else {
        aor::Log("[init] timed out waiting for game world");
    }

    aor::Log("[init] AOReloaded ready");
    return 0;
}

void OnProcessAttach(HINSTANCE self) {
    if (g_initialised) return;
    g_initialised = true;

    DisableThreadLibraryCalls(self);

    if (!IsAnarchyOnlineProcess()) return;

    aor::LogInit();
    aor::Log("[init] AOReloaded v0.1.0 loading");

    // Spawn the deferred init thread. We can't resolve game APIs here
    // because the game's DLLs aren't loaded yet during DLL_PROCESS_ATTACH.
    HANDLE thread = CreateThread(nullptr, 0, DeferredInit, nullptr, 0, nullptr);
    if (thread) {
        CloseHandle(thread);
    } else {
        aor::Log("[init] failed to create deferred init thread: %lu",
                 GetLastError());
    }
}

void OnProcessDetach() {
    if (!g_initialised) return;
    aor::Log("[shutdown] AOReloaded unloading");
    aor::LogShutdown();
    g_initialised = false;
}

}  // namespace

BOOL APIENTRY DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID /*reserved*/) {
    switch (fdwReason) {
        case DLL_PROCESS_ATTACH: OnProcessAttach(hinstDLL); break;
        case DLL_PROCESS_DETACH: OnProcessDetach(); break;
        default: break;
    }
    return TRUE;
}
