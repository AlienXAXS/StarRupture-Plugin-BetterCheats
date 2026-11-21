#pragma once

#include "plugin_interface.h"

namespace BetterCheats::Panels::Wave
{
	void Initialize();
	void Shutdown();

	// Takes back the wave pause the Pause Waves Entirely toggle wrote into the
	// timer, keeping the toggle itself. The plugin calls this when the Wave section
	// is switched off while the game runs; Tick writes the pause again if it comes
	// back on. Game thread only.
	void ReleaseValues();
	void Tick(float deltaSeconds);
	void RenderImGui(IModLoaderImGui* imgui);
	void ApplySavedConfig();
}
