/* aop_plugin.h - the AOPlugins plugin API.
 *
 * AOPlugins is a version.dll for the Anarchy Online client that does nothing by itself: it loads the plugins in the
 * client's AOPlugins\ folder, decides which of them may run together, and gives them a settings system (DValues bound
 * to the options window, saved in AOPlugins.ini), options tabs, in-memory GUI file patches and inline hooks with one
 * owner per patched function. See docs/plugin-api.md.
 *
 * Plain C, 32-bit, __cdecl. No C++ types, exceptions or allocations cross the boundary. Strings are ASCII / the
 * client's code page, null-terminated.
 *
 * A plugin is a DLL that exports:
 *
 *   const AopPluginInfo* AopPluginQuery(void);
 *       Who it is. Must have no side effects (no hooks, no threads, no files): the host calls it for every DLL in the
 *       folder, and unloads the ones it doesn't pick. DllMain must do nothing either.
 *
 *   const AopPlugin* AopPluginAttach(const AopHost* host);
 *       Picked: register settings, tabs, patches and hooks, return the plugin's callbacks - or null to refuse (having
 *       undone everything; hooks can't be undone, so check what you depend on before hooking).
 *
 * Versions: AOP_API_VERSION goes up when a table or struct grows; new fields go at the end only. Every table carries
 * its size, so either side can tell what the other one knows.
 */
#ifndef AOP_PLUGIN_H
#define AOP_PLUGIN_H

#include <stdint.h>

/* 1: the first. 2: AopHost.patch, dvalueRange; dvalue* take names longer than 15 characters; AopPlugin.gameFrame. */
#define AOP_API_VERSION 2

/* A plugin's version as one number, comparable with < and >: AOP_VERSION(1, 2, 0). */
#define AOP_VERSION(major, minor, patch) \
    (((uint32_t)(major) << 16) | ((uint32_t)((minor) & 0xFF) << 8) | (uint32_t)((patch) & 0xFF))

