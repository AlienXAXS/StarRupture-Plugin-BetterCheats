#pragma once

#include "plugin_interface.h"

namespace BetterCheats::Panels::Building
{
	void Initialize();
	void Shutdown();

	// Takes back what the Unlock All Buildings toggle wrote into the tech keeper,
	// keeping the hooks and the user's setting. Shutdown does the same as part of
	// removing everything; the plugin calls this when the section is switched off
	// while the game runs, and Tick applies the toggle again if it comes back on.
	// Game thread only.
	void ReleaseValues();

	// For a shut cheats gate: ReleaseValues, then drops every toggle, since the
	// detours act on a toggle and its feature flag alone. ApplySavedConfig reads
	// them back. Game thread only.
	void ForgetSettings();

	// For a world that has ended: ForgetSettings without writing to it.
	void ForgetWorld();
	void Tick(float deltaSeconds);
	void RenderImGui(IModLoaderImGui* imgui);

	// Re-applies the toggles persisted in the active session's JSON config
	// (see session_config.h). Call on the game thread after
	// SessionConfig::Reload(), e.g. from OnExperienceLoadComplete.
	void ApplySavedConfig();
}
