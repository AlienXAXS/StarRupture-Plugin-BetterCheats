#pragma once

#include "plugin_helpers.h"

// The game ships a full developer cheat suite — UCrCheatManager (~110 exec
// UFunctions) plus a UMG dev menu (ChimeraUI.CrUW_CheatMenu). This module drives
// it, and is only compiled into debug builds of the plugin.
#if BETTERCHEATS_DEV_BUILD

namespace BetterCheats::Panels::DevMenus
{
	void Initialize();
	void Shutdown();
	void Tick(float deltaSeconds);
	void RenderImGui(IModLoaderImGui* imgui);
}

#endif // BETTERCHEATS_DEV_BUILD
