#include "player_building.h"
#include "plugin_helpers.h"
#include "aob_resolver.h"
#include "session_config.h"
#include "ui_widgets.h"

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
		// ACrAPHelperActorCustom::CheckStability is the native function that actually
		// decides whether a placement passes the stability graph — its bool return is
		// the real gate. (The data-asset flags bCheckStability /
		// RequirePlatformConnecting are never read on this path, which is why patching
		// those did nothing.) We let the original run — so the HUD stability bar still
		// reflects the real computed value — and just force the return to true while
		// the cheat is active.
		//
		// ACrAPHelperDynamicPillar::CheckStability is the equivalent gate for the
		// multi-point/zoop path (chained foundations), which does not go through the
		// Custom overload — it needs its own hook or zoop placements stay blocked.
		// -------------------------------------------------------------------------

		using CheckStabilityFn = bool(__fastcall*)(void* self, const void* placementData);

		CheckStabilityFn g_originalCheckStabilityCustom        = nullptr;
		CheckStabilityFn g_originalCheckStabilityDynamicPillar = nullptr;
		HookHandle       g_hookCheckStabilityCustom            = nullptr;
		HookHandle       g_hookCheckStabilityDynamicPillar     = nullptr;
		bool             g_noStabilityCheck                    = false;

		bool __fastcall Detour_CheckStabilityCustom(void* self, const void* placementData)
		{
			bool result = g_originalCheckStabilityCustom(self, placementData);
			return g_noStabilityCheck ? true : result;
		}

		bool __fastcall Detour_CheckStabilityDynamicPillar(void* self, const void* placementData)
		{
			bool result = g_originalCheckStabilityDynamicPillar(self, placementData);
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
			if (!pc) return nullptr;

			// See player_skills.cpp's GetLocalController — the controller isn't a
			// Chimera one for the whole life of the world.
			SDK::UClass* controllerClass = SDK::ACrPlayerControllerBase::StaticClass();
			if (!controllerClass || !pc->IsA(controllerClass)) return nullptr;

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
		IPluginHookUtils* hooks = GetHooks() ? GetHooks()->Hooks : nullptr;

		if (!hooks)
		{
			LOG_WARN("Building: hook utils unavailable, no-build-cost hook skipped");
			return;
		}

		const AOB::ResolvedAddresses& aob = AOB::Resolved();

		uintptr_t addr = aob.GetResourceConditionResult;
		if (!addr)
		{
			LOG_WARN("Building: GetResourceConditionResult unresolved");
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

		uintptr_t checkAddr = aob.CheckAvailableBuildings;
		if (!checkAddr)
		{
			LOG_WARN("Building: CheckAvailableBuildings unresolved — unlock all buildings will not refresh the menu");
		}
		else
		{
			g_checkAvailableBuildings = reinterpret_cast<CheckAvailableBuildingsFn>(checkAddr);
			LOG_INFO("Building: CheckAvailableBuildings resolved");
		}

		uintptr_t recipeAddr = aob.IsRecipeUnlocked;
		if (!recipeAddr)
		{
			LOG_WARN("Building: IsRecipeUnlocked unresolved");
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

		uintptr_t stabilityCustomAddr = aob.CheckStability_Custom;
		if (!stabilityCustomAddr)
		{
			LOG_WARN("Building: CheckStability_Custom unresolved");
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

		uintptr_t stabilityPillarAddr = aob.CheckStability_DynamicPillar;
		if (!stabilityPillarAddr)
		{
			LOG_WARN("Building: CheckStability_DynamicPillar unresolved — zoop placements will still be stability-checked");
		}
		else
		{
			g_hookCheckStabilityDynamicPillar = hooks->Install(
				stabilityPillarAddr,
				reinterpret_cast<void*>(&Detour_CheckStabilityDynamicPillar),
				reinterpret_cast<void**>(&g_originalCheckStabilityDynamicPillar));

			if (!g_hookCheckStabilityDynamicPillar)
				LOG_WARN("Building: failed to install CheckStability_DynamicPillar hook");
			else
				LOG_INFO("Building: CheckStability_DynamicPillar hook installed");
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

		if (hooks && g_hookCheckStabilityCustom)
		{
			hooks->Remove(g_hookCheckStabilityCustom);
			g_hookCheckStabilityCustom     = nullptr;
			g_originalCheckStabilityCustom = nullptr;
		}

		if (hooks && g_hookCheckStabilityDynamicPillar)
		{
			hooks->Remove(g_hookCheckStabilityDynamicPillar);
			g_hookCheckStabilityDynamicPillar     = nullptr;
			g_originalCheckStabilityDynamicPillar = nullptr;
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
		// No Stability Check is purely hook-driven (see Detour_CheckStabilityCustom /
		// Detour_CheckStabilityDynamicPillar above) — nothing to do here per-tick.

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

		// No built-in presets here, so saved presets sit at the very top --
		// same "above every control" position every group uses.
		{
			static BetterCheats::UI::SavedPresetRowState s_presetRow;
			constexpr int kFieldCount = 4;
			BetterCheats::PresetStore::Field fields[kFieldCount];

			auto getLive = [](BetterCheats::PresetStore::Field* out)
			{
				out[0] = { "noBuildCost",         g_noBuildCost         ? 1.0f : 0.0f };
				out[1] = { "noStabilityCheck",    g_noStabilityCheck    ? 1.0f : 0.0f };
				out[2] = { "unlockAllBuildings",  g_unlockAllBuildings  ? 1.0f : 0.0f };
				out[3] = { "unlockAllRecipes",    g_unlockAllRecipes    ? 1.0f : 0.0f };
			};
			auto applyFields = [](const BetterCheats::PresetStore::Field* f, int count)
			{
				if (count > 0) { g_noBuildCost        = f[0].value != 0.0f; SessionConfig::Set("playerBuilding.noBuildCost", g_noBuildCost); }
				if (count > 1) { g_noStabilityCheck   = f[1].value != 0.0f; SessionConfig::Set("playerBuilding.noStabilityCheck", g_noStabilityCheck); }
				if (count > 2) { g_unlockAllBuildings = f[2].value != 0.0f; SessionConfig::Set("playerBuilding.unlockAllBuildings", g_unlockAllBuildings); }
				if (count > 3) { g_unlockAllRecipes   = f[3].value != 0.0f; SessionConfig::Set("playerBuilding.unlockAllRecipes", g_unlockAllRecipes); }
			};
			auto isBuiltin      = [](const char*) { return false; };
			auto computeSuggest = [](char* out, int cap) { snprintf(out, cap, "Custom"); };

			BetterCheats::UI::RenderSavedPresetsRow(imgui, "building_saved_presets", "Building",
				fields, kFieldCount, getLive, applyFields, isBuiltin, computeSuggest, s_presetRow);
		}
		imgui->Spacing();

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
