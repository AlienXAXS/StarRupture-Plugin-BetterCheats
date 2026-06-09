#include "player_attributes.h"
#include "plugin_helpers.h"
#include "aob_patterns.h"

#include "Chimera_classes.hpp"
#include "ChimeraUI_classes.hpp"
#include "UMG_classes.hpp"

#include <cstring>
#include <mutex>

namespace BetterCheats::Panels::Attributes
{
	namespace
	{
		constexpr const char* kChimeraMainWorldName = "ChimeraMain";

		// Set/cleared by the world begin/end-play hooks below — Tick() reads this
		// instead of probing UWorld::GetWorld() every engine tick.
		bool g_inChimeraMain = false;

		void OnWorldBeginPlay(SDK::UWorld* /*world*/)
		{
			// Only fires for the ChimeraMain world (main game world only).
			g_inChimeraMain = true;
		}

		void OnWorldEndPlay(SDK::UWorld* /*world*/, const char* worldName)
		{
			if (worldName && std::strcmp(worldName, kChimeraMainWorldName) == 0)
				g_inChimeraMain = false;
		}

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

		// Only ever called from Tick() (the engine-tick callback, which runs on the
		// game thread) — never from RenderImGui(), which runs on a different thread
		// where UWorld::GetWorld() and UObject access intermittently crash inside the
		// renderer (FD3D12DynamicRHI::HandleFailedD3D12Result, no useful callstack).
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

		SDK::FGameplayAttributeData* GetMinAttribute(SDK::ACrCharacterPlayerBase* character, AttrId id)
		{
			switch (id)
			{
				case AttrId::Toxicity:    return character->ToxicityAttributes    ? &character->ToxicityAttributes->MinToxicity       : nullptr;
				case AttrId::Radiation:   return character->RadiationAttributes   ? &character->RadiationAttributes->MinRadiation     : nullptr;
				case AttrId::Heat:        return character->HeatAttributes        ? &character->HeatAttributes->MinHeat               : nullptr;
				case AttrId::Drain:       return character->DrainAttributes       ? &character->DrainAttributes->MinDrain             : nullptr;
				case AttrId::Corrosion:   return character->CorrosionAttributes   ? &character->CorrosionAttributes->MinCorrosion     : nullptr;
				case AttrId::Infection:   return character->InfectionAttributes   ? &character->InfectionAttributes->MinInfection     : nullptr;
				case AttrId::Temperature: return character->TemperatureAttributes ? &character->TemperatureAttributes->MinTemperature : nullptr;
				default:                  return nullptr; // resource attributes floor at zero
			}
		}

		SDK::FGameplayAttributeData* GetMaxAttribute(SDK::ACrCharacterPlayerBase* character, AttrId id)
		{
			switch (id)
			{
				case AttrId::Health:      return character->HealthAttributes      ? &character->HealthAttributes->MaxHealth           : nullptr;
				case AttrId::Energy:      return character->EnergyAttributes      ? &character->EnergyAttributes->MaxEnergy           : nullptr;
				case AttrId::Shield:      return character->ShieldAttributes      ? &character->ShieldAttributes->MaxShield           : nullptr;
				case AttrId::Hydration:   return character->HydrationAttributes   ? &character->HydrationAttributes->MaxHydration     : nullptr;
				case AttrId::Calories:    return character->CaloriesAttributes    ? &character->CaloriesAttributes->MaxCalories       : nullptr;
				case AttrId::Oxygen:      return character->OxygenAttributes      ? &character->OxygenAttributes->MaxOxygen           : nullptr;
				case AttrId::Toxicity:    return character->ToxicityAttributes    ? &character->ToxicityAttributes->MaxToxicity       : nullptr;
				case AttrId::Radiation:   return character->RadiationAttributes   ? &character->RadiationAttributes->MaxRadiation     : nullptr;
				case AttrId::Heat:        return character->HeatAttributes        ? &character->HeatAttributes->MaxHeat               : nullptr;
				case AttrId::Drain:       return character->DrainAttributes       ? &character->DrainAttributes->MaxDrain             : nullptr;
				case AttrId::Corrosion:   return character->CorrosionAttributes   ? &character->CorrosionAttributes->MaxCorrosion     : nullptr;
				case AttrId::Infection:   return character->InfectionAttributes   ? &character->InfectionAttributes->MaxInfection     : nullptr;
				case AttrId::Temperature: return character->TemperatureAttributes ? &character->TemperatureAttributes->MaxTemperature : nullptr;
				default:                  return nullptr;
			}
		}

