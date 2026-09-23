#include "player_tools.h"
#include "plugin_helpers.h"
#include "aob_resolver.h"
#include "session_config.h"
#include "ui_widgets.h"
#include "player_lookup.h"

#include "Chimera_classes.hpp"

#include <atomic>
#include <cmath>
#include <cstdint>

namespace BetterCheats::Panels::Tools
{
	namespace
	{
		// ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp
		constexpr int kToolsTableFlags = (1 << 6) | (1 << 9) | (3 << 13);

		using GetMiningDamageFn            = float(__fastcall*)(void* self, bool isHittingWeakSpot);
		using UpdateRepHarvesterHeatStackFn = void(__fastcall*)(void* self);

		GetMiningDamageFn             g_originalGetMiningDamage            = nullptr;
		HookHandle                    g_hookGetMiningDamage                 = nullptr;

		UpdateRepHarvesterHeatStackFn g_originalUpdateRepHarvesterHeatStack = nullptr;
		HookHandle                    g_hookUpdateRepHarvesterHeatStack      = nullptr;

		bool     g_overload        = false;   // One Hit Kill Laser
		bool     g_noDrillOverheat = false;
		int32_t  g_overheatTickCounter = 0;

		// A value that differs from the game default is applied -- there is no separate
		// enable toggle to keep in sync, so resetting a row IS disabling it.
		constexpr float kActiveEpsilon = 0.0001f;

		// Scales the game's own mining damage instead of replacing it, so the tool
		// still respects weak spots and per-ore resistances. 1.0 = untouched. Written
		// from RenderImGui, read every tick from Tick and from the hooked detour below.
		std::atomic<float> g_damageMultiplier{ 1.0f };

		// UCrMiningBoostAttributeSet on the character. Unlike the two hooks above this
		// is a plain attribute write -- no byte-pattern scanning, so it cannot break
		// from pattern drift on a game update. 1.0 = untouched.
		std::atomic<float> g_miningBoostValue{ 1.0f };

		bool DamageMultActive() { return std::fabs(g_damageMultiplier.load() - 1.0f) > kActiveEpsilon; }
		bool MiningBoostActive() { return std::fabs(g_miningBoostValue.load()  - 1.0f) > kActiveEpsilon; }

		float __fastcall Detour_GetMiningDamage(void* self, bool isHittingWeakSpot)
		{
			if (g_overload)
				return 10000.0f;

			const float base = g_originalGetMiningDamage(self, isHittingWeakSpot);
			return DamageMultActive() ? base * g_damageMultiplier.load() : base;
		}

		void __fastcall Detour_UpdateRepHarvesterHeatStack(void* self)
		{
			if (g_noDrillOverheat)
				return;

			g_originalUpdateRepHarvesterHeatStack(self);
		}

		void InstallHook(uintptr_t addr, void* detour, void** original, HookHandle* outHandle, const char* name)
		{
			if (!addr) { LOG_WARN("Tools: %s unresolved, hook skipped", name); return; }

			IPluginHookUtils* hooks = GetHooks() ? GetHooks()->Hooks : nullptr;
			if (!hooks) { LOG_WARN("Tools: hook utils unavailable, %s hook skipped", name); return; }

			*outHandle = hooks->Install(addr, detour, original);
			if (!*outHandle)
			{
				LOG_WARN("Tools: failed to install %s hook", name);
			}
			else
			{
				LOG_INFO("Tools: %s hook installed", name);
			}
		}

		void RemoveHook(HookHandle* handle, void** original, const char* name)
		{
			IPluginHookUtils* hooks = GetHooks() ? GetHooks()->Hooks : nullptr;
			if (hooks && *handle)
			{
				hooks->Remove(*handle);
				*handle   = nullptr;
				*original = nullptr;
			}
		}

	}

	void Initialize()
	{
		InstallHook(
			AOB::Resolved().GetMiningDamage,
			reinterpret_cast<void*>(&Detour_GetMiningDamage),
			reinterpret_cast<void**>(&g_originalGetMiningDamage),
			&g_hookGetMiningDamage,
			"GetMiningDamage");

		InstallHook(
			AOB::Resolved().UpdateRepHarvesterHeatStack,
			reinterpret_cast<void*>(&Detour_UpdateRepHarvesterHeatStack),
			reinterpret_cast<void**>(&g_originalUpdateRepHarvesterHeatStack),
			&g_hookUpdateRepHarvesterHeatStack,
			"UpdateRepHarvesterHeatStack");
	}

	void Shutdown()
	{
		RemoveHook(&g_hookGetMiningDamage,            reinterpret_cast<void**>(&g_originalGetMiningDamage),            "GetMiningDamage");
		RemoveHook(&g_hookUpdateRepHarvesterHeatStack, reinterpret_cast<void**>(&g_originalUpdateRepHarvesterHeatStack), "UpdateRepHarvesterHeatStack");
	}

