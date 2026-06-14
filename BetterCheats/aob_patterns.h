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
	// TEMP — UCrMassBuildingStabilityRemoveProcessorBase
	// -------------------------------------------------------------------------

	// Class::Function  UCrMassBuildingStabilityRemoveProcessorBase::GatherNeihbours
	// Parameters       (UCrMassBuildingStabilityRemoveProcessorBase* this, FCrMassPersistentEntityID* Entity, FCrBuildingGraphData* GraphData, FMassEntityManager* EntityManager)
	constexpr const char* GatherNeihbours = "40 53 57 41 54 41 56 48 81 EC ?? ?? ?? ?? ?? ?? ?? 4D 8B E1";

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
