#pragma once

#include "plugin_interface.h"

namespace BetterCheats::Panels::Wave
{
	void Initialize();
	void Shutdown();
	void Tick(float deltaSeconds);
	void RenderImGui(IModLoaderImGui* imgui);
	void ApplySavedConfig();
}
