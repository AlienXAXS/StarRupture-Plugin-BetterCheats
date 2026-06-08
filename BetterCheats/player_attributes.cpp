#include "player_attributes.h"
#include "plugin_helpers.h"
#include "aob_patterns.h"

#include "Chimera_classes.hpp"
#include "ChimeraUI_classes.hpp"
#include "UMG_classes.hpp"

namespace BetterCheats::Panels::Attributes
{
	namespace
	{
		// -------------------------------------------------------------------------
		// One slot per lockable attribute. "Locked" pins CurrentValue (and
		// BaseValue, so gameplay-effect re-evaluation can't override it) to
		// `value` every engine tick — independent of whether the menu is open.
		// -------------------------------------------------------------------------
		enum class AttrId : int
		{
			Health, Energy, Shield, Hydration, Calories, Oxygen,
			Toxicity, Radiation, Heat, Drain, Corrosion, Infection, Temperature,
			Count
		};
		constexpr int kAttrCount = static_cast<int>(AttrId::Count);

		// `value` doubles as both the slider's live value and, while locked, the
		// value Tick() pins the attribute to — there's no separate lock value.
		struct LockSlot
		{
			bool  locked = false;
			float value  = 0.0f;
		};
		LockSlot g_locks[kAttrCount];

		// ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp
		constexpr int kAttributeTableFlags = (1 << 6) | (1 << 9) | (3 << 13);
		// ImGuiTableColumnFlags_WidthFixed
		constexpr int kColumnWidthFixed = 1 << 4;

		// ImGuiCol_ indices (matches the ordering used in cheat_menu.cpp's Col_* constants)
		constexpr int Col_FrameBg        = 7;
		constexpr int Col_FrameBgHovered = 8;
		constexpr int Col_FrameBgActive  = 9;

		constexpr const char* kLockTooltip =
			"Continuously pin this attribute to the value set on its slider, "
			"even while the menu is closed.";

		void SetupAttributeTableColumns(IModLoaderImGui* imgui)
		{
			imgui->TableSetupColumn("Attribute", kColumnWidthFixed, 110.0f);
			imgui->TableSetupColumn("Value", 0, 0.0f);
			imgui->TableSetupColumn("Locked", kColumnWidthFixed, 50.0f);
		}

		// Draws the lock checkbox with a green frame while active and a tooltip
		// explaining its effect, instead of a "Locked" label that overflows the column.
		void RenderLockCheckbox(IModLoaderImGui* imgui, LockSlot& lock)
		{
			const bool wasLocked = lock.locked;
			if (wasLocked)
			{
				imgui->PushStyleColor(Col_FrameBg,        0.20f, 0.55f, 0.25f, 1.0f);
				imgui->PushStyleColor(Col_FrameBgHovered, 0.25f, 0.65f, 0.30f, 1.0f);
				imgui->PushStyleColor(Col_FrameBgActive,  0.30f, 0.75f, 0.35f, 1.0f);
			}

			imgui->Checkbox("##locked", &lock.locked);

			if (wasLocked)
				imgui->PopStyleColor(3);

			imgui->SetItemTooltip(kLockTooltip);
		}

		SDK::ACrCharacterPlayerBase* GetLocalCharacter()
		{
			SDK::UWorld* world = nullptr;
			try { world = SDK::UWorld::GetWorld(); }
			catch (...) { return nullptr; }
			if (!world) return nullptr;

			SDK::APlayerController* pc = SDK::UGameplayStatics::GetPlayerController(world, 0);
			if (!pc || !pc->Pawn) return nullptr;

			return static_cast<SDK::ACrCharacterPlayerBase*>(pc->Pawn);
		}

		// Sets both BaseValue and CurrentValue so the change survives gameplay-effect
		// re-evaluation (e.g. survival ticks reapplying from the base attribute).
		void SetAttributeValue(SDK::FGameplayAttributeData& attribute, float value)
		{
			attribute.BaseValue    = value;
			attribute.CurrentValue = value;
		}

		// UCrUW_HealthHud::SetupProgressBar re-pulls the owning character's current
		// attribute values and pushes them into the HUD's progress bars. The HUD only
		// otherwise refreshes via gameplay-attribute delegates, which lag behind direct
		// writes — calling this forces it to catch up immediately.
		using HealthHud_SetupProgressBarFn = void(__fastcall*)(SDK::UCrUW_HealthHud*);

