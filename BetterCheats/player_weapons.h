#pragma once

#include "plugin_interface.h"

namespace BetterCheats::Panels::Weapons
{
	void RenderImGui(IModLoaderImGui* imgui);

	// Re-applies enabled weapon attribute overrides every engine tick. There is
	// no reflected way to set a GAS attribute's base value, so an active row
	// composes onto CurrentValue every tick instead of writing it once -- see
	// attribute_compose.h.
	void Tick(float deltaSeconds);

	// Re-applies the overrides persisted in the active session's JSON config
	// (see session_config.h). Call on the game thread after
	// SessionConfig::Reload(), e.g. from OnExperienceLoadComplete.
	void ApplySavedConfig();

	// Hands every composed attribute back to the game if the owning character is
	// still valid -- call once from PluginShutdown.
	void Shutdown();
}