	void Tick(float /*deltaSeconds*/)
	{
		SDK::ACrCharacterPlayerBase* character = GetLocalCharacter();
		if (!character)
			return;

		// Mining boost -- plain attribute write, no hook. MaxBoost is raised alongside
		// Current because the game clamps Current against it.
		if (MiningBoostActive())
		{
			const float boostValue = g_miningBoostValue.load();
			try
			{
				if (SDK::UCrMiningBoostAttributeSet* boost = character->MiningBoostAttributes)
				{
					if (boost->MaxBoostMultiplierValue.CurrentValue < boostValue)
					{
						boost->MaxBoostMultiplierValue.BaseValue    = boostValue;
						boost->MaxBoostMultiplierValue.CurrentValue = boostValue;
					}
					boost->CurrentBoostMultiplierValue.BaseValue    = boostValue;
					boost->CurrentBoostMultiplierValue.CurrentValue = boostValue;
				}
			}
			catch (...) {}
		}

		SDK::UCrAbilitySystemComponent* asc = character->GetCrAbilitySystemComponent();
		if (!asc)
			return;

		SDK::UOreDeveloperSettings* oreSettings = SDK::UOreDeveloperSettings::GetDefaultObj();
		if (!oreSettings)
			return;

		SDK::FGameplayTagContainer tags;
		tags.GameplayTags.Add(oreSettings->MiningHeatStackTag);

		try
		{
			SDK::TArray<SDK::FActiveGameplayEffectHandle> effects = asc->GetActiveEffectsWithAllTags(tags);

			if (g_noDrillOverheat)
			{
				++g_overheatTickCounter;
				if (g_overheatTickCounter >= 300)
				{
					g_overheatTickCounter = 0;
					for (int32_t i = 0; i < effects.Num(); ++i)
					{
						if (effects[i].Handle == -1)
							continue;

						const SDK::UGameplayEffect* ge = SDK::UAbilitySystemBlueprintLibrary::GetGameplayEffectFromActiveEffectHandle(effects[i]);
						if (!ge || ge->GetName() != "Default__GE_WeaponHeatStackBase_C")
							continue;

						asc->RemoveActiveGameplayEffect(effects[i], -1);
					}
				}
			}
		}
		catch (...) {}
	}

	void ApplySavedConfig()
	{
		if (!SessionConfig::IsLoaded())
			return;

		g_overload            = SessionConfig::Get("playerTools.overloadMining", false);
		g_noDrillOverheat     = SessionConfig::Get("playerTools.noDrillOverheat", false);
		g_damageMultiplier.store(SessionConfig::Get("playerTools.miningDamageMult.value", 1.0f));
		g_miningBoostValue.store(SessionConfig::Get("playerTools.miningBoost.value", 1.0f));

		LOG_INFO("Tools: applied saved config for session '%s'.", SessionConfig::GetSessionName().c_str());
	}

