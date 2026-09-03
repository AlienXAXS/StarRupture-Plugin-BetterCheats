#include "aob_resolver.h"
#include "aob_patterns.h"
#include "plugin_helpers.h"

namespace BetterCheats::AOB
{
	namespace
	{
		ResolvedAddresses g_resolved;

		void ResolveOptional(IPluginSelf* self, IPluginHookScanner* scanner,
			uintptr_t& out, const char* hookName, const char* pattern)
		{
			out = scanner->ResolveOptional(self, hookName, pattern);
			if (!out)
				LOG_WARN("AOB: %s did not resolve — the feature using it will be unavailable.", hookName);
		}

#if BETTERCHEATS_DEV_BUILD
		// The InitCheatManager prologue is generic enough to appear elsewhere in the
		// image, so take every match and only accept an unambiguous one. Raw scans
		// record nothing with the loader, hence the explicit ReportWarning.
		void ResolveInitCheatManager(IPluginSelf* self, IPluginHookScanner* scanner)
		{
			static constexpr const char* kHookName = "UCheatManager::InitCheatManager";

			uintptr_t matches[8] = {};
			const int count = scanner->FindAllPatternsInMainModule(
				self, CheatManager_InitCheatManager, matches, 8);

			if (count <= 0)
			{
				scanner->ReportWarning(self, kHookName,
					"Pattern not found — dev menu falls back to ReceiveInitCheatManager only.");
				LOG_WARN("DevMenus: UCheatManager::InitCheatManager pattern not found - "
					"falling back to ReceiveInitCheatManager only.");
				return;
			}

			const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));

			if (count > 1)
			{
				scanner->ReportWarning(self, kHookName,
					"Pattern is ambiguous (multiple matches) — refusing to call it. Narrow the pattern in aob_patterns.h.");
				LOG_WARN("DevMenus: UCheatManager::InitCheatManager pattern is ambiguous (%d matches) - "
					"refusing to call it. Narrow the pattern in aob_patterns.h.", count);
				for (int i = 0; i < count && i < 8; ++i)
					LOG_WARN("DevMenus:   candidate %d at RVA 0x%llX.", i, static_cast<unsigned long long>(matches[i] - base));
				return;
			}

			g_resolved.CheatManager_InitCheatManager = matches[0];
			LOG_INFO("DevMenus: resolved UCheatManager::InitCheatManager at RVA 0x%llX (expected 0x47BA90 for the dumped build).",
				static_cast<unsigned long long>(matches[0] - base));
		}
#endif
	}

	const ResolvedAddresses& Resolved() { return g_resolved; }

	void ResolveAll(IPluginSelf* self, IPluginHookScanner* scanner)
	{
		if (!self || !scanner)
			return;

		ResolveOptional(self, scanner, g_resolved.HealthHud_SetupProgressBar,
			"UCrUW_HealthHud::SetupProgressBar", HealthHud_SetupProgressBar);

		ResolveOptional(self, scanner, g_resolved.GetResourceConditionResult,
			"UCrBuildingComponent::GetResourceConditionResult", GetResourceConditionResult);
		ResolveOptional(self, scanner, g_resolved.CheckAvailableBuildings,
			"ACrTechnologyKeeper::CheckAvailableBuildings", CheckAvailableBuildings);
		ResolveOptional(self, scanner, g_resolved.IsRecipeUnlocked,
			"ACrCraftingRecipeOwner::IsRecipeUnlocked", IsRecipeUnlocked);
		ResolveOptional(self, scanner, g_resolved.CheckStability_Custom,
			"ACrAPHelperActorCustom::CheckStability", CheckStability_Custom);
		ResolveOptional(self, scanner, g_resolved.CheckStability_DynamicPillar,
			"ACrAPHelperDynamicPillar::CheckStability", CheckStability_DynamicPillar);

		ResolveOptional(self, scanner, g_resolved.GetMiningDamage,
			"UCrMiningToolComponent::GetMiningDamage", GetMiningDamage);
		ResolveOptional(self, scanner, g_resolved.UpdateRepHarvesterHeatStack,
			"UCrMiningToolComponent::UpdateRepHarvesterHeatStack", UpdateRepHarvesterHeatStack);

		ResolveOptional(self, scanner, g_resolved.AddNewItem,
			"UAuItemsComponent::AddNewItem", AddNewItem);

		ResolveOptional(self, scanner, g_resolved.FMassEntityConfig_DestroyEntityTemplate,
			"FMassEntityConfig::DestroyEntityTemplate", FMassEntityConfig_DestroyEntityTemplate);
		ResolveOptional(self, scanner, g_resolved.FMassEntityConfig_GetOrCreateEntityTemplate,
			"FMassEntityConfig::GetOrCreateEntityTemplate", FMassEntityConfig_GetOrCreateEntityTemplate);
		ResolveOptional(self, scanner, g_resolved.UWorld_GetMassEntitySubsystem,
			"UWorld::GetSubsystem<UMassEntitySubsystem>", UWorld_GetMassEntitySubsystem);
		ResolveOptional(self, scanner, g_resolved.FMassEntityManager_ConstSharedFragments_FindOrAdd,
			"TSharedFragmentsContainer<FConstSharedStruct>::FindOrAdd", FMassEntityManager_ConstSharedFragments_FindOrAdd);
		ResolveOptional(self, scanner, g_resolved.StructUtils_GetStructInstanceCrc32,
			"UE::StructUtils::GetStructInstanceCrc32", StructUtils_GetStructInstanceCrc32);

		ResolveOptional(self, scanner, g_resolved.FWeakObjectPtr_AssignFObjectPtr,
			"FWeakObjectPtr::operator=(FObjectPtr)", FWeakObjectPtr_AssignFObjectPtr);
		ResolveOptional(self, scanner, g_resolved.UMassActorSubsystem_GetEntityHandleFromActor,
			"UMassActorSubsystem::GetEntityHandleFromActor", UMassActorSubsystem_GetEntityHandleFromActor);
		ResolveOptional(self, scanner, g_resolved.FMassEntityManager_InternalGetFragmentDataPtr,
			"FMassEntityManager::InternalGetFragmentDataPtr", FMassEntityManager_InternalGetFragmentDataPtr);

#if BETTERCHEATS_DEV_BUILD
		ResolveInitCheatManager(self, scanner);
#endif
	}
}
