#include "dev_menus.h"

#if BETTERCHEATS_DEV_BUILD

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "aob_resolver.h"
#include "Chimera_classes.hpp"

namespace BetterCheats::Panels::DevMenus
{
	namespace
	{
		// ---------------------------------------------------------------------
		// Snapshot — written on the game thread (Tick), read on the ImGui render
		// thread. RenderImGui must never touch SDK objects itself.
		// ---------------------------------------------------------------------
		struct Snapshot
		{
			bool        controllerFound   = false;
			bool        cheatManagerFound = false;
			bool        isCrCheatManager  = false;
			bool        outerIsController = false;
			bool        worldResolved     = false;
			std::string cheatManagerClass;
			std::string outerName;
			std::string controllerCheatClass;
		};

		std::mutex g_snapshotMutex;
		Snapshot   g_snapshot;

		// The name is always a button label (a string literal), so storing the pointer
		// is safe and keeps the log readable when an action misbehaves.
		struct CheatAction
		{
			const char*                                name;
			std::function<void(SDK::UCrCheatManager*)> fn;
		};

		std::mutex               g_queueMutex;
		std::vector<CheatAction> g_queue;

		// Handled separately from the queue: the queue resolves a UCrCheatManager
		// before running anything, which is precisely what this request creates.
		std::atomic<bool> g_createCheatManagerRequested{ false };

		using InitCheatManagerFn = void(__fastcall*)(SDK::UCheatManager*);
		InitCheatManagerFn g_initCheatManager = nullptr;

		// Mirrored into the panel so the dispatch outcome is visible without going to
		// the log file - it is the one fact that separates "never ran" from "ran and
		// the game ignored it".
		std::mutex  g_statusMutex;
		std::string g_lastStatus = "(nothing dispatched yet)";

		void SetStatus(std::string text)
		{
			std::lock_guard<std::mutex> lock(g_statusMutex);
			g_lastStatus = std::move(text);
		}

		// A click enqueues one action, so a backlog this deep means Tick has stopped
		// draining (no world, or cheats disallowed) and the queue would otherwise
		// grow for as long as the panel stays open.
		constexpr size_t kMaxQueuedActions = 64;

		// Only ever called from Tick() — the engine-tick callback runs on the game
		// thread, whereas UWorld::GetWorld() and UObject access from the ImGui render
		// thread intermittently crash inside the renderer.
		SDK::ACrPlayerControllerBase* GetLocalController()
		{
			SDK::UWorld* world = nullptr;
			try { world = SDK::UWorld::GetWorld(); }
			catch (...) { return nullptr; }
			if (!world) return nullptr;

			SDK::APlayerController* pc = SDK::UGameplayStatics::GetPlayerController(world, 0);
			if (!pc) return nullptr;

			SDK::UClass* controllerClass = SDK::ACrPlayerControllerBase::StaticClass();
			if (!controllerClass || !pc->IsA(controllerClass)) return nullptr;

			return static_cast<SDK::ACrPlayerControllerBase*>(pc);
		}

		// APlayerController::CheatManager is only constructed when the game mode
		// permits cheats, and AGameModeBase::AllowCheats returns true for
		// NM_Standalone only — so this stays null in any multiplayer session.
		SDK::UCrCheatManager* GetCheatManager()
		{
			SDK::ACrPlayerControllerBase* pc = GetLocalController();
			if (!pc || !pc->CheatManager) return nullptr;

			SDK::UClass* cheatClass = SDK::UCrCheatManager::StaticClass();
			if (!cheatClass || !pc->CheatManager->IsA(cheatClass)) return nullptr;

			return static_cast<SDK::UCrCheatManager*>(pc->CheatManager);
		}

		void RefreshSnapshot()
		{
			Snapshot snap;

			try
			{
				if (SDK::ACrPlayerControllerBase* pc = GetLocalController())
				{
					snap.controllerFound = true;

					SDK::UClass* pcCheatClass = pc->CheatClass;
					snap.controllerCheatClass = pcCheatClass ? pcCheatClass->GetName() : "<null>";

					if (SDK::UObject* mgr = pc->CheatManager)
					{
						snap.cheatManagerFound = true;
						snap.cheatManagerClass = mgr->Class ? mgr->Class->GetName() : "<unknown>";
						snap.outerName         = mgr->Outer ? mgr->Outer->GetName() : "<null>";

						// Every cheat exec starts by calling GetWorld(), which UCheatManager
						// resolves as AActor::GetWorld(Outer) - so a wrong outer silently
						// turns all of them into no-ops.
						snap.outerIsController = (mgr->Outer == static_cast<SDK::UObject*>(pc));

						SDK::UClass* cheatClass = SDK::UCrCheatManager::StaticClass();
						snap.isCrCheatManager = cheatClass && mgr->IsA(cheatClass);

						try { snap.worldResolved = SDK::UWorld::GetWorld() != nullptr; }
						catch (...) { snap.worldResolved = false; }
					}
				}
			}
			catch (...)
			{
				snap = Snapshot{};
			}

			std::lock_guard<std::mutex> lock(g_snapshotMutex);
			g_snapshot = snap;
		}