	void RenderImGui(IModLoaderImGui* imgui)
	{
		// No built-in presets here, so saved presets sit at the very top --
		// same "above every control" position every group uses.
		{
			static BetterCheats::UI::SavedPresetRowState s_presetRow;
			constexpr int kFieldCount = 4;
			BetterCheats::PresetStore::Field fields[kFieldCount];

			auto getLive = [](BetterCheats::PresetStore::Field* out)
			{
				out[0] = { "overloadMining",    g_overload        ? 1.0f : 0.0f };
				out[1] = { "noDrillOverheat",   g_noDrillOverheat ? 1.0f : 0.0f };
				out[2] = { "miningDamageMult",  g_damageMultiplier.load() };
				out[3] = { "miningBoost",       g_miningBoostValue.load() };
			};
			auto applyFields = [](const BetterCheats::PresetStore::Field* f, int count)
			{
				if (count > 0) { g_overload        = f[0].value != 0.0f; SessionConfig::Set("playerTools.overloadMining", g_overload); }
				if (count > 1) { g_noDrillOverheat = f[1].value != 0.0f; SessionConfig::Set("playerTools.noDrillOverheat", g_noDrillOverheat); }
				if (count > 2) { g_damageMultiplier.store(f[2].value); SessionConfig::Set("playerTools.miningDamageMult.value", f[2].value); }
				if (count > 3) { g_miningBoostValue.store(f[3].value); SessionConfig::Set("playerTools.miningBoost.value", f[3].value); }
			};
			auto isBuiltin      = [](const char*) { return false; };
			auto computeSuggest = [](char* out, int cap) { snprintf(out, cap, "Custom"); };

			BetterCheats::UI::RenderSavedPresetsRow(imgui, "tools_saved_presets", "Tools",
				fields, kFieldCount, getLive, applyFields, isBuiltin, computeSuggest, s_presetRow);
		}
		imgui->Spacing();

		imgui->SeparatorText("Mining");

		if (imgui->BeginTable("##mining_table", 2, kToolsTableFlags))
		{
			imgui->TableSetupColumn("Option", 0, 0.85f);
			imgui->TableSetupColumn("Enabled", 0, 0.15f);

			imgui->TableNextRow(0, 0.0f);
			imgui->TableSetColumnIndex(0);
			imgui->Text("One Hit Kill Laser");
			if (imgui->IsItemHovered())
				imgui->SetTooltip("Replaces mining damage with a flat 10000 - everything breaks in one hit.\n"
				                  "Overrides the damage multiplier below while enabled.");
			imgui->TableSetColumnIndex(1);
			if (imgui->Checkbox("##overload_mining", &g_overload))
				SessionConfig::Set("playerTools.overloadMining", g_overload);

			imgui->TableNextRow(0, 0.0f);
			imgui->TableSetColumnIndex(0);
			imgui->Text("No Handheld Drill Overheat");
			imgui->TableSetColumnIndex(1);
			if (imgui->Checkbox("##no_drill_overheat", &g_noDrillOverheat))
				SessionConfig::Set("playerTools.noDrillOverheat", g_noDrillOverheat);

			imgui->EndTable();
		}

		imgui->Spacing();
		imgui->SeparatorText("Mining Power");

		imgui->TextDisabled("A value that differs from 1.00x is applied. Reset a row to turn it off.");

		if (imgui->BeginTable("##mining_power_table", 3, kToolsTableFlags))
		{
			imgui->TableSetupColumn("Option", 0, 0.36f);
			imgui->TableSetupColumn("Value",  0, 0.54f);
			imgui->TableSetupColumn("",       0, 0.10f);

			// --- mining damage multiplier (scales the real value, unlike One Hit Kill)
			imgui->PushIDStr("mining_dmg");
			imgui->TableNextRow(0, 0.0f);
			imgui->TableSetColumnIndex(0);
			if (DamageMultActive() && !g_overload) imgui->Text("Mining Damage");
			else                                   imgui->TextDisabled("Mining Damage");
			if (imgui->IsItemHovered())
				imgui->SetTooltip("Multiplies the game's own mining damage, so weak spots and\n"
				                  "per-ore resistances still apply. Ignored while One Hit Kill is on.");
			imgui->TableSetColumnIndex(1);
			imgui->BeginDisabled(g_overload);
			imgui->SetNextItemWidth(-1.0f);
			float damageMultiplier = g_damageMultiplier.load();
			if (imgui->SliderFloat("##value", &damageMultiplier, 0.1f, 25.0f, "%.2fx"))
			{
				g_damageMultiplier.store(damageMultiplier);
				SessionConfig::Set("playerTools.miningDamageMult.value", damageMultiplier);
			}
			imgui->EndDisabled();
			imgui->TableSetColumnIndex(2);
			if (BetterCheats::UI::ResetButton(imgui, "##reset"))
			{
				g_damageMultiplier.store(1.0f);
				SessionConfig::Set("playerTools.miningDamageMult.value", 1.0f);
			}
			if (imgui->IsItemHovered())
				imgui->SetTooltip("Reset to the game default (turns this row off).");
			imgui->PopID();

			// --- UCrMiningBoostAttributeSet (attribute write, no byte-pattern hook)
			imgui->PushIDStr("mining_boost");
			imgui->TableNextRow(0, 0.0f);
			imgui->TableSetColumnIndex(0);
			if (MiningBoostActive()) imgui->Text("Mining Boost");
			else                     imgui->TextDisabled("Mining Boost");
			if (imgui->IsItemHovered())
				imgui->SetTooltip("UCrMiningBoostAttributeSet::CurrentBoostMultiplierValue.\n"
				                  "An attribute write rather than a hook, so a game update cannot break it.");
			imgui->TableSetColumnIndex(1);
			imgui->SetNextItemWidth(-1.0f);
			float miningBoostValue = g_miningBoostValue.load();
			if (imgui->SliderFloat("##value", &miningBoostValue, 0.1f, 10.0f, "%.2fx"))
			{
				g_miningBoostValue.store(miningBoostValue);
				SessionConfig::Set("playerTools.miningBoost.value", miningBoostValue);
			}
			imgui->TableSetColumnIndex(2);
			if (BetterCheats::UI::ResetButton(imgui, "##reset"))
			{
				g_miningBoostValue.store(1.0f);
				SessionConfig::Set("playerTools.miningBoost.value", 1.0f);
			}
			if (imgui->IsItemHovered())
				imgui->SetTooltip("Reset to the game default (turns this row off).");
			imgui->PopID();

			imgui->EndTable();
		}
	}
}