		HealthHud_SetupProgressBarFn g_setupProgressBar = nullptr;

		// Does the actual world/widget access — must run on the game thread, since
		// UWorld::GetWorld() and widget traversal are not safe to call from the
		// ImGui render callback that triggers RefreshHealthHud().
		void RefreshHealthHudOnGameThread(void* /*context*/)
		{
			try
			{
				SDK::UWorld* world = SDK::UWorld::GetWorld();
				if (!world)
				{
					LOG_DEBUG("RefreshHealthHud: no world loaded, skipping.");
					return;
				}

				SDK::TArray<SDK::UUserWidget*> widgets;
				// HealthHud is nested inside a parent HUD widget, not a top-level viewport
				// widget — TopLevelOnly=true would miss it entirely.
				SDK::UWidgetBlueprintLibrary::GetAllWidgetsOfClass(world, &widgets, SDK::UCrUW_HealthHud::StaticClass(), false);

				LOG_DEBUG("RefreshHealthHud: found %d HealthHud widget(s).", widgets.Num());

				SDK::UUserWidget* const* data = widgets.GetDataPtr();
				for (int i = 0; i < widgets.Num(); ++i)
				{
					if (auto* hud = static_cast<SDK::UCrUW_HealthHud*>(data[i]))
					{
						g_setupProgressBar(hud);
						LOG_DEBUG("RefreshHealthHud: refreshed HealthHud widget %p.", static_cast<void*>(hud));
					}
				}
			}
			catch (...)
			{
				LOG_DEBUG("RefreshHealthHud: exception while resolving world/widgets.");
			}
		}

		// Forces the on-screen HUD to immediately reflect attribute values the plugin
		// just wrote, instead of waiting for the game's own delegate-driven refresh.
		// Hops onto the game thread since this is called from the ImGui render
		// callback, which does not run on the game thread.
		void RefreshHealthHud()
		{
			if (!g_setupProgressBar)
			{
				LOG_TRACE("RefreshHealthHud: SetupProgressBar function pointer not set, skipping HUD refresh.");
				return;
			}

			IPluginHooks* hooks = GetHooks();
			if (!hooks)
				return;

			hooks->Engine->PostToGameThread(&RefreshHealthHudOnGameThread, nullptr);
		}

		// Resolves the live "Current*" attribute data for the given slot, or
		// nullptr if that attribute set isn't present on the character.
		SDK::FGameplayAttributeData* GetCurrentAttribute(SDK::ACrCharacterPlayerBase* character, AttrId id)
		{
			switch (id)
			{
				case AttrId::Health:      return character->HealthAttributes      ? &character->HealthAttributes->CurrentHealth           : nullptr;
				case AttrId::Energy:      return character->EnergyAttributes      ? &character->EnergyAttributes->CurrentEnergy           : nullptr;
				case AttrId::Shield:      return character->ShieldAttributes      ? &character->ShieldAttributes->CurrentShield           : nullptr;
				case AttrId::Hydration:   return character->HydrationAttributes   ? &character->HydrationAttributes->CurrentHydration     : nullptr;
				case AttrId::Calories:    return character->CaloriesAttributes    ? &character->CaloriesAttributes->CurrentCalories       : nullptr;
				case AttrId::Oxygen:      return character->OxygenAttributes      ? &character->OxygenAttributes->CurrentOxygen           : nullptr;
				case AttrId::Toxicity:    return character->ToxicityAttributes    ? &character->ToxicityAttributes->CurrentToxicity       : nullptr;
				case AttrId::Radiation:   return character->RadiationAttributes   ? &character->RadiationAttributes->CurrentRadiation     : nullptr;
				case AttrId::Heat:        return character->HeatAttributes        ? &character->HeatAttributes->CurrentHeat               : nullptr;
				case AttrId::Drain:       return character->DrainAttributes       ? &character->DrainAttributes->CurrentDrain             : nullptr;
				case AttrId::Corrosion:   return character->CorrosionAttributes   ? &character->CorrosionAttributes->CurrentCorrosion     : nullptr;
				case AttrId::Infection:   return character->InfectionAttributes   ? &character->InfectionAttributes->CurrentInfection     : nullptr;
				case AttrId::Temperature: return character->TemperatureAttributes ? &character->TemperatureAttributes->CurrentTemperature : nullptr;
				default:                  return nullptr;
			}
		}

