#pragma once

#include "plugin_interface.h"

namespace BetterCheats::Panels::Items
{
	void Initialize();
	// `server`: Give Item asks the joined server instead.
	void RenderImGui(IModLoaderImGui* imgui, bool greyed, bool server = false);
	void Shutdown();
}
