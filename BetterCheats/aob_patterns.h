#pragma once

// ---------------------------------------------------------------------------
// AOB Pattern Registry
//
// Format for each entry:
//   Pattern string  — byte sequence with ?? wildcards
//   Class::Function — fully qualified name of the function being located
//   Parameters      — function signature for reference
//
// All patterns are passed to IPluginScanner::FindPatternInMainModule.
// Add new patterns here; never scatter raw byte strings across feature files.
// ---------------------------------------------------------------------------

namespace BetterCheats::AOB
{
	// -------------------------------------------------------------------------
	// Example / template (remove when first real pattern is added)
	// -------------------------------------------------------------------------
	// Class::Function  AExampleClass::ExampleFunction
	// Parameters       (AExampleClass* self, float deltaTime)
	// constexpr const char* ExampleFunction = "48 89 5C 24 ?? 57 48 83 EC 20 ?? ?? ?? ?? ?? ??";

	// -------------------------------------------------------------------------
	// TEMP — UCrMassBuildingStabilityRemoveProcessorBase
	// -------------------------------------------------------------------------

	// Class::Function  UCrMassBuildingStabilityRemoveProcessorBase::GatherNeihbours
	// Parameters       (UCrMassBuildingStabilityRemoveProcessorBase* this, FCrMassPersistentEntityID* Entity, FCrBuildingGraphData* GraphData, FMassEntityManager* EntityManager)
	constexpr const char* GatherNeihbours = "40 53 57 41 54 41 56 48 81 EC ?? ?? ?? ?? ?? ?? ?? 4D 8B E1";

	// -------------------------------------------------------------------------
	// HUD refresh
	// -------------------------------------------------------------------------

	// Class::Function  UCrUW_HealthHud::SetupProgressBar
	// Parameters       (UCrUW_HealthHud* this)
	// Re-pulls the owning character's current Health/Energy/Shield/survival values
	// and pushes them into the HUD's progress bars — used to force an immediate
	// HUD refresh after the plugin writes attribute values directly.
	constexpr const char* HealthHud_SetupProgressBar =
		"40 57 48 83 EC ?? 48 83 B9 ?? ?? ?? ?? ?? 48 8B F9 0F 84 ?? ?? ?? ?? ?? ?? ?? 48 89 5C 24 ?? FF 90 ?? ?? ?? ?? "
		"48 8B C8 E8 ?? ?? ?? ?? 48 8B D8 48 85 C0 0F 84 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B 4B ?? 48 83 C0 ?? 48 63 50 ?? "
		"3B 51 ?? 0F 8F ?? ?? ?? ?? 48 8B 49 ?? ?? ?? ?? ?? 0F 85 ?? ?? ?? ?? 80 3D ?? ?? ?? ?? ?? 48 8B 83 ?? ?? ?? ?? "
		"48 89 44 24 ?? 48 89 74 24 ?? 74 ?? 48 85 C0 74 ?? 48 8B C8 E8 ?? ?? ?? ?? 48 8B 44 24 ?? 48 85 C0 74 ?? "
		"E8 ?? ?? ?? ?? 48 8B D0 48 8D 4C 24 ?? E8 ?? ?? ?? ?? 84 C0 74 ?? 48 8B 74 24 ?? EB ?? 33 F6 48 85 F6 0F 84 "
		"?? ?? ?? ?? 48 8D 8E ?? ?? ?? ?? ?? ?? ?? FF 50 ?? 48 8B CE 48 8B D8 E8 ?? ?? ?? ?? 48 8B F0 48 85 DB 0F 84 "
		"?? ?? ?? ?? 48 85 C0 0F 84 ?? ?? ?? ?? 48 89 6C 24";

	// -------------------------------------------------------------------------
	// Mining
	// -------------------------------------------------------------------------

	// Class::Function  UCrMiningComponent::GetMiningDamage
	// Parameters       (UCrMiningComponent* this, bool IsHittingWeakSpot)
	constexpr const char* GetMiningDamage =
		"40 55 56 57 41 56 48 8D 6C 24 ?? 48 81 EC ?? ?? ?? ?? 48 8B B9";

} // namespace BetterCheats::AOB