		// -------------------------------------------------------------------------
		// Shared row widget — one aligned table row: label, a slider that both
		// displays and edits the live value (dragging it writes straight through
		// to the attribute), a snap-to button (max for resources, min for
		// afflictions), and the Locked checkbox on the right of the bar.
		// -------------------------------------------------------------------------
		void RenderAttributeRow(IModLoaderImGui* imgui, const char* label, AttrId id,
			SDK::FGameplayAttributeData& current, const SDK::FGameplayAttributeData& min, const SDK::FGameplayAttributeData& max,
			const char* snapLabel, float snapTarget)
		{
			LockSlot& lock = g_locks[static_cast<int>(id)];

			// While unlocked, keep the slider following the live attribute value;
			// once locked, it holds steady at the value Tick() pins to.
			if (!lock.locked)
				lock.value = current.CurrentValue;

			imgui->PushIDStr(label);
			imgui->TableNextRow(0, 0.0f);

			imgui->TableSetColumnIndex(0);
			imgui->Text(label);

			imgui->TableSetColumnIndex(1);
			imgui->SetNextItemWidth(-1.0f);
			char format[32];
			snprintf(format, sizeof(format), "%%.0f / %.0f", max.CurrentValue);
			if (imgui->SliderFloat("##value", &lock.value, min.CurrentValue, max.CurrentValue, format))
			{
				SetAttributeValue(current, lock.value);
				RefreshHealthHud();
			}
			imgui->SameLine(0.0f, -1.0f);
			if (imgui->SmallButton(snapLabel))
			{
				lock.value = snapTarget;
				SetAttributeValue(current, snapTarget);
				RefreshHealthHud();
			}

			imgui->TableSetColumnIndex(2);
			RenderLockCheckbox(imgui, lock);

			imgui->PopID();
		}

		void RenderResourceRow(IModLoaderImGui* imgui, const char* label, AttrId id,
			SDK::FGameplayAttributeData& current, const SDK::FGameplayAttributeData& max)
		{
			SDK::FGameplayAttributeData zero{};
			RenderAttributeRow(imgui, label, id, current, zero, max, "Fill", max.CurrentValue);
		}

		void RenderAfflictionRow(IModLoaderImGui* imgui, const char* label, AttrId id,
			SDK::FGameplayAttributeData& current, const SDK::FGameplayAttributeData& min, const SDK::FGameplayAttributeData& max)
		{
			RenderAttributeRow(imgui, label, id, current, min, max, "Clear", min.CurrentValue);
		}
	}

	void Initialize()
	{
		IPluginScanner* scanner = GetScanner();
		if (!scanner)
			return;

		if (uintptr_t address = scanner->FindPatternInMainModule(AOB::HealthHud_SetupProgressBar))
			g_setupProgressBar = reinterpret_cast<HealthHud_SetupProgressBarFn>(address);
		else
			LOG_WARN("Failed to resolve UCrUW_HealthHud::SetupProgressBar pattern — HUD will not refresh immediately.");
	}

	void Tick(float /*deltaSeconds*/)
	{
		SDK::ACrCharacterPlayerBase* character = nullptr;

		for (int i = 0; i < kAttrCount; ++i)
		{
			LockSlot& lock = g_locks[i];
			if (!lock.locked)
				continue;

			if (!character)
			{
				character = GetLocalCharacter();
				if (!character)
					return;
			}

			if (SDK::FGameplayAttributeData* attribute = GetCurrentAttribute(character, static_cast<AttrId>(i)))
				SetAttributeValue(*attribute, lock.value);
		}
	}

