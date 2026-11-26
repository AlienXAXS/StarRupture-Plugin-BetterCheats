#pragma once

#include "plugin_interface.h"

namespace SDK { class AActor; }

namespace BetterCheats::Panels::Enemies
{
	void Initialize();
	// Gives back what Disable AI changed when called on the game thread.
	void Shutdown();

	// The world's enemies are gone: drops everything tracked about them without
	// writing to the game.
	void ForgetWorld();

	// Switches the enemies' AI back on if Disable AI had it off, keeping the panel's
	// setting. The plugin calls this when the Enemies section is switched off while
	// the game runs; Tick turns the AI off again if the section comes back on.
	// Game thread only.
	void ReleaseValues();
	void Tick(float deltaSeconds);
	void RenderImGui(IModLoaderImGui* imgui, bool greyed);
	void ApplySavedConfig();

	// Read-only health sampling for the grenade hit probe. PrepareHealthReads
	// refreshes the Mass subsystems when they are missing; TryReadMassHealth
	// reads the actor's Mass HP, false for anything that is not a Mass enemy.
	// Game thread only.
	void PrepareHealthReads(float deltaSeconds);
	bool TryReadMassHealth(SDK::AActor* actor, float& hp);
}
