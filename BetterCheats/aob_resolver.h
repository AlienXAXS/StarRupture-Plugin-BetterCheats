#pragma once

#include "plugin_interface.h"

#include <cstdint>

// ---------------------------------------------------------------------------
// AOB Resolver
//
// The loader only lets a plugin pattern scan from the OnPluginLoadHooks event,
// which runs after GetPluginInfo and before PluginInit. IPluginHookScanner
// rejects any call made outside that window, so every pattern in
// aob_patterns.h is resolved here, once, and the resulting addresses are
// stashed in ResolvedAddresses for the feature modules to install from.
//
// self->hooks is null for the duration of the event on purpose: a plugin that
// misses a required pattern is unloaded, so nothing may be detoured until
// PluginInit. Resolve here, install there.
//
// Every pattern is resolved as OPTIONAL. Each one backs a single cheat that
// already degrades on its own when unresolved, and none is load-bearing enough
// that the whole menu should be refused over it — a miss is still recorded and
// surfaced to the user in the loader's startup failure report.
// ---------------------------------------------------------------------------

namespace BetterCheats::AOB
{
	// A member left at 0 means that pattern did not resolve on the running build;
	// the feature behind it must null-check and degrade.
	struct ResolvedAddresses
	{
		// HUD
		uintptr_t HealthHud_SetupProgressBar = 0;

		// Building
		uintptr_t GetResourceConditionResult    = 0;
		uintptr_t CheckAvailableBuildings       = 0;
		uintptr_t IsRecipeUnlocked              = 0;
		uintptr_t CheckStability_Custom         = 0;
		uintptr_t CheckStability_DynamicPillar  = 0;

		// Tools
		uintptr_t GetMiningDamage               = 0;
		uintptr_t UpdateRepHarvesterHeatStack   = 0;

		// Items
		uintptr_t AddNewItem                    = 0;

		// Machine power
		uintptr_t FMassEntityConfig_DestroyEntityTemplate       = 0;
		uintptr_t FMassEntityConfig_GetOrCreateEntityTemplate   = 0;
		uintptr_t UWorld_GetMassEntitySubsystem                 = 0;
		uintptr_t FMassEntityManager_ConstSharedFragments_FindOrAdd = 0;
		uintptr_t StructUtils_GetStructInstanceCrc32            = 0;

		// Enemies
		uintptr_t FWeakObjectPtr_AssignFObjectPtr               = 0;
		uintptr_t UMassActorSubsystem_GetEntityHandleFromActor  = 0;
		uintptr_t FMassEntityManager_InternalGetFragmentDataPtr = 0;

		// Dev menu (debug builds only)
		uintptr_t CheatManager_InitCheatManager = 0;
	};

	// Called from the OnPluginLoadHooks export, and only from there.
	void ResolveAll(IPluginSelf* self, IPluginHookScanner* scanner);

	// Valid from PluginInit onwards; all-zero if OnPluginLoadHooks never ran.
	const ResolvedAddresses& Resolved();
}
