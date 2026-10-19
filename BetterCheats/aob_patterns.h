#pragma once
#include <cstddef>

// ---------------------------------------------------------------------------
// AOB Pattern Registry
//
// Format for each entry:
//   Pattern string  — byte sequence with ?? wildcards
//   Class::Function — fully qualified name of the function being located
//   Parameters      — function signature for reference
//
// Patterns are resolved exclusively by aob_resolver.cpp, from the plugin's
// OnPluginLoadHooks event — the only window in which the loader lets a plugin
// pattern scan. Add new patterns here, add a matching field and Resolve* call
// in aob_resolver.h/.cpp, and read the address back through AOB::Resolved().
// Never scatter raw byte strings, or scan, from a feature file.
// ---------------------------------------------------------------------------

namespace BetterCheats::AOB
{

	// UCrGameInstance::ServerSessionName — offset within UCrGameInstance,
		// not exposed by the generated SDK (folded into trailing padding bytes).
	constexpr const std::ptrdiff_t kServerSessionNameOffset = 0x290;

	// UCrEnviroWaveTimerSubsystem — the countdown to the next rupture. The
	// generated SDK folds everything before TimerActor into Pad_30, but these
	// are what actually gate the cycle: Tick() only decrements NextWaveTimer
	// while !bPause && bWaitingForNextWave, and the whole block is persisted
	// into the save (FCrEnviroWaveTimerSaveData) so a bad value survives a
	// reload. ACrWaveTimerActor::bPause is only a replicated HUD mirror.
	constexpr const std::ptrdiff_t kWaveTimerWaitingDurationOffset    = 0x78; // float
	constexpr const std::ptrdiff_t kWaveTimerNextWaveTimerOffset      = 0x7C; // float, seconds remaining
	constexpr const std::ptrdiff_t kWaveTimerWaitingForNextWaveOffset = 0x80; // bool
	constexpr const std::ptrdiff_t kWaveTimerPauseOffset              = 0x81; // bool
	constexpr const std::ptrdiff_t kWaveTimerStopWavesOffset          = 0x83; // bool


	// -------------------------------------------------------------------------
	// Example / template (remove when first real pattern is added)
	// -------------------------------------------------------------------------
	// Class::Function  AExampleClass::ExampleFunction
	// Parameters       (AExampleClass* self, float deltaTime)
	// constexpr const char* ExampleFunction = "48 89 5C 24 ?? 57 48 83 EC 20 ?? ?? ?? ?? ?? ??";

	// -------------------------------------------------------------------------
	// HUD refresh
	// -------------------------------------------------------------------------

	// Class::Function  UCrUW_HealthHud::SetupProgressBar
	// Parameters       (UCrUW_HealthHud* this)
	// Re-pulls the owning character's current Health/Energy/Shield/survival values
	// and pushes them into the HUD's progress bars — used to force an immediate
	// HUD refresh after the plugin writes attribute values directly.
	constexpr const char* HealthHud_SetupProgressBar =
		"40 57 48 83 EC ?? 48 83 B9 ?? ?? ?? ?? ?? 48 8B F9 0F 84 ?? ?? ?? ?? ?? ?? ?? 48 89 5C 24 ?? FF 90 ?? ?? ?? ?? "
		"48 8B C8 E8 ?? ?? ?? ?? 48 8B D8 48 85 C0 0F 84 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B 4B ?? 48 83 C0 ?? 48 63 50 ?? "
		"3B 51 ?? 0F 8F ?? ?? ?? ?? 48 8B 49 ?? ?? ?? ?? ?? 0F 85 ?? ?? ?? ?? 80 3D ?? ?? ?? ?? ?? 48 8B 83 ?? ?? ?? ?? "
		"48 89 44 24 ?? 48 89 74 24 ?? 74 ?? 48 85 C0 74 ?? 48 8B C8 E8 ?? ?? ?? ?? 48 8B 44 24 ?? 48 85 C0 74 ?? "
		"E8 ?? ?? ?? ?? 48 8B D0 48 8D 4C 24 ?? E8 ?? ?? ?? ?? 84 C0 74 ?? 48 8B 74 24 ?? EB ?? 33 F6 48 85 F6 0F 84 "
		"?? ?? ?? ?? 48 8D 8E ?? ?? ?? ?? ?? ?? ?? FF 50 ?? 48 8B CE 48 8B D8 E8 ?? ?? ?? ?? 48 8B F0 48 85 DB 0F 84 "
		"?? ?? ?? ?? 48 85 C0 0F 84 ?? ?? ?? ?? 48 89 6C 24";

