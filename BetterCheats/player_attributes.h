#pragma once

#include "plugin_interface.h"

namespace BetterCheats::Panels::Attributes
{
	// Resolves and caches AOB-pattern function pointers, and registers the world
	// begin/end-play hooks used to gate Tick() to ChimeraMain — call once during plugin init.
	void Initialize();

	// Unregisters the world begin/end-play hooks — call once during plugin shutdown.
	void Shutdown();

	void RenderImGui(IModLoaderImGui* imgui);

	// Applies continuous effects (e.g. God Mode) — call once per engine tick.
	void Tick(float deltaSeconds);
}
