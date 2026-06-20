#include "enemies.h"
#include "plugin_helpers.h"
#include "aob_patterns.h"
#include "session_config.h"

#include "Chimera_classes.hpp"
#include "MassAIPrototypeEnemyRuntime_classes.hpp"
#include "MassActors_classes.hpp"
#include "MassEntity_classes.hpp"
#include "MassCommon_structs.hpp"
#include "AIModule_classes.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace BetterCheats::Panels::Enemies
{
	namespace
	{
		std::atomic<bool> g_disableAI{ false };
		std::atomic<bool> g_oneHitKill{ false };

		// Minimal view over the native FMassEnemyHealthFragment layout (confirmed via
		// PDB-backed IDA inspection — Dumper-7 can't reflect it since HP isn't a
		// UPROPERTY, so the SDK header pads it out as unnamed bytes). Only the first
		// field is needed here, so this stands in for the full struct.
		struct FMassEnemyHealthFragmentView
		{
			float HP;
		};

		// Pairs each tracked enemy actor with its UMassAgentComponent — the official
		// handle the One-Hit Kill fallback uses (KillEntity), found via
		// AActor::GetComponentByClass since AMassEnemyCharacterBase doesn't expose it
		// as a named field the way ACrCharacterPlayerBase does. Not every enemy
		// subclass has one attached, so it's only ever a fallback now — see
		// ResolveHealthFragment for the universal actor->entity path.
		struct TrackedEnemy
		{
			SDK::AMassEnemyCharacterBase* actor = nullptr;
			SDK::UMassAgentComponent*     agent = nullptr;
		};

		// Enemies are Mass Entity actors that pool in/out of existence constantly —
		// unlike the singleton subsystems other panels scan for, this list needs a
		// periodic re-scan rather than a one-shot lookup on world load.
		float                          g_rescanTimer = 0.0f;
		std::vector<TrackedEnemy>      g_enemies;

		// Tracks which actors we've already called UMassAgentComponent::KillEntity on,
		// so One-Hit Kill fires exactly once per enemy instead of every tick. Cleared
		// on Shutdown — fine to hold stale pointers briefly, every use is try/catch'd
		// and the GObjects rescan naturally drops dead/freed actors from g_enemies.
		std::unordered_set<SDK::AMassEnemyCharacterBase*> g_killedEnemies;

		// Edge-detected so Disable()/Enable() (which likely does real work — removing
		// or re-adding the entity from Mass simulation/replication) only fires once
		// per state change, not every engine tick.
		bool g_lastDisableAI = false;

		// -------------------------------------------------------------------------
		// One-Hit Kill — fragment-level health pin.
		//
		// Pins each tracked enemy's FMassEnemyHealthFragment::HP to 1 whenever it's
		// above 1, instead of calling KillEntity() outright — this way any incoming
		// damage (even chip damage) finishes them in a single hit, rather than the
		// plugin insta-killing them itself. Resolved via
		// (actor -> TObjectKey -> FMassEntityHandle -> fragment pointer); see the
		// AOB-resolved functions below. KillEntity() is kept as a fallback for
		// enemies whose fragment hasn't resolved yet.
		//
		// UMassReplicationSubsystem::FindEntity (NetID-based) was tried first, but
		// only finds entities actually being replicated to a remote client — in
		// single-player the local instance is the authority, so most enemies'
		// NetID (AMassEnemyCharacterBase::CurrentOwnerEntity) is never populated
		// and FindEntity always returns Index=0. UMassActorSubsystem::
		// GetEntityHandleFromActor is the authority-side equivalent and works
		// regardless of replication state.
		// -------------------------------------------------------------------------
		// FObjectPtr is passed by pointer here, not by value — the function does
		// `mov rdx, [rdx]` as its first step (confirmed via raw disassembly), so
		// the second parameter must be the address of a variable holding the
		// actor pointer, not the actor pointer's own value.
		using WeakObjectPtrAssignFn = void(__fastcall*)(SDK::FWeakObjectPtr* self, void* const* objectPtrAddr);
		using GetEntityHandleFromActorFn = SDK::FMassEntityHandle*(__fastcall*)(
			SDK::UMassActorSubsystem* self, SDK::FMassEntityHandle* result, SDK::FWeakObjectPtr actorKey);
		using InternalGetFragmentDataPtrFn = void*(__fastcall*)(
			void* entityManager, SDK::FMassEntityHandle entity, const SDK::UScriptStruct* fragmentType);

		WeakObjectPtrAssignFn        g_buildActorKey      = nullptr;
		GetEntityHandleFromActorFn   g_getEntityHandle    = nullptr;
		InternalGetFragmentDataPtrFn g_getFragmentDataPtr = nullptr;
		SDK::UScriptStruct*          g_healthFragmentStruct = nullptr;

		// Re-resolved every rescan (cheap — both are per-world singletons) so a world
		// reload doesn't leave these dangling.
		SDK::UMassEntitySubsystem* g_entitySubsystem = nullptr;
		SDK::UMassActorSubsystem*  g_actorSubsystem   = nullptr;
		void*                      g_entityManager    = nullptr;

		// Disable AI: UMassAgentComponent::Disable() alone has no visible effect (it's
		// native and does run — see plugin.log — but apparently only affects
		// replication/pooling bookkeeping, not the Mass simulation's actual movement/
		// attack logic). Instead: stop the AI controller's behaviour tree ticking, and
		// zero the attack-range fields so even if it keeps approaching it can never
		// register as "in range" to attack. Original values restored on toggle-off.
		struct AttackRangeCache
		{
			float distance         = 0.0f;
			float coneHalfAngle    = 0.0f;
			float buildingDistance = 0.0f;
		};
		std::unordered_map<SDK::AMassEnemyCharacterBase*, AttackRangeCache> g_attackRangeCache;

		struct EnemiesSnapshot { int count = 0; };
		std::mutex        g_snapshotMutex;
		EnemiesSnapshot   g_snapshot;

		FMassEnemyHealthFragmentView* ResolveHealthFragment(const TrackedEnemy& enemy);

		// Re-finds the per-world UMassEntitySubsystem/UMassActorSubsystem singletons
		// and re-reads the FMassEntityManager pointer cached inside the entity
		// subsystem (TSharedPtr<FMassEntityManager> at +0x38 — see
		// UMassEntitySubsystem's layout; Dumper-7 can't reflect it since
		// FMassEntityManager isn't a UObject, so this offset came from a PDB-backed
		// IDA inspection, not the SDK header). Only ever called from Tick().
		void RescanMassSubsystems()
		{
			g_entitySubsystem = nullptr;
			g_actorSubsystem  = nullptr;
			g_entityManager   = nullptr;

			SDK::TUObjectArray* arr = SDK::UObject::GObjects.GetTypedPtr();
			if (!arr)
				return;

			SDK::UClass* entityClass = SDK::UMassEntitySubsystem::StaticClass();
			SDK::UClass* actorClass  = SDK::UMassActorSubsystem::StaticClass();
			if (!entityClass || !actorClass)
				return;

			const SDK::UObject* entityCDO = SDK::UMassEntitySubsystem::GetDefaultObj();
			const SDK::UObject* actorCDO  = SDK::UMassActorSubsystem::GetDefaultObj();

			bool doneEntity = false, doneActor = false;

			// Pointer comparisons against cached UClass* (like RescanEnemies' IsA
			// check) instead of obj->Class->GetName() string comparisons — GetName()
			// on every entry of GObjects (hundreds of thousands of objects) was the
			// actual source of the freeze-induced framerate drop, not enemy count.
			for (int i = 0; i < arr->Num() && (!doneEntity || !doneActor); ++i)
			{
				SDK::UObject* obj = arr->GetByIndex(i);
				if (!obj || !obj->Class) continue;

				if (!doneEntity && obj != entityCDO && obj->Class == entityClass)
				{
					g_entitySubsystem = static_cast<SDK::UMassEntitySubsystem*>(obj);
					doneEntity = true;
					continue;
				}
				if (!doneActor && obj != actorCDO && obj->Class == actorClass)
				{
					g_actorSubsystem = static_cast<SDK::UMassActorSubsystem*>(obj);
					doneActor = true;
					continue;
				}
			}

			if (g_entitySubsystem)
			{
				auto* base = reinterpret_cast<uint8_t*>(g_entitySubsystem);
				g_entityManager = *reinterpret_cast<void**>(base + 0x38);
			}
		}

		// Only ever called from Tick() (the game thread) — never from RenderImGui().
		void RescanEnemies()
		{
			g_enemies.clear();

			SDK::TUObjectArray* arr = SDK::UObject::GObjects.GetTypedPtr();
			if (!arr)
				return;

			SDK::UClass* enemyClass = SDK::AMassEnemyCharacterBase::StaticClass();
			SDK::UClass* agentClass = SDK::UMassAgentComponent::StaticClass();
			if (!enemyClass)
				return;

			const bool disableAI = g_disableAI.load();

			for (int i = 0; i < arr->Num(); ++i)
			{
				SDK::UObject* obj = arr->GetByIndex(i);
				if (!obj || !obj->Class || !obj->IsA(enemyClass))
					continue;

				if (obj->Flags & SDK::EObjectFlags::ClassDefaultObject)
					continue;

				auto* enemy = static_cast<SDK::AMassEnemyCharacterBase*>(obj);
				if (enemy->bIsInPool) // pooled/inactive — not a live world actor
					continue;

				TrackedEnemy entry;
				entry.actor = enemy;

				if (agentClass)
				{
					try
					{
						entry.agent = static_cast<SDK::UMassAgentComponent*>(enemy->GetComponentByClass(agentClass));
					}
					catch (...)
					{
						entry.agent = nullptr;
					}
				}

				// Top-up: while Disable AI is already on, newly-discovered enemies
				// (just spawned since the last scan) need disabling too — the edge
				// detector in Tick() only fires on the on/off transition itself.
				if (disableAI && entry.agent)
				{
					try { entry.agent->Disable(); }
					catch (...) { LOG_WARN("Enemies: exception disabling a newly-found enemy's Mass agent."); }
				}

				g_enemies.push_back(entry);
			}
		}

		// Resolves the enemy's live FMassEnemyHealthFragment, or nullptr if any link
		// in the chain (actor-key build, entity-handle lookup, fragment pointer)
		// isn't available. Called fresh every tick from EnforceOneHitKill —
		// deliberately not cached, see that function's comment for why.
		//
		// Goes actor -> TObjectKey -> FMassEntityHandle via
		// UMassActorSubsystem::GetEntityHandleFromActor — the authority-side
		// lookup, which works for every Mass actor regardless of replication
		// state or whether it has a UMassAgentComponent attached.
		FMassEnemyHealthFragmentView* ResolveHealthFragment(const TrackedEnemy& enemy)
		{
			if (!enemy.actor || !g_buildActorKey || !g_getEntityHandle || !g_getFragmentDataPtr
				|| !g_healthFragmentStruct || !g_entityManager || !g_actorSubsystem)
				return nullptr;

			SDK::FWeakObjectPtr actorKey{};
			void* actorPtr = enemy.actor;
			g_buildActorKey(&actorKey, &actorPtr);

			SDK::FMassEntityHandle handle{};
			g_getEntityHandle(g_actorSubsystem, &handle, actorKey);
			if (handle.Index == 0)
				return nullptr;

			void* fragmentPtr = g_getFragmentDataPtr(g_entityManager, handle, g_healthFragmentStruct);
			return reinterpret_cast<FMassEnemyHealthFragmentView*>(fragmentPtr);
		}

		void EnforceDisableAI(const TrackedEnemy& enemy)
		{
			if (auto* aiController = static_cast<SDK::AAIController*>(enemy.actor->Controller))
			{
				if (aiController->BrainComponent)
					aiController->BrainComponent->SetComponentTickEnabled(false);
			}

			auto it = g_attackRangeCache.find(enemy.actor);
			if (it == g_attackRangeCache.end())
			{
				AttackRangeCache cache;
				cache.distance         = enemy.actor->AllowedAttackDistance;
				cache.coneHalfAngle    = enemy.actor->AllowedAttackConeHalfAngle;
				cache.buildingDistance = enemy.actor->AllowedBuildingAttackDistance;
				g_attackRangeCache.emplace(enemy.actor, cache);
			}

			enemy.actor->AllowedAttackDistance         = 0.0f;
			enemy.actor->AllowedAttackConeHalfAngle    = 0.0f;
			enemy.actor->AllowedBuildingAttackDistance = 0.0f;
		}

		// Restores whatever EnforceDisableAI zeroed, for actors still in g_enemies
		// when the cheat is turned back off.
		void RestoreDisableAI()
		{
			for (const TrackedEnemy& enemy : g_enemies)
			{
				if (!enemy.actor)
					continue;

				try
				{
					if (auto* aiController = static_cast<SDK::AAIController*>(enemy.actor->Controller))
					{
						if (aiController->BrainComponent)
							aiController->BrainComponent->SetComponentTickEnabled(true);
					}

					auto it = g_attackRangeCache.find(enemy.actor);
					if (it != g_attackRangeCache.end())
					{
						enemy.actor->AllowedAttackDistance         = it->second.distance;
						enemy.actor->AllowedAttackConeHalfAngle    = it->second.coneHalfAngle;
						enemy.actor->AllowedBuildingAttackDistance = it->second.buildingDistance;
					}
				}
				catch (...)
				{
					LOG_WARN("Enemies: exception restoring an enemy's AI/attack-range state.");
				}
			}

			g_attackRangeCache.clear();
		}

		// Pins HP to 1 (only ever lowering it, never healing) on the enemy's real
		// Mass FMassEnemyHealthFragment, so any subsequent damage — even chip
		// damage — finishes them in a single hit. GAS attribute pinning (GAS
		// MovementSpeed etc.) doesn't affect Mass-simulated combat, hence going
		// straight to the entity fragment instead.
		//
		// Resolved fresh every tick (not cached across the 0.25s rescan window) —
		// caching the raw fragment pointer briefly caused a crash: the underlying
		// Mass entity can be destroyed (e.g. killed) and its archetype chunk
		// freed/recycled before the next rescan, since the puppet actor's pooling
		// state and the entity's fragment memory don't share a lifetime. A
		// dangling-pointer write like that is a genuine access violation, not a
		// catchable C++ exception, so try/catch around the dereference didn't
		// help — the only real fix is to not hold the pointer across ticks.
		// GetEntityHandleFromActor itself safely returns Index=0 for a destroyed
		// entity, so re-resolving here is the actual safety net.
		void EnforceOneHitKill(const TrackedEnemy& enemy)
		{
			FMassEnemyHealthFragmentView* healthFragment = nullptr;
			try { healthFragment = ResolveHealthFragment(enemy); }
			catch (...) { healthFragment = nullptr; }

			if (healthFragment)
			{
				try
				{
					if (healthFragment->HP > 1.0f)
						healthFragment->HP = 1.0f;
				}
				catch (...)
				{
					LOG_WARN("Enemies: exception writing to healthFragment for actor=%p.", enemy.actor);
				}
				return;
			}

			// Fallback for enemies whose fragment can't be resolved at all (e.g.
			// AOB resolution failed at Initialize) — kills outright once per actor.
			if (!enemy.agent || g_killedEnemies.count(enemy.actor))
				return;

			enemy.agent->KillEntity(false);
			g_killedEnemies.insert(enemy.actor);
		}
	}

	void Initialize()
	{
		IPluginScanner* scanner = GetScanner();
		if (scanner)
		{
			if (uintptr_t addr = scanner->FindPatternInMainModule(AOB::FWeakObjectPtr_AssignFObjectPtr))
				g_buildActorKey = reinterpret_cast<WeakObjectPtrAssignFn>(addr);
			else
				LOG_WARN("Enemies: failed to resolve FWeakObjectPtr::operator=(FObjectPtr) — One-Hit Kill will fall back to KillEntity only.");

			if (uintptr_t addr = scanner->FindPatternInMainModule(AOB::UMassActorSubsystem_GetEntityHandleFromActor))
				g_getEntityHandle = reinterpret_cast<GetEntityHandleFromActorFn>(addr);
			else
				LOG_WARN("Enemies: failed to resolve UMassActorSubsystem::GetEntityHandleFromActor — One-Hit Kill will fall back to KillEntity only.");

			if (uintptr_t addr = scanner->FindPatternInMainModule(AOB::FMassEntityManager_InternalGetFragmentDataPtr))
				g_getFragmentDataPtr = reinterpret_cast<InternalGetFragmentDataPtrFn>(addr);
			else
				LOG_WARN("Enemies: failed to resolve FMassEntityManager::InternalGetFragmentDataPtr — One-Hit Kill will fall back to KillEntity only.");
		}

		try
		{
			SDK::UObject* obj = SDK::UObject::FindObjectFastImpl("MassEnemyHealthFragment", SDK::EClassCastFlags::ScriptStruct);
			g_healthFragmentStruct = static_cast<SDK::UScriptStruct*>(obj);
		}
		catch (...) { g_healthFragmentStruct = nullptr; }

		if (!g_healthFragmentStruct)
			LOG_WARN("Enemies: failed to resolve FMassEnemyHealthFragment UScriptStruct — One-Hit Kill will fall back to KillEntity only.");
	}

	void Shutdown()
	{
		g_enemies.clear();
		g_killedEnemies.clear();
		g_attackRangeCache.clear();
		g_lastDisableAI = false;
	}

	void Tick(float deltaSeconds)
	{
		const bool disableAI   = g_disableAI.load();
		const bool oneHitKill  = g_oneHitKill.load();
		const bool anyEnabled  = disableAI || oneHitKill;

		g_rescanTimer += deltaSeconds;
		if (anyEnabled && (g_rescanTimer >= 0.25f || g_enemies.empty()))
		{
			g_rescanTimer = 0.0f;

			// Refreshes the per-world UMassActorSubsystem/FMassEntityManager pointers
			// that EnforceOneHitKill resolves each enemy's health fragment through
			// every tick. These are stable per-world singletons, so a 0.25s refresh
			// cadence is fine (unlike the per-entity fragment pointer, which must
			// never be cached — see EnforceOneHitKill's comment).
			if (oneHitKill)
			{
				try { RescanMassSubsystems(); }
				catch (...) { LOG_WARN("Enemies: exception resolving Mass subsystems."); }
			}

			try { RescanEnemies(); }
			catch (...) { LOG_WARN("Enemies: exception scanning GObjects."); }
		}

		// Disable()/Enable() likely does real work (un/re-registering the entity
		// from Mass simulation and replication) — only call it on the on/off
		// transition, not every tick. New spawns are caught by RescanEnemies()'s
		// top-up above.
		if (disableAI != g_lastDisableAI)
		{
			for (const TrackedEnemy& enemy : g_enemies)
			{
				if (!enemy.agent)
					continue;

				try { disableAI ? enemy.agent->Disable() : enemy.agent->Enable(); }
				catch (...) { LOG_WARN("Enemies: exception toggling a Mass agent's Disable/Enable state."); }
			}

			if (!disableAI)
				RestoreDisableAI();

			g_lastDisableAI = disableAI;
		}

		if (disableAI || oneHitKill)
		{
			for (const TrackedEnemy& enemy : g_enemies)
			{
				if (!enemy.actor)
					continue;

				try
				{
					if (disableAI)   EnforceDisableAI(enemy);
					if (oneHitKill)  EnforceOneHitKill(enemy);
				}
				catch (...)
				{
					LOG_WARN("Enemies: exception enforcing cheat on an enemy actor.");
				}
			}
		}

		std::lock_guard<std::mutex> lock(g_snapshotMutex);
		g_snapshot.count = static_cast<int>(g_enemies.size());
	}

	void ApplySavedConfig()
	{
		if (!SessionConfig::IsLoaded())
			return;

		g_disableAI   = SessionConfig::Get("enemies.disableAI", false);
		g_oneHitKill  = SessionConfig::Get("enemies.oneHitKill", false);

		LOG_INFO("Enemies: applied saved config for session '%s'.", SessionConfig::GetSessionName().c_str());
	}

	void RenderImGui(IModLoaderImGui* imgui)
	{
		int count;
		{
			std::lock_guard<std::mutex> lock(g_snapshotMutex);
			count = g_snapshot.count;
		}

		imgui->SeparatorText("Behaviour");

		bool disableAI = g_disableAI.load();
		if (imgui->Checkbox("Disable AI", &disableAI))
		{
			g_disableAI = disableAI;
			SessionConfig::Set("enemies.disableAI", disableAI);
		}
		imgui->SetItemTooltip("Stops behaviour-tree ticking and zeroes attack range so enemies can never land a hit.");

		imgui->Spacing();
		imgui->SeparatorText("Combat");

		bool oneHitKill = g_oneHitKill.load();
		if (imgui->Checkbox("One-Hit Kill", &oneHitKill))
		{
			g_oneHitKill = oneHitKill;
			SessionConfig::Set("enemies.oneHitKill", oneHitKill);
		}
		imgui->SetItemTooltip("Pins every tracked enemy's health to 1 — any hit, even chip damage, finishes them.");

		imgui->Spacing();
		if (disableAI || oneHitKill)
		{
			char text[64];
			snprintf(text, sizeof(text), "Tracking %d active enem%s.", count, count == 1 ? "y" : "ies");
			imgui->TextDisabled(text);
		}
		else
		{
			imgui->TextDisabled("Enable a cheat above to start tracking active enemies.");
		}
	}
}
