#include "player_building.h"
#include "plugin_helpers.h"
#include "aob_resolver.h"
#include "session_config.h"

#include "AuActorPlacement_classes.hpp"
#include "Chimera_classes.hpp"
#include "Engine_classes.hpp"

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>

namespace BetterCheats::Panels::Building
{
	namespace
	{
		// -------------------------------------------------------------------------
		// No Build Cost
		// Hook UCrBuildingComponent::GetPlacementResourceConditionResult to always return
		// Valid (1) when active, bypassing the inventory check entirely.
		// -------------------------------------------------------------------------

		using GetPlacementResourceConditionResultFn = int64_t(__fastcall*)(void* self);

		GetPlacementResourceConditionResultFn g_originalGetPlacementResourceConditionResult = nullptr;
		HookHandle                            g_hookGetPlacementResourceConditionResult     = nullptr;
		bool                                  g_noBuildCost                                 = false;

		int64_t __fastcall Detour_GetPlacementResourceConditionResult(void* self)
		{
			if (g_noBuildCost)
				return 1; // EAuAPlacementConditionResult::Valid

			return g_originalGetPlacementResourceConditionResult(self);
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
		// Deconstruct Windows During Waves
		// ACrPlayerControllerBase::FindDeconstructibleTarget refuses any building
		// whose temperature is above 0, and a wave heats the habitat's windows. When
		// it finds nothing, it runs once more with that one jump turned into a
		// no-op, and the answer is kept only if it is a window, so every other
		// heated building stays protected. The function reads only its inputs and
		// writes nothing, so running it twice changes nothing else.
		// -------------------------------------------------------------------------

		using FindDeconstructibleTargetFn = SDK::AActor*(__fastcall*)(SDK::ACrPlayerControllerBase* self, const void* traceData);

		FindDeconstructibleTargetFn g_originalFindDeconstructibleTarget = nullptr;
		HookHandle                  g_hookFindDeconstructibleTarget     = nullptr;
		uint8_t*                    g_heatGate                          = nullptr;
		uint8_t                     g_heatGateBytes[2]                  = {};
		std::mutex                  g_heatGateLock;
		std::atomic<bool>           g_deconstructWindowsDuringWaves{ false };

		bool IsWindowId(SDK::ECrBuildingID id)
		{
			return id == SDK::ECrBuildingID::ViewportLeft  || id == SDK::ECrBuildingID::ViewportMiddle
			    || id == SDK::ECrBuildingID::ViewportRight || id == SDK::ECrBuildingID::ViewportSingle;
		}

		bool TryGetBuildingId(SDK::AActor* actor, SDK::ECrBuildingID& out)
		{
			SDK::UClass* buildingClass = SDK::ACrBuildingActorBase::StaticClass();
			if (!actor || !buildingClass || !actor->IsA(buildingClass))
				return false;

			out = static_cast<SDK::ACrBuildingActorBase*>(actor)->GetBuildingID();
			return true;
		}

		bool IsWindow(SDK::AActor* actor)
		{
			SDK::ECrBuildingID id{};
			return TryGetBuildingId(actor, id) && IsWindowId(id);
		}

		// IPluginMemoryUtils::Patch logs every write, and this flips the jump on
		// every call, so it writes the two bytes itself. The lock keeps Shutdown,
		// which can run on the render thread, from interleaving its page-protection
		// change with the game thread's.
		bool WriteHeatGate(const uint8_t (&bytes)[2])
		{
			std::lock_guard<std::mutex> lock(g_heatGateLock);

			DWORD oldProtect = 0;
			if (!VirtualProtect(g_heatGate, sizeof(bytes), PAGE_EXECUTE_READWRITE, &oldProtect))
				return false;

			memcpy(g_heatGate, bytes, sizeof(bytes));
			VirtualProtect(g_heatGate, sizeof(bytes), oldProtect, &oldProtect);
			FlushInstructionCache(GetCurrentProcess(), g_heatGate, sizeof(bytes));
			return true;
		}

		class HeatGateBypass
		{
		public:
			HeatGateBypass()
			{
				static constexpr uint8_t kNops[2] = { 0x90, 0x90 };
				m_open = WriteHeatGate(kNops);
			}
			~HeatGateBypass()
			{
				if (m_open)
					WriteHeatGate(g_heatGateBytes);
			}
			HeatGateBypass(const HeatGateBypass&) = delete;
			HeatGateBypass& operator=(const HeatGateBypass&) = delete;

			explicit operator bool() const { return m_open; }

		private:
			bool m_open = false;
		};

		SDK::AActor* __fastcall Detour_FindDeconstructibleTarget(SDK::ACrPlayerControllerBase* self, const void* traceData)
		{
			SDK::AActor* target = g_originalFindDeconstructibleTarget(self, traceData);
			if (target || !g_deconstructWindowsDuringWaves.load())
				return target;

			{
				HeatGateBypass bypass;
				if (!bypass)
					return nullptr;
				target = g_originalFindDeconstructibleTarget(self, traceData);
			}

			try
			{
				return IsWindow(target) ? target : nullptr;
			}
			catch (...)
			{
				return nullptr;
			}
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

		uintptr_t addr = aob.GetPlacementResourceConditionResult;
		if (!addr)
		{
			LOG_WARN("Building: GetPlacementResourceConditionResult unresolved");
		}
		else
		{
			g_hookGetPlacementResourceConditionResult = hooks->Install(
				addr,
				reinterpret_cast<void*>(&Detour_GetPlacementResourceConditionResult),
				reinterpret_cast<void**>(&g_originalGetPlacementResourceConditionResult));

			if (!g_hookGetPlacementResourceConditionResult)
			{
				LOG_WARN("Building: failed to install GetPlacementResourceConditionResult hook");
			}
			else
			{
				LOG_INFO("Building: GetPlacementResourceConditionResult hook installed");
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

		// aob_resolver.cpp has already logged the line when this did not resolve.
		if (aob.FindDeconstructibleTarget)
		{
			g_heatGate = reinterpret_cast<uint8_t*>(aob.FindDeconstructibleTarget_HeatGate);
			memcpy(g_heatGateBytes, g_heatGate, sizeof(g_heatGateBytes));

			g_hookFindDeconstructibleTarget = hooks->Install(
				aob.FindDeconstructibleTarget,
				reinterpret_cast<void*>(&Detour_FindDeconstructibleTarget),
				reinterpret_cast<void**>(&g_originalFindDeconstructibleTarget));
			if (!g_hookFindDeconstructibleTarget)
				LOG_WARN("Building: failed to install FindDeconstructibleTarget hook - Deconstruct Windows During Waves stays off");
			else
				LOG_INFO("Building: FindDeconstructibleTarget hook installed");
		}
	}

	void Shutdown()
	{
		IPluginHookUtils* hooks = GetHooks() ? GetHooks()->Hooks : nullptr;
		if (hooks && g_hookGetPlacementResourceConditionResult)
		{
			hooks->Remove(g_hookGetPlacementResourceConditionResult);
			g_hookGetPlacementResourceConditionResult     = nullptr;
			g_originalGetPlacementResourceConditionResult = nullptr;
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

		if (hooks && g_hookFindDeconstructibleTarget)
		{
			hooks->Remove(g_hookFindDeconstructibleTarget);
			g_hookFindDeconstructibleTarget     = nullptr;
			g_originalFindDeconstructibleTarget = nullptr;
		}
		if (g_heatGate)
			WriteHeatGate(g_heatGateBytes);
		g_deconstructWindowsDuringWaves = false;

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
		g_deconstructWindowsDuringWaves = SessionConfig::Get("playerBuilding.deconstructWindowsDuringWaves", false);

		LOG_INFO("Building: applied saved config for session '%s'.", SessionConfig::GetSessionName().c_str());
	}

	namespace
	{
		// The row's description, wrapped to its column, under the title.
		void Description(IModLoaderImGui* imgui, const char* text)
		{
			imgui->PushTextWrapPos(0.0f);
			imgui->TextDisabled(text);
			imgui->PopTextWrapPos();
		}
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
			Description(imgui,
				"Lets you place buildings without having the resources for them.");
			imgui->TableSetColumnIndex(1);
			if (imgui->Checkbox("##no_build_cost", &g_noBuildCost))
				SessionConfig::Set("playerBuilding.noBuildCost", g_noBuildCost);

			imgui->TableNextRow(0, 0.0f);
			imgui->TableSetColumnIndex(0);
			imgui->Text("No Stability Check");
			Description(imgui,
				"Lets you place buildings that the stability check would otherwise refuse.");
			imgui->TableSetColumnIndex(1);
			if (imgui->Checkbox("##no_stability_check", &g_noStabilityCheck))
				SessionConfig::Set("playerBuilding.noStabilityCheck", g_noStabilityCheck);

			imgui->TableNextRow(0, 0.0f);
			imgui->TableSetColumnIndex(0);
			imgui->Text("Deconstruct Windows During Waves");
			Description(imgui,
				"Lets habitat windows be deconstructed while a wave has heated them. Other heated buildings stay protected.");
			imgui->TableSetColumnIndex(1);
			bool deconstructWindows = g_deconstructWindowsDuringWaves.load();
			if (imgui->Checkbox("##deconstruct_windows_waves", &deconstructWindows))
			{
				g_deconstructWindowsDuringWaves = deconstructWindows;
				SessionConfig::Set("playerBuilding.deconstructWindowsDuringWaves", deconstructWindows);
			}

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
			Description(imgui,
				"Makes every building available in the build menu, without the research or reputation it needs.");
			imgui->TableSetColumnIndex(1);
			if (imgui->Checkbox("##unlock_buildings", &g_unlockAllBuildings))
				SessionConfig::Set("playerBuilding.unlockAllBuildings", g_unlockAllBuildings);

			imgui->TableNextRow(0, 0.0f);
			imgui->TableSetColumnIndex(0);
			imgui->Text("Unlock All Recipes");
			Description(imgui,
				"Makes every crafting recipe available, researched or not.");
			imgui->TableSetColumnIndex(1);
			if (imgui->Checkbox("##unlock_recipes", &g_unlockAllRecipes))
				SessionConfig::Set("playerBuilding.unlockAllRecipes", g_unlockAllRecipes);

			imgui->EndTable();
		}
	}
}
