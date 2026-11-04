#pragma once

#include "plugin_interface.h"

namespace SDK { class AActor; }

namespace BetterCheats::Panels::Enemies
{
	void Initialize();
	void Shutdown();
	void Tick(float deltaSeconds);
	void RenderImGui(IModLoaderImGui* imgui);
	void ApplySavedConfig();

	// Read-only health sampling for the grenade hit probe. PrepareHealthReads
	// refreshes the Mass subsystems when they are missing; TryReadMassHealth
	// reads the actor's Mass HP, false for anything that is not a Mass enemy.
	// Game thread only.
	void PrepareHealthReads(float deltaSeconds);
	bool TryReadMassHealth(SDK::AActor* actor, float& hp);
}
