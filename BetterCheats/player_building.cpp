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
		// Build Windows Anywhere
		// A habitat's built-in machines (Item Printer, Food Processor and the rest)
		// are not custom buildings, so they are never registered on the wall slot
		// they fill: UCrBuildingStabilitySubsystem::IsSocketFree still reports the
		// slot free and the window snaps into it. What turns the ghost red is the
		// collision verdict, from the main mesh overlap or the box pass after it,
		// because the machine is neither snapped to nor connected to the window.
		//
		// While a window sits snapped to a habitat or the Hub, BuildingColliding
		// and IsColliding from those checks count as Valid. Both overlap passes
		// rank BuildingColliding above creatures, other characters, balloons,
		// plants and actors they count as invalid, so whenever a building is in
		// the window's volume, those are let through with it. Still refused:
		// players and drones in the collision box (the box pass ranks them above
		// buildings), a slot that holds a window, airlock or bridge
		// (IsSocketFree, before this), and a window not snapped to a habitat or
		// the Hub. Stability, sockets, base-core areas, terrain and
		// resources keep their own verdicts.
		//
		// The placing client decides placement, so this covers your own
		// placements in any world, including as a client, and nobody else's.
		// -------------------------------------------------------------------------

		using CheckMainMeshCollisionFn = uint8_t(__fastcall*)(SDK::ACrAPHelper* self, const void* placementData);
		using GetBoxCollisionResultFn  = uint8_t(__fastcall*)(SDK::ACrAPHelper* self, const void* min, const void* max,
			const void* ignoredActors, const void* ignoredClasses, bool flag);

		constexpr uint8_t kPlacementValid    = 1;  // EAuAPlacementConditionResult::Valid
		constexpr uint8_t kIsColliding       = 40; // EAuAPlacementConditionResult::IsColliding
		constexpr uint8_t kBuildingColliding = 43; // EAuAPlacementConditionResult::BuildingColliding

		CheckMainMeshCollisionFn g_originalCheckMainMeshCollision = nullptr;
		GetBoxCollisionResultFn  g_originalGetBoxCollisionResult  = nullptr;
		HookHandle               g_hookCheckMainMeshCollision     = nullptr;
		HookHandle               g_hookGetBoxCollisionResult      = nullptr;
		std::atomic<bool>        g_buildWindowsAnywhere{ false };

		std::atomic<bool>        g_buildBenchesAnywhere{ false };

		// The habitat machines, placed into wall slots like windows but not
		// custom buildings. Personal storage is a module too, but it is a
		// custom building and registers on the slot, so it is left out.
		bool IsBenchId(SDK::ECrBuildingID id)
		{
			switch (id)
			{
			case SDK::ECrBuildingID::Analyzer:
			case SDK::ECrBuildingID::Armory:
			case SDK::ECrBuildingID::CloningBed:
			case SDK::ECrBuildingID::FoodProcessor:
			case SDK::ECrBuildingID::ItemPrinter:
			case SDK::ECrBuildingID::RecipeTable:
			case SDK::ECrBuildingID::Recycler:
			case SDK::ECrBuildingID::ResearchTerminal:
			case SDK::ECrBuildingID::SuitWorkshop:
			case SDK::ECrBuildingID::UpgradeStation:
				return true;
			default:
				return false;
			}
		}

		bool IsHabitat(SDK::AActor* actor)
		{
			SDK::ECrBuildingID id{};
			return TryGetBuildingId(actor, id)
			    && (id == SDK::ECrBuildingID::HabitatBig || id == SDK::ECrBuildingID::HabitatSmall
			        || id == SDK::ECrBuildingID::Hub);
		}

		bool IsRelaxedPiece(SDK::ECrBuildingID id)
		{
			return (IsWindowId(id) && g_buildWindowsAnywhere.load())
			    || (IsBenchId(id) && g_buildBenchesAnywhere.load());
		}

		bool IsRelaxedPieceInHabitatSlot(SDK::ACrAPHelper* helper)
		{
			if (!helper || !helper->BuildingData || !IsRelaxedPiece(helper->BuildingData->BuildingID))
				return false;

			for (SDK::AActor* snapped : helper->SnappedActors)
			{
				if (IsHabitat(snapped))
					return true;
			}
			return false;
		}

		// Shared by both checks. The box pass never returns IsColliding, so
		// only BuildingColliding matters there.
		uint8_t RelaxForHabitatPiece(SDK::ACrAPHelper* helper, uint8_t result)
		{
			if ((result != kBuildingColliding && result != kIsColliding)
			    || (!g_buildWindowsAnywhere.load() && !g_buildBenchesAnywhere.load()))
				return result;

			try
			{
				return IsRelaxedPieceInHabitatSlot(helper) ? kPlacementValid : result;
			}
			catch (...)
			{
				return result;
			}
		}

		uint8_t __fastcall Detour_CheckMainMeshCollision(SDK::ACrAPHelper* self, const void* placementData)
		{
			return RelaxForHabitatPiece(self, g_originalCheckMainMeshCollision(self, placementData));
		}

		uint8_t __fastcall Detour_GetBoxCollisionResult(SDK::ACrAPHelper* self, const void* min, const void* max,
			const void* ignoredActors, const void* ignoredClasses, bool flag)
		{
			return RelaxForHabitatPiece(self, g_originalGetBoxCollisionResult(self, min, max, ignoredActors, ignoredClasses, flag));
		}

		void RemoveWindowsAnywhereHooks(IPluginHookUtils* hooks)
		{
			for (HookHandle* hook : { &g_hookGetBoxCollisionResult, &g_hookCheckMainMeshCollision })
			{
				if (*hook)
					hooks->Remove(*hook);
				*hook = nullptr;
			}
			g_originalGetBoxCollisionResult  = nullptr;
			g_originalCheckMainMeshCollision = nullptr;
		}

		// -------------------------------------------------------------------------
		// Build Benches Anywhere
		// A bench (the habitat machines in IsBenchId) uses the same wall-slot
		// placement as a window. Next to a window, or overlapping a wall piece,
		// it is refused by the same two collision checks, which relax it above.
		// In a slot that already holds a window it never gets that far: the
		// window is a custom building registered on the slot, so IsSocketFree
		// reports the slot taken and the bench does not snap.
		//
		// So while a bench is being snapped to a habitat or the Hub, an occupied
		// slot counts as free. IsSocketFree only records that something holds
		// the slot, not what, so this also lets a bench into a slot holding an
		// airlock, bridge or personal storage. Everything after the snap keeps
		// the collision rules above.
		// -------------------------------------------------------------------------

		using GetSnappedSocketFn = void*(__fastcall*)(void* self, const SDK::UAuActorPlacementData* data, void* a3, void* a4,
			void* a5, void* a6, void* a7, void* a8, void* a9, void* a10);
		using IsSocketFreeFn = bool(__fastcall*)(void* self, SDK::AActor* building, const void* socket);

		GetSnappedSocketFn g_originalGetSnappedSocket = nullptr;
		IsSocketFreeFn     g_originalIsSocketFree     = nullptr;
		HookHandle         g_hookGetSnappedSocket     = nullptr;
		HookHandle         g_hookIsSocketFree         = nullptr;

		// IsSocketFree's only caller is GetSnappedSocket, which runs inside the
		// placement helper's tick.
		thread_local bool t_snappingBench = false;

		void* __fastcall Detour_GetSnappedSocket(void* self, const SDK::UAuActorPlacementData* data, void* a3, void* a4,
			void* a5, void* a6, void* a7, void* a8, void* a9, void* a10)
		{
			const bool bench = g_buildBenchesAnywhere.load() && data && IsBenchId(data->BuildingID);
			t_snappingBench = bench;
			void* socket = g_originalGetSnappedSocket(self, data, a3, a4, a5, a6, a7, a8, a9, a10);
			t_snappingBench = false;
			return socket;
		}

		bool __fastcall Detour_IsSocketFree(void* self, SDK::AActor* building, const void* socket)
		{
			const bool isFree = g_originalIsSocketFree(self, building, socket);
			if (isFree || !t_snappingBench)
				return isFree;

			try
			{
				return IsHabitat(building);
			}
			catch (...)
			{
				return false;
			}
		}

		void RemoveBenchesAnywhereHooks(IPluginHookUtils* hooks)
		{
			for (HookHandle* hook : { &g_hookIsSocketFree, &g_hookGetSnappedSocket })
			{
				if (*hook)
					hooks->Remove(*hook);
				*hook = nullptr;
			}
			g_originalIsSocketFree     = nullptr;
			g_originalGetSnappedSocket = nullptr;
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

		// Both hooks or neither: one alone would relax only half the verdict.
		if (aob.CheckMainMeshCollision_Custom)
		{
			g_hookCheckMainMeshCollision = hooks->Install(
				aob.CheckMainMeshCollision_Custom,
				reinterpret_cast<void*>(&Detour_CheckMainMeshCollision),
				reinterpret_cast<void**>(&g_originalCheckMainMeshCollision));
			g_hookGetBoxCollisionResult = hooks->Install(
				aob.GetBoxCollisionResult,
				reinterpret_cast<void*>(&Detour_GetBoxCollisionResult),
				reinterpret_cast<void**>(&g_originalGetBoxCollisionResult));

			if (g_hookCheckMainMeshCollision && g_hookGetBoxCollisionResult)
			{
				LOG_INFO("Building: CheckMainMeshCollision and GetBoxCollisionResult hooks installed");
			}
			else
			{
				RemoveWindowsAnywhereHooks(hooks);
				LOG_WARN("Building: failed to install the collision hooks - Build Windows Anywhere and Build Benches Anywhere will do nothing");
			}
		}

		// Benches need the collision hooks as well as these two.
		if (g_hookCheckMainMeshCollision && aob.GetSnappedSocket)
		{
			g_hookGetSnappedSocket = hooks->Install(
				aob.GetSnappedSocket,
				reinterpret_cast<void*>(&Detour_GetSnappedSocket),
				reinterpret_cast<void**>(&g_originalGetSnappedSocket));
			g_hookIsSocketFree = hooks->Install(
				aob.IsSocketFree,
				reinterpret_cast<void*>(&Detour_IsSocketFree),
				reinterpret_cast<void**>(&g_originalIsSocketFree));

			if (g_hookGetSnappedSocket && g_hookIsSocketFree)
			{
				LOG_INFO("Building: GetSnappedSocket and IsSocketFree hooks installed");
			}
			else
			{
				RemoveBenchesAnywhereHooks(hooks);
				LOG_WARN("Building: failed to install the socket hooks - Build Benches Anywhere will do nothing");
			}
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

		if (hooks)
		{
			RemoveBenchesAnywhereHooks(hooks);
			RemoveWindowsAnywhereHooks(hooks);
		}
		g_buildWindowsAnywhere = false;
		g_buildBenchesAnywhere = false;

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
		g_buildWindowsAnywhere          = SessionConfig::Get("playerBuilding.buildWindowsAnywhere", false);
		g_buildBenchesAnywhere          = SessionConfig::Get("playerBuilding.buildBenchesAnywhere", false);

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

			imgui->TableNextRow(0, 0.0f);
			imgui->TableSetColumnIndex(0);
			imgui->Text("Build Windows Anywhere");
			Description(imgui,
				"Lets a window go into a habitat or Hub wall slot even when a machine or anything else is in it, "
				"except players and drones in its collision box. "
				"Applies to your own placements, also as a client. Other pieces keep the normal rules.");
			imgui->TableSetColumnIndex(1);
			bool buildWindowsAnywhere = g_buildWindowsAnywhere.load();
			if (imgui->Checkbox("##build_windows_anywhere", &buildWindowsAnywhere))
			{
				g_buildWindowsAnywhere = buildWindowsAnywhere;
				SessionConfig::Set("playerBuilding.buildWindowsAnywhere", buildWindowsAnywhere);
			}

			imgui->TableNextRow(0, 0.0f);
			imgui->TableSetColumnIndex(0);
			imgui->Text("Build Benches Anywhere");
			Description(imgui,
				"Lets a habitat machine (Item Printer, Food Processor, Suit Workshop and the rest) go into a habitat or Hub "
				"wall slot even when a window, another piece or anything else is in it, except players and drones in its collision box. "
				"Applies to your own placements, also as a client. Other pieces keep the normal rules.");
			imgui->TableSetColumnIndex(1);
			bool buildBenchesAnywhere = g_buildBenchesAnywhere.load();
			if (imgui->Checkbox("##build_benches_anywhere", &buildBenchesAnywhere))
			{
				g_buildBenchesAnywhere = buildBenchesAnywhere;
				SessionConfig::Set("playerBuilding.buildBenchesAnywhere", buildBenchesAnywhere);
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
