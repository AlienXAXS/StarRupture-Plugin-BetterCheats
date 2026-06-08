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

} // namespace BetterCheats::AOB