		// UCrUW_HealthHud::SetupProgressBar re-pulls the owning character's current
		// attribute values and pushes them into the HUD's progress bars. The HUD only
		// otherwise refreshes via gameplay-attribute delegates, which lag behind direct
		// writes — calling this forces it to catch up immediately.
		using HealthHud_SetupProgressBarFn = void(__fastcall*)(SDK::UCrUW_HealthHud*);

		HealthHud_SetupProgressBarFn g_setupProgressBar = nullptr;

		// Does the actual world/widget access — must run on the game thread, since
		// UWorld::GetWorld() and widget traversal are not safe to call from the
		// ImGui render callback. Called either directly from Tick() (already on the
		// game thread) or via PostToGameThread from RefreshHealthHud().
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
		// Hops onto the game thread since this may be called from contexts that are
		// not the game thread (e.g. indirectly from the ImGui render callback).
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

		// -------------------------------------------------------------------------
		// Snapshot — plain data describing the current character's attributes.
		// Refreshed on the game thread by Tick() and read by RenderImGui(), which
		// runs on a different thread and must never touch SDK/UObjects directly
		// (doing so intermittently crashes inside the renderer with no useful
		// callstack — FD3D12DynamicRHI::HandleFailedD3D12Result).
		// -------------------------------------------------------------------------
		struct AttrSnapshot
		{
			bool  present = false;
			float current = 0.0f;
			float min     = 0.0f;
			float max     = 0.0f;
		};

		struct AttributesSnapshot
		{
			bool         hasCharacter = false;
			bool         hasHealth    = false;
			float        maxHealth    = 0.0f;
			AttrSnapshot slots[kAttrCount];
		};

		std::mutex         g_snapshotMutex;
		AttributesSnapshot g_snapshot;

		// Edits queued by RenderImGui() (UI thread) for Tick() (game thread) to
		// apply. g_pendingMaxHealth is separate since "Max Health" isn't one of the
		// lockable AttrId slots.
		struct PendingEdit { bool pending = false; float value = 0.0f; };

		std::mutex  g_pendingMutex;
		PendingEdit g_pendingValue[kAttrCount];
		PendingEdit g_pendingMaxHealth;

		void QueueAttributeEdit(AttrId id, float value)
		{
			std::lock_guard<std::mutex> lock(g_pendingMutex);
			g_pendingValue[static_cast<int>(id)] = { true, value };
		}

		void QueueMaxHealthEdit(float value)
		{
			std::lock_guard<std::mutex> lock(g_pendingMutex);
			g_pendingMaxHealth = { true, value };
		}

		// Copies the character's current attribute state into the snapshot —
		// must run on the game thread.
		void RefreshSnapshot(SDK::ACrCharacterPlayerBase* character)
		{
			AttributesSnapshot snapshot;
			snapshot.hasCharacter = character != nullptr;

			if (character)
			{
				if (SDK::UCrHealthAttributeSet* health = character->HealthAttributes)
				{
					snapshot.hasHealth = true;
					snapshot.maxHealth = health->MaxHealth.CurrentValue;
				}

				for (int i = 0; i < kAttrCount; ++i)
				{
					const AttrId id = static_cast<AttrId>(i);
					SDK::FGameplayAttributeData* current = GetCurrentAttribute(character, id);
					if (!current)
						continue;

					AttrSnapshot& slot = snapshot.slots[i];
					slot.present = true;
					slot.current = current->CurrentValue;

					SDK::FGameplayAttributeData* min = GetMinAttribute(character, id);
					SDK::FGameplayAttributeData* max = GetMaxAttribute(character, id);
					slot.min = min ? min->CurrentValue : 0.0f;
					slot.max = max ? max->CurrentValue : 0.0f;
				}
			}

			std::lock_guard<std::mutex> lock(g_snapshotMutex);
			g_snapshot = snapshot;
		}

