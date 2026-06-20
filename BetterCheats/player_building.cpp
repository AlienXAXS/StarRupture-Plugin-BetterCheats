#include "player_building.h"
#include "plugin_helpers.h"
#include "aob_patterns.h"
#include "session_config.h"

#include "AuActorPlacement_classes.hpp"
#include "Chimera_classes.hpp"
#include "Engine_classes.hpp"

#include <cstdint>

namespace BetterCheats::Panels::Building
{
	namespace
	{
		// -------------------------------------------------------------------------
		// No Build Cost
		// Hook UCrBuildingComponent::GetResourceConditionResult to always return
		// Valid (1) when active, bypassing the inventory check entirely.
		// -------------------------------------------------------------------------

		using GetResourceConditionResultFn = int64_t(__fastcall*)(void* self);

		GetResourceConditionResultFn g_originalGetResourceConditionResult = nullptr;
		HookHandle                   g_hookGetResourceConditionResult      = nullptr;
		bool                         g_noBuildCost                         = false;

		int64_t __fastcall Detour_GetResourceConditionResult(void* self)
		{
			if (g_noBuildCost)
				return 1; // EAuAPlacementConditionResult::Valid

			return g_originalGetResourceConditionResult(self);
		}

		// -------------------------------------------------------------------------
		// No Stability Check
		// ACrAPHelperActorBase::CheckStability / ACrAPHelperActorCustom::CheckStability
		// are the native functions that actually decide whether a placement passes
		// the stability graph — their bool return is the real gate. (The data-asset
		// flags bCheckStability / RequirePlatformConnecting are never read on this
		// path, which is why patching those did nothing.) We let the original run
		// — so the HUD stability bar still reflects the real computed value — and
		// just force the return to true while the cheat is active.
		// -------------------------------------------------------------------------

		using CheckStabilityFn = bool(__fastcall*)(void* self, const void* placementData);

		CheckStabilityFn g_originalCheckStabilityBase   = nullptr;
		CheckStabilityFn g_originalCheckStabilityCustom = nullptr;
		HookHandle       g_hookCheckStabilityBase       = nullptr;
		HookHandle       g_hookCheckStabilityCustom     = nullptr;
		bool             g_noStabilityCheck             = false;

		bool __fastcall Detour_CheckStabilityBase(void* self, const void* placementData)
		{
			bool result = g_originalCheckStabilityBase(self, placementData);
			return g_noStabilityCheck ? true : result;
		}

		bool __fastcall Detour_CheckStabilityCustom(void* self, const void* placementData)
		{
			bool result = g_originalCheckStabilityCustom(self, placementData);
			return g_noStabilityCheck ? true : result;
		}

		// -------------------------------------------------------------------------
		// No Stability Check — Multi-Point / Zoop
		// ACrAPHelperActorCustom::CheckDynamicHelperStability is a separate gate used
		// by the multi-point "dynamic helper" placement path (chained foundations) —
		// it does not go through CheckStability at all, which is why zoop placements
		// were still being blocked. Same approach: let the original run (it also
		// updates the HUD strength value via SetStabilityStrength) and force true.
		// -------------------------------------------------------------------------

		using CheckDynamicHelperStabilityFn = bool(__fastcall*)(void* self);

		CheckDynamicHelperStabilityFn g_originalCheckDynamicHelperStability = nullptr;
		HookHandle                    g_hookCheckDynamicHelperStability     = nullptr;

		bool __fastcall Detour_CheckDynamicHelperStability(void* self)
		{
			bool result = g_originalCheckDynamicHelperStability(self);
			return g_noStabilityCheck ? true : result;
		}

		// -------------------------------------------------------------------------
		// Unlock All Buildings
		// ACrTechnologyKeeper::bUnlockAllBuildings (0x03A8) short-circuits
		// IsBuildingAvailable to return true unconditionally. Setting the flag alone
		// is not enough — CheckAvailableBuildings must be called to rebuild
		// AvailableBuildings and broadcast the menu refresh delegate.
		// When bUnlockAllBuildings is true the function ignores Corporation/Reputation,
		// so we pass nullptr/0 safely in both directions.
		// This flag may be serialised into the save file — the UI warns accordingly.
		// -------------------------------------------------------------------------

		constexpr ptrdiff_t kBUnlockAllBuildingsOffset = 0x03A8;

		bool g_unlockAllBuildings     = false;
		bool g_prevUnlockAllBuildings = false;

		using CheckAvailableBuildingsFn = void(__fastcall*)(void* self, void* corporation, int64_t reputation);
		CheckAvailableBuildingsFn g_checkAvailableBuildings = nullptr;