#ifdef __cplusplus
#define AOP_EXPORT extern "C" __declspec(dllexport)
extern "C" {
#else
#define AOP_EXPORT __declspec(dllexport)
#endif

/* ---- Who a plugin is ------------------------------------------------------------------------------------------ */

enum {
    /* A project: changes the game enough that it runs alone, unless it and the others declare support for each other
     * (AopPluginInfo.compatible). Without the flag a plugin is a small utility that loads alongside anything. */
    AOP_PLUGIN_EXCLUSIVE = 1
};

/* "I have built-in support for running alongside plugin `id`, versions minVersion..maxVersion." */
typedef struct AopCompat {
    const char* id;
    uint32_t minVersion; /* inclusive */
    uint32_t maxVersion; /* inclusive; 0: any version from minVersion on */
} AopCompat;

typedef struct AopPluginInfo {
    uint32_t size;       /* sizeof(AopPluginInfo) */
    uint32_t apiVersion; /* AOP_API_VERSION */
    const char* id;      /* unique, 1-31 characters of a-z 0-9 . _ - (e.g. "ao-vk"); its section in AOPlugins.ini */
    const char* name;    /* for people: the options tab, the log */
    uint32_t version;    /* AOP_VERSION(...) */
    uint32_t flags;      /* AOP_PLUGIN_* */
    const AopCompat* compatible; /* exclusive plugins this one can run with */
    uint32_t compatibleCount;
} AopPluginInfo;

/* ---- Values ------------------------------------------------------------------------------------------------- */

/* The game's DValue (Variant) types a plugin can read and write. */
enum { AOP_VALUE_BOOL = 0, AOP_VALUE_INT = 1, AOP_VALUE_FLOAT = 2, AOP_VALUE_DOUBLE = 3 };

typedef struct AopValue {
    uint32_t type;   /* AOP_VALUE_*; dvalueGet: what the DValue holds (other Variant types read as failure) */
    uint32_t reserved;
    double value;    /* bool 0 / 1, int a whole number */
} AopValue;

/* A setting: a DValue the options window binds to (opt_variable="<name>"), saved in AOPlugins.ini under the plugin's
 * section, its changes reported through AopPlugin.settingChanged. */
enum { AOP_SETTING_BOOL = 0, AOP_SETTING_INT = 1 };

enum {
    /* Not kept in AOPlugins.ini: the plugin saves it itself (e.g. a value that belongs to another program). */
    AOP_SETTING_NOT_SAVED = 1
};

typedef struct AopSetting {
    uint32_t size;           /* sizeof(AopSetting) */
    const char* name;        /* the DValue: 1-15 characters (the game's limit), A-Z a-z 0-9 _, unique among plugins */
    uint32_t type;           /* AOP_SETTING_* (sliders and radio buttons want INT) */
    int32_t defaultValue;
    int32_t minValue;        /* INT: the range sliders show; values are clamped to it */
    int32_t maxValue;
    uint32_t flags;          /* AOP_SETTING_* */
} AopSetting;

/* ---- Buffers and callbacks the host calls ------------------------------------------------------------------ */

/* Text handed back to the host: call append as often as needed. */
typedef struct AopBuffer {
    void* context;
    void(__cdecl* append)(struct AopBuffer* buffer, const char* data, uint32_t length);
} AopBuffer;

/* An options tab's content (the widgets: TextView, OptionCheckBox, OptionSlider, OptionRadioButtonGroup, ...), written
 * whenever the game opens OptionPanel/Root.xml. The host puts it in a tab of its own. */
typedef void(__cdecl* AopTabWriter)(void* user, AopBuffer* out);

/* A GUI file patch: the file's bytes in, the patched bytes into `out`; return non-zero to serve them, 0 to serve the
 * file unchanged. Runs inside the game's file open: fast, and no opening of the file itself. */
typedef int(__cdecl* AopFilePatcher)(void* user, const char* data, uint32_t length, AopBuffer* out);

/* ---- The host's table ------------------------------------------------------------------------------------------ */

/* Each plugin gets its own; every call takes it as its first argument. Calls return non-zero on success. */
typedef struct AopHost AopHost;
struct AopHost {
    uint32_t version; /* AOP_API_VERSION of the host */
    uint32_t size;    /* sizeof(AopHost) of the host: refuse a smaller one than you need */

    /* A line in AOPlugins.log, prefixed with the plugin's id. */
    void(__cdecl* log)(const AopHost* host, const char* fmt, ...);
    /* The client folder (AnarchyOnline.exe's), with a trailing backslash. */
    const char*(__cdecl* clientDir)(const AopHost* host);

    /* The plugin's own section of AOPlugins.ini ([<id>]). */
    int32_t(__cdecl* iniGetInt)(const AopHost* host, const char* key, int32_t defaultValue);
    int(__cdecl* iniSetInt)(const AopHost* host, const char* key, int32_t value);
    uint32_t(__cdecl* iniGetString)(const AopHost* host, const char* key, const char* defaultValue, char* out,
                                    uint32_t outSize);
    int(__cdecl* iniSetString)(const AopHost* host, const char* key, const char* value);

    /* Settings. Added in attach or later; from gameReady on they become DValues at once. */
    int(__cdecl* addSetting)(const AopHost* host, const AopSetting* setting);
    int(__cdecl* getSetting)(const AopHost* host, const char* name, int32_t* value);
    /* Set one of the plugin's own settings from code: the DValue (the options window follows) and the ini. Its
     * settingChanged isn't called for it. */
    int(__cdecl* setSetting)(const AopHost* host, const char* name, int32_t value);

    /* Any DValue (the game's own too), names up to 63 characters (API 1: 15). dvalue* work from gameReady on. */
    int(__cdecl* dvalueExists)(const AopHost* host, const char* name);
    int(__cdecl* dvalueGet)(const AopHost* host, const char* name, AopValue* out);
    int(__cdecl* dvalueSet)(const AopHost* host, const char* name, const AopValue* value);
    /* Report changes of this DValue (any, not only the plugin's settings) through AopPlugin.dvalueChanged. */
    int(__cdecl* watchDValue)(const AopHost* host, const char* name);

    /* An options window (F10) tab. Tabs of the same label already in the file (older copies) are replaced. */
    int(__cdecl* addOptionsTab)(const AopHost* host, const char* label, AopTabWriter writer, void* user);
    /* Patch a GUI file in memory as the game reads it - the install is never changed. `relPath` matches the end of
     * the opened path (either slash, any case), e.g. "Views/Skills.xml". */
    int(__cdecl* addFilePatcher)(const AopHost* host, const char* relPath, AopFilePatcher patcher, void* user);

    /* A 5-byte inline hook (jmp rel32) on a known MSVC prologue. One owner per function: refused when another plugin
     * (or the host) hooks it already, or something outside AOPlugins has patched it. `*trampoline` calls the
     * original. Hooks stay for the life of the process. */
    int(__cdecl* hook)(const AopHost* host, void* target, void* detour, void** trampoline);
    /* Who hooks `target`: a plugin id, "aoplugins", or null. */
    const char*(__cdecl* hookOwner)(const AopHost* host, const void* target);
    /* A loaded module's base + rva, or null when it isn't loaded or rva is outside it. */
    void*(__cdecl* moduleRva)(const AopHost* host, const char* module, uint32_t rva);

    /* Other plugins. Active: attached and running (version optional). */
    int(__cdecl* pluginActive)(const AopHost* host, const char* id, uint32_t* version);
    /* What another active plugin's getInterface returns for `name` - how plugins that support each other talk. */
    void*(__cdecl* pluginInterface)(const AopHost* host, const char* id, const char* name);

    /* ---- API 2 ---- */

    /* Patch `length` bytes of code or data at `target` (an operand, a jump over a check), with the same ownership as
     * hooks: refused when the bytes overlap another plugin's (or the host's) hook or patch, or when they aren't
     * `expected` (another client build, or patched by something outside AOPlugins). Bytes that are `bytes` already
     * count as done. Executable code stays executable while it is written. Patches stay for the life of the process. */
    int(__cdecl* patch)(const AopHost* host, void* target, const void* expected, const void* bytes, uint32_t length);
    /* Any DValue's range: what its option slider spans and what SetDValue clamps to (e.g. raising a game slider's
     * maximum). */
    int(__cdecl* dvalueRange)(const AopHost* host, const char* name, int32_t minValue, int32_t maxValue);
};

/* ---- The plugin's table ---------------------------------------------------------------------------------------- */

/* Returned by AopPluginAttach; any entry may be null. attach, gameReady and worldReady run on the host's start-up
 * thread; settingChanged and dvalueChanged on the thread that changed the DValue (the game's); gameFrame on the game
 * thread. */
typedef struct AopPlugin {
    uint32_t version; /* AOP_API_VERSION */
    uint32_t size;    /* sizeof(AopPlugin) */

    /* Utils.dll is loaded, the settings are DValues: dvalue* work. Every plugin is attached by now. */
    void(__cdecl* gameReady)(void);
    /* The game world is up (the first login of the session). Settings changes are reported from here on. */
    void(__cdecl* worldReady)(void);
    /* One of the plugin's settings was changed (the options window, another plugin). Already clamped and saved. */
    void(__cdecl* settingChanged)(const char* name, int32_t value);
    /* A DValue the plugin watches changed. */
    void(__cdecl* dvalueChanged)(const char* name, const AopValue* value);
    /* An interface of the plugin's own for other plugins (pluginInterface), or null. */
    void*(__cdecl* getInterface)(const char* name);

    /* ---- API 2 ---- */

    /* Once a frame, on the game thread (from AFCM.dll's frame limiter, Timer_t::FrameProcess), from gameReady on:
     * where to touch the game's objects. Keep it short - it runs every frame. */
    void(__cdecl* gameFrame)(void);
} AopPlugin;

typedef const AopPluginInfo*(__cdecl* AopPluginQueryFn)(void);
typedef const AopPlugin*(__cdecl* AopPluginAttachFn)(const AopHost* host);

#ifdef __cplusplus
}
#endif

#endif /* AOP_PLUGIN_H */
