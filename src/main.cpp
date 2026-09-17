#include "runtime/PluginStartup.h"

SKSE_PLUGIN_LOAD(const SKSE::LoadInterface* a_skse)
{
    SKSE::Init(a_skse);
    PluginStartup::Install();
    return true;
}
