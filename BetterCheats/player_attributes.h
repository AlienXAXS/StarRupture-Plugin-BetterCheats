#pragma once

#include "plugin_interface.h"

namespace BetterCheats::Panels::Attributes
{
	// Resolves and caches AOB-pattern function pointers — call once during plugin init.
	void Initialize();

	void RenderImGui(IModLoaderImGui* imgui);

	// Applies continuous effects (e.g. God Mode) — call once per engine tick, in ChimeraMain.
	void Tick(float deltaSeconds);

	// Re-applies the locks/values persisted in the active session's JSON
	// config (see session_config.h). Call on the game thread after
	// SessionConfig::Reload(), e.g. from OnExperienceLoadComplete.
	void ApplySavedConfig();
}