		// Applies any edits RenderImGui() queued since the last tick — must run on
		// the game thread.
		void ApplyPendingEdits(SDK::ACrCharacterPlayerBase* character)
		{
			if (!character)
				return;

			bool refreshHud = false;

			{
				std::lock_guard<std::mutex> lock(g_pendingMutex);

				for (int i = 0; i < kAttrCount; ++i)
				{
					PendingEdit& edit = g_pendingValue[i];
					if (!edit.pending)
						continue;

					if (SDK::FGameplayAttributeData* attribute = GetCurrentAttribute(character, static_cast<AttrId>(i)))
					{
						SetAttributeValue(*attribute, edit.value);
						refreshHud = true;
					}
					edit.pending = false;
				}

				if (g_pendingMaxHealth.pending)
				{
					if (SDK::UCrHealthAttributeSet* health = character->HealthAttributes)
					{
						SetAttributeValue(health->MaxHealth, g_pendingMaxHealth.value);
						SetAttributeValue(health->CurrentHealth, g_pendingMaxHealth.value);
						refreshHud = true;
					}
					g_pendingMaxHealth.pending = false;
				}
			}

			// Already on the game thread here (called from Tick) — refresh directly
			// rather than hopping through PostToGameThread again.
			if (refreshHud)
				RefreshHealthHudOnGameThread(nullptr);
		}

		// -------------------------------------------------------------------------
		// Shared row widget — one aligned table row: label, a slider that both
		// displays and edits the live value (dragging it queues a write that
		// Tick() applies on the game thread), a snap-to button (max for resources,
		// min for afflictions), and the Locked checkbox on the right of the bar.
		// -------------------------------------------------------------------------
		void RenderAttributeRow(IModLoaderImGui* imgui, const char* label, AttrId id, const AttrSnapshot& attr,
			const char* snapLabel, float snapTarget)
		{
			LockSlot& lock = g_locks[static_cast<int>(id)];

			// While unlocked, keep the slider following the live attribute value;
			// once locked, it holds steady at the value Tick() pins to.
			if (!lock.locked)
				lock.value = attr.current;

			imgui->PushIDStr(label);
			imgui->TableNextRow(0, 0.0f);

			imgui->TableSetColumnIndex(0);
			imgui->Text(label);

			imgui->TableSetColumnIndex(1);
			imgui->SetNextItemWidth(-1.0f);
			char format[32];
			snprintf(format, sizeof(format), "%%.0f / %.0f", attr.max);
			if (imgui->SliderFloat("##value", &lock.value, attr.min, attr.max, format))
				QueueAttributeEdit(id, lock.value);

			imgui->SameLine(0.0f, -1.0f);
			if (imgui->SmallButton(snapLabel))
			{
				lock.value = snapTarget;
				QueueAttributeEdit(id, snapTarget);
			}

			imgui->TableSetColumnIndex(2);
			RenderLockCheckbox(imgui, lock);

			imgui->PopID();
		}

		void RenderResourceRow(IModLoaderImGui* imgui, const char* label, AttrId id, const AttrSnapshot& attr)
		{
			RenderAttributeRow(imgui, label, id, attr, "Fill", attr.max);
		}

		void RenderAfflictionRow(IModLoaderImGui* imgui, const char* label, AttrId id, const AttrSnapshot& attr)
		{
			RenderAttributeRow(imgui, label, id, attr, "Clear", attr.min);
		}
	}

	void Initialize()
	{
		IPluginSelf* self = GetSelf();
		if (self)
		{
			self->hooks->World->RegisterOnWorldBeginPlay(&OnWorldBeginPlay);
			self->hooks->World->RegisterOnAfterWorldEndPlay(&OnWorldEndPlay);

			// Hot-reload: the world begin-play event already fired before we registered
			// for it, so probe the current world directly to pick up an in-progress session.
			try
			{
				SDK::UWorld* world = SDK::UWorld::GetWorld();
				if (world && world->GetName() == kChimeraMainWorldName)
					g_inChimeraMain = true;
			}
			catch (...) {}
		}

		IPluginScanner* scanner = GetScanner();
		if (!scanner)
			return;

		if (uintptr_t address = scanner->FindPatternInMainModule(AOB::HealthHud_SetupProgressBar))
			g_setupProgressBar = reinterpret_cast<HealthHud_SetupProgressBarFn>(address);
		else
			LOG_WARN("Failed to resolve UCrUW_HealthHud::SetupProgressBar pattern — HUD will not refresh immediately.");
	}

