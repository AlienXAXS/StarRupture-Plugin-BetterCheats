#pragma once

#include "plugin_interface.h"

namespace BetterCheats::Panels::Attributes
{
	// Resolves and caches AOB-pattern function pointers — call once during plugin init.
	void Initialize();

	void RenderImGui(IModLoaderImGui* imgui, bool greyed);

	// Applies continuous effects (e.g. God Mode) — call once per engine tick, in ChimeraMain.
	void Tick(float deltaSeconds);

	// Re-applies the locks/values persisted in the active session's JSON
	// config (see session_config.h). Call on the game thread after
	// SessionConfig::Reload(), e.g. from OnExperienceLoadComplete.
	void ApplySavedConfig();

	// Hands the Max Health raise back and drops queued edits, for a section
	// switched off at runtime. Game thread only.
	void ReleaseValues();

	// Game thread. Finishes a release that found no character; for the ticks
	// the section is off.
	void RetryRelease();

	// For a world that has ended: drops queued edits and the Max Health record.
	// Game thread only.
	void ForgetWorld();
}
