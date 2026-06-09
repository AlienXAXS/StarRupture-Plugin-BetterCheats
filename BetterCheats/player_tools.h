#pragma once

#include "plugin_interface.h"

namespace BetterCheats::Panels::Tools
{
	void Initialize();
	void Shutdown();
	void RenderImGui(IModLoaderImGui* imgui);
}
