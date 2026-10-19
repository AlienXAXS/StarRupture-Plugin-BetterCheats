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

		// The start of the function addr belongs to, following chained unwind
		// entries back to their parent the way the loader's FUNCTION_START check
		// does, so a cold chunk does not pass for a function of its own. 0 when
		// no unwind data covers addr.
		uintptr_t OwningFunctionStart(uintptr_t addr)
		{
			DWORD64 imageBase = 0;
			PRUNTIME_FUNCTION entry = RtlLookupFunctionEntry(addr, &imageBase, nullptr);
			for (int depth = 0; entry && depth < 32; ++depth)
			{
				const auto* unwind = reinterpret_cast<const uint8_t*>(imageBase + entry->UnwindData);
				if (((unwind[0] >> 3) & UNW_FLAG_CHAININFO) == 0)
					return static_cast<uintptr_t>(imageBase + entry->BeginAddress);

				// The parent entry follows the unwind codes, padded to an even count.
				const size_t codeSlots = (unwind[2] + 1u) & ~1u;
				entry = reinterpret_cast<PRUNTIME_FUNCTION>(const_cast<uint8_t*>(unwind) + 4 + codeSlots * 2);
			}
			return 0;
		}

		uintptr_t FindUnique(IPluginSelf* self, IPluginHookScanner* scanner, const char* pattern)
		{
			uintptr_t hits[2] = {};
			return scanner->FindAllPatternsInMainModule(self, pattern, hits, 2) == 1 ? hits[0] : 0;
		}

		// A function pattern from the raw scan: it must match exactly once, at a
		// function entry, the two verdicts Resolve applies. 0 otherwise.
		uintptr_t FindFunction(IPluginSelf* self, IPluginHookScanner* scanner, const char* pattern)
		{
			const uintptr_t addr = FindUnique(self, scanner, pattern);
			return addr && OwningFunctionStart(addr) == addr ? addr : 0;
		}

		// Deconstruct Windows During Waves is opt-in, so its patterns go through
		// the raw scan, which records nothing with the loader: after a game update
		// a miss turns that one toggle off instead of refusing the plugin. The same
		// two verdicts as Resolve still apply. The function must match exactly once
		// and be a function entry. The heat gate is a jump inside it, so it must
		// sit in that function, and its short jb must land where the near jb
		// opening the match (the infection gate) lands, which is the function's
		// return-null path.
		void ResolveDeconstructWindows(IPluginSelf* self, IPluginHookScanner* scanner)
		{
			const uintptr_t function = FindFunction(self, scanner, FindDeconstructibleTarget);
			const uintptr_t match    = FindUnique(self, scanner, FindDeconstructibleTarget_HeatGate);

			bool ok = function && match && OwningFunctionStart(match) == function;
			if (ok)
			{
				const auto* code = reinterpret_cast<const uint8_t*>(match);
				const uintptr_t nullPath   = match + 9 + *reinterpret_cast<const int32_t*>(code + 5);
				const uintptr_t gateTarget = match + kHeatGateJumpOffset + 2 + static_cast<int8_t>(code[kHeatGateJumpOffset + 1]);
				ok = gateTarget == nullPath;
			}

			if (!ok)
			{
				LOG_WARN("AOB: ACrPlayerControllerBase::FindDeconstructibleTarget or its heat gate did not resolve - "
					"Deconstruct Windows During Waves stays off.");
				return;
			}
			g_resolved.FindDeconstructibleTarget          = function;
			g_resolved.FindDeconstructibleTarget_HeatGate = match + kHeatGateJumpOffset;
		}

		// Build Windows Anywhere is opt-in too, and its two hooks only make sense
		// together, so they resolve as a pair or not at all.
		void ResolveBuildWindowsAnywhere(IPluginSelf* self, IPluginHookScanner* scanner)
		{
			const uintptr_t mainMesh = FindFunction(self, scanner, CheckMainMeshCollision_Custom);
			const uintptr_t box      = FindFunction(self, scanner, GetBoxCollisionResult);
			if (!mainMesh || !box)
			{
				LOG_WARN("AOB: ACrAPHelperActorCustom::CheckMainMeshCollision or ACrAPHelper::GetBoxCollisionResult "
					"did not resolve - Build Windows Anywhere will do nothing.");
				return;
			}
			g_resolved.CheckMainMeshCollision_Custom = mainMesh;
			g_resolved.GetBoxCollisionResult         = box;
		}

		// Build Benches Anywhere also needs the two Build Windows Anywhere hooks,
		// which player_building.cpp checks before installing these.
		void ResolveBuildBenchesAnywhere(IPluginSelf* self, IPluginHookScanner* scanner)
		{
			const uintptr_t snapped    = FindFunction(self, scanner, GetSnappedSocket);
			const uintptr_t socketFree = FindFunction(self, scanner, IsSocketFree);
			if (!snapped || !socketFree)
			{
				LOG_WARN("AOB: UCrSocketSnapPlacementMethod::GetSnappedSocket or UCrBuildingStabilitySubsystem::IsSocketFree "
					"did not resolve - Build Benches Anywhere will do nothing.");
				return;
			}
			g_resolved.GetSnappedSocket = snapped;
			g_resolved.IsSocketFree     = socketFree;
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

		ResolveDeconstructWindows(self, scanner);
		ResolveBuildWindowsAnywhere(self, scanner);
		ResolveBuildBenchesAnywhere(self, scanner);

		ResolveFunction(self, scanner, g_resolved.GetMiningDamage,
			"UCrMiningToolComponent::GetMiningDamage", GetMiningDamage);

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