		// -------------------------------------------------------------------------
		// Unlock All Recipes
		// Hook ACrCraftingRecipeOwner::IsRecipeUnlocked to always return true.
		// bUnlockAllRecipes on ACrTechnologyKeeper has no xrefs — the actual gate
		// is this function, called by the crafting UI per recipe.
		// -------------------------------------------------------------------------

		using IsRecipeUnlockedFn = bool(__fastcall*)(void* self, void* recipe);

		IsRecipeUnlockedFn g_originalIsRecipeUnlocked = nullptr;
		HookHandle         g_hookIsRecipeUnlocked      = nullptr;
		bool               g_unlockAllRecipes          = false;

		bool __fastcall Detour_IsRecipeUnlocked(void* self, void* recipe)
		{
			if (g_unlockAllRecipes)
				return true;

			return g_originalIsRecipeUnlocked(self, recipe);
		}

		// -------------------------------------------------------------------------
		// World accessors — game-thread only
		// -------------------------------------------------------------------------

		SDK::ACrPlayerControllerBase* GetLocalController()
		{
			SDK::UWorld* world = nullptr;
			try { world = SDK::UWorld::GetWorld(); }
			catch (...) { return nullptr; }
			if (!world) return nullptr;

			SDK::APlayerController* pc = SDK::UGameplayStatics::GetPlayerController(world, 0);
			return static_cast<SDK::ACrPlayerControllerBase*>(pc);
		}

		SDK::ACrTechnologyKeeper* GetTechnologyKeeper()
		{
			SDK::UWorld* world = nullptr;
			try { world = SDK::UWorld::GetWorld(); }
			catch (...) { return nullptr; }
			if (!world) return nullptr;

			SDK::AGameStateBase* gs = SDK::UGameplayStatics::GetGameState(world);
			if (!gs) return nullptr;

			return static_cast<SDK::ACrGameStateBase*>(gs)->TechnologyKeeper;
		}
	}

	void Initialize()
	{
		IPluginScanner*  scanner = GetScanner();
		IPluginHookUtils* hooks  = GetHooks() ? GetHooks()->Hooks : nullptr;

		if (!scanner || !hooks)
		{
			LOG_WARN("Building: scanner or hook utils unavailable, no-build-cost hook skipped");
			return;
		}

		uintptr_t addr = scanner->FindPatternInMainModule(AOB::GetResourceConditionResult);
		if (!addr)
		{
			LOG_WARN("Building: GetResourceConditionResult pattern not found");
		}
		else
		{
			g_hookGetResourceConditionResult = hooks->Install(
				addr,
				reinterpret_cast<void*>(&Detour_GetResourceConditionResult),
				reinterpret_cast<void**>(&g_originalGetResourceConditionResult));

			if (!g_hookGetResourceConditionResult)
			{
				LOG_WARN("Building: failed to install GetResourceConditionResult hook");
			}
			else
			{
				LOG_INFO("Building: GetResourceConditionResult hook installed");
			}
		}

		uintptr_t checkAddr = scanner->FindPatternInMainModule(AOB::CheckAvailableBuildings);
		if (!checkAddr)
		{
			LOG_WARN("Building: CheckAvailableBuildings pattern not found — unlock all buildings will not refresh the menu");
		}
		else
		{
			g_checkAvailableBuildings = reinterpret_cast<CheckAvailableBuildingsFn>(checkAddr);
			LOG_INFO("Building: CheckAvailableBuildings resolved");
		}

		uintptr_t recipeAddr = scanner->FindPatternInMainModule(AOB::IsRecipeUnlocked);
		if (!recipeAddr)
		{
			LOG_WARN("Building: IsRecipeUnlocked pattern not found");
		}
		else
		{
			g_hookIsRecipeUnlocked = hooks->Install(
				recipeAddr,
				reinterpret_cast<void*>(&Detour_IsRecipeUnlocked),
				reinterpret_cast<void**>(&g_originalIsRecipeUnlocked));

			if (!g_hookIsRecipeUnlocked)
				LOG_WARN("Building: failed to install IsRecipeUnlocked hook");
			else
				LOG_INFO("Building: IsRecipeUnlocked hook installed");
		}

		uintptr_t stabilityBaseAddr = scanner->FindPatternInMainModule(AOB::CheckStability_Base);
		if (!stabilityBaseAddr)
		{
			LOG_WARN("Building: CheckStability_Base pattern not found");
		}
		else
		{
			g_hookCheckStabilityBase = hooks->Install(
				stabilityBaseAddr,
				reinterpret_cast<void*>(&Detour_CheckStabilityBase),
				reinterpret_cast<void**>(&g_originalCheckStabilityBase));

			if (!g_hookCheckStabilityBase)
				LOG_WARN("Building: failed to install CheckStability_Base hook");
			else
				LOG_INFO("Building: CheckStability_Base hook installed");
		}

		uintptr_t stabilityCustomAddr = scanner->FindPatternInMainModule(AOB::CheckStability_Custom);
		if (!stabilityCustomAddr)
		{
			LOG_WARN("Building: CheckStability_Custom pattern not found");
		}
		else
		{
			g_hookCheckStabilityCustom = hooks->Install(
				stabilityCustomAddr,
				reinterpret_cast<void*>(&Detour_CheckStabilityCustom),
				reinterpret_cast<void**>(&g_originalCheckStabilityCustom));

			if (!g_hookCheckStabilityCustom)
				LOG_WARN("Building: failed to install CheckStability_Custom hook");
			else
				LOG_INFO("Building: CheckStability_Custom hook installed");
		}

		uintptr_t dynamicStabilityAddr = scanner->FindPatternInMainModule(AOB::CheckDynamicHelperStability);
		if (!dynamicStabilityAddr)
		{
			LOG_WARN("Building: CheckDynamicHelperStability pattern not found");
		}
		else
		{
			g_hookCheckDynamicHelperStability = hooks->Install(
				dynamicStabilityAddr,
				reinterpret_cast<void*>(&Detour_CheckDynamicHelperStability),
				reinterpret_cast<void**>(&g_originalCheckDynamicHelperStability));

			if (!g_hookCheckDynamicHelperStability)
				LOG_WARN("Building: failed to install CheckDynamicHelperStability hook");
			else
				LOG_INFO("Building: CheckDynamicHelperStability hook installed");
		}
	}

