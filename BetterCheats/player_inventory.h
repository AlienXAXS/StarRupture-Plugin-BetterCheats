#pragma once

#include "plugin_interface.h"

namespace BetterCheats::Panels::Inventory
{
	// Registers the "bc_invsize" console command — call once during plugin init.
	void Initialize();

	// Unregisters the console command — call once during plugin shutdown.
	void Shutdown();

	// Tab or E scans for the inventory widget for up to 3 seconds, applying the
	// queued resize and slot scaling when it appears. Also refreshes the snapshot.
	void Tick(float deltaSeconds);

	void RenderImGui(IModLoaderImGui* imgui);

	// Restores the target slot count persisted in the active session's JSON
	// config (see session_config.h). Queues the saved grid for the next Tab/E
	// scan. Call on the game thread after SessionConfig::Reload().
	void ApplySavedConfig();
}
