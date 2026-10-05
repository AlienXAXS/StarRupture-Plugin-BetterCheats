#include "aob_resolver.h"
#include "aob_patterns.h"
#include "plugin_helpers.h"

namespace BetterCheats::AOB
{
	namespace
	{
		ResolvedAddresses g_resolved;

		// Every address in this file is the entry point of a compiled function --
		// each one is either called directly or has a detour written over it --
		// so they all declare PLUGIN_SCAN_FUNCTION_START. That is the whole point
		// of the kind: a pattern that lands 0x37 bytes into an unrelated function
		// still matches, and without a kind the loader hands that address back.
		//
		// Optional is a label on the report line, not a lighter verdict: a miss
		// refuses the plugin either way. It marks the addresses the feature
		// modules genuinely null-check.
		void ResolveFunction(IPluginSelf* self, IPluginHookScanner* scanner,
			uintptr_t& out, const char* hookName, const char* pattern)
		{
			PluginScanRequest req = PLUGIN_SCAN_REQUEST_INIT;
			req.hookName = hookName;
			req.pattern  = pattern;
			req.kind     = PLUGIN_SCAN_FUNCTION_START;
			req.flags    = PLUGIN_SCAN_FLAG_OPTIONAL;

			out = scanner->Resolve(self, &req);
			if (!out)
				LOG_WARN("AOB: %s did not resolve - the feature using it will be unavailable.", hookName);
		}

#if BETTERCHEATS_DEV_BUILD
		// The InitCheatManager prologue is generic enough to appear elsewhere in
		// the image, which used to mean scanning for every match by hand and
		// refusing an ambiguous one. The loader does both now: a pattern that
		// matches twice is a failure it reports (listing each match with the
		// function it landed in), and FUNCTION_START rejects a match that is not
		// a real function entry -- which is what makes calling it with a
		// UCheatManager* in RCX safe.
		void ResolveInitCheatManager(IPluginSelf* self, IPluginHookScanner* scanner)
		{
			static constexpr const char* kHookName = "UCheatManager::InitCheatManager";

			PluginScanRequest req = PLUGIN_SCAN_REQUEST_INIT;
			req.hookName = kHookName;
			req.pattern  = CheatManager_InitCheatManager;
			req.kind     = PLUGIN_SCAN_FUNCTION_START;
			req.flags    = PLUGIN_SCAN_FLAG_OPTIONAL;

			const uintptr_t addr = scanner->Resolve(self, &req);
			if (!addr)
			{
				LOG_WARN("DevMenus: UCheatManager::InitCheatManager unresolved - "
					"falling back to ReceiveInitCheatManager only.");
				return;
			}

			g_resolved.CheatManager_InitCheatManager = addr;

			const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
			LOG_INFO("DevMenus: resolved UCheatManager::InitCheatManager at RVA 0x%llX (expected 0x47BA90 for the dumped build).",
				static_cast<unsigned long long>(addr - base));
		}
#endif
	}

	const ResolvedAddresses& Resolved() { return g_resolved; }

	void ResolveAll(IPluginSelf* self, IPluginHookScanner* scanner)
	{
		if (!self || !scanner)
			return;

		ResolveFunction(self, scanner, g_resolved.HealthHud_SetupProgressBar,
			"UCrUW_HealthHud::SetupProgressBar", HealthHud_SetupProgressBar);

		ResolveFunction(self, scanner, g_resolved.GetPlacementResourceConditionResult,
			"UCrBuildingComponent::GetPlacementResourceConditionResult", GetPlacementResourceConditionResult);
		ResolveFunction(self, scanner, g_resolved.CheckAvailableBuildings,
			"ACrTechnologyKeeper::CheckAvailableBuildings", CheckAvailableBuildings);
		ResolveFunction(self, scanner, g_resolved.IsRecipeUnlocked,
			"ACrCraftingRecipeOwner::IsRecipeUnlocked", IsRecipeUnlocked);
		ResolveFunction(self, scanner, g_resolved.CheckStability_Custom,
			"ACrAPHelperActorCustom::CheckStability", CheckStability_Custom);
		ResolveFunction(self, scanner, g_resolved.CheckStability_DynamicPillar,
			"ACrAPHelperDynamicPillar::CheckStability", CheckStability_DynamicPillar);

		ResolveFunction(self, scanner, g_resolved.GetMiningDamage,
			"UCrMiningToolComponent::GetMiningDamage", GetMiningDamage);
		ResolveFunction(self, scanner, g_resolved.UpdateRepHarvesterHeatStack,
			"UCrMiningToolComponent::UpdateRepHarvesterHeatStack", UpdateRepHarvesterHeatStack);

		ResolveFunction(self, scanner, g_resolved.AddNewItem,
			"UAuItemsComponent::AddNewItem", AddNewItem);
		ResolveFunction(self, scanner, g_resolved.InventoryContainer_InitInventorySlots,
			"UCrUW_InventoryContainer::InitInventorySlots", InventoryContainer_InitInventorySlots);

		ResolveFunction(self, scanner, g_resolved.FMassEntityConfig_DestroyEntityTemplate,
			"FMassEntityConfig::DestroyEntityTemplate", FMassEntityConfig_DestroyEntityTemplate);
		ResolveFunction(self, scanner, g_resolved.FMassEntityConfig_GetOrCreateEntityTemplate,
			"FMassEntityConfig::GetOrCreateEntityTemplate", FMassEntityConfig_GetOrCreateEntityTemplate);
		ResolveFunction(self, scanner, g_resolved.UWorld_GetMassEntitySubsystem,
			"UWorld::GetSubsystem<UMassEntitySubsystem>", UWorld_GetMassEntitySubsystem);
		ResolveFunction(self, scanner, g_resolved.FMassEntityManager_ConstSharedFragments_FindOrAdd,
			"TSharedFragmentsContainer<FConstSharedStruct>::FindOrAdd", FMassEntityManager_ConstSharedFragments_FindOrAdd);
		ResolveFunction(self, scanner, g_resolved.StructUtils_GetStructInstanceCrc32,
			"UE::StructUtils::GetStructInstanceCrc32", StructUtils_GetStructInstanceCrc32);

		ResolveFunction(self, scanner, g_resolved.FWeakObjectPtr_AssignFObjectPtr,
			"FWeakObjectPtr::operator=(FObjectPtr)", FWeakObjectPtr_AssignFObjectPtr);
		ResolveFunction(self, scanner, g_resolved.UMassActorSubsystem_GetEntityHandleFromActor,
			"UMassActorSubsystem::GetEntityHandleFromActor", UMassActorSubsystem_GetEntityHandleFromActor);
		ResolveFunction(self, scanner, g_resolved.FMassEntityManager_InternalGetFragmentDataPtr,
			"FMassEntityManager::InternalGetFragmentDataPtr", FMassEntityManager_InternalGetFragmentDataPtr);

#if BETTERCHEATS_DEV_BUILD
		ResolveInitCheatManager(self, scanner);
#endif
	}
}
