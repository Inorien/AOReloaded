// plugin_core.cpp - AOReloaded's core API (logging.h, hook_engine.h, settings.h) on AOPlugins.
//
// In the AOPlugins build (aoreloaded.dll) the feature code (src/hooks/) is compiled unchanged; what it builds on is
// this file instead of the version.dll's own core: log lines go to AOPlugins.log, hooks through AOPlugins' hook
// engine (one owner per function), settings are AOPlugins settings (DValues bound to the AOReloaded tab, saved in
// AOPlugins.ini [aoreloaded]).

#include "plugin/plugin.h"

#include "core/logging.h"
#include "core/settings.h"
#include "hooks/hook_engine.h"

#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace aor {

namespace plugin {
const AopHost* g_host = nullptr;
}  // namespace plugin

using plugin::g_host;

// ── logging.h ──────────────────────────────────────────────────────────

void LogInit() {}
void LogShutdown() {}

void Log(const char* fmt, ...) {
    if (!g_host) return;
    char line[2048];
    va_list args;
    va_start(args, fmt);
    _vsnprintf_s(line, sizeof(line), _TRUNCATE, fmt, args);
    va_end(args);
    g_host->log(g_host, "%s", line);
}

// ── hook_engine.h ──────────────────────────────────────────────────────

bool InstallHook(void* target, void* detour, void** trampoline_out) {
    return g_host && g_host->hook(g_host, target, detour, trampoline_out) != 0;
}

void* ResolveRVA(const char* module_name, uint32_t rva) {
    HMODULE mod = GetModuleHandleA(module_name);
    if (!mod) return nullptr;
    return reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(mod) + rva);
}

// ── settings.h ─────────────────────────────────────────────────────────

namespace {

constexpr int kMaxSettingCallbacks = 8;
SettingChangedCallback g_callbacks[kMaxSettingCallbacks] = {};
int g_callbackCount = 0;

}  // namespace

namespace plugin {
void SettingChanged(const char* name, int32_t value) {
    for (int i = 0; i < g_callbackCount; ++i) g_callbacks[i](name, value);
}
}  // namespace plugin

void RegisterSettingCallback(SettingChangedCallback cb) {
    if (cb && g_callbackCount < kMaxSettingCallbacks) g_callbacks[g_callbackCount++] = cb;
}

bool IsDebugLogEnabled() { return true; }   // AOPlugins.log is AOPlugins' to switch
void SettingsInit() {}
void SettingsRegisterAll() {}               // plugin_main.cpp registers them in attach
void PatchOptionsXml() {}                   // the AOReloaded tab: plugin_main.cpp
bool SettingsInstallHook() { return true; } // AOPlugins owns SetDValue

// The client folder with a trailing backslash (AOPlugins knows it).
static bool GetClientDir(char* out, int outSize) {
    if (!g_host) return false;
    _snprintf_s(out, outSize, _TRUNCATE, "%s", g_host->clientDir(g_host));
    return out[0] != '\0';
}

// Unchanged from settings.cpp: the skill favorites' repair of files older builds edited uses it.
void ForEachGuiFile(const char* relPath, void (*fn)(const char* path)) {
    char clientDir[MAX_PATH];
    if (!GetClientDir(clientDir, MAX_PATH)) {
        Log("[settings] could not resolve client directory");
        return;
    }

    // 1. Always patch Default in cd_image.
    {
        char path[MAX_PATH];
        _snprintf_s(path, sizeof(path), _TRUNCATE,
                    "%scd_image\\gui\\Default\\%s", clientDir, relPath);
        fn(path);
    }

    // 2. Scan every GUI in cd_image/gui/ (covers non-Default GUIs shipped
    //    alongside the client).
    {
        char searchPattern[MAX_PATH];
        _snprintf_s(searchPattern, sizeof(searchPattern), _TRUNCATE,
                    "%scd_image\\gui\\*", clientDir);

        WIN32_FIND_DATAA entry;
        HANDLE hFind = FindFirstFileA(searchPattern, &entry);
        if (hFind != INVALID_HANDLE_VALUE) {
            do {
                if (!(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                if (entry.cFileName[0] == '.') continue;
                if (_stricmp(entry.cFileName, "Default") == 0) continue;  // already done

                char path[MAX_PATH];
                _snprintf_s(path, sizeof(path), _TRUNCATE,
                            "%scd_image\\gui\\%s\\%s",
                            clientDir, entry.cFileName, relPath);
                fn(path);
            } while (FindNextFileA(hFind, &entry));
            FindClose(hFind);
        }
    }

    // 3. Scan %LocalAppData%\Funcom\Anarchy Online\<hash>\<sub>\Gui\*\
    //    for custom GUIs installed via the standard AO user-install method.
    //    Structure: <hash>\<sub>\Gui\<GUIName>\<relPath>
    {
        char appDataBase[MAX_PATH];
        DWORD len = GetEnvironmentVariableA("LOCALAPPDATA", appDataBase, MAX_PATH);
        if (len == 0 || len >= MAX_PATH) goto done_appdata;

        char hashPattern[MAX_PATH];
        _snprintf_s(hashPattern, sizeof(hashPattern), _TRUNCATE,
                    "%s\\Funcom\\Anarchy Online\\*", appDataBase);

        WIN32_FIND_DATAA hashEntry;
        HANDLE hHash = FindFirstFileA(hashPattern, &hashEntry);
        if (hHash == INVALID_HANDLE_VALUE) goto done_appdata;

        do {
            if (!(hashEntry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (hashEntry.cFileName[0] == '.') continue;

            // Enumerate subdirectories (e.g. "Anarchy Online" or "client").
            char subPattern[MAX_PATH];
            _snprintf_s(subPattern, sizeof(subPattern), _TRUNCATE,
                        "%s\\Funcom\\Anarchy Online\\%s\\*",
                        appDataBase, hashEntry.cFileName);

            WIN32_FIND_DATAA subEntry;
            HANDLE hSub = FindFirstFileA(subPattern, &subEntry);
            if (hSub == INVALID_HANDLE_VALUE) continue;

            do {
                if (!(subEntry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                if (subEntry.cFileName[0] == '.') continue;

                // Enumerate GUI names inside this Gui folder.
                char guiPattern[MAX_PATH];
                _snprintf_s(guiPattern, sizeof(guiPattern), _TRUNCATE,
                            "%s\\Funcom\\Anarchy Online\\%s\\%s\\Gui\\*",
                            appDataBase, hashEntry.cFileName, subEntry.cFileName);

                WIN32_FIND_DATAA guiEntry;
                HANDLE hGui = FindFirstFileA(guiPattern, &guiEntry);
                if (hGui == INVALID_HANDLE_VALUE) continue;

                do {
                    if (!(guiEntry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                    if (guiEntry.cFileName[0] == '.') continue;

                    char path[MAX_PATH];
                    _snprintf_s(path, sizeof(path), _TRUNCATE,
                                "%s\\Funcom\\Anarchy Online\\%s\\%s"
                                "\\Gui\\%s\\%s",
                                appDataBase, hashEntry.cFileName,
                                subEntry.cFileName, guiEntry.cFileName, relPath);
                    fn(path);
                } while (FindNextFileA(hGui, &guiEntry));
                FindClose(hGui);
            } while (FindNextFileA(hSub, &subEntry));
            FindClose(hSub);
        } while (FindNextFileA(hHash, &hashEntry));
        FindClose(hHash);
    }
done_appdata:;
}

}  // namespace aor
