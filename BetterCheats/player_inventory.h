#pragma once

#include "plugin_interface.h"

namespace BetterCheats::Panels::Inventory
{
	// Registers the "bc_invsize" console command — call once during plugin init.
	void Initialize();

	// Unregisters the console command — call once during plugin shutdown.
	void Shutdown();

	// Applies any queued resize and refreshes the panel snapshot — call once per
	// engine tick.
	void Tick(float deltaSeconds);

	void RenderImGui(IModLoaderImGui* imgui);

	// Restores the target slot count persisted in the active session's JSON
	// config (see session_config.h). Only seeds the UI field — growing the
	// inventory already persists in the save itself, so nothing is re-applied.
	// Call on the game thread after SessionConfig::Reload().
	void ApplySavedConfig();
}
