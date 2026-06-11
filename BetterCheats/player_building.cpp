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
		// Unlimited Foundation Zoop
		// UAuActorPlacementData::MaxMultiConfirmPoints (offset 0x0068) caps how many
		// foundations can be chained in a single right-click drag. We write 9999
		// every tick on the top-level data asset AND on all sub-variant pointers
		// (FoundationData, TilesData, FoundationHelperVariant, ZOffsetVariantUp/Down,
		// AdditionalHelper) because the multi-confirm system may read the limit from
		// a sub-variant rather than the top-level object.
		// -------------------------------------------------------------------------

		constexpr ptrdiff_t kMaxMultiConfirmPointsOffset  = 0x0068;
		constexpr ptrdiff_t kTilesDataOffset              = 0x0168;
		constexpr ptrdiff_t kFoundationDataOffset         = 0x0170;
		constexpr ptrdiff_t kFoundationHelperVariantOffset = 0x01A0;
		constexpr ptrdiff_t kZOffsetVariantUpOffset       = 0x01A8;
		constexpr ptrdiff_t kZOffsetVariantDownOffset     = 0x01B0;
		constexpr ptrdiff_t kAdditionalHelperOffset       = 0x01B8;
		constexpr int32_t   kZoopLimit                    = 9999;

		bool                       g_unlimitedZoop        = false;
		SDK::UAuActorPlacementData* g_lastZoopData        = nullptr;
		int32_t                    g_originalMaxConfirm   = 0;

		void PatchZoopOnObject(SDK::UAuActorPlacementData* obj)
		{
			if (!obj) return;
			*reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(obj) + kMaxMultiConfirmPointsOffset) = kZoopLimit;
		}

		void RestoreZoopData()
		{
			if (!g_lastZoopData)
				return;

			*reinterpret_cast<int32_t*>(
				reinterpret_cast<uint8_t*>(g_lastZoopData) + kMaxMultiConfirmPointsOffset) = g_originalMaxConfirm;

			g_lastZoopData      = nullptr;
			g_originalMaxConfirm = 0;
		}

		SDK::UAuActorPlacementData* ReadSubVariant(SDK::UAuActorPlacementData* obj, ptrdiff_t offset)
		{
			return *reinterpret_cast<SDK::UAuActorPlacementData**>(reinterpret_cast<uint8_t*>(obj) + offset);
		}

		void ApplyZoop(SDK::UCrBuildingComponent* bc)
		{
			try
			{
				const SDK::UAuActorPlacementData* data = bc->BP_GetPlacementData();
				if (!data)
				{
					RestoreZoopData();
					return;
				}

				auto* mutableData = const_cast<SDK::UAuActorPlacementData*>(data);
				int32_t& maxPoints = *reinterpret_cast<int32_t*>(
					reinterpret_cast<uint8_t*>(mutableData) + kMaxMultiConfirmPointsOffset);

				if (mutableData != g_lastZoopData)
				{
					// Building type changed — restore old asset, cache new
					RestoreZoopData();
					g_lastZoopData       = mutableData;
					g_originalMaxConfirm = maxPoints;
				}

				maxPoints = kZoopLimit;

				// Patch sub-variants — the multi-confirm system may read from these
				PatchZoopOnObject(ReadSubVariant(mutableData, kFoundationDataOffset));
				PatchZoopOnObject(ReadSubVariant(mutableData, kTilesDataOffset));
				PatchZoopOnObject(ReadSubVariant(mutableData, kFoundationHelperVariantOffset));
				PatchZoopOnObject(ReadSubVariant(mutableData, kZOffsetVariantUpOffset));
				PatchZoopOnObject(ReadSubVariant(mutableData, kZOffsetVariantDownOffset));
				PatchZoopOnObject(ReadSubVariant(mutableData, kAdditionalHelperOffset));
			}
			catch (...) {}
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

		RestoreZoopData();

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
		// Zoop — write on every tick while active so the limit stays overridden
		// even if the player switches building types mid-session.
		if (g_unlimitedZoop)
		{
			if (SDK::ACrPlayerControllerBase* pc = GetLocalController())
				if (pc->BuildingComponent)
					ApplyZoop(pc->BuildingComponent);
		}
		else if (g_lastZoopData)
		{
			RestoreZoopData();
		}

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

		g_noBuildCost      = SessionConfig::Get("playerBuilding.noBuildCost", false);
		g_unlimitedZoop    = SessionConfig::Get("playerBuilding.unlimitedZoop", false);
		g_unlockAllBuildings = SessionConfig::Get("playerBuilding.unlockAllBuildings", false);
		g_unlockAllRecipes = SessionConfig::Get("playerBuilding.unlockAllRecipes", false);

		LOG_INFO("Building: applied saved config for session '%s'.", SessionConfig::GetSessionName().c_str());
	}

	void RenderImGui(IModLoaderImGui* imgui)
	{
		// ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp
		constexpr int kTableFlags    = (1 << 6) | (1 << 9) | (3 << 13);
		// ImGuiTableColumnFlags_WidthFixed
		constexpr int kColumnFixed   = 1 << 4;

		imgui->SeparatorText("Placement");

		if (imgui->BeginTable("##building_placement_table", 2, kTableFlags))
		{
			imgui->TableSetupColumn("Option",  kColumnFixed, 500.0f);
			imgui->TableSetupColumn("Enabled", 0,            0.0f);

			imgui->TableNextRow(0, 0.0f);
			imgui->TableSetColumnIndex(0);
			imgui->Text("No Build Cost");
			imgui->TableSetColumnIndex(1);
			if (imgui->Checkbox("##no_build_cost", &g_noBuildCost))
				SessionConfig::Set("playerBuilding.noBuildCost", g_noBuildCost);

			imgui->TableNextRow(0, 0.0f);
			imgui->TableSetColumnIndex(0);
			imgui->Text("Unlimited Foundation Zoop (Not working in this build)");
			imgui->TableSetColumnIndex(1);
			imgui->BeginDisabled(true);
			if (imgui->Checkbox("##unlimited_zoop", &g_unlimitedZoop))
				SessionConfig::Set("playerBuilding.unlimitedZoop", g_unlimitedZoop);
			imgui->EndDisabled();

			imgui->EndTable();
		}

		imgui->Spacing();
		imgui->SeparatorText("Research");
		imgui->TextWrapped(
			"Warning: While attention has been put into this to try to not make these changes permanent in your save file, it may still happen.");
		imgui->Spacing();

		if (imgui->BeginTable("##building_research_table", 2, kTableFlags))
		{
			imgui->TableSetupColumn("Option",  kColumnFixed, 500.0f);
			imgui->TableSetupColumn("Enabled", 0,            0.0f);

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