	// -------------------------------------------------------------------------
	// Building
	// -------------------------------------------------------------------------

	// Class::Function  UCrBuildingComponent::GetPlacementResourceConditionResult
	// Parameters       (UCrBuildingComponent* this) -> EAuAPlacementConditionResult
	// Hooked to return Valid (1) when no-build-cost cheat is active.
	// Formerly GetResourceConditionResult — renamed by the game, same contract.
	constexpr const char* GetPlacementResourceConditionResult =
		"48 89 5C 24 ?? 48 89 6C 24 ?? 56 57 41 56 48 83 EC ?? 48 8B F9 48 8B 89";

	// Class::Function  UAuActorPlacementComponent::AddPoint
	// Parameters       (UAuActorPlacementComponent* this) -> FScriptContainerElement*
	// Called each time the player adds a foundation to the zoop chain.
	// Documented for reference; zoop limit is bypassed via MaxMultiConfirmPoints field writes
	// on the top-level UAuActorPlacementData and all its sub-variant pointers.
	constexpr const char* AddPoint =
		"48 89 5C 24 ?? 48 89 74 24 ?? 48 89 7C 24 ?? 4C 89 74 24 ?? 55 48 8D AC 24 ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? 48 8B D9";

	// Class::Function  ACrAPHelperActorCustom::CheckStability
	// Parameters       (ACrAPHelperActorCustom* this, const UCrBuildingData* PlacementData) -> bool
	// Native stability gate for the "Custom" placement helper actor
	// (foundations/buildings using snap sockets). Computes the stability graph
	// result (or the simpler neighbour-trace result, depending on
	// UAuActorPlacementComponent::NewStability) and returns whether the current
	// placement is structurally valid; it is also the only caller of
	// ACrAPHelper::CheckStability, so hooking here covers that path too. Hooked to
	// force a `true` result — after letting the original run so the HUD stability
	// bar still reflects the real computed value — when the No Stability Check
	// cheat is active. The multi-point/zoop path needs CheckStability_DynamicPillar
	// below as well.
	//
	// Re-derived for CL-125897. The function did not move or change shape, it was
	// just re-compiled: the frame grew past 0x80, so `sub rsp, imm8` became
	// `sub rsp, imm32`, and the two argument moves swapped registers. The old
	// pattern pinned both the imm8 encoding and the modrm bytes of those moves,
	// which is why it stopped matching. Register bytes and the frame size are
	// wildcarded now so the next re-allocation does not break it again.
	constexpr const char* CheckStability_Custom =
		"40 53 57 48 81 EC ?? ?? ?? ?? 48 8B DA 48 8B F9 48 85 D2 75 ?? 32 C0";


	// Class::Function  ACrAPHelperDynamicPillar::CheckStability
	// Parameters       (ACrAPHelperDynamicPillar* this, const UCrBuildingData* PlacementData) -> bool
	// Stability gate for the dynamic pillar helper used by the multi-point/zoop
	// placement path (chained foundations) — it does not go through
	// ACrAPHelperActorCustom::CheckStability, which is why zoop placements are
	// still blocked with only that hook installed. Hooked the same way: let the
	// original run so the HUD strength value stays honest, then force the return
	// to true while the No Stability Check cheat is active.
	//
	// Re-derived for CL-125897, same story as the Custom helper above: identical
	// code, but rbx was re-allocated to rsi, so the push/pop and the `this` move
	// all changed bytes. The register bytes and the member offset are wildcarded;
	// the two register saves after the early-out keep the match unique.
	constexpr const char* CheckStability_DynamicPillar =
		"40 56 48 83 EC ?? 48 83 B9 ?? ?? ?? ?? ?? 48 8B F1 75 ?? 32 C0";


	// Class::Function  ACrTechnologyKeeper::CheckAvailableBuildings
	// Parameters       (ACrTechnologyKeeper* this, UCrCorporationData* Corporation, int64_t Reputation)
	// Rebuilds AvailableBuildings from AllBuildings, filtering by research state.
	// When bUnlockAllBuildings is true, Corporation/Reputation are unused — safe to pass nullptr/0.
	constexpr const char* CheckAvailableBuildings =
		"48 89 4C 24 ?? 55 56 48 8D 6C 24 ?? 48 81 EC ?? ?? ?? ?? 80 B9";