	void Shutdown()
	{
		IPluginHookUtils* hooks = GetHooks() ? GetHooks()->Hooks : nullptr;
		if (hooks && g_hookGetResourceConditionResult)
		{
			hooks->Remove(g_hookGetResourceConditionResult);
			g_hookGetResourceConditionResult      = nullptr;
			g_originalGetResourceConditionResult  = nullptr;
		}

		if (hooks && g_hookIsRecipeUnlocked)
		{
			hooks->Remove(g_hookIsRecipeUnlocked);
			g_hookIsRecipeUnlocked      = nullptr;
			g_originalIsRecipeUnlocked  = nullptr;
		}
		g_unlockAllRecipes = false;

		if (hooks && g_hookCheckStabilityBase)
		{
			hooks->Remove(g_hookCheckStabilityBase);
			g_hookCheckStabilityBase     = nullptr;
			g_originalCheckStabilityBase = nullptr;
		}

		if (hooks && g_hookCheckStabilityCustom)
		{
			hooks->Remove(g_hookCheckStabilityCustom);
			g_hookCheckStabilityCustom     = nullptr;
			g_originalCheckStabilityCustom = nullptr;
		}

		if (hooks && g_hookCheckDynamicHelperStability)
		{
			hooks->Remove(g_hookCheckDynamicHelperStability);
			g_hookCheckDynamicHelperStability     = nullptr;
			g_originalCheckDynamicHelperStability = nullptr;
		}
		g_noStabilityCheck = false;

		if (g_unlockAllBuildings)
		{
			g_unlockAllBuildings     = false;
			g_prevUnlockAllBuildings = false;
			try
			{
				if (SDK::ACrTechnologyKeeper* keeper = GetTechnologyKeeper())
				{
					*reinterpret_cast<bool*>(reinterpret_cast<uint8_t*>(keeper) + kBUnlockAllBuildingsOffset) = false;
					if (g_checkAvailableBuildings)
						g_checkAvailableBuildings(keeper, nullptr, 0);
				}
			}
			catch (...) {}
		}
		g_prevUnlockAllBuildings  = false;
		g_checkAvailableBuildings = nullptr;
	}