	void RenderImGui(IModLoaderImGui* imgui)
	{
		SDK::ACrCharacterPlayerBase* character = GetLocalCharacter();
		if (!character)
		{
			imgui->TextDisabled("No local player character found.");
			return;
		}

		imgui->TextWrapped("Tick \"Locked\" to continuously pin an attribute to the value "
			"set on its slider, even while the menu is closed.");
		imgui->Spacing();

		// ---------------------------------------------------------------------
		// Health
		// ---------------------------------------------------------------------
		imgui->SeparatorText("Health");

		if (SDK::UCrHealthAttributeSet* health = character->HealthAttributes)
		{
			if (imgui->BeginTable("##health_table", 3, kAttributeTableFlags))
			{
				SetupAttributeTableColumns(imgui);

				SDK::FGameplayAttributeData zero{};
				RenderAttributeRow(imgui, "Health", AttrId::Health, health->CurrentHealth, zero, health->MaxHealth,
					"Full Heal", health->MaxHealth.CurrentValue);

				imgui->EndTable();
			}

			imgui->Spacing();

			static float maxHealthValue = 0.0f;
			static bool  maxHealthInit  = false;
			if (!maxHealthInit)
			{
				maxHealthValue = health->MaxHealth.CurrentValue;
				maxHealthInit  = true;
			}

			if (imgui->BeginTable("##max_health_table", 3, kAttributeTableFlags))
			{
				SetupAttributeTableColumns(imgui);

				imgui->TableNextRow(0, 0.0f);

				imgui->TableSetColumnIndex(0);
				imgui->Text("Max Health");

				imgui->TableSetColumnIndex(1);
				imgui->SetNextItemWidth(-60.0f);
				imgui->InputFloat("##max_health", &maxHealthValue, 10.0f, 100.0f, "%.0f");
				imgui->SameLine(0.0f, -1.0f);
				if (imgui->Button("Set##max_health"))
				{
					SetAttributeValue(health->MaxHealth, maxHealthValue);
					SetAttributeValue(health->CurrentHealth, maxHealthValue);
					RefreshHealthHud();
				}

				// Column 3 left blank — Max Health has no lock toggle.
				imgui->TableSetColumnIndex(2);

				imgui->EndTable();
			}
		}
		else
		{
			imgui->TextDisabled("Health attributes not available.");
		}

		// ---------------------------------------------------------------------
		// Survival resources
		// ---------------------------------------------------------------------
		imgui->Spacing();
		imgui->SeparatorText("Survival Resources");

		if (imgui->BeginTable("##survival_table", 3, kAttributeTableFlags))
		{
			SetupAttributeTableColumns(imgui);

			if (auto* a = character->EnergyAttributes)    RenderResourceRow(imgui, "Energy",    AttrId::Energy,    a->CurrentEnergy,    a->MaxEnergy);
			if (auto* a = character->ShieldAttributes)    RenderResourceRow(imgui, "Shield",    AttrId::Shield,    a->CurrentShield,    a->MaxShield);
			if (auto* a = character->HydrationAttributes) RenderResourceRow(imgui, "Hydration", AttrId::Hydration, a->CurrentHydration, a->MaxHydration);
			if (auto* a = character->CaloriesAttributes)  RenderResourceRow(imgui, "Calories",  AttrId::Calories,  a->CurrentCalories,  a->MaxCalories);
			if (auto* a = character->OxygenAttributes)    RenderResourceRow(imgui, "Oxygen",    AttrId::Oxygen,    a->CurrentOxygen,    a->MaxOxygen);

			imgui->EndTable();
		}

		// ---------------------------------------------------------------------
		// Afflictions
		// ---------------------------------------------------------------------
		imgui->SeparatorText("Afflictions");

		if (imgui->BeginTable("##afflictions_table", 3, kAttributeTableFlags))
		{
			SetupAttributeTableColumns(imgui);

			if (auto* a = character->ToxicityAttributes)    RenderAfflictionRow(imgui, "Toxicity",    AttrId::Toxicity,    a->CurrentToxicity,    a->MinToxicity,    a->MaxToxicity);
			if (auto* a = character->RadiationAttributes)   RenderAfflictionRow(imgui, "Radiation",   AttrId::Radiation,   a->CurrentRadiation,   a->MinRadiation,   a->MaxRadiation);
			if (auto* a = character->HeatAttributes)        RenderAfflictionRow(imgui, "Heat",        AttrId::Heat,        a->CurrentHeat,        a->MinHeat,        a->MaxHeat);
			if (auto* a = character->DrainAttributes)       RenderAfflictionRow(imgui, "Drain",       AttrId::Drain,       a->CurrentDrain,       a->MinDrain,       a->MaxDrain);
			if (auto* a = character->CorrosionAttributes)   RenderAfflictionRow(imgui, "Corrosion",   AttrId::Corrosion,   a->CurrentCorrosion,   a->MinCorrosion,   a->MaxCorrosion);
			if (auto* a = character->InfectionAttributes)   RenderAfflictionRow(imgui, "Infection",   AttrId::Infection,   a->CurrentInfection,   a->MinInfection,   a->MaxInfection);
			if (auto* a = character->TemperatureAttributes) RenderAfflictionRow(imgui, "Temperature", AttrId::Temperature, a->CurrentTemperature, a->MinTemperature, a->MaxTemperature);

			imgui->EndTable();
		}
	}
}
