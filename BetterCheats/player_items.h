#pragma once

#include "plugin_interface.h"

namespace BetterCheats::Panels::Items
{
	void Initialize();
	void RenderImGui(IModLoaderImGui* imgui);
	void Shutdown();
}