		void DrainQueue()
		{
			std::vector<CheatAction> actions;
			{
				std::lock_guard<std::mutex> lock(g_queueMutex);
				if (g_queue.empty()) return;
				actions.swap(g_queue);
			}

			SDK::UCrCheatManager* mgr = nullptr;
			try { mgr = GetCheatManager(); }
			catch (...) { mgr = nullptr; }

			if (!mgr)
			{
				LOG_WARN("DevMenus: dropping %zu queued cheat action(s) - no UCrCheatManager on the local controller.",
					actions.size());
				SetStatus("DROPPED '" + std::string(actions.front().name) + "' - no UCrCheatManager resolved");
				return;
			}

			for (const CheatAction& action : actions)
			{
				// Each exec runs through ProcessEvent into native game code; one bad
				// call must not take the rest of the batch (or the tick) down with it.
				LOG_INFO("DevMenus: dispatching '%s'.", action.name);
				try
				{
					action.fn(mgr);
					LOG_INFO("DevMenus: '%s' returned.", action.name);
					SetStatus("ProcessEvent OK: '" + std::string(action.name) + "'");
				}
				catch (...)
				{
					LOG_ERROR("DevMenus: exception while running '%s'.", action.name);
					SetStatus("EXCEPTION in '" + std::string(action.name) + "'");
				}
			}
		}

		// APlayerController::EnableCheats is useless here: this build never populates
		// CheatManager on its own, so the manager has to be constructed and attached
		// by hand — the same thing the UE4SS CheatManagerEnabler mod does from Lua via
		// StaticConstructObject on ClientRestart.
		void ServiceCreateCheatManagerRequest()
		{
			if (!g_createCheatManagerRequested.exchange(false))
				return;

			try
			{
				SDK::ACrPlayerControllerBase* pc = GetLocalController();
				if (!pc)
				{
					LOG_WARN("DevMenus: no ACrPlayerControllerBase - cannot create a cheat manager.");
					return;
				}

				if (pc->CheatManager)
				{
					LOG_INFO("DevMenus: CheatManager already exists, skipping creation.");
					return;
				}

				// The Lua mod falls back to /Script/Engine.CheatManager, but that base
				// class carries none of the ~110 Chimera execs this panel drives, so
				// prefer the controller's CheatClass only while it stays in that tree.
				SDK::UClass* crCheatClass = SDK::UCrCheatManager::StaticClass();
				SDK::UClass* cheatClass   = pc->CheatClass;

				LOG_INFO("DevMenus: controller CheatClass is %s.",
					cheatClass ? cheatClass->GetName().c_str() : "<null>");

				if (!cheatClass || !crCheatClass || !cheatClass->IsSubclassOf(crCheatClass))
					cheatClass = crCheatClass;

				if (!cheatClass)
				{
					LOG_ERROR("DevMenus: could not resolve UCrCheatManager's UClass.");
					return;
				}

				SDK::UObject* created = SDK::UGameplayStatics::SpawnObject(cheatClass, pc);
				if (!created)
				{
					LOG_ERROR("DevMenus: SpawnObject returned null - cheat manager not created.");
					return;
				}

				pc->CheatManager = static_cast<SDK::UCheatManager*>(created);
				LOG_INFO("DevMenus: constructed %s (outer '%s') and attached it to the player controller.",
					cheatClass->GetName().c_str(),
					created->Outer ? created->Outer->GetName().c_str() : "<null>");

				// UCheatManager::GetWorld() resolves through Outer, so a manager whose
				// outer is not the controller leaves every cheat exec dead on arrival.
				if (created->Outer != static_cast<SDK::UObject*>(pc))
					LOG_ERROR("DevMenus: cheat manager outer is NOT the player controller - execs will not resolve a world.");

				// AddCheats runs this immediately after construction; the Lua enabler
				// skips it. The real function also broadcasts the static
				// OnCheatManagerCreatedDelegate, so prefer it and only fall back to the
				// blueprint event on its own when the scan was unusable.
				if (g_initCheatManager)
				{
					try
					{
						g_initCheatManager(pc->CheatManager);
						LOG_INFO("DevMenus: InitCheatManager completed.");
					}
					catch (...)
					{
						LOG_ERROR("DevMenus: InitCheatManager threw - the manager may be half-initialised.");
					}
				}
				else
				{
					try
					{
						pc->CheatManager->ReceiveInitCheatManager();
						LOG_INFO("DevMenus: ReceiveInitCheatManager dispatched (InitCheatManager unavailable, "
							"OnCheatManagerCreatedDelegate was NOT broadcast).");
					}
					catch (...)
					{
						LOG_WARN("DevMenus: ReceiveInitCheatManager threw - continuing anyway.");
					}
				}
			}
			catch (...)
			{
				LOG_ERROR("DevMenus: exception while creating the cheat manager.");
			}
		}

