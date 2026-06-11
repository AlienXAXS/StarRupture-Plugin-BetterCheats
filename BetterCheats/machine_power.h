#pragma once

#include "plugin_interface.h"

namespace BetterCheats::Panels::Power
{
	// Resolves AOB-pattern-based engine functions up front during PluginInit, so
	// scan failures are logged immediately at startup rather than on first Apply.
	void Initialize();

	void RenderImGui(IModLoaderImGui* imgui);

	// Re-applies the electricity overrides persisted in the active session's
	// JSON config (see session_config.h) by triggering a fresh building scan.
	// Call on the game thread after SessionConfig::Reload(), e.g. from
	// OnExperienceLoadComplete.
	void ApplySavedConfig();
}