	void Tick(float /*deltaSeconds*/)
	{
		// No Stability Check is purely hook-driven (see Detour_CheckStabilityBase /
		// Detour_CheckStabilityCustom / Detour_CheckDynamicHelperStability above) —
		// nothing to do here per-tick.

		// Unlock all buildings — only act on change so we don't spam CheckAvailableBuildings.
		// Writing the flag alone is insufficient; the function must run to rebuild
		// AvailableBuildings and fire the delegate that refreshes the build menu.
		if (g_unlockAllBuildings != g_prevUnlockAllBuildings)
		{
			g_prevUnlockAllBuildings = g_unlockAllBuildings;
			LOG_INFO("Building: unlock all buildings -> %s", g_unlockAllBuildings ? "enabled" : "disabled");
			try
			{
				SDK::ACrTechnologyKeeper* keeper = GetTechnologyKeeper();
				LOG_DEBUG("Building: TechnologyKeeper = 0x%llx", reinterpret_cast<unsigned long long>(keeper));
				if (keeper)
				{
					*reinterpret_cast<bool*>(reinterpret_cast<uint8_t*>(keeper) + kBUnlockAllBuildingsOffset) = g_unlockAllBuildings;
					if (g_checkAvailableBuildings)
					{
						LOG_DEBUG("Building: calling CheckAvailableBuildings (keeper=0x%llx, bUnlockAll=%d)",
							reinterpret_cast<unsigned long long>(keeper), g_unlockAllBuildings);
						g_checkAvailableBuildings(keeper, nullptr, 0);
						LOG_DEBUG("Building: CheckAvailableBuildings returned");
					}
					else
					{
						LOG_WARN("Building: CheckAvailableBuildings not resolved — menu will not refresh");
					}
				}
			}
			catch (...) { LOG_WARN("Building: exception in unlock all buildings toggle"); }
		}

	}

	void ApplySavedConfig()
	{
		if (!SessionConfig::IsLoaded())
			return;

		g_noBuildCost        = SessionConfig::Get("playerBuilding.noBuildCost", false);
		g_noStabilityCheck   = SessionConfig::Get("playerBuilding.noStabilityCheck", false);
		g_unlockAllBuildings = SessionConfig::Get("playerBuilding.unlockAllBuildings", false);
		g_unlockAllRecipes   = SessionConfig::Get("playerBuilding.unlockAllRecipes", false);

		LOG_INFO("Building: applied saved config for session '%s'.", SessionConfig::GetSessionName().c_str());
	}

	void RenderImGui(IModLoaderImGui* imgui)
	{
		// ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp
		constexpr int kTableFlags    = (1 << 6) | (1 << 9) | (3 << 13);

		imgui->SeparatorText("Placement");

		if (imgui->BeginTable("##building_placement_table", 2, kTableFlags))
		{
			imgui->TableSetupColumn("Option",  0, 0.85f);
			imgui->TableSetupColumn("Enabled", 0, 0.15f);

			imgui->TableNextRow(0, 0.0f);
			imgui->TableSetColumnIndex(0);
			imgui->Text("No Build Cost");
			imgui->TableSetColumnIndex(1);
			if (imgui->Checkbox("##no_build_cost", &g_noBuildCost))
				SessionConfig::Set("playerBuilding.noBuildCost", g_noBuildCost);

			imgui->TableNextRow(0, 0.0f);
			imgui->TableSetColumnIndex(0);
			imgui->Text("No Stability Check");
			imgui->TableSetColumnIndex(1);
			if (imgui->Checkbox("##no_stability_check", &g_noStabilityCheck))
				SessionConfig::Set("playerBuilding.noStabilityCheck", g_noStabilityCheck);

			imgui->EndTable();
		}

		imgui->Spacing();
		imgui->SeparatorText("Research");
		imgui->TextWrapped(
			"Warning: While attention has been put into this to try to not make these changes permanent in your save file, it may still happen.");
		imgui->Spacing();

		if (imgui->BeginTable("##building_research_table", 2, kTableFlags))
		{
			imgui->TableSetupColumn("Option",  0, 0.85f);
			imgui->TableSetupColumn("Enabled", 0, 0.15f);

			imgui->TableNextRow(0, 0.0f);
			imgui->TableSetColumnIndex(0);
			imgui->Text("Unlock All Buildings");
			imgui->TableSetColumnIndex(1);
			if (imgui->Checkbox("##unlock_buildings", &g_unlockAllBuildings))
				SessionConfig::Set("playerBuilding.unlockAllBuildings", g_unlockAllBuildings);

			imgui->TableNextRow(0, 0.0f);
			imgui->TableSetColumnIndex(0);
			imgui->Text("Unlock All Recipes");
			imgui->TableSetColumnIndex(1);
			if (imgui->Checkbox("##unlock_recipes", &g_unlockAllRecipes))
				SessionConfig::Set("playerBuilding.unlockAllRecipes", g_unlockAllRecipes);

			imgui->EndTable();
		}
	}
}