	// Class::Function  ACrCraftingRecipeOwner::IsRecipeUnlocked
	// Parameters       (ACrCraftingRecipeOwner* this, UCrItemRecipeData* InRecipe) -> bool
	// Returns true if the given recipe has been unlocked for crafting.
	// Hooked to always return true when unlock all recipes cheat is active.
	constexpr const char* IsRecipeUnlocked =
		"48 89 5C 24 ?? 57 48 83 EC ?? 80 B9 ?? ?? ?? ?? ?? 48 8B DA 48 8B F9 75 ?? 48 85 D2 0F 84";

	// -------------------------------------------------------------------------
	// Deconstruct Windows During Waves
	//
	// These back an opt-in toggle, so aob_resolver.cpp scans them without
	// recording a miss: a pattern that stops matching leaves the toggle off
	// instead of refusing the whole plugin.
	// -------------------------------------------------------------------------

	// Class::Function  ACrPlayerControllerBase::FindDeconstructibleTarget
	// Parameters       (ACrPlayerControllerBase* this, const FTraceDatum& TraceData) -> AActor*
	// Scores the deconstruct trace's hits, then runs the winner through the
	// tag, infection, heat, habitat-inside and airlock-inside gates; null when
	// one of them refuses it. Reads nothing but its inputs and writes nothing.
	constexpr const char* FindDeconstructibleTarget =
		"48 8B C4 55 53 56 57 41 55 41 56 41 57 48 8D A8 ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? 0F 29 70 ?? "
		"4C 8B FA 44 0F 29 48 ?? 4C 8B F1 44 0F 29 90 ?? ?? ?? ?? 33 F6 48 8B 01 FF 90 D8 07 00 00 84 C0 0F";

	// Inside FindDeconstructibleTarget: the infection gate
	// (`comiss xmm6,[rax]; jb <return null>`), then the temperature fragment
	// lookup and the heat gate, whose 2-byte `jb <return null>` sits
	// kHeatGateJumpOffset bytes into the match. Any temperature above 0 takes it.
	constexpr const char* FindDeconstructibleTarget_HeatGate =
		"0F 2F 30 0F 82 ?? ?? ?? ?? 48 8B 9D ?? ?? ?? ?? E8 ?? ?? ?? ?? 4C 8B C0 48 8B D3 48 8B CE "
		"E8 ?? ?? ?? ?? 48 85 C0 74 05 0F 2F 30 72 ?? 48 8B 07 48 8B CF FF 90 00 08 00 00";
	constexpr const std::ptrdiff_t kHeatGateJumpOffset = 43;

	// -------------------------------------------------------------------------
	// Build Windows Anywhere
	//
	// Opt-in as well, and scanned the same way: a miss leaves the toggle doing
	// nothing.
	// -------------------------------------------------------------------------

	// Class::Function  ACrAPHelperActorCustom::CheckMainMeshCollision
	// Parameters       (ACrAPHelperActorCustom* this, const UCrBuildingData* PlacementData) -> EAuAPlacementConditionResult
	// Overlap test of the helper's main mesh. Overlapping actors are skipped when
	// they are snapped to, connected to or ignored by the piece; any other
	// building makes it BuildingColliding or IsColliding.
	constexpr const char* CheckMainMeshCollision_Custom =
		"40 55 53 56 57 41 55 48 8D AC 24 ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? 48 83 B9 ?? ?? ?? ?? 00 "
		"4C 8B E9 48 8B 01 0F 84 ?? ?? ?? ?? 33 FF 48 89 7D ?? 48 89 7D ??";

	// Class::Function  ACrAPHelper::GetBoxCollisionResult
	// Parameters       (ACrAPHelper* this, const FVector& Min, const FVector& Max,
	//                   const TArray<AActor*>& IgnoredActors, const TArray<TSubclassOf<ACrBuildingActorBase>>& IgnoredClasses,
	//                   bool) -> EAuAPlacementConditionResult
	// The box overlap pass GetHelperConditionResult runs once the main mesh has
	// passed, skipping the same kinds of actor.
	constexpr const char* GetBoxCollisionResult =
		"48 89 5C 24 18 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? "
		"48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 85 ?? ?? ?? ?? 48 8B 01 45 33 E4 48 8B B5 ?? ?? ?? ?? "
		"4D 8B F1 4C 89 4C 24 ?? 49 8B D8 48 8B FA 48 89 4D ?? 4C 8B E9";

