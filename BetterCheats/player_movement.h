#pragma once

#include "plugin_interface.h"

namespace BetterCheats::Panels::Movement
{
	// Registers the no-clip toggle keybind — call once during plugin init.
	void Initialize();

	// Unregisters the keybind and restores the character's collision and
	// movement mode if no-clip is still on — call once during plugin shutdown.
	void Shutdown();

	// Drives no-clip: applies/reverts it, re-asserts flying mode, and feeds the
	// vertical (Space / Left Ctrl) input — call once per engine tick.
	void Tick(float deltaSeconds);

	void RenderImGui(IModLoaderImGui* imgui);

	// Re-applies the fly speed persisted in the active session's JSON config
	// (see session_config.h). No-clip itself is deliberately not restored — it
	// would drop the player inside whatever they logged out in. Call on the
	// game thread after SessionConfig::Reload().
	void ApplySavedConfig();
}
