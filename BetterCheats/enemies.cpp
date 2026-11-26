#include "enemies.h"
#include "object_ref.h"
#include "game_thread.h"
#include "plugin_helpers.h"
#include "aob_resolver.h"
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
#include "map_fields.h"

namespace BetterCheats::Panels::Enemies
{
	namespace
	{
		std::atomic<bool> g_disableAI{ false };
		std::atomic<bool> g_oneHitKill{ false };

		// Pairs each tracked enemy actor with its UMassAgentComponent — the official
		// handle the One-Hit Kill fallback uses (KillEntity), found via
		// AActor::GetComponentByClass since AMassEnemyCharacterBase doesn't expose it
		// as a named field the way ACrCharacterPlayerBase does. Not every enemy
		// subclass has one attached, so it's only ever a fallback now — see
		// ResolveHealthFragment for the universal actor->entity path.
		//
		// Held across ticks until the next rescan, so both are ObjectRefs: an
		// enemy can be destroyed, or the whole world torn down, in between.
		struct TrackedEnemy
		{
			ObjectRef<SDK::AMassEnemyCharacterBase> actor;
			ObjectRef<SDK::UMassAgentComponent>     agent;
		};

		// Enemies are Mass Entity actors that pool in/out of existence constantly —
		// unlike the singleton subsystems other panels scan for, this list needs a
		// periodic re-scan rather than a one-shot lookup on world load.
		float                          g_rescanTimer = 0.0f;
		std::vector<TrackedEnemy>      g_enemies;

		// Tracks which actors we've already called UMassAgentComponent::KillEntity on,
		// so One-Hit Kill fires exactly once per enemy instead of every tick. Cleared
		// on Shutdown. Only ever used as a lookup key, never dereferenced.
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

		// Re-resolved every rescan (cheap — both are per-world singletons), and
		// checked on every use, since a world travel frees both between rescans.
		ObjectRef<SDK::UMassEntitySubsystem> g_entitySubsystem;
		ObjectRef<SDK::UMassActorSubsystem>  g_actorSubsystem;

		// Disable AI: UMassAgentComponent::Disable() alone has no visible effect (it's
		// native and does run — see plugin.log — but apparently only affects
		// replication/pooling bookkeeping, not the Mass simulation's actual movement/
		// attack logic). Instead: stop the AI controller's behaviour tree ticking, and
		// zero the attack-range fields so even if it keeps approaching it can never
		// register as "in range" to attack. Original values restored on toggle-off.
		struct AttackRangeCache
		{
			// Who was changed. The map key is only an address, which a later
			// enemy can reuse, so these say whether the entry is still its own.
			ObjectRef<SDK::AMassEnemyCharacterBase> actor;
			ObjectRef<SDK::UMassAgentComponent>     agent;

			float distance         = 0.0f;
			float coneHalfAngle    = 0.0f;
			float buildingDistance = 0.0f;
			bool  brainTicked      = false;
		};
		std::unordered_map<SDK::AMassEnemyCharacterBase*, AttackRangeCache> g_attackRangeCache;

		// What One-Hit Kill lowered. The actor pools between enemies, so the
		// entity handle says whether it is still the same one.
		struct LoweredHealth
		{
			ObjectRef<SDK::AMassEnemyCharacterBase> actor;
			SDK::FMassEntityHandle                  entity{};
			float                                   hp = 0.0f;
		};
		std::unordered_map<SDK::AMassEnemyCharacterBase*, LoweredHealth> g_loweredHealth;

		struct EnemiesSnapshot { int count = 0; };
		std::mutex        g_snapshotMutex;
		EnemiesSnapshot   g_snapshot;

		void* ResolveHealthFragment(SDK::AMassEnemyCharacterBase* actor, SDK::FMassEntityHandle* entity = nullptr);