	// -------------------------------------------------------------------------
	// Build Benches Anywhere
	//
	// Opt-in, scanned the same way as a pair: a miss leaves the toggle doing
	// nothing.
	// -------------------------------------------------------------------------

	// Class::Function  UCrSocketSnapPlacementMethod::GetSnappedSocket
	// Parameters       (UCrSocketSnapPlacementMethod* this, const UAuActorPlacementData* Data, const FHitResult& Hit,
	//                   USceneComponent* Component, + 6 more on the stack) -> UStaticMeshSocket*
	// Picks the free socket of the custom building under the cursor that a
	// socket-snap piece (window, habitat machine) snaps to.
	constexpr const char* GetSnappedSocket =
		"4C 89 4C 24 20 48 89 54 24 10 48 89 4C 24 08 55 53 57 41 55 41 57 48 8D AC 24 ?? ?? ?? ?? "
		"48 81 EC ?? ?? ?? ?? 48 8B 85 ?? ?? ?? ?? 33 FF 49 8B D9 4D 8B E8 4C 8B FA C6 40 08 00";

	// Class::Function  UCrBuildingStabilitySubsystem::IsSocketFree
	// Parameters       (UCrBuildingStabilitySubsystem* this, ACrCustomBuilding* Building, const UStaticMeshSocket* Socket) -> bool
	// False when any custom building is registered on that socket. Its only
	// caller is GetSnappedSocket.
	constexpr const char* IsSocketFree =
		"4C 89 44 24 18 55 53 56 57 41 56 41 57 48 8D 6C 24 ?? 48 81 EC ?? ?? ?? ?? 4C 8B FA 48 8B F1 "
		"E8 ?? ?? ?? ?? 48 8B C8 E8 ?? ?? ?? ?? 33 FF 4C 8B F0";

	// -------------------------------------------------------------------------
	// Mining
	// -------------------------------------------------------------------------

	// Class::Function  UCrMiningComponent::GetMiningDamage
	// Parameters       (UCrMiningComponent* this, bool IsHittingWeakSpot)
	constexpr const char* GetMiningDamage =
		"40 55 56 57 41 56 48 8D 6C 24 ?? 48 81 EC ?? ?? ?? ?? 48 8B B9";

	// -------------------------------------------------------------------------
	// Mass Entity Templates
	// -------------------------------------------------------------------------

	// Class::Function  FMassEntityConfig::DestroyEntityTemplate
	// Parameters       (FMassEntityConfig* self, const UWorld* world)
	// Removes the cached FMassEntityTemplate (and its baked FConstSharedStruct
	// fragments, e.g. FCrElectricityParameters) for this config from the world's
	// FMassEntityTemplateRegistry, so the next GetOrCreateEntityTemplate rebuilds
	// it from the trait's current values.
	constexpr const char* FMassEntityConfig_DestroyEntityTemplate =
		"48 8B C4 48 89 58 ?? 48 89 70 ?? 57 48 83 EC ?? 33 FF 4C 8D 40";

	// Class::Function  FMassEntityConfig::GetOrCreateEntityTemplate
	// Parameters       (FMassEntityConfig* self, const UWorld* world) -> const FMassEntityTemplate*
	// Rebuilds (or returns the cached) FMassEntityTemplate for this config,
	// re-running each trait's BuildTemplate (e.g. UCrElectricityTrait::BuildTemplate)
	// against the trait's current property values.
	constexpr const char* FMassEntityConfig_GetOrCreateEntityTemplate =
		"40 55 53 56 41 57 48 8D AC 24 ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 85 ?? ?? ?? ?? 45 33 FF";

	// Class::Function  UWorld::GetSubsystem<UMassEntitySubsystem>
	// Parameters       (UWorld* self) -> UMassEntitySubsystem*
	constexpr const char* UWorld_GetMassEntitySubsystem =
		"48 89 5C 24 ?? 57 48 83 EC ?? 48 8D B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 80 3D ?? ?? ?? ?? ?? 48 8B D8 74 ?? 48 85 C0 74 ?? 48 8B C8 E8 ?? ?? ?? ?? EB ?? 48 85 DB 74 ?? E8 ?? ?? ?? ?? 48 85 C0 74 ?? 48 8D 50 ?? 48 63 40 ?? 3B 43 ?? 7F ?? 48 8B C8 48 8B 43 ?? ?? ?? ?? ?? 74 ?? 33 DB 48 8B D3 48 8B CF 48 8B 5C 24 ?? 48 83 C4 ?? 5F E9 ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 4C 89 4C 24 ?? 4C 89 44 24";

