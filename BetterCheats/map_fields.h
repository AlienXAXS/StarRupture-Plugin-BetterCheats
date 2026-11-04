#pragma once

#include "plugin_interface.h"
#include "Chimera_classes.hpp"
#include "ChimeraUI_classes.hpp"
#include "AuItems_classes.hpp"
#include "AuActorPlacement_classes.hpp"
#include "WBP_InventorySlot_classes.hpp"
#include "plugin_map.h"
#include "Engine_classes.hpp"
#include "MassAIPrototypeEnemyRuntime_classes.hpp"
#include <string>

// Every game-class member BetterCheats reads or writes through the SDK, taken
// from the loader's map for the running game build (see plugin_map.h).
// Resolved once in OnPluginLoadHooks; read on the game thread with
// field.In(object), which is null when the object is null or the member did
// not resolve. The key is the class that declares the member.

#define BC_FIELD(Class, Member) SR_FIELD(decltype(SDK::Class::Member), Class, Member, 0)

// A member the SDK dumps as padding, so it has no name or offset to compile in:
// the loader's map holds it under this key, and Offset is the row for the build
// this plugin was compiled against.
#define BC_NATIVE(Type, Class, Member, Offset) \
	PluginField<Type>{ { #Class "::" #Member, #Class, #Member, PLUGIN_FIELD_NATIVE, sizeof(Type), (Offset) } }

namespace BetterCheats::Fields
{
	// ACrCharacterPlayerBase
	inline auto CrCharacterPlayerBase_ADSMoveSpeed = BC_FIELD(ACrCharacterPlayerBase, ADSMoveSpeed);
	inline auto CrCharacterPlayerBase_AccelerationAndDeceleration = BC_FIELD(ACrCharacterPlayerBase, AccelerationAndDeceleration);
	inline auto CrCharacterPlayerBase_CrouchedMoveSpeed = BC_FIELD(ACrCharacterPlayerBase, CrouchedMoveSpeed);
	inline auto CrCharacterPlayerBase_DoubleJumpEnergyUsage = BC_FIELD(ACrCharacterPlayerBase, DoubleJumpEnergyUsage);
	inline auto CrCharacterPlayerBase_DurationOfAccelerationToSprint = BC_FIELD(ACrCharacterPlayerBase, DurationOfAccelerationToSprint);
	inline auto CrCharacterPlayerBase_EnergyLoweringMultiplierDuringSprint = BC_FIELD(ACrCharacterPlayerBase, EnergyLoweringMultiplierDuringSprint);
	inline auto CrCharacterPlayerBase_NormalEnergyRestorationRate = BC_FIELD(ACrCharacterPlayerBase, NormalEnergyRestorationRate);
	inline auto CrCharacterPlayerBase_NormalMoveSpeed = BC_FIELD(ACrCharacterPlayerBase, NormalMoveSpeed);
	inline auto CrCharacterPlayerBase_SprintMoveSpeed = BC_FIELD(ACrCharacterPlayerBase, SprintMoveSpeed);

	// UCrCharacterMovementComponent
	inline auto CrCharacterMovementComponent_CoyoteTime = BC_FIELD(UCrCharacterMovementComponent, CoyoteTime);
	inline auto CrCharacterMovementComponent_DashCooldownSeconds = BC_FIELD(UCrCharacterMovementComponent, DashCooldownSeconds);
	inline auto CrCharacterMovementComponent_DashDuration = BC_FIELD(UCrCharacterMovementComponent, DashDuration);
	inline auto CrCharacterMovementComponent_DashEnergyCostBase = BC_FIELD(UCrCharacterMovementComponent, DashEnergyCostBase);
	inline auto CrCharacterMovementComponent_DefaultZiplineAcceleration = BC_FIELD(UCrCharacterMovementComponent, DefaultZiplineAcceleration);
	inline auto CrCharacterMovementComponent_DefaultZiplineSpeed = BC_FIELD(UCrCharacterMovementComponent, DefaultZiplineSpeed);
	inline auto CrCharacterMovementComponent_JumpEnergyCost = BC_FIELD(UCrCharacterMovementComponent, JumpEnergyCost);
	inline auto CrCharacterMovementComponent_MaxDashDistance = BC_FIELD(UCrCharacterMovementComponent, MaxDashDistance);
	inline auto CrCharacterMovementComponent_MaxSlideSpeed = BC_FIELD(UCrCharacterMovementComponent, MaxSlideSpeed);
	inline auto CrCharacterMovementComponent_SlideEnterImpulse = BC_FIELD(UCrCharacterMovementComponent, SlideEnterImpulse);
	inline auto CrCharacterMovementComponent_SlideFrictionFactor = BC_FIELD(UCrCharacterMovementComponent, SlideFrictionFactor);
	inline auto CrCharacterMovementComponent_SlideGravityForce = BC_FIELD(UCrCharacterMovementComponent, SlideGravityForce);

	// UCrGemAttributeSet
	inline auto CrGemAttributeSet_DodgeCostMultiplier = BC_FIELD(UCrGemAttributeSet, DodgeCostMultiplier);
	inline auto CrGemAttributeSet_DoubleJumpCostMultiplier = BC_FIELD(UCrGemAttributeSet, DoubleJumpCostMultiplier);
	inline auto CrGemAttributeSet_FallDamageMultiplier = BC_FIELD(UCrGemAttributeSet, FallDamageMultiplier);
	inline auto CrGemAttributeSet_SlideStaminaRegenMultiplier = BC_FIELD(UCrGemAttributeSet, SlideStaminaRegenMultiplier);
	inline auto CrGemAttributeSet_StaminaRegenMultiplier = BC_FIELD(UCrGemAttributeSet, StaminaRegenMultiplier);
	inline auto CrGemAttributeSet_ZiplineSpeedMultiplier = BC_FIELD(UCrGemAttributeSet, ZiplineSpeedMultiplier);

	// UCrMovementAttributeSet
	inline auto CrMovementAttributeSet_JumpMultiplier = BC_FIELD(UCrMovementAttributeSet, JumpMultiplier);
	inline auto CrMovementAttributeSet_SprintSpeedMultiplier = BC_FIELD(UCrMovementAttributeSet, SprintSpeedMultiplier);

	// UCrMovementSpeedMultiplierAttributeSet
	inline auto CrMovementSpeedMultiplierAttributeSet_CurrentMovementSpeedMultiplier = BC_FIELD(UCrMovementSpeedMultiplierAttributeSet, CurrentMovementSpeedMultiplier);
	inline auto CrMovementSpeedMultiplierAttributeSet_MaxMovementSpeedMultiplier = BC_FIELD(UCrMovementSpeedMultiplierAttributeSet, MaxMovementSpeedMultiplier);
	inline auto CrMovementSpeedMultiplierAttributeSet_MinMovementSpeedMultiplier = BC_FIELD(UCrMovementSpeedMultiplierAttributeSet, MinMovementSpeedMultiplier);

	// UCrWeaponAttributeSet
	inline auto CrWeaponAttributeSet_ADS_TransitionSpeedModMultiplier = BC_FIELD(UCrWeaponAttributeSet, ADS_TransitionSpeedModMultiplier);
	inline auto CrWeaponAttributeSet_DamageFallOffModMultiplier = BC_FIELD(UCrWeaponAttributeSet, DamageFallOffModMultiplier);
	inline auto CrWeaponAttributeSet_DamageModMultiplier = BC_FIELD(UCrWeaponAttributeSet, DamageModMultiplier);
	inline auto CrWeaponAttributeSet_FireRateModMultiplier = BC_FIELD(UCrWeaponAttributeSet, FireRateModMultiplier);
	inline auto CrWeaponAttributeSet_MaxMagAmmoModOffset = BC_FIELD(UCrWeaponAttributeSet, MaxMagAmmoModOffset);
	inline auto CrWeaponAttributeSet_PossibleEnemiesHitPerTrace = BC_FIELD(UCrWeaponAttributeSet, PossibleEnemiesHitPerTrace);
	inline auto CrWeaponAttributeSet_RecoilModMultiplier = BC_FIELD(UCrWeaponAttributeSet, RecoilModMultiplier);
	inline auto CrWeaponAttributeSet_ReloadSpeedModMultiplier = BC_FIELD(UCrWeaponAttributeSet, ReloadSpeedModMultiplier);
	inline auto CrWeaponAttributeSet_SpreadModMultiplier = BC_FIELD(UCrWeaponAttributeSet, SpreadModMultiplier);
	inline auto CrWeaponAttributeSet_SwayModMultiplier = BC_FIELD(UCrWeaponAttributeSet, SwayModMultiplier);

	// UCrGameInstance
	inline auto CrGameInstance_ServerSessionName = BC_NATIVE(SDK::FString, UCrGameInstance, ServerSessionName, 0x290);

	// ACrTechnologyKeeper
	inline auto CrTechnologyKeeper_bUnlockAllBuildings = BC_NATIVE(bool, ACrTechnologyKeeper, bUnlockAllBuildings, 0x3A8);

	// UCrEnviroWaveTimerSubsystem: the countdown to the next rupture. The SDK folds
	// everything before TimerActor into Pad_30; Tick() only decrements NextWaveTimer
	// while !bPause && bWaitingForNextWave, and the block is persisted into the save,
	// so a bad value survives a reload. WaitingDuration (0x78 on CL-127004) is not
	// a member of this class on CL-128786, and a native that cannot resolve would
	// refuse the whole plugin there, so it is not requested: world_wave.cpp keeps
	// the calm length it reads from NextWaveTimer instead.
	inline auto CrEnviroWaveTimerSubsystem_NextWaveTimer       = BC_NATIVE(float, UCrEnviroWaveTimerSubsystem, NextWaveTimer, 0x7C);
	inline auto CrEnviroWaveTimerSubsystem_bWaitingForNextWave = BC_NATIVE(bool, UCrEnviroWaveTimerSubsystem, bWaitingForNextWave, 0x80);
	inline auto CrEnviroWaveTimerSubsystem_bPause              = BC_NATIVE(bool, UCrEnviroWaveTimerSubsystem, bPause, 0x81);
	inline auto CrEnviroWaveTimerSubsystem_bStopWaves          = BC_NATIVE(bool, UCrEnviroWaveTimerSubsystem, bStopWaves, 0x83);

	// FMassEnemyHealthFragment: a Mass fragment, not a UObject, so Dumper-7 pads it out as
	// unnamed bytes and HP (the first member) has no name in the SDK. Confirmed from the PDB.
	inline auto MassEnemyHealthFragment_HP = BC_NATIVE(float, FMassEnemyHealthFragment, HP, 0x0);

	// UCrUW_InventoryContainer: a private TWeakObjectPtr<UCrInventoryComponent>
	inline auto CrUW_InventoryContainer_InventoryComponent = BC_NATIVE(SDK::FWeakObjectPtr, UCrUW_InventoryContainer, InventoryComponent, 0x468);

	// Members the plugin reads and writes by name, one row each for the running build.

	// ACrCharacterPlayerBase
	inline auto CrCharacterPlayerBase_InventoryComponent = BC_FIELD(ACrCharacterPlayerBase, InventoryComponent);
	inline auto CrCharacterPlayerBase_HiddenInventoryComponent = BC_FIELD(ACrCharacterPlayerBase, HiddenInventoryComponent);
	inline auto CrCharacterPlayerBase_WeaponSystem = BC_FIELD(ACrCharacterPlayerBase, WeaponSystem);
	inline auto CrCharacterPlayerBase_WeaponAttributes = BC_FIELD(ACrCharacterPlayerBase, WeaponAttributes);
	inline auto CrCharacterPlayerBase_MiningBoostAttributes = BC_FIELD(ACrCharacterPlayerBase, MiningBoostAttributes);
	inline auto CrCharacterPlayerBase_GemAttributes = BC_FIELD(ACrCharacterPlayerBase, GemAttributes);
	inline auto CrCharacterPlayerBase_MovementAttributes = BC_FIELD(ACrCharacterPlayerBase, MovementAttributes);
	inline auto CrCharacterPlayerBase_MovementSpeedMultiplierAttributes = BC_FIELD(ACrCharacterPlayerBase, MovementSpeedMultiplierAttributes);
	inline auto CrCharacterPlayerBase_EnergyLogicComponent = BC_FIELD(ACrCharacterPlayerBase, EnergyLogicComponent);
	inline auto CrCharacterPlayerBase_CrCharacterMovementComponent = BC_FIELD(ACrCharacterPlayerBase, CrCharacterMovementComponent);
	inline auto CrCharacterPlayerBase_GrenadeChargeAttributes = BC_FIELD(ACrCharacterPlayerBase, GrenadeChargeAttributes);
	inline auto CrCharacterPlayerBase_HealthAttributes = BC_FIELD(ACrCharacterPlayerBase, HealthAttributes);
	inline auto CrCharacterPlayerBase_EnergyAttributes = BC_FIELD(ACrCharacterPlayerBase, EnergyAttributes);
	inline auto CrCharacterPlayerBase_ShieldAttributes = BC_FIELD(ACrCharacterPlayerBase, ShieldAttributes);
	inline auto CrCharacterPlayerBase_HydrationAttributes = BC_FIELD(ACrCharacterPlayerBase, HydrationAttributes);
	inline auto CrCharacterPlayerBase_CaloriesAttributes = BC_FIELD(ACrCharacterPlayerBase, CaloriesAttributes);
	inline auto CrCharacterPlayerBase_OxygenAttributes = BC_FIELD(ACrCharacterPlayerBase, OxygenAttributes);
	inline auto CrCharacterPlayerBase_ToxicityAttributes = BC_FIELD(ACrCharacterPlayerBase, ToxicityAttributes);
	inline auto CrCharacterPlayerBase_RadiationAttributes = BC_FIELD(ACrCharacterPlayerBase, RadiationAttributes);
	inline auto CrCharacterPlayerBase_HeatAttributes = BC_FIELD(ACrCharacterPlayerBase, HeatAttributes);
	inline auto CrCharacterPlayerBase_DrainAttributes = BC_FIELD(ACrCharacterPlayerBase, DrainAttributes);
	inline auto CrCharacterPlayerBase_CorrosionAttributes = BC_FIELD(ACrCharacterPlayerBase, CorrosionAttributes);
	inline auto CrCharacterPlayerBase_InfectionAttributes = BC_FIELD(ACrCharacterPlayerBase, InfectionAttributes);
	inline auto CrCharacterPlayerBase_TemperatureAttributes = BC_FIELD(ACrCharacterPlayerBase, TemperatureAttributes);

	// UCrEnergyAttributeSet
	inline auto CrEnergyAttributeSet_CurrentEnergy = BC_FIELD(UCrEnergyAttributeSet, CurrentEnergy);
	inline auto CrEnergyAttributeSet_MaxEnergy = BC_FIELD(UCrEnergyAttributeSet, MaxEnergy);

	// UCrShieldAttributeSet
	inline auto CrShieldAttributeSet_CurrentShield = BC_FIELD(UCrShieldAttributeSet, CurrentShield);
	inline auto CrShieldAttributeSet_MaxShield = BC_FIELD(UCrShieldAttributeSet, MaxShield);

	// UCrHydrationAttributeSet
	inline auto CrHydrationAttributeSet_CurrentHydration = BC_FIELD(UCrHydrationAttributeSet, CurrentHydration);
	inline auto CrHydrationAttributeSet_MaxHydration = BC_FIELD(UCrHydrationAttributeSet, MaxHydration);

	// UCrCaloriesAttributeSet
	inline auto CrCaloriesAttributeSet_CurrentCalories = BC_FIELD(UCrCaloriesAttributeSet, CurrentCalories);
	inline auto CrCaloriesAttributeSet_MaxCalories = BC_FIELD(UCrCaloriesAttributeSet, MaxCalories);

	// UCrOxygenAttributeSet
	inline auto CrOxygenAttributeSet_CurrentOxygen = BC_FIELD(UCrOxygenAttributeSet, CurrentOxygen);
	inline auto CrOxygenAttributeSet_MaxOxygen = BC_FIELD(UCrOxygenAttributeSet, MaxOxygen);

	// UCrToxicityAttributeSet
	inline auto CrToxicityAttributeSet_CurrentToxicity = BC_FIELD(UCrToxicityAttributeSet, CurrentToxicity);
	inline auto CrToxicityAttributeSet_MaxToxicity = BC_FIELD(UCrToxicityAttributeSet, MaxToxicity);
	inline auto CrToxicityAttributeSet_MinToxicity = BC_FIELD(UCrToxicityAttributeSet, MinToxicity);

	// UCrRadiationAttributeSet
	inline auto CrRadiationAttributeSet_CurrentRadiation = BC_FIELD(UCrRadiationAttributeSet, CurrentRadiation);
	inline auto CrRadiationAttributeSet_MaxRadiation = BC_FIELD(UCrRadiationAttributeSet, MaxRadiation);
	inline auto CrRadiationAttributeSet_MinRadiation = BC_FIELD(UCrRadiationAttributeSet, MinRadiation);

	// UCrHeatAttributeSet
	inline auto CrHeatAttributeSet_CurrentHeat = BC_FIELD(UCrHeatAttributeSet, CurrentHeat);
	inline auto CrHeatAttributeSet_MaxHeat = BC_FIELD(UCrHeatAttributeSet, MaxHeat);
	inline auto CrHeatAttributeSet_MinHeat = BC_FIELD(UCrHeatAttributeSet, MinHeat);

	// UCrDrainAttributeSet
	inline auto CrDrainAttributeSet_CurrentDrain = BC_FIELD(UCrDrainAttributeSet, CurrentDrain);
	inline auto CrDrainAttributeSet_MaxDrain = BC_FIELD(UCrDrainAttributeSet, MaxDrain);
	inline auto CrDrainAttributeSet_MinDrain = BC_FIELD(UCrDrainAttributeSet, MinDrain);

	// UCrCorrosionAttributeSet
	inline auto CrCorrosionAttributeSet_CurrentCorrosion = BC_FIELD(UCrCorrosionAttributeSet, CurrentCorrosion);
	inline auto CrCorrosionAttributeSet_MaxCorrosion = BC_FIELD(UCrCorrosionAttributeSet, MaxCorrosion);
	inline auto CrCorrosionAttributeSet_MinCorrosion = BC_FIELD(UCrCorrosionAttributeSet, MinCorrosion);

	// UCrInfectionAttributeSet
	inline auto CrInfectionAttributeSet_CurrentInfection = BC_FIELD(UCrInfectionAttributeSet, CurrentInfection);
	inline auto CrInfectionAttributeSet_MaxInfection = BC_FIELD(UCrInfectionAttributeSet, MaxInfection);
	inline auto CrInfectionAttributeSet_MinInfection = BC_FIELD(UCrInfectionAttributeSet, MinInfection);

	// UCrTemperatureAttributeSet
	inline auto CrTemperatureAttributeSet_CurrentTemperature = BC_FIELD(UCrTemperatureAttributeSet, CurrentTemperature);
	inline auto CrTemperatureAttributeSet_MaxTemperature = BC_FIELD(UCrTemperatureAttributeSet, MaxTemperature);
	inline auto CrTemperatureAttributeSet_MinTemperature = BC_FIELD(UCrTemperatureAttributeSet, MinTemperature);

	// UCrHealthAttributeSet
	inline auto CrHealthAttributeSet_CurrentHealth = BC_FIELD(UCrHealthAttributeSet, CurrentHealth);
	inline auto CrHealthAttributeSet_MaxHealth = BC_FIELD(UCrHealthAttributeSet, MaxHealth);

	// UCrMiningBoostAttributeSet
	inline auto CrMiningBoostAttributeSet_CurrentBoostMultiplierValue = BC_FIELD(UCrMiningBoostAttributeSet, CurrentBoostMultiplierValue);
	inline auto CrMiningBoostAttributeSet_MaxBoostMultiplierValue = BC_FIELD(UCrMiningBoostAttributeSet, MaxBoostMultiplierValue);

	// UCrGrenadeChargeAttributeSet
	inline auto CrGrenadeChargeAttributeSet_CurrentGrenadeCharge = BC_FIELD(UCrGrenadeChargeAttributeSet, CurrentGrenadeCharge);
	inline auto CrGrenadeChargeAttributeSet_MaxGrenadeCharge = BC_FIELD(UCrGrenadeChargeAttributeSet, MaxGrenadeCharge);
	inline auto CrGrenadeChargeAttributeSet_MinGrenadeCharge = BC_FIELD(UCrGrenadeChargeAttributeSet, MinGrenadeCharge);

	// UCrWeaponAttributeSet
	inline auto CrWeaponAttributeSet_AggroPerShotModMultiplier = BC_FIELD(UCrWeaponAttributeSet, AggroPerShotModMultiplier);

	// ACrAIBase
	inline auto CrAIBase_BaseAttributes = BC_FIELD(ACrAIBase, BaseAttributes);

	// UCrCharacterMovementComponent
	inline auto CrCharacterMovementComponent_MaxAirDashesToExecute = BC_FIELD(UCrCharacterMovementComponent, MaxAirDashesToExecute);

	// UCrEnergyLogicComponent
	inline auto CrEnergyLogicComponent_DelayBeforeEnergyRegeneration = BC_FIELD(UCrEnergyLogicComponent, DelayBeforeEnergyRegeneration);

	// UCrWeaponComponent
	inline auto CrWeaponComponent_LastEquippedWeaponData = BC_FIELD(UCrWeaponComponent, LastEquippedWeaponData);

	// UAuWeaponItemDataBase
	inline auto AuWeaponItemDataBase_BaseDamage = BC_FIELD(UAuWeaponItemDataBase, BaseDamage);
	inline auto AuWeaponItemDataBase_BaseRange = BC_FIELD(UAuWeaponItemDataBase, BaseRange);
	inline auto AuWeaponItemDataBase_RoundsPerMinute = BC_FIELD(UAuWeaponItemDataBase, RoundsPerMinute);
	inline auto AuWeaponItemDataBase_BaseMagazine = BC_FIELD(UAuWeaponItemDataBase, BaseMagazine);
	inline auto AuWeaponItemDataBase_BaseDamageFalloff = BC_FIELD(UAuWeaponItemDataBase, BaseDamageFalloff);
	inline auto AuWeaponItemDataBase_ReloadTime = BC_FIELD(UAuWeaponItemDataBase, ReloadTime);
	inline auto AuWeaponItemDataBase_Spread = BC_FIELD(UAuWeaponItemDataBase, Spread);
	inline auto AuWeaponItemDataBase_AimCameraSpeed = BC_FIELD(UAuWeaponItemDataBase, AimCameraSpeed);
	inline auto AuWeaponItemDataBase_RequiredItem = BC_FIELD(UAuWeaponItemDataBase, RequiredItem);

	// UCrWeaponItemDataBase
	inline auto CrWeaponItemDataBase_MiningTypeDamage = BC_FIELD(UCrWeaponItemDataBase, MiningTypeDamage);
	inline auto CrWeaponItemDataBase_WeakpointDamageMultiplier = BC_FIELD(UCrWeaponItemDataBase, WeakpointDamageMultiplier);
	inline auto CrWeaponItemDataBase_ShotAggro = BC_FIELD(UCrWeaponItemDataBase, ShotAggro);
	inline auto CrWeaponItemDataBase_PossibleEnemiesHitPerTrace = BC_FIELD(UCrWeaponItemDataBase, PossibleEnemiesHitPerTrace);

	// UCrGrenadeWeaponItemDataBase
	inline auto CrGrenadeWeaponItemDataBase_GrenadeThrowCostOfGrenadeCharge = BC_FIELD(UCrGrenadeWeaponItemDataBase, GrenadeThrowCostOfGrenadeCharge);

	// UCrWeaponModDataAsset
	inline auto CrWeaponModDataAsset_BaseDamageFalloffCurveOverride = BC_FIELD(UCrWeaponModDataAsset, BaseDamageFalloffCurveOverride);

	// UCrInventoryComponent
	inline auto CrInventoryComponent_GridColumns = BC_FIELD(UCrInventoryComponent, GridColumns);
	inline auto CrInventoryComponent_GridRows = BC_FIELD(UCrInventoryComponent, GridRows);
	inline auto CrInventoryComponent_Slots = BC_FIELD(UCrInventoryComponent, Slots);

	// UCrUW_InventoryContainer
	inline auto CrUW_InventoryContainer_ItemGridPanel = BC_FIELD(UCrUW_InventoryContainer, ItemGridPanel);

	// UAuItemDataBase
	inline auto AuItemDataBase_UniqueItemName = BC_FIELD(UAuItemDataBase, UniqueItemName);
	inline auto AuItemDataBase_ItemName = BC_FIELD(UAuItemDataBase, ItemName);
	inline auto AuItemDataBase_MaxStack = BC_FIELD(UAuItemDataBase, MaxStack);
	inline auto AuItemDataBase_StackingType = BC_FIELD(UAuItemDataBase, StackingType);
	inline auto AuItemDataBase_UIItemType = BC_FIELD(UAuItemDataBase, UIItemType);
	inline auto AuItemDataBase_ItemIcon = BC_FIELD(UAuItemDataBase, ItemIcon);

	// ACrGatherableBaseActor
	inline auto CrGatherableBaseActor_ResourceCount = BC_FIELD(ACrGatherableBaseActor, ResourceCount);

	// FAuAddedItem
	inline auto AuAddedItem_Amount = BC_FIELD(FAuAddedItem, Amount);

	// UCrElectricityTrait
	inline auto CrElectricityTrait_Parameters = BC_FIELD(UCrElectricityTrait, Parameters);

	// FCrElectricityParameters
	inline auto CrElectricityParameters_ElectricityValue = BC_FIELD(FCrElectricityParameters, ElectricityValue);
	inline auto CrElectricityParameters_Type = BC_FIELD(FCrElectricityParameters, Type);

	// UAuActorPlacementData
	inline auto AuActorPlacementData_EntityType = BC_FIELD(UAuActorPlacementData, EntityType);
	inline auto AuActorPlacementData_BuildingID = BC_FIELD(UAuActorPlacementData, BuildingID);

	// FAuAPMassSpawnedEntityType
	inline auto AuAPMassSpawnedEntityType_EntityConfig = BC_FIELD(FAuAPMassSpawnedEntityType, EntityConfig);

	// UCrBuildingData
	inline auto CrBuildingData_BuildingName = BC_FIELD(UCrBuildingData, BuildingName);

	// ACrAPHelper
	inline auto CrAPHelper_BuildingData = BC_FIELD(ACrAPHelper, BuildingData);
	// Absent on some builds (CL-128786 has no such member): found by name on first
	// use, null there, and the one feature that reads it (Features::BuildAnywhere,
	// "Place windows and benches anywhere") leaves collision alone.
	inline PluginLazyField<SDK::TArray<SDK::AActor*>> CrAPHelper_SnappedActors{ "SnappedActors" };

	// ACrPlayerControllerBase
	inline auto CrPlayerControllerBase_PlayerSkills = BC_FIELD(ACrPlayerControllerBase, PlayerSkills);

	// FCrSkillData
	inline auto CrSkillData_Skill = BC_FIELD(FCrSkillData, Skill);
	inline auto CrSkillData_Level = BC_FIELD(FCrSkillData, Level);
	inline auto CrSkillData_Experience = BC_FIELD(FCrSkillData, Experience);

	// ACrGameStateBase
	inline auto CrGameStateBase_CorporationsOwner = BC_FIELD(ACrGameStateBase, CorporationsOwner);
	inline auto CrGameStateBase_TechnologyKeeper = BC_FIELD(ACrGameStateBase, TechnologyKeeper);

	// ACrCorporationsOwner
	inline auto CrCorporationsOwner_DataPoints = BC_FIELD(ACrCorporationsOwner, DataPoints);

	// UCrEnviroWaveTimerSubsystem
	inline auto CrEnviroWaveTimerSubsystem_TimerActor = BC_FIELD(UCrEnviroWaveTimerSubsystem, TimerActor);

	// ACrWaveTimerActor
	inline auto CrWaveTimerActor_bPause = BC_FIELD(ACrWaveTimerActor, bPause);

	// UAuWeaponItemDataBase (flag)
	inline auto AuWeaponItemDataBase_bIsUsingDamageFalloff = BC_FIELD(UAuWeaponItemDataBase, bIsUsingDamageFalloff);

	// AMassEnemyCharacterBase
	inline auto MassEnemyCharacterBase_bIsInPool = BC_FIELD(AMassEnemyCharacterBase, bIsInPool);
	inline auto MassEnemyCharacterBase_AllowedAttackDistance = BC_FIELD(AMassEnemyCharacterBase, AllowedAttackDistance);
	inline auto MassEnemyCharacterBase_AllowedAttackConeHalfAngle = BC_FIELD(AMassEnemyCharacterBase, AllowedAttackConeHalfAngle);
	inline auto MassEnemyCharacterBase_AllowedBuildingAttackDistance = BC_FIELD(AMassEnemyCharacterBase, AllowedBuildingAttackDistance);

	// UWBP_InventorySlot_C: a Blueprint member, found on first use through the loader's property reader.
	inline PluginLazyField<SDK::USizeBox*> WBP_InventorySlot_SizeBox{ "SizeBox" };

	// Structs the plugin copies or indexes by value; they fail the plugin when the
	// running build's size differs from the one compiled in.
	inline auto AuAddedItem_Size             = SR_SIZE_CHECK(FAuAddedItem);
	inline auto CrSkillData_Size             = SR_SIZE_CHECK(FCrSkillData);
	inline auto CrElectricityParameters_Size = SR_SIZE_CHECK(FCrElectricityParameters);

	// The member's value, or a default when the object is null or the member did not resolve.
	using PluginMap::ValueOr;

	// The Value of a FScalableFloat member, or null when the object is null or the member did not resolve.
	// FScalableFloat itself is an engine type, so its own layout stays compiled in.
	template <class Object>
	float* ScalableValue(const PluginField<SDK::FScalableFloat>& field, Object* object)
	{
		SDK::FScalableFloat* at = field.In(static_cast<void*>(object));
		return at ? &at->Value : nullptr;
	}

	template <class Object>
	float ScalableOr(const PluginField<SDK::FScalableFloat>& field, const Object* object, float fallback = 0.0f)
	{
		const SDK::FScalableFloat* at = field.In(static_cast<const void*>(object));
		return at ? at->Value : fallback;
	}

	// Writes the member; false when the object is null or the member did not resolve.
	template <class T, class Object>
	bool Store(const PluginField<T>& field, Object* object, const T& value)
	{
		T* at = field.In(static_cast<void*>(object));
		if (!at)
			return false;
		*at = value;
		return true;
	}

	// CurrentValue of a FGameplayAttributeData member, or the fallback. FGameplayAttributeData is an engine type.
	template <class Object>
	float AttrValueOr(const PluginField<SDK::FGameplayAttributeData>& field, const Object* object, float fallback = 0.0f)
	{
		const SDK::FGameplayAttributeData* at = field.In(static_cast<const void*>(object));
		return at ? at->CurrentValue : fallback;
	}

	// Number of slots in an inventory component.
	inline int SlotCount(const SDK::UCrInventoryComponent* inventory)
	{
		const auto* slots = CrInventoryComponent_Slots.In(static_cast<const void*>(inventory));
		return slots ? slots->Num() : 0;
	}

	// An item's unique name, or empty.
	inline std::string UniqueNameOf(const SDK::UAuItemDataBase* item)
	{
		const SDK::FName* name = AuItemDataBase_UniqueItemName.In(static_cast<const void*>(item));
		return name ? name->ToString() : std::string();
	}

	// An item's localised display name, or empty.
	inline std::string DisplayNameOf(const SDK::UAuItemDataBase* item)
	{
		const SDK::FText* text = AuItemDataBase_ItemName.In(static_cast<const void*>(item));
		return text ? SDK::UKismetTextLibrary::Conv_TextToString(*text).ToString() : std::string();
	}

	// A reference to the member, or to a scratch value when the pointer is null (object null or member
	// unresolved), so a compose helper that wants a float& never dereferences null.
	// The scratch is shared by every caller of the same type that gets a null, so
	// only pass a pointer that was checked or whose result is discarded.
	template <class T>
	T& Ref(T* at)
	{
		static T scratch{};
		return at ? *at : scratch;
	}

	// Asks the loader for every row above. False when any did not resolve; the
	// loader has then recorded the failure and refuses the plugin.
	inline bool Resolve(IPluginSelf* self, IPluginHookScanner* scanner)
	{
		bool all = PluginMap::ResolveFields(scanner, self,
			CrGameInstance_ServerSessionName, CrTechnologyKeeper_bUnlockAllBuildings,
			CrEnviroWaveTimerSubsystem_NextWaveTimer, CrEnviroWaveTimerSubsystem_bWaitingForNextWave, CrEnviroWaveTimerSubsystem_bPause,
			CrEnviroWaveTimerSubsystem_bStopWaves, CrUW_InventoryContainer_InventoryComponent,
			MassEnemyHealthFragment_HP,
			AuAddedItem_Size, CrSkillData_Size, CrElectricityParameters_Size,
			CrCharacterPlayerBase_ADSMoveSpeed, CrCharacterPlayerBase_AccelerationAndDeceleration, CrCharacterPlayerBase_CrouchedMoveSpeed,
			CrCharacterPlayerBase_DoubleJumpEnergyUsage, CrCharacterPlayerBase_DurationOfAccelerationToSprint, CrCharacterPlayerBase_EnergyLoweringMultiplierDuringSprint,
			CrCharacterPlayerBase_NormalEnergyRestorationRate, CrCharacterPlayerBase_NormalMoveSpeed, CrCharacterPlayerBase_SprintMoveSpeed,
			CrCharacterMovementComponent_CoyoteTime, CrCharacterMovementComponent_DashCooldownSeconds, CrCharacterMovementComponent_DashDuration,
			CrCharacterMovementComponent_DashEnergyCostBase, CrCharacterMovementComponent_DefaultZiplineAcceleration, CrCharacterMovementComponent_DefaultZiplineSpeed,
			CrCharacterMovementComponent_JumpEnergyCost, CrCharacterMovementComponent_MaxDashDistance, CrCharacterMovementComponent_MaxSlideSpeed,
			CrCharacterMovementComponent_SlideEnterImpulse, CrCharacterMovementComponent_SlideFrictionFactor, CrCharacterMovementComponent_SlideGravityForce,
			CrGemAttributeSet_DodgeCostMultiplier, CrGemAttributeSet_DoubleJumpCostMultiplier, CrGemAttributeSet_FallDamageMultiplier,
			CrGemAttributeSet_SlideStaminaRegenMultiplier, CrGemAttributeSet_StaminaRegenMultiplier, CrGemAttributeSet_ZiplineSpeedMultiplier,
			CrMovementAttributeSet_JumpMultiplier, CrMovementAttributeSet_SprintSpeedMultiplier, CrMovementSpeedMultiplierAttributeSet_CurrentMovementSpeedMultiplier,
			CrMovementSpeedMultiplierAttributeSet_MaxMovementSpeedMultiplier, CrMovementSpeedMultiplierAttributeSet_MinMovementSpeedMultiplier, CrWeaponAttributeSet_ADS_TransitionSpeedModMultiplier,
			CrWeaponAttributeSet_DamageFallOffModMultiplier, CrWeaponAttributeSet_DamageModMultiplier, CrWeaponAttributeSet_FireRateModMultiplier,
			CrWeaponAttributeSet_MaxMagAmmoModOffset, CrWeaponAttributeSet_PossibleEnemiesHitPerTrace, CrWeaponAttributeSet_RecoilModMultiplier,
			CrWeaponAttributeSet_ReloadSpeedModMultiplier, CrWeaponAttributeSet_SpreadModMultiplier, CrWeaponAttributeSet_SwayModMultiplier
		);
		all = PluginMap::ResolveFields(scanner, self,
			AuWeaponItemDataBase_bIsUsingDamageFalloff, MassEnemyCharacterBase_bIsInPool, MassEnemyCharacterBase_AllowedAttackDistance,
			MassEnemyCharacterBase_AllowedAttackConeHalfAngle, MassEnemyCharacterBase_AllowedBuildingAttackDistance,
			CrCharacterPlayerBase_InventoryComponent, CrCharacterPlayerBase_HiddenInventoryComponent, CrCharacterPlayerBase_WeaponSystem,
			CrCharacterPlayerBase_WeaponAttributes, CrCharacterPlayerBase_MiningBoostAttributes, CrCharacterPlayerBase_GemAttributes,
			CrCharacterPlayerBase_MovementAttributes, CrCharacterPlayerBase_MovementSpeedMultiplierAttributes, CrCharacterPlayerBase_EnergyLogicComponent,
			CrCharacterPlayerBase_CrCharacterMovementComponent, CrCharacterPlayerBase_GrenadeChargeAttributes, CrCharacterPlayerBase_HealthAttributes,
			CrCharacterPlayerBase_EnergyAttributes, CrCharacterPlayerBase_ShieldAttributes, CrCharacterPlayerBase_HydrationAttributes,
			CrCharacterPlayerBase_CaloriesAttributes, CrCharacterPlayerBase_OxygenAttributes, CrCharacterPlayerBase_ToxicityAttributes,
			CrCharacterPlayerBase_RadiationAttributes, CrCharacterPlayerBase_HeatAttributes, CrCharacterPlayerBase_DrainAttributes,
			CrCharacterPlayerBase_CorrosionAttributes, CrCharacterPlayerBase_InfectionAttributes, CrCharacterPlayerBase_TemperatureAttributes,
			CrEnergyAttributeSet_CurrentEnergy, CrEnergyAttributeSet_MaxEnergy, CrShieldAttributeSet_CurrentShield,
			CrShieldAttributeSet_MaxShield, CrHydrationAttributeSet_CurrentHydration, CrHydrationAttributeSet_MaxHydration,
			CrCaloriesAttributeSet_CurrentCalories, CrCaloriesAttributeSet_MaxCalories, CrOxygenAttributeSet_CurrentOxygen,
			CrOxygenAttributeSet_MaxOxygen, CrToxicityAttributeSet_CurrentToxicity, CrToxicityAttributeSet_MaxToxicity,
			CrToxicityAttributeSet_MinToxicity, CrRadiationAttributeSet_CurrentRadiation, CrRadiationAttributeSet_MaxRadiation,
			CrRadiationAttributeSet_MinRadiation, CrHeatAttributeSet_CurrentHeat, CrHeatAttributeSet_MaxHeat,
			CrHeatAttributeSet_MinHeat, CrDrainAttributeSet_CurrentDrain, CrDrainAttributeSet_MaxDrain,
			CrDrainAttributeSet_MinDrain, CrCorrosionAttributeSet_CurrentCorrosion, CrCorrosionAttributeSet_MaxCorrosion,
			CrCorrosionAttributeSet_MinCorrosion, CrInfectionAttributeSet_CurrentInfection, CrInfectionAttributeSet_MaxInfection,
			CrInfectionAttributeSet_MinInfection, CrTemperatureAttributeSet_CurrentTemperature, CrTemperatureAttributeSet_MaxTemperature,
			CrTemperatureAttributeSet_MinTemperature, CrHealthAttributeSet_CurrentHealth, CrHealthAttributeSet_MaxHealth,
			CrMiningBoostAttributeSet_CurrentBoostMultiplierValue, CrMiningBoostAttributeSet_MaxBoostMultiplierValue, CrGrenadeChargeAttributeSet_CurrentGrenadeCharge,
			CrGrenadeChargeAttributeSet_MaxGrenadeCharge, CrGrenadeChargeAttributeSet_MinGrenadeCharge, CrWeaponAttributeSet_AggroPerShotModMultiplier,
			CrAIBase_BaseAttributes, CrCharacterMovementComponent_MaxAirDashesToExecute, CrEnergyLogicComponent_DelayBeforeEnergyRegeneration,
			CrWeaponComponent_LastEquippedWeaponData, AuWeaponItemDataBase_BaseDamage, AuWeaponItemDataBase_BaseRange,
			AuWeaponItemDataBase_RoundsPerMinute, AuWeaponItemDataBase_BaseMagazine, AuWeaponItemDataBase_BaseDamageFalloff,
			AuWeaponItemDataBase_ReloadTime, AuWeaponItemDataBase_Spread, AuWeaponItemDataBase_AimCameraSpeed,
			AuWeaponItemDataBase_RequiredItem, CrWeaponItemDataBase_MiningTypeDamage, CrWeaponItemDataBase_WeakpointDamageMultiplier,
			CrWeaponItemDataBase_ShotAggro, CrWeaponItemDataBase_PossibleEnemiesHitPerTrace, CrGrenadeWeaponItemDataBase_GrenadeThrowCostOfGrenadeCharge,
			CrWeaponModDataAsset_BaseDamageFalloffCurveOverride, CrInventoryComponent_GridColumns, CrInventoryComponent_GridRows,
			CrInventoryComponent_Slots, CrUW_InventoryContainer_ItemGridPanel, AuItemDataBase_UniqueItemName,
			AuItemDataBase_ItemName, AuItemDataBase_MaxStack, AuItemDataBase_StackingType,
			AuItemDataBase_UIItemType, AuItemDataBase_ItemIcon, CrGatherableBaseActor_ResourceCount,
			AuAddedItem_Amount, CrElectricityTrait_Parameters, CrElectricityParameters_ElectricityValue,
			CrElectricityParameters_Type, AuActorPlacementData_EntityType, AuActorPlacementData_BuildingID,
			AuAPMassSpawnedEntityType_EntityConfig, CrBuildingData_BuildingName, CrAPHelper_BuildingData,
			CrPlayerControllerBase_PlayerSkills, CrSkillData_Skill,
			CrSkillData_Level, CrSkillData_Experience, CrGameStateBase_CorporationsOwner,
			CrGameStateBase_TechnologyKeeper, CrCorporationsOwner_DataPoints, CrEnviroWaveTimerSubsystem_TimerActor,
			CrWaveTimerActor_bPause
		) && all;
		all = PluginMap::ResolveMassEntityManager(scanner, self) && all;
		return all;
	}
}

// The parts of the plugin that can be off while the rest runs, grouped by the
// cheat panel a player sees. A feature lists the loader keys its code reads:
// the fields above (by key) and the functions aob_resolver.cpp resolves. When
// the loader cannot resolve one of them on the running game build, it turns
// that feature off and keeps the plugin; any key not listed here still refuses
// the plugin. The entry points of an off feature (Initialize, Tick, ApplySavedConfig,
// RenderImGui, Shutdown) return before touching a key, and its menu page says so.
// The cheat settings live in the in-game menu and the per-session JSON, not on
// the loader's config page, so no feature lists a config key.
namespace BetterCheats::Features
{
	inline PluginMap::Feature Stats{ "player-stats", "Health, energy and survival cheats", {
		"UCrUW_HealthHud::SetupProgressBar", Fields::CrCharacterPlayerBase_HealthAttributes.key,
		Fields::CrCharacterPlayerBase_EnergyAttributes.key, Fields::CrCharacterPlayerBase_ShieldAttributes.key,
		Fields::CrCharacterPlayerBase_HydrationAttributes.key, Fields::CrCharacterPlayerBase_CaloriesAttributes.key,
		Fields::CrCharacterPlayerBase_OxygenAttributes.key, Fields::CrCharacterPlayerBase_ToxicityAttributes.key,
		Fields::CrCharacterPlayerBase_RadiationAttributes.key, Fields::CrCharacterPlayerBase_HeatAttributes.key,
		Fields::CrCharacterPlayerBase_DrainAttributes.key, Fields::CrCharacterPlayerBase_CorrosionAttributes.key,
		Fields::CrCharacterPlayerBase_InfectionAttributes.key, Fields::CrCharacterPlayerBase_TemperatureAttributes.key,
		Fields::CrEnergyAttributeSet_CurrentEnergy.key, Fields::CrEnergyAttributeSet_MaxEnergy.key,
		Fields::CrShieldAttributeSet_CurrentShield.key, Fields::CrShieldAttributeSet_MaxShield.key,
		Fields::CrHydrationAttributeSet_CurrentHydration.key, Fields::CrHydrationAttributeSet_MaxHydration.key,
		Fields::CrCaloriesAttributeSet_CurrentCalories.key, Fields::CrCaloriesAttributeSet_MaxCalories.key,
		Fields::CrOxygenAttributeSet_CurrentOxygen.key, Fields::CrOxygenAttributeSet_MaxOxygen.key,
		Fields::CrToxicityAttributeSet_CurrentToxicity.key, Fields::CrToxicityAttributeSet_MaxToxicity.key,
		Fields::CrToxicityAttributeSet_MinToxicity.key, Fields::CrRadiationAttributeSet_CurrentRadiation.key,
		Fields::CrRadiationAttributeSet_MaxRadiation.key, Fields::CrRadiationAttributeSet_MinRadiation.key,
		Fields::CrHeatAttributeSet_CurrentHeat.key, Fields::CrHeatAttributeSet_MaxHeat.key,
		Fields::CrHeatAttributeSet_MinHeat.key, Fields::CrDrainAttributeSet_CurrentDrain.key,
		Fields::CrDrainAttributeSet_MaxDrain.key, Fields::CrDrainAttributeSet_MinDrain.key,
		Fields::CrCorrosionAttributeSet_CurrentCorrosion.key, Fields::CrCorrosionAttributeSet_MaxCorrosion.key,
		Fields::CrCorrosionAttributeSet_MinCorrosion.key, Fields::CrInfectionAttributeSet_CurrentInfection.key,
		Fields::CrInfectionAttributeSet_MaxInfection.key, Fields::CrInfectionAttributeSet_MinInfection.key,
		Fields::CrTemperatureAttributeSet_CurrentTemperature.key, Fields::CrTemperatureAttributeSet_MaxTemperature.key,
		Fields::CrTemperatureAttributeSet_MinTemperature.key, Fields::CrHealthAttributeSet_CurrentHealth.key,
		Fields::CrHealthAttributeSet_MaxHealth.key
	} };

	inline PluginMap::Feature Movement{ "movement", "Movement tuning", {
		Fields::CrCharacterPlayerBase_ADSMoveSpeed.key, Fields::CrCharacterPlayerBase_AccelerationAndDeceleration.key,
		Fields::CrCharacterPlayerBase_CrouchedMoveSpeed.key, Fields::CrCharacterPlayerBase_DoubleJumpEnergyUsage.key,
		Fields::CrCharacterPlayerBase_DurationOfAccelerationToSprint.key, Fields::CrCharacterPlayerBase_EnergyLoweringMultiplierDuringSprint.key,
		Fields::CrCharacterPlayerBase_NormalEnergyRestorationRate.key, Fields::CrCharacterPlayerBase_NormalMoveSpeed.key,
		Fields::CrCharacterPlayerBase_SprintMoveSpeed.key, Fields::CrCharacterMovementComponent_CoyoteTime.key,
		Fields::CrCharacterMovementComponent_DashCooldownSeconds.key, Fields::CrCharacterMovementComponent_DashDuration.key,
		Fields::CrCharacterMovementComponent_DashEnergyCostBase.key, Fields::CrCharacterMovementComponent_DefaultZiplineAcceleration.key,
		Fields::CrCharacterMovementComponent_DefaultZiplineSpeed.key, Fields::CrCharacterMovementComponent_JumpEnergyCost.key,
		Fields::CrCharacterMovementComponent_MaxDashDistance.key, Fields::CrCharacterMovementComponent_MaxSlideSpeed.key,
		Fields::CrCharacterMovementComponent_SlideEnterImpulse.key, Fields::CrCharacterMovementComponent_SlideFrictionFactor.key,
		Fields::CrCharacterMovementComponent_SlideGravityForce.key, Fields::CrGemAttributeSet_DodgeCostMultiplier.key,
		Fields::CrGemAttributeSet_DoubleJumpCostMultiplier.key, Fields::CrGemAttributeSet_FallDamageMultiplier.key,
		Fields::CrGemAttributeSet_SlideStaminaRegenMultiplier.key, Fields::CrGemAttributeSet_StaminaRegenMultiplier.key,
		Fields::CrGemAttributeSet_ZiplineSpeedMultiplier.key, Fields::CrMovementAttributeSet_JumpMultiplier.key,
		Fields::CrMovementAttributeSet_SprintSpeedMultiplier.key, Fields::CrMovementSpeedMultiplierAttributeSet_CurrentMovementSpeedMultiplier.key,
		Fields::CrMovementSpeedMultiplierAttributeSet_MaxMovementSpeedMultiplier.key, Fields::CrMovementSpeedMultiplierAttributeSet_MinMovementSpeedMultiplier.key,
		Fields::CrCharacterPlayerBase_GemAttributes.key, Fields::CrCharacterPlayerBase_MovementAttributes.key,
		Fields::CrCharacterPlayerBase_MovementSpeedMultiplierAttributes.key, Fields::CrCharacterPlayerBase_EnergyLogicComponent.key,
		Fields::CrCharacterPlayerBase_CrCharacterMovementComponent.key, Fields::CrCharacterPlayerBase_EnergyAttributes.key,
		Fields::CrEnergyAttributeSet_CurrentEnergy.key, Fields::CrEnergyAttributeSet_MaxEnergy.key,
		Fields::CrCharacterMovementComponent_MaxAirDashesToExecute.key, Fields::CrEnergyLogicComponent_DelayBeforeEnergyRegeneration.key
	} };

	inline PluginMap::Feature Weapons{ "weapons", "Weapon and grenade tuning", {
		"UAuItemsComponent::AddNewItem", Fields::CrWeaponAttributeSet_ADS_TransitionSpeedModMultiplier.key,
		Fields::CrWeaponAttributeSet_DamageFallOffModMultiplier.key, Fields::CrWeaponAttributeSet_DamageModMultiplier.key,
		Fields::CrWeaponAttributeSet_FireRateModMultiplier.key, Fields::CrWeaponAttributeSet_MaxMagAmmoModOffset.key,
		Fields::CrWeaponAttributeSet_PossibleEnemiesHitPerTrace.key, Fields::CrWeaponAttributeSet_RecoilModMultiplier.key,
		Fields::CrWeaponAttributeSet_ReloadSpeedModMultiplier.key, Fields::CrWeaponAttributeSet_SpreadModMultiplier.key,
		Fields::CrWeaponAttributeSet_SwayModMultiplier.key, Fields::CrCharacterPlayerBase_InventoryComponent.key,
		Fields::CrCharacterPlayerBase_HiddenInventoryComponent.key, Fields::CrCharacterPlayerBase_WeaponSystem.key,
		Fields::CrCharacterPlayerBase_WeaponAttributes.key, Fields::CrCharacterPlayerBase_GrenadeChargeAttributes.key,
		Fields::CrCharacterPlayerBase_HealthAttributes.key, Fields::CrHealthAttributeSet_CurrentHealth.key,
		Fields::CrHealthAttributeSet_MaxHealth.key, Fields::CrGrenadeChargeAttributeSet_CurrentGrenadeCharge.key,
		Fields::CrGrenadeChargeAttributeSet_MaxGrenadeCharge.key, Fields::CrGrenadeChargeAttributeSet_MinGrenadeCharge.key,
		Fields::CrAIBase_BaseAttributes.key, Fields::CrWeaponComponent_LastEquippedWeaponData.key,
		Fields::AuWeaponItemDataBase_BaseDamage.key, Fields::AuWeaponItemDataBase_BaseRange.key,
		Fields::AuWeaponItemDataBase_RoundsPerMinute.key, Fields::AuWeaponItemDataBase_BaseMagazine.key,
		Fields::AuWeaponItemDataBase_BaseDamageFalloff.key, Fields::AuWeaponItemDataBase_ReloadTime.key,
		Fields::AuWeaponItemDataBase_Spread.key, Fields::AuWeaponItemDataBase_AimCameraSpeed.key,
		Fields::AuWeaponItemDataBase_RequiredItem.key, Fields::CrWeaponItemDataBase_PossibleEnemiesHitPerTrace.key,
		Fields::CrGrenadeWeaponItemDataBase_GrenadeThrowCostOfGrenadeCharge.key, Fields::CrWeaponModDataAsset_BaseDamageFalloffCurveOverride.key,
		Fields::AuWeaponItemDataBase_bIsUsingDamageFalloff.key, Fields::MassEnemyCharacterBase_bIsInPool.key,
		Fields::MassEnemyHealthFragment_HP.key, Fields::AuAddedItem_Amount.key, Fields::AuAddedItem_Size.key
	} };

	inline PluginMap::Feature Mining{ "mining-tools", "Mining tool tuning", {
		"UCrMiningToolComponent::GetMiningDamage", Fields::CrWeaponAttributeSet_DamageModMultiplier.key,
		Fields::CrWeaponAttributeSet_FireRateModMultiplier.key, Fields::CrCharacterPlayerBase_WeaponSystem.key,
		Fields::CrCharacterPlayerBase_WeaponAttributes.key, Fields::CrCharacterPlayerBase_MiningBoostAttributes.key,
		Fields::CrMiningBoostAttributeSet_CurrentBoostMultiplierValue.key, Fields::CrMiningBoostAttributeSet_MaxBoostMultiplierValue.key,
		Fields::CrWeaponAttributeSet_AggroPerShotModMultiplier.key, Fields::CrWeaponComponent_LastEquippedWeaponData.key,
		Fields::AuWeaponItemDataBase_BaseDamage.key, Fields::AuWeaponItemDataBase_BaseRange.key,
		Fields::AuWeaponItemDataBase_RoundsPerMinute.key, Fields::CrWeaponItemDataBase_MiningTypeDamage.key,
		Fields::CrWeaponItemDataBase_WeakpointDamageMultiplier.key, Fields::CrWeaponItemDataBase_ShotAggro.key
	} };

	inline PluginMap::Feature Inventory{ "inventory", "Inventory size and stack limits", {
		"UCrUW_InventoryContainer::InitInventorySlots", Fields::CrUW_InventoryContainer_InventoryComponent.key,
		Fields::CrCharacterPlayerBase_InventoryComponent.key, Fields::CrInventoryComponent_GridColumns.key,
		Fields::CrInventoryComponent_GridRows.key, Fields::CrUW_InventoryContainer_ItemGridPanel.key,
		Fields::AuItemDataBase_MaxStack.key, Fields::AuItemDataBase_StackingType.key,
		Fields::AuItemDataBase_UIItemType.key, Fields::CrGatherableBaseActor_ResourceCount.key,
		Fields::CrInventoryComponent_Slots.key, Fields::AuItemDataBase_UniqueItemName.key,
		Fields::AuItemDataBase_ItemName.key
	} };

	inline PluginMap::Feature Items{ "item-spawner", "Item spawner", {
		"UAuItemsComponent::AddNewItem", Fields::CrCharacterPlayerBase_InventoryComponent.key,
		Fields::AuItemDataBase_MaxStack.key, Fields::AuItemDataBase_ItemIcon.key,
		Fields::AuAddedItem_Amount.key, Fields::AuItemDataBase_UniqueItemName.key,
		Fields::AuItemDataBase_ItemName.key, Fields::AuAddedItem_Size.key
	} };

	inline PluginMap::Feature Skills{ "skills", "Skill levels", {
		Fields::CrPlayerControllerBase_PlayerSkills.key, Fields::CrSkillData_Skill.key,
		Fields::CrSkillData_Level.key, Fields::CrSkillData_Experience.key,
		Fields::CrSkillData_Size.key
	} };

	inline PluginMap::Feature BuildFree{ "build-free", "Build without cost or stability checks", {
		"UCrBuildingComponent::GetPlacementResourceConditionResult", "ACrAPHelperActorCustom::CheckStability",
		"ACrAPHelperDynamicPillar::CheckStability"
	} };

	inline PluginMap::Feature BuildUnlock{ "build-unlock", "Unlock all buildings and recipes", {
		"ACrTechnologyKeeper::CheckAvailableBuildings", "ACrCraftingRecipeOwner::IsRecipeUnlocked",
		Fields::CrTechnologyKeeper_bUnlockAllBuildings.key, Fields::CrGameStateBase_TechnologyKeeper.key
	} };

	inline PluginMap::Feature BuildAnywhere{ "build-anywhere", "Place windows and benches anywhere", {
		Fields::CrAPHelper_BuildingData.key, Fields::AuActorPlacementData_BuildingID.key
	} };

	inline PluginMap::Feature Enemies{ "enemies", "Enemy tuning", {
		"FWeakObjectPtr::operator=(FObjectPtr)", "UMassActorSubsystem::GetEntityHandleFromActor",
		"FMassEntityManager::InternalGetFragmentDataPtr", PluginMap::MassEntityManagerField.key,
		Fields::MassEnemyCharacterBase_bIsInPool.key,
		Fields::MassEnemyCharacterBase_AllowedAttackDistance.key, Fields::MassEnemyCharacterBase_AllowedAttackConeHalfAngle.key,
		Fields::MassEnemyCharacterBase_AllowedBuildingAttackDistance.key, Fields::MassEnemyHealthFragment_HP.key
	} };

	inline PluginMap::Feature Power{ "machine-power", "Machine power", {
		"FMassEntityConfig::DestroyEntityTemplate", "FMassEntityConfig::GetOrCreateEntityTemplate",
		"UWorld::GetSubsystem<UMassEntitySubsystem>", "TSharedFragmentsContainer<FConstSharedStruct>::FindOrAdd",
		"UE::StructUtils::GetStructInstanceCrc32", Fields::CrElectricityTrait_Parameters.key,
		Fields::CrElectricityParameters_ElectricityValue.key, Fields::CrElectricityParameters_Type.key,
		Fields::AuActorPlacementData_EntityType.key, Fields::AuAPMassSpawnedEntityType_EntityConfig.key,
		Fields::CrBuildingData_BuildingName.key, Fields::CrElectricityParameters_Size.key,
		PluginMap::MassEntityManagerField.key
	} };

	inline PluginMap::Feature Corporations{ "corporations", "Corporation data points", {
		Fields::CrGameStateBase_CorporationsOwner.key, Fields::CrCorporationsOwner_DataPoints.key
	} };

	inline PluginMap::Feature Wave{ "rupture-wave", "Rupture wave timer", {
		Fields::CrEnviroWaveTimerSubsystem_NextWaveTimer.key, Fields::CrEnviroWaveTimerSubsystem_bWaitingForNextWave.key,
		Fields::CrEnviroWaveTimerSubsystem_bPause.key, Fields::CrEnviroWaveTimerSubsystem_bStopWaves.key,
		Fields::CrEnviroWaveTimerSubsystem_TimerActor.key, Fields::CrWaveTimerActor_bPause.key
	} };

	// Declares every feature. Call before the resolves, so a miss lands on the
	// feature, not on the plugin.
	inline bool Declare(IPluginSelf* self, IPluginHookScanner* scanner)
	{
		return PluginMap::DeclareFeatures(scanner, self,
			Stats, Movement, Weapons, Mining, Inventory, Items, Skills,
			BuildFree, BuildUnlock, BuildAnywhere, Enemies, Power, Corporations, Wave);
	}

	// What an off feature shows in place of its menu page.
	inline void RenderOff(IModLoaderImGui* imgui, const PluginMap::Feature& feature)
	{
		imgui->SeparatorText(feature.title);
		imgui->TextDisabled("Off: this game build changed something it needs. See the loader's hook report.");
	}
}