		// Re-finds the per-world UMassEntitySubsystem/UMassActorSubsystem
		// singletons. Only ever called from Tick().
		void RescanMassSubsystems()
		{
			g_entitySubsystem.Reset();
			g_actorSubsystem.Reset();

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
					g_entitySubsystem.Set(static_cast<SDK::UMassEntitySubsystem*>(obj));
					doneEntity = true;
					continue;
				}
				if (!doneActor && obj != actorCDO && obj->Class == actorClass)
				{
					g_actorSubsystem.Set(static_cast<SDK::UMassActorSubsystem*>(obj));
					doneActor = true;
					continue;
				}
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
				if (BetterCheats::Fields::ValueOr(BetterCheats::Fields::MassEnemyCharacterBase_bIsInPool, enemy)) // pooled/inactive — not a live world actor
					continue;

				SDK::UMassAgentComponent* agent = nullptr;
				if (agentClass)
				{
					try
					{
						agent = static_cast<SDK::UMassAgentComponent*>(enemy->GetComponentByClass(agentClass));
					}
					catch (...)
					{
						agent = nullptr;
					}
				}

				TrackedEnemy entry;
				entry.actor.Set(enemy);
				entry.agent.Set(agent);

				// Top-up: while Disable AI is already on, newly-discovered enemies
				// (just spawned since the last scan) need disabling too — the edge
				// detector in Tick() only fires on the on/off transition itself.
				if (disableAI && agent)
				{
					try { agent->Disable(); }
					catch (...) { LOG_WARN("Enemies: exception disabling a newly-found enemy's Mass agent."); }
				}

