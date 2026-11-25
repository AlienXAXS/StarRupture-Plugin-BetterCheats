#pragma once

#include "plugin_interface.h"

namespace BetterCheats::Panels::Wind
{
	void Initialize();
	void Shutdown();

	// Gives the wind sound back at once. The plugin calls this when the wind mute
	// is switched off while the game runs; Tick mutes again when it comes back on.
	// Game thread only.
	void ReleaseValues();
	void Tick(float deltaSeconds);
	void RenderImGui(IModLoaderImGui* imgui);
}