	// Class::Function  FMassEntityManager::InternalGetFragmentDataPtr
	// Parameters       (FMassEntityManager* this, FMassEntityHandle Entity, const UScriptStruct* FragmentType) -> void*
	// The single real implementation behind every FMassEntityManager::GetFragmentDataPtr<T>
	// template instantiation — hands back a pointer to the given entity's live fragment
	// data of the requested type within its archetype chunk, or nullptr if that entity
	// doesn't have a fragment of that type. Used to write directly into an enemy's
	// FMassEnemyHealthFragment for One-Hit Kill, since Mass-simulated enemies
	// don't respond to GAS attribute pinning the way the player does.
	// Byte-for-byte near-identical to FMassEntityManager::InternalGetFragmentDataChecked
	// (same assertion macro expansion) except for one literal: the "41 B8 14 07 00 00"
	// immediate below is a compiler-embedded source line number for the second
	// CheckVerifyFailedImpl2 call, which happens to differ between the two functions
	// (Ptr=0x0714, Checked=0x070A) — pinned deliberately so this pattern can't match
	// the Checked variant, which asserts/crashes on a missing fragment instead of
	// returning nullptr.
	constexpr const char* FMassEntityManager_InternalGetFragmentDataPtr =
		"48 89 5C 24 ?? 48 89 74 24 ?? 48 89 54 24 ?? 57 48 83 EC 30 49 8B F0 48 8B DA 48 8B F9 E8 ?? ?? ?? ?? "
		"84 C0 75 ?? 8B 44 24 ?? 4C 8D 0D ?? ?? ?? ?? 89 44 24 ?? 48 8D 15 ?? ?? ?? ?? 41 B8 4A 07 00 00 89 5C "
		"24 ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 ?? 90 CC 0F B6 47 ?? 48 8D 57 ?? 4C 8D 05 ?? ?? ?? "
		"?? C6 44 24 ?? ?? 48 8D 4C 24 ?? ?? ?? ?? ?? 41 FF D0 8B D3 ?? ?? ?? 4C 8B 41 ?? 48 8B C8 41 FF D0 48 "
		"8B F8 48 85 C0 75 ?? 4C 8D 0D ?? ?? ?? ?? 41 B8 14 07 00 00";

	// Class::Function  FWeakObjectPtr::operator=(FObjectPtr)
	// Parameters       (FWeakObjectPtr* this, FObjectPtr* ObjectPtr) -> void
	// Builds a weak/object-key handle (ObjectIndex + lazily-allocated
	// ObjectSerialNumber) from a raw UObject*. The second parameter is passed by
	// pointer, not by value — confirmed via raw disassembly: the function's first
	// instruction is `mov rdx, [rdx]`, dereferencing it once to get the actual
	// FObjectPtr (which, in this build, is a bit-identical wrapper around the raw
	// pointer — no late-resolve indirection). So callers must pass the *address*
	// of a variable holding the AActor*, not the AActor* value itself. Used to
	// build the TObjectKey<const AActor> argument for
	// UMassActorSubsystem::GetEntityHandleFromActor below, since
	// UMassReplicationSubsystem::FindEntity (NetID-based) only finds entities that
	// are actually being replicated to a remote client — useless in single-player,
	// where the local instance is the authority and most enemies' NetID is never
	// populated.
	constexpr const char* FWeakObjectPtr_AssignFObjectPtr =
		"40 53 48 83 EC 20 ?? ?? ?? 48 8B D9 48 85 D2 74 ?? 8B 52";

	// Class::Function  UMassActorSubsystem::GetEntityHandleFromActor
	// Parameters       (UMassActorSubsystem* this, FMassEntityHandle* result, TObjectKey<const AActor> Actor) -> FMassEntityHandle*
	// The authority-side actor->entity lookup — works for every Mass actor
	// regardless of whether it's being network-replicated, unlike
	// UMassReplicationSubsystem::FindEntity. TObjectKey is built via
	// FWeakObjectPtr_AssignFObjectPtr above; passed by value (8 bytes), same
	// calling convention as the old NetID-based lookup.
	constexpr const char* UMassActorSubsystem_GetEntityHandleFromActor =
		"48 89 5C 24 ?? 48 89 74 24 ?? 57 48 83 EC ?? 48 83 79 ?? ?? 49 8B D8 48 8B FA 48 8B F1 75 ?? 4C 8D 0D ?? ?? ?? ?? "
		"41 B8 ?? ?? ?? ?? 48 8D 15 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 ?? 90 ?? 48 8B 4E";

