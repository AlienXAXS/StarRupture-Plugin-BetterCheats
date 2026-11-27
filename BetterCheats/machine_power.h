#pragma once

#include "plugin_interface.h"

namespace BetterCheats::Panels::Power
{
	// Resolves AOB-pattern-based engine functions up front during PluginInit, so
	// scan failures are logged immediately at startup rather than on first Apply.
	void Initialize();

	void RenderImGui(IModLoaderImGui* imgui, bool greyed);

	// For a joined player: asks the server to set a building's power.
	void RenderServerImGui(IModLoaderImGui* imgui);

	// Re-applies the electricity overrides persisted in the active session's
	// JSON config (see session_config.h) by triggering a fresh building scan.
	// Call on the game thread after SessionConfig::Reload(), e.g. from
	// OnExperienceLoadComplete.
	void ApplySavedConfig();

	// Runs a building scan that was skipped between worlds, which also puts the
	// saved overrides back. Game thread only.
	void ResumeSkippedScan();

	// Puts the game's own electricity values back where this plugin wrote an
	// override. The saved overrides stay. Game thread only.
	void ReleaseValues();

	// Tries again, now and then, to take back an override ReleaseValues could not.
	// Call each tick while the section is off. Game thread only.
	void RetryRelease();

	// Puts them back on the assets once the world they were written in has ended.
	// Game thread only.
	void ForgetWorld();
}