				g_enemies.push_back(entry);
			}
		}

		// Resolves the enemy's live FMassEnemyHealthFragment (read its HP through
		// Fields::MassEnemyHealthFragment_HP), or nullptr if any link
		// in the chain (actor-key build, entity-handle lookup, fragment pointer)
		// isn't available. Called fresh every tick from EnforceOneHitKill —
		// deliberately not cached, see that function's comment for why.
		//
		// Goes actor -> TObjectKey -> FMassEntityHandle via
		// UMassActorSubsystem::GetEntityHandleFromActor — the authority-side
		// lookup, which works for every Mass actor regardless of replication
		// state or whether it has a UMassAgentComponent attached.
		void* ResolveHealthFragment(SDK::AMassEnemyCharacterBase* actor, SDK::FMassEntityHandle* entity)
		{
			if (!actor || !g_buildActorKey || !g_getEntityHandle || !g_getFragmentDataPtr || !g_healthFragmentStruct)
				return nullptr;

			SDK::UMassEntitySubsystem* entitySubsystem = g_entitySubsystem.Get();
			SDK::UMassActorSubsystem*  actorSubsystem  = g_actorSubsystem.Get();
			void* entityManager = PluginMap::MassEntityManagerOf(entitySubsystem);
			if (!entityManager || !actorSubsystem)
				return nullptr;

			SDK::FWeakObjectPtr actorKey{};
			void* actorPtr = actor;
			g_buildActorKey(&actorKey, &actorPtr);

			SDK::FMassEntityHandle handle{};
			g_getEntityHandle(actorSubsystem, &handle, actorKey);
			if (handle.Index == 0)
				return nullptr;
			if (entity)
				*entity = handle;

			return g_getFragmentDataPtr(entityManager, handle, g_healthFragmentStruct);
		}

		void EnforceDisableAI(SDK::AMassEnemyCharacterBase* actor, SDK::UMassAgentComponent* agent)
		{
			auto* aiController = static_cast<SDK::AAIController*>(actor->Controller);
			auto* brain = aiController ? aiController->BrainComponent : nullptr;

			// The agent has no readable on/off state; the brain's tick has.
			AttackRangeCache& cache = g_attackRangeCache[actor];
			if (cache.actor.Get() != actor)
			{
				cache.brainTicked = brain && brain->IsComponentTickEnabled();
				cache.actor.Set(actor);
				cache.distance         = BetterCheats::Fields::ValueOr(BetterCheats::Fields::MassEnemyCharacterBase_AllowedAttackDistance, actor);
				cache.coneHalfAngle    = BetterCheats::Fields::ValueOr(BetterCheats::Fields::MassEnemyCharacterBase_AllowedAttackConeHalfAngle, actor);
				cache.buildingDistance = BetterCheats::Fields::ValueOr(BetterCheats::Fields::MassEnemyCharacterBase_AllowedBuildingAttackDistance, actor);
			}
			cache.agent.Set(agent);

			if (brain)
				brain->SetComponentTickEnabled(false);

			BetterCheats::Fields::Store(BetterCheats::Fields::MassEnemyCharacterBase_AllowedAttackDistance, actor, 0.0f);
			BetterCheats::Fields::Store(BetterCheats::Fields::MassEnemyCharacterBase_AllowedAttackConeHalfAngle, actor, 0.0f);
			BetterCheats::Fields::Store(BetterCheats::Fields::MassEnemyCharacterBase_AllowedBuildingAttackDistance, actor, 0.0f);
		}

		// Undoes Disable AI on every enemy EnforceDisableAI changed that is still
		// alive, including any the latest rescan no longer lists.
		void RestoreDisableAI()
		{
			for (const auto& entry : g_attackRangeCache)
			{
				const AttackRangeCache& cache = entry.second;

				SDK::AMassEnemyCharacterBase* actor = cache.actor.Get();
				if (!actor)
					continue;

				try
				{
					// A pooled actor's agent and AI are left for the pool to
					// switch on when it hands the actor out again.
					if (!BetterCheats::Fields::ValueOr(BetterCheats::Fields::MassEnemyCharacterBase_bIsInPool, actor))
					{
						if (SDK::UMassAgentComponent* agent = cache.agent.Get())
							agent->Enable();

						if (auto* aiController = static_cast<SDK::AAIController*>(actor->Controller))
						{
							if (cache.brainTicked && aiController->BrainComponent)
								aiController->BrainComponent->SetComponentTickEnabled(true);
						}
					}

					BetterCheats::Fields::Store(BetterCheats::Fields::MassEnemyCharacterBase_AllowedAttackDistance, actor, cache.distance);
					BetterCheats::Fields::Store(BetterCheats::Fields::MassEnemyCharacterBase_AllowedAttackConeHalfAngle, actor, cache.coneHalfAngle);
					BetterCheats::Fields::Store(BetterCheats::Fields::MassEnemyCharacterBase_AllowedBuildingAttackDistance, actor, cache.buildingDistance);
				}
				catch (...)
				{
					LOG_WARN("Enemies: exception restoring an enemy's AI/attack-range state.");
				}
			}

			g_attackRangeCache.clear();
		}

		// Gives back the health of every lowered enemy that is still alive, still
		// at what we wrote and still the same one.
		void RestoreLoweredHealth()
		{
			for (const auto& entry : g_loweredHealth)
			{
				const LoweredHealth& lowered = entry.second;

				SDK::AMassEnemyCharacterBase* actor = lowered.actor.Get();
				if (!actor)
					continue;

				try
				{
					if (BetterCheats::Fields::ValueOr(BetterCheats::Fields::MassEnemyCharacterBase_bIsInPool, actor))
						continue;

					SDK::FMassEntityHandle entity{};
					void* fragment = ResolveHealthFragment(actor, &entity);
					if (!fragment || entity.Index != lowered.entity.Index || entity.SerialNumber != lowered.entity.SerialNumber)
						continue;

					float* hp = BetterCheats::Fields::MassEnemyHealthFragment_HP.In(fragment);
					if (hp && *hp == 1.0f)
						*hp = lowered.hp;
				}
				catch (...)
				{
					LOG_WARN("Enemies: exception restoring an enemy's health.");
				}
			}

			g_loweredHealth.clear();
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
		void EnforceOneHitKill(SDK::AMassEnemyCharacterBase* actor, SDK::UMassAgentComponent* agent)
		{
			void* healthFragment = nullptr;
			SDK::FMassEntityHandle entity{};
			try { healthFragment = ResolveHealthFragment(actor, &entity); }
			catch (...) { healthFragment = nullptr; }

			if (healthFragment)
			{
				try
				{
					float* hp = BetterCheats::Fields::MassEnemyHealthFragment_HP.In(healthFragment);
					if (hp && *hp > 1.0f)
					{
						LoweredHealth& lowered = g_loweredHealth[actor];
						if (lowered.actor.Get() != actor || lowered.entity.Index != entity.Index
							|| lowered.entity.SerialNumber != entity.SerialNumber)
						{
							lowered.actor.Set(actor);
							lowered.entity = entity;
							lowered.hp = *hp;
						}
						*hp = 1.0f;
					}
				}
				catch (...)
				{
					LOG_WARN("Enemies: exception writing to healthFragment for actor=%p.", static_cast<void*>(actor));
				}
				return;
			}

			// Fallback for enemies whose fragment can't be resolved at all (e.g.
			// AOB resolution failed at Initialize) — kills outright once per actor.
			if (!agent || g_killedEnemies.count(actor))
				return;

			agent->KillEntity(false);
			g_killedEnemies.insert(actor);
		}
	}

	void Initialize()
	{
		const AOB::ResolvedAddresses& aob = AOB::Resolved();

		if (uintptr_t addr = aob.FWeakObjectPtr_AssignFObjectPtr)
			g_buildActorKey = reinterpret_cast<WeakObjectPtrAssignFn>(addr);
		else
			LOG_WARN("Enemies: FWeakObjectPtr::operator=(FObjectPtr) unresolved — One-Hit Kill will fall back to KillEntity only.");

		if (uintptr_t addr = aob.UMassActorSubsystem_GetEntityHandleFromActor)
			g_getEntityHandle = reinterpret_cast<GetEntityHandleFromActorFn>(addr);
		else
			LOG_WARN("Enemies: UMassActorSubsystem::GetEntityHandleFromActor unresolved — One-Hit Kill will fall back to KillEntity only.");

		if (uintptr_t addr = aob.FMassEntityManager_InternalGetFragmentDataPtr)
			g_getFragmentDataPtr = reinterpret_cast<InternalGetFragmentDataPtrFn>(addr);
		else
			LOG_WARN("Enemies: FMassEntityManager::InternalGetFragmentDataPtr unresolved — One-Hit Kill will fall back to KillEntity only.");

		try
		{
			SDK::UObject* obj = SDK::UObject::FindObjectFastImpl("MassEnemyHealthFragment", SDK::EClassCastFlags::ScriptStruct);
			g_healthFragmentStruct = static_cast<SDK::UScriptStruct*>(obj);
		}
		catch (...) { g_healthFragmentStruct = nullptr; }

		if (!g_healthFragmentStruct)
			LOG_WARN("Enemies: failed to resolve FMassEnemyHealthFragment UScriptStruct — One-Hit Kill will fall back to KillEntity only.");
	}

	void ForgetWorld()
	{
		g_enemies.clear();
		g_killedEnemies.clear();
		g_attackRangeCache.clear();
		g_loweredHealth.clear();
		g_entitySubsystem.Reset();
		g_actorSubsystem.Reset();
		g_lastDisableAI = false;
		g_rescanTimer = 0.0f;

		std::lock_guard<std::mutex> lock(g_snapshotMutex);
		g_snapshot = {};
	}

	void Shutdown()
	{
		// Left switched off, the zeroed ranges would be read back as the
		// originals after a reload.
		if (BetterCheats::IsGameThread())
			ReleaseValues();
		ForgetWorld();
	}

	// Disable()/Enable() likely does real work (un/re-registering the entity
	// from Mass simulation and replication) — only call it on the on/off
	// transition, not every tick. New spawns are caught by RescanEnemies()'s
	// top-up in Tick.
	static void SwitchDisableAI(bool disableAI)
	{
		if (disableAI)
		{
			for (const TrackedEnemy& enemy : g_enemies)
			{
				SDK::UMassAgentComponent* agent = enemy.agent.Get();
				if (!agent)
					continue;

				try { agent->Disable(); }
				catch (...) { LOG_WARN("Enemies: exception disabling a Mass agent."); }
			}
		}
		else
		{
			RestoreDisableAI();
		}

		g_lastDisableAI = disableAI;
	}

	void ReleaseValues()
	{
		if (g_lastDisableAI)
			SwitchDisableAI(false);
		RestoreLoweredHealth();
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

		if (disableAI != g_lastDisableAI)
			SwitchDisableAI(disableAI);
		if (!oneHitKill && !g_loweredHealth.empty())
			RestoreLoweredHealth();

		if (disableAI || oneHitKill)
		{
			for (const TrackedEnemy& enemy : g_enemies)
			{
				SDK::AMassEnemyCharacterBase* actor = enemy.actor.Get();
				if (!actor)
					continue;

				try
				{
					if (disableAI)   EnforceDisableAI(actor, enemy.agent.Get());
					if (oneHitKill)  EnforceOneHitKill(actor, enemy.agent.Get());
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

	void PrepareHealthReads(float deltaSeconds)
	{
		// Called by the weapon hit tracker, which runs whether or not this feature is on.
		if (!Features::Usable(Features::Enemies))
			return;
		static float cooldown = 0.0f;
		cooldown -= deltaSeconds;
		if (g_entitySubsystem.Get() && g_actorSubsystem.Get())
			return;
		if (cooldown > 0.0f)
			return;
		cooldown = 1.0f;
		try { RescanMassSubsystems(); }
		catch (...) { LOG_WARN("Enemies: exception resolving Mass subsystems."); }
	}

	bool TryReadMassHealth(SDK::AActor* actor, float& hp)
	{
		if (!Features::Usable(Features::Enemies))
			return false;
		if (!actor || !actor->IsA(SDK::AMassEnemyCharacterBase::StaticClass()))
			return false;
		try
		{
			const void* fragment = ResolveHealthFragment(static_cast<SDK::AMassEnemyCharacterBase*>(actor));
			const float* value = BetterCheats::Fields::MassEnemyHealthFragment_HP.In(fragment);
			if (!value)
				return false;
			hp = *value;
			return true;
		}
		catch (...) { return false; }
	}

	void ApplySavedConfig()
	{
		if (!SessionConfig::IsLoaded())
			return;

		g_disableAI   = SessionConfig::Get("enemies.disableAI", false);
		g_oneHitKill  = SessionConfig::Get("enemies.oneHitKill", false);

		LOG_INFO("Enemies: applied saved config for session '%s'.", SessionConfig::GetSessionName().c_str());
	}

	void RenderImGui(IModLoaderImGui* imgui, bool greyed)
	{
		imgui->SeparatorText("Behaviour");

		bool disableAI = !greyed && g_disableAI.load();
		if (imgui->Checkbox("Disable AI", &disableAI))
		{
			g_disableAI = disableAI;
			SessionConfig::Set("enemies.disableAI", disableAI);
		}
		imgui->SetItemTooltip("Stops behaviour-tree ticking and zeroes attack range so enemies can never land a hit.");

		imgui->Spacing();
		imgui->SeparatorText("Combat");

		bool oneHitKill = !greyed && g_oneHitKill.load();
		if (imgui->Checkbox("One-Hit Kill", &oneHitKill))
		{
			g_oneHitKill = oneHitKill;
			SessionConfig::Set("enemies.oneHitKill", oneHitKill);
		}
		imgui->SetItemTooltip("Pins every tracked enemy's health to 1 — any hit, even chip damage, finishes them.");

		// Nothing is tracked while the section is greyed, so there is no count.
		if (greyed)
			return;

		int count;
		{
			std::lock_guard<std::mutex> lock(g_snapshotMutex);
			count = g_snapshot.count;
		}

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