	void Shutdown()
	{
		if (IPluginSelf* self = GetSelf())
		{
			self->hooks->World->UnregisterOnWorldBeginPlay(&OnWorldBeginPlay);
			self->hooks->World->UnregisterOnAfterWorldEndPlay(&OnWorldEndPlay);
		}
	}

	void Tick(float /*deltaSeconds*/)
	{
		if (!g_inChimeraMain)
			return;

		SDK::ACrCharacterPlayerBase* character = GetLocalCharacter();

		// Keep RenderImGui()'s snapshot fresh and apply any edits it queued —
		// both must happen here, on the game thread.
		RefreshSnapshot(character);
		ApplyPendingEdits(character);

		if (!character)
			return;

		for (int i = 0; i < kAttrCount; ++i)
		{
			LockSlot& lock = g_locks[i];
			if (!lock.locked)
				continue;

			if (SDK::FGameplayAttributeData* attribute = GetCurrentAttribute(character, static_cast<AttrId>(i)))
				SetAttributeValue(*attribute, lock.value);
		}
	}

	void RenderImGui(IModLoaderImGui* imgui)
	{
		// RenderImGui() runs on the ImGui render thread, not the game thread —
		// it must only ever read the snapshot Tick() refreshed, never touch
		// SDK/UObjects directly (see GetLocalCharacter()'s comment for why).
		AttributesSnapshot snapshot;
		{
			std::lock_guard<std::mutex> lock(g_snapshotMutex);
			snapshot = g_snapshot;
		}

		if (!snapshot.hasCharacter)
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

		const AttrSnapshot& health = snapshot.slots[static_cast<int>(AttrId::Health)];
		if (snapshot.hasHealth && health.present)
		{
			if (imgui->BeginTable("##health_table", 3, kAttributeTableFlags))
			{
				SetupAttributeTableColumns(imgui);
				RenderAttributeRow(imgui, "Health", AttrId::Health, health, "Full Heal", health.max);
				imgui->EndTable();
			}

			imgui->Spacing();

			static float maxHealthValue = 0.0f;
			static bool  maxHealthInit  = false;
			if (!maxHealthInit)
			{
				maxHealthValue = snapshot.maxHealth;
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
					QueueMaxHealthEdit(maxHealthValue);

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

			auto renderIfPresent = [&](const char* label, AttrId id)
			{
				const AttrSnapshot& attr = snapshot.slots[static_cast<int>(id)];
				if (attr.present)
					RenderResourceRow(imgui, label, id, attr);
			};

			renderIfPresent("Energy",    AttrId::Energy);
			renderIfPresent("Shield",    AttrId::Shield);
			renderIfPresent("Hydration", AttrId::Hydration);
			renderIfPresent("Calories",  AttrId::Calories);
			renderIfPresent("Oxygen",    AttrId::Oxygen);

			imgui->EndTable();
		}

		// ---------------------------------------------------------------------
		// Afflictions
		// ---------------------------------------------------------------------
		imgui->SeparatorText("Afflictions");

		if (imgui->BeginTable("##afflictions_table", 3, kAttributeTableFlags))
		{
			SetupAttributeTableColumns(imgui);

			auto renderIfPresent = [&](const char* label, AttrId id)
			{
				const AttrSnapshot& attr = snapshot.slots[static_cast<int>(id)];
				if (attr.present)
					RenderAfflictionRow(imgui, label, id, attr);
			};

			renderIfPresent("Toxicity",    AttrId::Toxicity);
			renderIfPresent("Radiation",   AttrId::Radiation);
			renderIfPresent("Heat",        AttrId::Heat);
			renderIfPresent("Drain",       AttrId::Drain);
			renderIfPresent("Corrosion",   AttrId::Corrosion);
			renderIfPresent("Infection",   AttrId::Infection);
			renderIfPresent("Temperature", AttrId::Temperature);

			imgui->EndTable();
		}
	}
}
