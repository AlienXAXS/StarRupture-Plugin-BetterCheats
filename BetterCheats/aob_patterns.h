#pragma once

// ---------------------------------------------------------------------------
// AOB Pattern Registry
//
// Format for each entry:
//   Pattern string  — byte sequence with ?? wildcards
//   Class::Function — fully qualified name of the function being located
//   Parameters      — function signature for reference
//
// All patterns are passed to IPluginScanner::FindPatternInMainModule.
// Add new patterns here; never scatter raw byte strings across feature files.
// ---------------------------------------------------------------------------

namespace BetterCheats::AOB
{
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

	// Class::Function  UCrBuildingComponent::GetResourceConditionResult
	// Parameters       (UCrBuildingComponent* this) -> EAuAPlacementConditionResult
	// Hooked to return Valid (1) when no-build-cost cheat is active.
	constexpr const char* GetResourceConditionResult =
		"48 8B C4 53 57 48 83 EC ?? 48 89 68 ?? 48 8B D9 48 89 70 ?? 4C 89 70";

	// Class::Function  UAuActorPlacementComponent::AddPoint
	// Parameters       (UAuActorPlacementComponent* this) -> FScriptContainerElement*
	// Called each time the player adds a foundation to the zoop chain.
	// Documented for reference; zoop limit is bypassed via MaxMultiConfirmPoints field writes
	// on the top-level UAuActorPlacementData and all its sub-variant pointers.
	constexpr const char* AddPoint =
		"48 89 5C 24 ?? 48 89 74 24 ?? 48 89 7C 24 ?? 4C 89 74 24 ?? 55 48 8D AC 24 ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? 48 8B D9";

	// Class::Function  ACrAPHelperActorBase::CheckStability
	// Parameters       (ACrAPHelperActorBase* this, const UAuActorPlacementData* PlacementData) -> bool
	// Native stability gate for the base placement helper actor. Computes the
	// stability graph result (or the simpler neighbour-trace result, depending on
	// UAuActorPlacementComponent::NewStability) and returns whether the current
	// placement is structurally valid. Hooked to force a `true` result — after
	// letting the original run so the HUD stability bar still reflects the real
	// computed value — when the No Stability Check cheat is active. Identical
	// prologue to the Custom overload below except for the final instruction.
	constexpr const char* CheckStability_Base =
		"48 8B C4 48 89 58 ?? 55 56 57 41 54 41 55 41 56 41 57 48 8D A8 ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? 0F 29 70 ?? 0F 29 78 ?? 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 85 ?? ?? ?? ?? 48 83 B9 ?? ?? ?? ?? ?? 4C 8B E2";

	// Class::Function  ACrAPHelperActorCustom::CheckStability
	// Parameters       (ACrAPHelperActorCustom* this, const UAuActorPlacementData* PlacementData) -> bool
	// Same role as ACrAPHelperActorBase::CheckStability above, for the "Custom"
	// placement helper actor (foundations/buildings using snap sockets).
	constexpr const char* CheckStability_Custom =
		"48 8B C4 48 89 58 ?? 55 56 57 41 54 41 55 41 56 41 57 48 8D A8 ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? 0F 29 70 ?? 0F 29 78 ?? 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 85 ?? ?? ?? ?? 48 83 B9 ?? ?? ?? ?? ?? 4C 8B EA";

	// Class::Function  ACrAPHelperActorCustom::CheckDynamicHelperStability
	// Parameters       (ACrAPHelperActorCustom* this) -> bool
	// Separate stability gate used by the multi-point/zoop "dynamic helper" placement
	// path (chained foundations) — does not go through ACrAPHelperActorCustom::CheckStability
	// at all. Builds a per-point connection graph and returns false if any connected
	// point's stability strength would drop to/below zero. Still calls
	// AAuAPHelperActor::SetStabilityStrength internally, so letting the original run
	// before overriding the return keeps the HUD strength value honest, same as the
	// two CheckStability hooks above.
	constexpr const char* CheckDynamicHelperStability =
		"40 55 41 54 48 8D AC 24 ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 85 ?? ?? ?? ?? ?? ?? ?? 4C 8B E1 48 89 4D";

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
		"48 89 5C 24 ?? 57 48 83 EC ?? 48 8B DA 48 8B F9 E8 ?? ?? ?? ?? 84 C0 75 ?? 48 8B D3";

	// -------------------------------------------------------------------------
	// Mining
	// -------------------------------------------------------------------------

	// Class::Function  UCrMiningComponent::GetMiningDamage
	// Parameters       (UCrMiningComponent* this, bool IsHittingWeakSpot)
	constexpr const char* GetMiningDamage =
		"40 55 56 57 41 56 48 8D 6C 24 ?? 48 81 EC ?? ?? ?? ?? 48 8B B9";

	// Class::Function  ACrCharacterPlayerBase::UpdateRepHarvesterHeatStack
	// Parameters       (ACrCharacterPlayerBase* this)
	constexpr const char* UpdateRepHarvesterHeatStack =
		"40 57 48 81 EC ?? ?? ?? ?? 48 8B F9 E8 ?? ?? ?? ?? 83 F8";

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

	// Class::Function  UE::StructUtils::GetStructInstanceCrc32
	// Parameters       (const UScriptStruct* ScriptStruct, const uint8* StructMemory, uint32 CRC) -> uint32
	// Same hashing function UCrElectricityTrait::BuildTemplate uses to key the
	// const-shared-fragment pool - used to re-derive the hash of the pre-edit
	// FCrElectricityParameters so the existing shared block can be found.
	constexpr const char* StructUtils_GetStructInstanceCrc32 =
		"48 89 5C 24 ?? 57 48 81 EC ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 ?? ?? ?? ?? 48 8B D9 48 8B FA 48 C1 E9";

} // namespace BetterCheats::AOB
