#pragma once

#include "plugin_interface.h"

namespace BetterCheats::Panels::Tools
{
	void Initialize();
	void Shutdown();

	// Gives back the heat tag and the tool stats, keeping the hook and the
	// callbacks. Shutdown ends with it; the plugin also calls it when the Tools
	// section is switched off while the game runs. Game thread only.
	void ReleaseValues();
	void Tick(float deltaSeconds);
	void RenderImGui(IModLoaderImGui* imgui);

	// Re-applies the toggles persisted in the active session's JSON config
	// (see session_config.h). Call on the game thread after
	// SessionConfig::Reload(), e.g. from OnExperienceLoadComplete.
	void ApplySavedConfig();
}
