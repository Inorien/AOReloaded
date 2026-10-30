#pragma once

// aoreloaded.dll: AOReloaded as an AOPlugins plugin. See plugin_main.cpp.

#include "plugin/aop_plugin.h"

#include <cstdint>

namespace aor::plugin {

extern const AopHost* g_host;

// A setting changed (AopPlugin.settingChanged): to the callbacks the features registered (RegisterSettingCallback).
void SettingChanged(const char* name, int32_t value);

}  // namespace aor::plugin
