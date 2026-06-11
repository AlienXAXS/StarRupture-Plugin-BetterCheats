#pragma once

#include "plugin_interface.h"

namespace BetterCheats::Panels::Building
{
	void Initialize();
	void Shutdown();
	void Tick(float deltaSeconds);
	void RenderImGui(IModLoaderImGui* imgui);

	// Re-applies the toggles persisted in the active session's JSON config
	// (see session_config.h). Call on the game thread after
	// SessionConfig::Reload(), e.g. from OnExperienceLoadComplete.
	void ApplySavedConfig();
}
