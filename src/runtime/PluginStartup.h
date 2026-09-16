#pragma once

namespace PluginStartup
{
	// Allocates the trampoline, installs every engine hook, and registers the
	// data-loaded listener. Call once from SKSE plugin load.
	void Install();
}
