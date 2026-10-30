// plugin_main.cpp - aoreloaded.dll: AOReloaded's features as an AOPlugins plugin.
//
// The same features as the version.dll build - WoW-style camera and mouse-run, autorun, the numpad fix, cast / timer
// bars, the inventory clicks and pins, the skills window's Favorites, LargeAddressAware - for clients running
// AOPlugins as their version.dll, alongside any other plugin. What the version.dll did for itself AOPlugins does here:
// loading, settings (AOPlugins.ini [aoreloaded]; the old AOReloaded.ini values are taken over once), the options tab,
// GUI files from memory, hooks (plugin_core.cpp). Not here: the renderer settings, frame rate cap and view distance -
// ao-vk's plugin (ao-vk.dll) has those.
//
// Start-up as dllmain.cpp did it:
//   attach      LargeAddressAware, the settings, the tab, the Skills.xml patch
//   gameReady   (Utils.dll up) the game API, the inventory and skills features
//   worldReady  (the game world up) input, camera, numpad, timer bars

#include "plugin/plugin.h"

#include "ao/game_api.h"
#include "core/laa_patch.h"
#include "core/logging.h"
#include "hooks/alt_select.h"
#include "hooks/bag_move.h"
#include "hooks/camera_hook.h"
#include "hooks/input_handler.h"
#include "hooks/inventory_pins.h"
#include "hooks/numpad_fix.h"
#include "hooks/skill_favorites.h"
#include "hooks/timer_bar_drag.h"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>