	// Class::Function  FMassEntityManager::TSharedFragmentsContainer<FConstSharedStruct>::FindOrAdd
	// Parameters       (TSharedFragmentsContainer<FConstSharedStruct>* self, uint32 Hash, const UScriptStruct* Type, const uint8* Data) -> FScriptContainerElement*
	// Looks up (or registers) the content-hashed const-shared-fragment block for a
	// struct value. Used to locate the existing FCrElectricityParameters block that
	// already-placed buildings reference, so it can be patched in place.
	constexpr const char* FMassEntityManager_ConstSharedFragments_FindOrAdd =
		"48 8B C4 48 89 58 ?? 48 89 68 ?? 89 50 ?? 56 57 41 54 41 56 41 57 48 83 EC ?? BD ?? ?? ?? ?? 45 33 F6";

	// -------------------------------------------------------------------------
	// Items
	// -------------------------------------------------------------------------

	// Class::Function  UAuItemsComponent::AddNewItem
	// Parameters       (UAuItemsComponent* this, TArray<FAuAddedItem>* result, const UAuItemDataBase* NewItem, uint32 Amount) -> TArray<FAuAddedItem>*
	// The real server-authoritative "give item" path: performs the HaveSpace check,
	// inserts into OwnedItems, marks the array dirty for replication, and broadcasts
	// OnItemAdded/OnItemAddedEx. Returns an empty array when the add was rejected
	// (e.g. not enough inventory space, or not authority).
	constexpr const char* AddNewItem =
		"44 89 4C 24 ?? 4C 89 44 24 ?? 53 56 57 41 55 41 56 48 81 EC ?? ?? ?? ??";

	// Class::Function  UCrUW_InventoryContainer::InitInventorySlots
	// Parameters       (UCrUW_InventoryContainer* this)
	// Builds the slot widgets for a container bound to a player inventory. The
	// only two callers are InitPlayer (UCrUW_Inventory::InitOnShow, which every
	// machine screen opens as well as the inventory itself) and
	// HandleOnInventorySizeChanged, so it runs each time the grid is drawn or
	// rebuilt. Ends on `lea r12, [rcx+468h]` -- the container's private
	// TWeakObjectPtr<UCrInventoryComponent>.
	constexpr const char* InventoryContainer_InitInventorySlots =
		"48 89 5C 24 ?? 48 89 6C 24 ?? 56 57 41 54 41 56 41 57 48 83 EC 70 4C 8D A1";

	// Class::Function  UE::StructUtils::GetStructInstanceCrc32
	// Parameters       (const UScriptStruct* ScriptStruct, const uint8* StructMemory, uint32 CRC) -> uint32
	// Same hashing function UCrElectricityTrait::BuildTemplate uses to key the
	// const-shared-fragment pool - used to re-derive the hash of the pre-edit
	// FCrElectricityParameters so the existing shared block can be found.
	constexpr const char* StructUtils_GetStructInstanceCrc32 =
		"48 89 5C 24 ?? 57 48 81 EC ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 ?? ?? ?? ?? 48 8B D9 48 8B FA 48 C1 E9";

	// -------------------------------------------------------------------------
	// Cheat manager (developer menus, debug builds only)
	// -------------------------------------------------------------------------

	// Class::Function  UCheatManager::InitCheatManager
	// Parameters       (UCheatManager* self)
	// APlayerController::AddCheats calls this immediately after constructing the
	// manager. It dispatches the ReceiveInitCheatManager blueprint event and then
	// broadcasts the static UCheatManager::OnCheatManagerCreatedDelegate, which is
	// how the rest of the game learns a cheat manager exists and binds to its
	// per-instance delegates (OnCheatMenuDelegate and friends). It is a plain C++
	// method rather than a UFunction, so ProcessEvent cannot reach it.
	//
	// This prologue is not especially distinctive, so the call site resolves it
	// with FindAllPatternsInMainModule and refuses to use an ambiguous match -
	// invoking the wrong function with a UCheatManager* in RCX would crash.
	constexpr const char* CheatManager_InitCheatManager =
		"48 89 5C 24 ?? 48 89 6C 24 ?? 56 57 41 56 48 83 EC ?? 48 8B 15";

} // namespace BetterCheats::AOB
