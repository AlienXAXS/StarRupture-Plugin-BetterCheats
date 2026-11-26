#pragma once

#include "plugin_interface.h"

namespace BetterCheats::Panels::Inventory
{
	// Registers the "bc_invsize" console command — call once during plugin init.
	void Initialize();

	// Unregisters the console command — call once during plugin shutdown.
	void Shutdown();

	// Puts the gatherable values back to the game's own, keeping the command and
	// the hooks. Shutdown ends with it; the plugin also calls it when the Inventory
	// section is switched off while the game runs. The inventory grid size is not
	// undone (it lives in the save). Game thread only.
	void ReleaseValues();

	// The item stack sizes are their own section (the "stack-size" feature), so a
	// host can switch them off apart from the grid size and plant pickup.
	// Puts every item's own MaxStack back. Shutdown of the section ends with it; the
	// plugin also calls it when the flag is switched off while the game runs. Game thread only.
	void ReleaseStackSizes();

	// Shutdown of the stack section: releases on the game thread, and only forgets
	// what it wrote when the loader runs shutdown elsewhere.
	void ShutdownStackSizes();

	// Applies the stack sizes, once per engine tick while the flag is on. Game thread.
	void TickStackSizes();

	// Applies any queued resize and refreshes the panel snapshot — call once per
	// engine tick.
	void Tick(float deltaSeconds);

	void RenderImGui(IModLoaderImGui* imgui, bool greyed);

	// The stack-size section of the page, or the off notice when the flag is off.
	void RenderStackSizes(IModLoaderImGui* imgui);

	// Restores the target slot count persisted in the active session's JSON
	// config (see session_config.h). Only seeds the UI field — growing the
	// inventory already persists in the save itself, so nothing is re-applied.
	// Call on the game thread after SessionConfig::Reload().
	void ApplySavedConfig();

	// The stack-size settings of the active session, applied again from scratch.
	// Call on the game thread after SessionConfig::Reload(), and when the flag
	// comes back on.
	void ApplySavedStackConfig();

	// Asks again for a stack list refresh that was skipped between worlds.
	// Game thread only, once the next world has begun play.
	void ResumeStackListRefresh();
}