namespace aor::plugin {

namespace {

struct SettingDef {
    const char* name;
    uint32_t type;
    int32_t defaultValue, minValue, maxValue;
};

// The version.dll's table (settings.cpp) without what moved to ao-vk.dll (AOR_FpsCap, AOR_CharDist, AOR_GroundHQ,
// AOR_ObjDist) and AOR_DebugLog (AOPlugins.log is AOPlugins' to switch).
constexpr SettingDef kSettings[] = {
    {"AOR_CamOn", AOP_SETTING_BOOL, 1, 0, 1},
    {"AOR_CYawSpd", AOP_SETTING_INT, 2, 1, 10},
    {"AOR_MouseRun", AOP_SETTING_BOOL, 1, 0, 1},
    {"AOR_AutoRun", AOP_SETTING_BOOL, 1, 0, 1},
    {"AOR_NumpadFix", AOP_SETTING_BOOL, 1, 0, 1},
    {"AOR_TBarX", AOP_SETTING_INT, 40, 0, 4096},
    {"AOR_TBarY", AOP_SETTING_INT, 40, 0, 4096},
    {"AOR_TBarW", AOP_SETTING_INT, 113, 32, 1024},
    {"AOR_TBarH", AOP_SETTING_INT, 10, 8, 128},
    {"AOR_TBarPrev", AOP_SETTING_BOOL, 0, 0, 1},
    {"AOR_BagMove", AOP_SETTING_BOOL, 1, 0, 1},
    {"AOR_AltSelect", AOP_SETTING_BOOL, 1, 0, 1},
    {"AOR_InvPins", AOP_SETTING_BOOL, 1, 0, 1},
    {"AOR_SkillFavs", AOP_SETTING_BOOL, 1, 0, 1},
    {"AOR_SkillShift", AOP_SETTING_BOOL, 1, 0, 1},
};

// The tab's content (AOPlugins puts it in a ScrollView labelled "AOReloaded"): settings.cpp's, less the sections that
// moved (Performance, View distance) and Debug.
const char kTabXml[] =
    "        <TextView value=\"AOReloaded (AOPlugins plugin)\" layout_borders=\"Rect(0,0,0,5)\" />\n"
    "        <TextView value=\"Client mod framework. Settings are saved to AOPlugins.ini.\""
    " layout_borders=\"Rect(0,0,0,10)\" />\n"
    "\n"
    "        <TextView value=\"Camera\" layout_borders=\"Rect(0,10,0,3)\" />\n"
    "        <OptionCheckBox label=\"WoW-style camera (auto-recenter after LMB drag)\""
    " layout_borders=\"Rect(10,0,0,0)\" opt_type=\"variant\" opt_variable=\"AOR_CamOn\"/>\n"
    "        <OptionSlider label=\"Camera recenter speed:\""
    " layout_borders=\"Rect(10,0,0,3)\" opt_type=\"variant\" opt_variable=\"AOR_CYawSpd\""
    " value_fmt=\"&lt;font color=#70C4D0&gt;%.0f&lt;/font&gt;\" value_scale=\"1\"/>\n"
    "        <OptionCheckBox label=\"LMB+RMB mouse-run (hold both mouse buttons to run forward - requires WoW camera enabled)\""
    " layout_borders=\"Rect(10,0,0,0)\" opt_type=\"variant\" opt_variable=\"AOR_MouseRun\"/>\n"
    "        <OptionCheckBox label=\"Toggle autorun &amp; work smoothly with forward-key\""
    " layout_borders=\"Rect(10,0,0,0)\" opt_type=\"variant\" opt_variable=\"AOR_AutoRun\"/>\n"
    "\n"
    "        <TextView value=\"Timer Bars\" layout_borders=\"Rect(0,10,0,3)\" />\n"
    "        <TextView value=\"Cast/action timer bar position. Drag any active bar in-game, or use these sliders. Default: 40, 40.\""
    " layout_borders=\"Rect(10,0,0,3)\" />\n"
    "        <OptionSlider label=\"X position:\""
    " layout_borders=\"Rect(10,0,0,3)\" opt_type=\"variant\" opt_variable=\"AOR_TBarX\""
    " value_fmt=\"&lt;font color=#70C4D0&gt;%.0f&lt;/font&gt;\" value_scale=\"1\"/>\n"
    "        <OptionSlider label=\"Y position:\""
    " layout_borders=\"Rect(10,0,0,3)\" opt_type=\"variant\" opt_variable=\"AOR_TBarY\""
    " value_fmt=\"&lt;font color=#70C4D0&gt;%.0f&lt;/font&gt;\" value_scale=\"1\"/>\n"
    "        <OptionSlider label=\"Width (default: 113):\""
    " layout_borders=\"Rect(10,0,0,3)\" opt_type=\"variant\" opt_variable=\"AOR_TBarW\""
    " value_fmt=\"&lt;font color=#70C4D0&gt;%.0f&lt;/font&gt;\" value_scale=\"1\"/>\n"
    "        <OptionSlider label=\"Height (default: 10):\""
    " layout_borders=\"Rect(10,0,0,3)\" opt_type=\"variant\" opt_variable=\"AOR_TBarH\""
    " value_fmt=\"&lt;font color=#70C4D0&gt;%.0f&lt;/font&gt;\" value_scale=\"1\"/>\n"
    "        <OptionCheckBox label=\"Enable cast bar preview &amp; drag (stack extends downwards from the topmost bar)\""
    " layout_borders=\"Rect(10,5,0,0)\" opt_type=\"variant\" opt_variable=\"AOR_TBarPrev\"/>\n"
    "\n"
    "        <TextView value=\"Input\" layout_borders=\"Rect(0,10,0,3)\" />\n"
    "        <OptionCheckBox label=\"Numpad keys type in chat fix\""
    " layout_borders=\"Rect(10,0,0,0)\" opt_type=\"variant\" opt_variable=\"AOR_NumpadFix\"/>\n"
    "        <OptionCheckBox label=\"Ctrl+click moves items between the inventory and the last opened backpack\""
    " layout_borders=\"Rect(10,0,0,0)\" opt_type=\"variant\" opt_variable=\"AOR_BagMove\"/>\n"
    "        <OptionCheckBox label=\"Alt+click selects several items; moving one moves them all\""
    " layout_borders=\"Rect(10,0,0,0)\" opt_type=\"variant\" opt_variable=\"AOR_AltSelect\"/>\n"
    "        <OptionCheckBox label=\"Ctrl+Alt+click pins an item to the top of the inventory list view\""
    " layout_borders=\"Rect(10,0,0,0)\" opt_type=\"variant\" opt_variable=\"AOR_InvPins\"/>\n"
    "        <OptionCheckBox label=\"Ctrl+Alt+click a skill to add it to the Favorites group of the skills window\""
    " layout_borders=\"Rect(10,0,0,0)\" opt_type=\"variant\" opt_variable=\"AOR_SkillFavs\"/>\n"
    "        <OptionCheckBox label=\"Shift+click on a skill's + or - changes it by 5\""
    " layout_borders=\"Rect(10,0,0,0)\" opt_type=\"variant\" opt_variable=\"AOR_SkillShift\"/>\n";

void __cdecl WriteTab(void* /*user*/, AopBuffer* out) {
    out->append(out, kTabXml, sizeof(kTabXml) - 1);
}

// The version.dll kept its settings in AOReloaded.ini [AOReloaded]: the first time, those are the values.
int32_t Legacy(const char* name, int32_t fallback) {
    char ini[MAX_PATH];
    _snprintf_s(ini, sizeof(ini), _TRUNCATE, "%sAOReloaded.ini", g_host->clientDir(g_host));
    return static_cast<int32_t>(GetPrivateProfileIntA("AOReloaded", name, fallback, ini));
}

// Skills.xml with the Favorites group added, served by AOPlugins as the game reads it.
int __cdecl ServeSkillsXml(void* /*user*/, const char* data, uint32_t length, AopBuffer* out) {
    std::string patched;
    if (!PatchSkillsXmlText(std::string(data, length), patched)) return 0;
    out->append(out, patched.data(), static_cast<uint32_t>(patched.size()));
    return 1;
}

void Report(bool ok, const char* what) {
    if (!ok) Log("[init] %s failed - see above", what);
}

void __cdecl GameReady() {
    if (!GameAPI::Init()) Log("[init] game API init failed (partial resolve)");
    CleanSkillsXmlOnDisk();   // older builds' edits to Skills.xml
    Report(InitBagMove(), "bag move (ctrl+click)");
    Report(InitAltSelect(), "alt select");
    Report(InitInventoryPins(), "inventory pins");
    Report(InitSkillFavorites(), "skill favorites");
}

void __cdecl WorldReady() {
    Report(InitInputHandler(), "input handler");
    Report(InitCameraHooks(), "camera hooks");
    Report(InitNumpadFix(), "numpad fix");
    Report(InitTimerBarDrag(), "timer bar drag");
    Log("[init] AOReloaded ready");
}

void __cdecl OnSettingChanged(const char* name, int32_t value) { SettingChanged(name, value); }

const AopPluginInfo kInfo = {
    sizeof(AopPluginInfo), AOP_API_VERSION, "aoreloaded", "AOReloaded", AOP_VERSION(0, 1, 0),
    0,              // a utility: no other plugin's business is touched, and the hook registry catches overlaps
    nullptr, 0,
};

const AopPlugin kPlugin = {
    AOP_API_VERSION, sizeof(AopPlugin), &GameReady, &WorldReady, &OnSettingChanged, nullptr, nullptr, nullptr,
};

}  // namespace

}  // namespace aor::plugin

AOP_EXPORT const AopPluginInfo* __cdecl AopPluginQuery(void) {
    return &aor::plugin::kInfo;
}

AOP_EXPORT const AopPlugin* __cdecl AopPluginAttach(const AopHost* host) {
    using namespace aor::plugin;
    // API 1's entries are all it uses.
    if (host->size < offsetof(AopHost, patch)) return nullptr;
    g_host = host;

    aor::PatchLargeAddressAware();   // AnarchyOnline.exe on disk, from the next start
    for (const SettingDef& d : kSettings) {
        int32_t value = Legacy(d.name, d.defaultValue);
        value = value < d.minValue ? d.minValue : value > d.maxValue ? d.maxValue : value;
        AopSetting s = {sizeof(AopSetting), d.name, d.type, value, d.minValue, d.maxValue, 0};
        host->addSetting(host, &s);
    }
    host->addOptionsTab(host, "AOReloaded", &WriteTab, nullptr);
    host->addFilePatcher(host, "Views/Skills.xml", &ServeSkillsXml, nullptr);
    return &kPlugin;
}