		void Enqueue(const char* name, std::function<void(SDK::UCrCheatManager*)> fn)
		{
			std::lock_guard<std::mutex> lock(g_queueMutex);
			if (g_queue.size() >= kMaxQueuedActions)
			{
				LOG_WARN("DevMenus: action queue full - discarding '%s'.", name);
				return;
			}
			g_queue.push_back(CheatAction{ name, std::move(fn) });
		}

		// ---------------------------------------------------------------------
		// Render helpers
		// ---------------------------------------------------------------------
		// Objective and flow identifiers are ASCII asset names, so a straight widening
		// is enough — no locale-aware conversion needed.
		std::wstring Widen(const char* text)
		{
			std::wstring out;
			if (!text) return out;
			for (const char* p = text; *p; ++p)
				out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*p)));
			return out;
		}

		void Tooltip(IModLoaderImGui* imgui, const char* text)
		{
			if (text && imgui->IsItemHovered())
				imgui->SetTooltip(text);
		}

		void CheatButton(IModLoaderImGui* imgui, const char* label, const char* tooltip,
			std::function<void(SDK::UCrCheatManager*)> action)
		{
			if (imgui->Button(label))
				Enqueue(label, std::move(action));
			Tooltip(imgui, tooltip);
		}

		// Exec toggles that take an "Enabled" int rather than a bool — the checkbox
		// state is UI-side only, since the game exposes no getter to read it back.
		void CheatToggleInt(IModLoaderImGui* imgui, const char* label, const char* tooltip,
			bool* state, std::function<void(SDK::UCrCheatManager*, int32_t)> action)
		{
			if (imgui->Checkbox(label, state))
			{
				const int32_t value = *state ? 1 : 0;
				Enqueue(label, [action, value](SDK::UCrCheatManager* mgr) { action(mgr, value); });
			}
			Tooltip(imgui, tooltip);
		}

		void CheatToggleBool(IModLoaderImGui* imgui, const char* label, const char* tooltip,
			bool* state, std::function<void(SDK::UCrCheatManager*, bool)> action)
		{
			if (imgui->Checkbox(label, state))
			{
				const bool value = *state;
				Enqueue(label, [action, value](SDK::UCrCheatManager* mgr) { action(mgr, value); });
			}
			Tooltip(imgui, tooltip);
		}

		// ---------------------------------------------------------------------
		// Sections
		// ---------------------------------------------------------------------
		void RenderNativeMenus(IModLoaderImGui* imgui)
		{
			imgui->SeparatorText("Native Developer Menus");

			imgui->TextWrapped(
				"These broadcast the cheat manager's menu delegates. Nothing native subscribes "
				"to them - the listener is the Blueprint side of ChimeraUI.CrUW_CheatMenu - so a "
				"manager we constructed ourselves has no subscribers and these are expected to "
				"be no-ops. Showing that menu means building the widget directly.");
			imgui->Spacing();

			CheatButton(imgui, "Open Dev Cheat Menu", "UCrCheatManager::CheatMenu - broadcasts OnCheatMenuDelegate.",
				[](SDK::UCrCheatManager* mgr) { mgr->CheatMenu(); });

			imgui->SameLine(0.0f, 8.0f);
			CheatButton(imgui, "Open Profession Selection",
				"UCrCheatManager::CheatProfessionSelectionMenu - broadcasts OnProfessionSelectionMenuDelegate.",
				[](SDK::UCrCheatManager* mgr) { mgr->CheatProfessionSelectionMenu(); });

			imgui->Spacing();

			static bool waveTimer = false;
			CheatToggleBool(imgui, "Show Wave Timer", "Broadcasts OnCheatShowWaveTimerDelegate.",
				&waveTimer, [](SDK::UCrCheatManager* mgr, bool v) { mgr->ShowWaveTimer(v); });

			static bool mapBorder = false;
			CheatToggleBool(imgui, "Show Debug Map Border Texture", "Broadcasts OnCheatShowDebugMapBorderTextureDelegate.",
				&mapBorder, [](SDK::UCrCheatManager* mgr, bool v) { mgr->ShowDebugMapBorderTexture(v); });

			static int fogValue = 1;
			imgui->SetNextItemWidth(120.0f);
			imgui->InputInt("##fog_value", &fogValue, 1, 10);
			imgui->SameLine(0.0f, 8.0f);
			CheatButton(imgui, "Clear Fog Of War", "Broadcasts OnCheatClearFogOfWarDelegate with the value on the left.",
				[value = fogValue](SDK::UCrCheatManager* mgr) { mgr->ClearFogOfWar(value); });
		}

		void RenderCameraAndView(IModLoaderImGui* imgui)
		{
			if (!imgui->CollapsingHeader("Camera & Rendering")) return;

			CheatButton(imgui, "Cycle Debug Cameras", "UCrCheatManager::CycleDebugCameras.",
				[](SDK::UCrCheatManager* mgr) { mgr->CycleDebugCameras(); });
			imgui->SameLine(0.0f, 8.0f);
			CheatButton(imgui, "Toggle Fixed Camera", "UCrCheatManager::ToggleFixedCamera.",
				[](SDK::UCrCheatManager* mgr) { mgr->ToggleFixedCamera(); });

			CheatButton(imgui, "Cycle Ability System Debug", "Steps the GAS debug HUD through its categories.",
				[](SDK::UCrCheatManager* mgr) { mgr->CycleAbilitySystemDebug(); });
			imgui->SameLine(0.0f, 8.0f);
			CheatButton(imgui, "Disable Watermark", "UCrCheatManager::DisableWatermark - clears the build watermark overlay.",
				[](SDK::UCrCheatManager* mgr) { mgr->DisableWatermark(); });

			static bool noCameraEffects = false;
			CheatToggleInt(imgui, "Disable Camera Effects", "UCrCheatManager::DebugDisableCameraEffects.",
				&noCameraEffects, [](SDK::UCrCheatManager* mgr, int32_t v) { mgr->DebugDisableCameraEffects(v); });

			static bool flashlightShadow = false;
			CheatToggleInt(imgui, "Flashlight Shadow", "UCrCheatManager::FlashlightShadow.",
				&flashlightShadow, [](SDK::UCrCheatManager* mgr, int32_t v) { mgr->FlashlightShadow(v); });
		}

		void RenderProgression(IModLoaderImGui* imgui)
		{
			if (!imgui->CollapsingHeader("Progression & Unlocks")) return;

			CheatButton(imgui, "Unlock All Features", "UCrCheatManager::CheatUnlockAllFeatures.",
				[](SDK::UCrCheatManager* mgr) { mgr->CheatUnlockAllFeatures(); });
			imgui->SameLine(0.0f, 8.0f);
			CheatButton(imgui, "Unlock All Recipes", "UCrCheatManager::CheatUnlockAllRecipes(true).",
				[](SDK::UCrCheatManager* mgr) { mgr->CheatUnlockAllRecipes(true); });

			CheatButton(imgui, "Unlock All Buildings", "UCrCheatManager::CheatUnlockAllBuildings(true).",
				[](SDK::UCrCheatManager* mgr) { mgr->CheatUnlockAllBuildings(true); });
			imgui->SameLine(0.0f, 8.0f);
			CheatButton(imgui, "Unlock Upgradeable Buildings", "UCrCheatManager::CheatUnlockAllUpgradeableBuildings.",
				[](SDK::UCrCheatManager* mgr) { mgr->CheatUnlockAllUpgradeableBuildings(); });

			CheatButton(imgui, "Unlock Post Forgotten Engine", "UCrCheatManager::CheatUnlockPostForgottenEngine.",
				[](SDK::UCrCheatManager* mgr) { mgr->CheatUnlockPostForgottenEngine(); });
			imgui->SameLine(0.0f, 8.0f);
			CheatButton(imgui, "Max All Corporations", "UCrCheatManager::CheatMaxAllCorporations.",
				[](SDK::UCrCheatManager* mgr) { mgr->CheatMaxAllCorporations(); });

			CheatButton(imgui, "Max All Skills", "UCrCheatManager::CheatMaxAllSkills.",
				[](SDK::UCrCheatManager* mgr) { mgr->CheatMaxAllSkills(); });
			imgui->SameLine(0.0f, 8.0f);
			CheatButton(imgui, "Unconditional Base Core Upgrade", "UCrCheatManager::CheatUnconditionalBaseCoreUpgrade(true).",
				[](SDK::UCrCheatManager* mgr) { mgr->CheatUnconditionalBaseCoreUpgrade(true); });

			imgui->Spacing();

			static int skillLevel = 1;
			imgui->SetNextItemWidth(120.0f);
			imgui->InputInt("##skill_level", &skillLevel, 1, 5);
			imgui->SameLine(0.0f, 8.0f);
			CheatButton(imgui, "Set All Skills Level", "UCrCheatManager::CheatSetAllSkillsLevel.",
				[value = skillLevel](SDK::UCrCheatManager* mgr) { mgr->CheatSetAllSkillsLevel(value); });

			static int dataPoints = 1000;
			imgui->SetNextItemWidth(120.0f);
			imgui->InputInt("##data_points", &dataPoints, 100, 1000);
			imgui->SameLine(0.0f, 8.0f);
			CheatButton(imgui, "Add Data Points", "UCrCheatManager::CheatAddDataPoints.",
				[value = dataPoints](SDK::UCrCheatManager* mgr) { mgr->CheatAddDataPoints(value); });
			imgui->SameLine(0.0f, 8.0f);
			CheatButton(imgui, "Set Data Points", "UCrCheatManager::CheatSetDataPoints.",
				[value = dataPoints](SDK::UCrCheatManager* mgr) { mgr->CheatSetDataPoints(value); });
		}

		void RenderFlowAndObjectives(IModLoaderImGui* imgui)
		{
			if (!imgui->CollapsingHeader("Objectives, Tutorial & Flow")) return;

			CheatButton(imgui, "Tutorial Start", "UCrCheatManager::DebugTutorialStart.",
				[](SDK::UCrCheatManager* mgr) { mgr->DebugTutorialStart(); });
			imgui->SameLine(0.0f, 8.0f);
			CheatButton(imgui, "Tutorial Skip", "UCrCheatManager::DebugTutorialSkip.",
				[](SDK::UCrCheatManager* mgr) { mgr->DebugTutorialSkip(); });

			CheatButton(imgui, "Complete Current Objective", "UCrCheatManager::ObjectivesCompleteCurrent.",
				[](SDK::UCrCheatManager* mgr) { mgr->ObjectivesCompleteCurrent(); });
			imgui->SameLine(0.0f, 8.0f);
			CheatButton(imgui, "Reset Current Objective", "UCrCheatManager::ObjectivesResetCurrent.",
				[](SDK::UCrCheatManager* mgr) { mgr->ObjectivesResetCurrent(); });

			CheatButton(imgui, "Deactivate All Objectives", "UCrCheatManager::ObjectivesDeactivateAll.",
				[](SDK::UCrCheatManager* mgr) { mgr->ObjectivesDeactivateAll(); });
			imgui->SameLine(0.0f, 8.0f);
			CheatButton(imgui, "Skip Next Loading Screen", "UCrCheatManager::SkipNextLoadingScreen.",
				[](SDK::UCrCheatManager* mgr) { mgr->SkipNextLoadingScreen(); });

			CheatButton(imgui, "Run FE Main Engine", "UCrCheatManager::RunFEMainEngine.",
				[](SDK::UCrCheatManager* mgr) { mgr->RunFEMainEngine(); });

			imgui->Spacing();

			static char objectiveName[128] = {};
			imgui->SetNextItemWidth(220.0f);
			imgui->InputText("##objective_name", objectiveName, sizeof(objectiveName));
			imgui->SameLine(0.0f, 8.0f);
			CheatButton(imgui, "Activate Objective", "UCrCheatManager::ObjectivesActivate with the name on the left.",
				[name = Widen(objectiveName)](SDK::UCrCheatManager* mgr)
				{
					if (name.empty()) return;
					// SDK::FString aliases the buffer it is handed rather than copying,
					// so the captured string has to outlive the ProcessEvent call.
					SDK::FString arg(name.c_str());
					mgr->ObjectivesActivate(arg);
				});

			static char flowName[128] = {};
			imgui->SetNextItemWidth(220.0f);
			imgui->InputText("##flow_name", flowName, sizeof(flowName));
			imgui->SameLine(0.0f, 8.0f);
			CheatButton(imgui, "Exec Flow", "UCrCheatManager::DebugExecFlow with the name on the left.",
				[name = Widen(flowName)](SDK::UCrCheatManager* mgr)
				{
					if (name.empty()) return;
					SDK::FString arg(name.c_str());
					mgr->DebugExecFlow(arg);
				});
		}

		void RenderDebugToggles(IModLoaderImGui* imgui)
		{
			if (!imgui->CollapsingHeader("Debug Toggles & Visualisation")) return;

			CheatButton(imgui, "Toggle Player Progression EXP Info", "UCrCheatManager::DebugTogglePlayerProgressionExpInfo.",
				[](SDK::UCrCheatManager* mgr) { mgr->DebugTogglePlayerProgressionExpInfo(); });
			CheatButton(imgui, "Toggle Co-op HUD Health Info", "UCrCheatManager::DebugToggleCoopHudHealthInfo.",
				[](SDK::UCrCheatManager* mgr) { mgr->DebugToggleCoopHudHealthInfo(); });
			CheatButton(imgui, "Toggle Harvester No Damage", "UCrCheatManager::DebugToggleHarvesterNoDamage.",
				[](SDK::UCrCheatManager* mgr) { mgr->DebugToggleHarvesterNoDamage(); });
			CheatButton(imgui, "Toggle Mining Resource Durations", "UCrCheatManager::DebugToggleShowMiningResourceDurations.",
				[](SDK::UCrCheatManager* mgr) { mgr->DebugToggleShowMiningResourceDurations(); });
			CheatButton(imgui, "Toggle Gatherable Plant Visualisation", "UCrCheatManager::DebugToggleGatherablePlantRegularVisualisation.",
				[](SDK::UCrCheatManager* mgr) { mgr->DebugToggleGatherablePlantRegularVisualisation(); });
			CheatButton(imgui, "Toggle PCG Gatherables Generation", "UCrCheatManager::DebugTogglePCGGatherablesGenerationEnabled.",
				[](SDK::UCrCheatManager* mgr) { mgr->DebugTogglePCGGatherablesGenerationEnabled(); });
			CheatButton(imgui, "Toggle Single-Bullet Reload Debug", "UCrCheatManager::DebugToggleReloadSingleBulletPrintDebugInfo.",
				[](SDK::UCrCheatManager* mgr) { mgr->DebugToggleReloadSingleBulletPrintDebugInfo(); });
			CheatButton(imgui, "Show Zipline Connections", "UCrCheatManager::CheatShowZiplineConnections.",
				[](SDK::UCrCheatManager* mgr) { mgr->CheatShowZiplineConnections(); });

			imgui->Spacing();

			static bool baseCoreRange = false;
			CheatToggleBool(imgui, "Show Base Core Range", "UCrCheatManager::CheatShowBaseCoreRange.",
				&baseCoreRange, [](SDK::UCrCheatManager* mgr, bool v) { mgr->CheatShowBaseCoreRange(v); });

			static bool hints = false;
			CheatToggleInt(imgui, "Hints", "UCrCheatManager::Hints.",
				&hints, [](SDK::UCrCheatManager* mgr, int32_t v) { mgr->Hints(v); });

			static bool blockProgressionNotify = false;
			CheatToggleBool(imgui, "Block Progression Notifications", "UCrCheatManager::CheatSetBlockProgressionNotification.",
				&blockProgressionNotify, [](SDK::UCrCheatManager* mgr, bool v) { mgr->CheatSetBlockProgressionNotification(v); });

			static bool ignoreBaseCore = false;
			CheatToggleBool(imgui, "Ignore Base Core", "UCrCheatManager::CheatIgnoreBaseCore.",
				&ignoreBaseCore, [](SDK::UCrCheatManager* mgr, bool v) { mgr->CheatIgnoreBaseCore(v); });

			static bool ignoreBuildCost = false;
			CheatToggleInt(imgui, "Ignore Building Cost", "UCrCheatManager::CheatIgnoreBuildingCost.",
				&ignoreBuildCost, [](SDK::UCrCheatManager* mgr, int32_t v) { mgr->CheatIgnoreBuildingCost(v); });
		}

		void RenderWaveAndWorld(IModLoaderImGui* imgui)
		{
			if (!imgui->CollapsingHeader("Heat Wave & World")) return;

			CheatButton(imgui, "Start Heat Wave", "UCrCheatManager::CheatStartHeatWave.",
				[](SDK::UCrCheatManager* mgr) { mgr->CheatStartHeatWave(); });
			imgui->SameLine(0.0f, 8.0f);
			CheatButton(imgui, "Start Pre-Heat Wave", "UCrCheatManager::CheatStartPreHeatWave.",
				[](SDK::UCrCheatManager* mgr) { mgr->CheatStartPreHeatWave(); });
			imgui->SameLine(0.0f, 8.0f);
			CheatButton(imgui, "Pause Heat Wave", "UCrCheatManager::CheatPauseHeatWave.",
				[](SDK::UCrCheatManager* mgr) { mgr->CheatPauseHeatWave(); });

			CheatButton(imgui, "Uncover Map POIs", "UCrCheatManager::MapMenuUncoverPOI.",
				[](SDK::UCrCheatManager* mgr) { mgr->MapMenuUncoverPOI(); });

			imgui->Spacing();

			static int radiationBorders = 0;
			imgui->SetNextItemWidth(120.0f);
			imgui->InputInt("##radiation_borders", &radiationBorders, 1, 5);
			imgui->SameLine(0.0f, 8.0f);
			CheatButton(imgui, "Set Radiation Borders Level", "UCrCheatManager::SetRadiationBordersLevel.",
				[value = radiationBorders](SDK::UCrCheatManager* mgr) { mgr->SetRadiationBordersLevel(value); });
		}

		void RenderSelf(IModLoaderImGui* imgui)
		{
			if (!imgui->CollapsingHeader("Self & Loadout")) return;

			CheatButton(imgui, "Give Default Weapons", "UCrCheatManager::CheatGiveDefaultWeapons.",
				[](SDK::UCrCheatManager* mgr) { mgr->CheatGiveDefaultWeapons(); });
			imgui->SameLine(0.0f, 8.0f);
			CheatButton(imgui, "Quick Add Items", "UCrCheatManager::CheatQuickAddItems.",
				[](SDK::UCrCheatManager* mgr) { mgr->CheatQuickAddItems(); });
			imgui->SameLine(0.0f, 8.0f);
			CheatButton(imgui, "Quick Add Ammo", "UCrCheatManager::CheatQuickAddAmmo.",
				[](SDK::UCrCheatManager* mgr) { mgr->CheatQuickAddAmmo(); });

			CheatButton(imgui, "Immortal", "UCrCheatManager::Immortal.",
				[](SDK::UCrCheatManager* mgr) { mgr->Immortal(); });
			imgui->SameLine(0.0f, 8.0f);
			CheatButton(imgui, "Has All Keys", "UCrCheatManager::HasAllKeys.",
				[](SDK::UCrCheatManager* mgr) { mgr->HasAllKeys(); });
			imgui->SameLine(0.0f, 8.0f);
			CheatButton(imgui, "Cancel Activated Abilities", "UCrCheatManager::CancelActivatedAbilities.",
				[](SDK::UCrCheatManager* mgr) { mgr->CancelActivatedAbilities(); });

			imgui->Spacing();

			static bool unlimitedHealth = false;
			CheatToggleInt(imgui, "Unlimited Health", nullptr, &unlimitedHealth,
				[](SDK::UCrCheatManager* mgr, int32_t v) { mgr->UnlimitedHealth(v); });
			static bool unlimitedShield = false;
			CheatToggleInt(imgui, "Unlimited Shield", nullptr, &unlimitedShield,
				[](SDK::UCrCheatManager* mgr, int32_t v) { mgr->UnlimitedShield(v); });
			static bool unlimitedAmmo = false;
			CheatToggleInt(imgui, "Unlimited Ammo", nullptr, &unlimitedAmmo,
				[](SDK::UCrCheatManager* mgr, int32_t v) { mgr->UnlimitedAmmo(v); });
			static bool unlimitedGrenades = false;
			CheatToggleInt(imgui, "Unlimited Grenades", nullptr, &unlimitedGrenades,
				[](SDK::UCrCheatManager* mgr, int32_t v) { mgr->UnlimitedGrenades(v); });
			static bool unlimitedEnergy = false;
			CheatToggleInt(imgui, "Unlimited Energy", nullptr, &unlimitedEnergy,
				[](SDK::UCrCheatManager* mgr, int32_t v) { mgr->UnlimitedEnergy(v); });
			static bool unlimitedOxygen = false;
			CheatToggleInt(imgui, "Unlimited Oxygen", nullptr, &unlimitedOxygen,
				[](SDK::UCrCheatManager* mgr, int32_t v) { mgr->UnlimitedOxygen(v); });
			static bool unlimitedCalories = false;
			CheatToggleInt(imgui, "Unlimited Calories", nullptr, &unlimitedCalories,
				[](SDK::UCrCheatManager* mgr, int32_t v) { mgr->UnlimitedCalories(v); });
			static bool unlimitedHydration = false;
			CheatToggleInt(imgui, "Unlimited Hydration", nullptr, &unlimitedHydration,
				[](SDK::UCrCheatManager* mgr, int32_t v) { mgr->UnlimitedHydration(v); });
			static bool unlimitedMedTool = false;
			CheatToggleInt(imgui, "Unlimited Med Tool", nullptr, &unlimitedMedTool,
				[](SDK::UCrCheatManager* mgr, int32_t v) { mgr->UnlimitedMedTool(v); });
			static bool unlimitedWeaponHeat = false;
			CheatToggleInt(imgui, "Unlimited Weapon Heat", nullptr, &unlimitedWeaponHeat,
				[](SDK::UCrCheatManager* mgr, int32_t v) { mgr->UnlimitedWeaponHeat(v); });
			static bool superSpeed = false;
			CheatToggleInt(imgui, "Super Speed", "UCrCheatManager::CheatSuperSpeed.", &superSpeed,
				[](SDK::UCrCheatManager* mgr, int32_t v) { mgr->CheatSuperSpeed(v); });
		}
	}

	void Initialize()
	{
		// Resolved (and ambiguity-checked) in the load-hooks event; unresolved means
		// the panel falls back to ReceiveInitCheatManager only.
		if (uintptr_t address = AOB::Resolved().CheatManager_InitCheatManager)
			g_initCheatManager = reinterpret_cast<InitCheatManagerFn>(address);
	}

	void Shutdown()
	{
		std::lock_guard<std::mutex> lock(g_queueMutex);
		g_queue.clear();
	}

	void Tick(float /*deltaSeconds*/)
	{
		ServiceCreateCheatManagerRequest();
		DrainQueue();
		RefreshSnapshot();
	}

	void RenderImGui(IModLoaderImGui* imgui)
	{
		Snapshot snap;
		{
			std::lock_guard<std::mutex> lock(g_snapshotMutex);
			snap = g_snapshot;
		}

		imgui->TextColored(1.0f, 0.75f, 0.2f, 1.0f, "DEBUG BUILD ONLY - the game's own developer cheat suite.");
		imgui->Spacing();

		imgui->SeparatorText("Cheat Manager");

		if (!snap.controllerFound)
		{
			imgui->TextDisabled("No ACrPlayerControllerBase yet.");
			return;
		}

		if (!snap.cheatManagerFound)
		{
			imgui->TextColored(1.0f, 0.4f, 0.4f, 1.0f, "PlayerController->CheatManager is null.");
			imgui->TextWrapped(
				"This build never populates it on its own. Construct one and attach it to "
				"the local player controller to unlock the cheat execs below.");
			imgui->Spacing();
			if (imgui->Button("Create Cheat Manager"))
			{
				LOG_INFO("DevMenus: requesting cheat manager creation.");
				g_createCheatManagerRequested.store(true);
			}
			Tooltip(imgui, "Constructs a UCrCheatManager via UGameplayStatics::SpawnObject and assigns it to PlayerController->CheatManager.");
			return;
		}

		imgui->LabelText("Class", snap.cheatManagerClass.c_str());
		imgui->LabelText("Outer", snap.outerName.c_str());
		imgui->LabelText("PC CheatClass", snap.controllerCheatClass.c_str());

		if (!snap.outerIsController)
		{
			imgui->TextColored(1.0f, 0.4f, 0.4f, 1.0f,
				"Outer is not the player controller - GetWorld() will fail and every exec is a no-op.");
		}
		if (!snap.worldResolved)
		{
			imgui->TextColored(1.0f, 0.4f, 0.4f, 1.0f, "No world resolved.");
		}

		if (!snap.isCrCheatManager)
		{
			imgui->TextColored(1.0f, 0.4f, 0.4f, 1.0f,
				"Not a UCrCheatManager - the Chimera cheat execs are unavailable.");
			return;
		}

		{
			std::string status;
			{
				std::lock_guard<std::mutex> lock(g_statusMutex);
				status = g_lastStatus;
			}
			imgui->LabelText("Last dispatch", status.c_str());
		}

		imgui->Spacing();
		imgui->SeparatorText("Probes");
		imgui->TextWrapped(
			"These two are the only cheats whose full native body is visible in the IDA dump, "
			"so they are the honest test of whether dispatch works. SetRadiationBordersLevel "
			"writes a LogCrCheat warning when its subsystem is missing, so it produces evidence "
			"either way.");
		imgui->Spacing();

		static int probeRadiation = 1;
		imgui->SetNextItemWidth(120.0f);
		imgui->InputInt("##probe_radiation", &probeRadiation, 1, 5);
		imgui->SameLine(0.0f, 8.0f);
		CheatButton(imgui, "Probe: SetRadiationBordersLevel",
			"Confirmed real body: resolves UCrRadiationBordersSubsystem and calls SetRadiationLevel.",
			[value = probeRadiation](SDK::UCrCheatManager* mgr) { mgr->SetRadiationBordersLevel(value); });

		static bool probeHints = false;
		CheatToggleInt(imgui, "Probe: Hints",
			"Confirmed real body: resolves UCrHintsSubsystem.",
			&probeHints, [](SDK::UCrCheatManager* mgr, int32_t v) { mgr->Hints(v); });

		imgui->Spacing();
		RenderNativeMenus(imgui);
		imgui->Spacing();
		RenderCameraAndView(imgui);
		RenderProgression(imgui);
		RenderFlowAndObjectives(imgui);
		RenderDebugToggles(imgui);
		RenderWaveAndWorld(imgui);
		RenderSelf(imgui);
	}
}

#endif // BETTERCHEATS_DEV_BUILD
