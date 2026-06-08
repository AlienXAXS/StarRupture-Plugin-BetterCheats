#pragma once

#include "plugin_interface.h"

namespace BetterCheats::Panels::Attributes
{
	// Resolves and caches AOB-pattern function pointers — call once during plugin init.
	void Initialize();

	void RenderImGui(IModLoaderImGui* imgui);

	// Applies continuous effects (e.g. God Mode) — call once per engine tick.
	void Tick(float deltaSeconds);
}
