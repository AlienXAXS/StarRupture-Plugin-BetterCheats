#pragma once

#include "plugin_interface.h"

namespace BetterCheats::Panels::Tools
{
	void Initialize();
	void Shutdown();
	void Tick(float deltaSeconds);
	void RenderImGui(IModLoaderImGui* imgui);
}
