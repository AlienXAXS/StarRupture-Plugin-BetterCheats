#include "player_tools.h"
#include "plugin_helpers.h"
#include "aob_resolver.h"
#include "session_config.h"
#include "ui_widgets.h"
#include "player_lookup.h"
#include "attribute_compose.h"
#include "cheat_math.h"
#include "cdo_lookup.h"
#include "game_thread.h"
#include "object_ref.h"

#include "Chimera_classes.hpp"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace BetterCheats::Panels::Tools
{
	namespace
	{
		// ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp
		constexpr int kToolsTableFlags = (1 << 6) | (1 << 9) | (3 << 13);

		constexpr const char* kChimeraMainWorldName = "ChimeraMain";

		// What "One Hit Kill Laser" replaces mining damage with.
		constexpr float kOneHitKillDamage = 10000.0f;

		using GetMiningDamageFn = float(__fastcall*)(void* self, bool isHittingWeakSpot);

		GetMiningDamageFn g_originalGetMiningDamage = nullptr;
		HookHandle        g_hookGetMiningDamage     = nullptr;

		// Written from RenderImGui, read from Tick and the detour below.
		std::atomic<bool> g_overload{ false };   // One Hit Kill Laser
		std::atomic<bool> g_noDrillOverheat{ false };

		// ---------------------------------------------------------------------
		// No Handheld Drill Overheat. The game's own heat logic (and its
		// UnlimitedWeaponHeat cheat) stops heating a weapon while the player
		// carries the Cheat.UnlimitedWeaponHeat gameplay tag, so the toggle
		// just keeps that tag on the local character as a loose tag. Loose tag
		// counts stack, so the tag is only added when the character has none,
		// and only a tag we added is ever removed.
		//
		// All of this is game thread only (Tick, the world-end callback, and
		// Shutdown when it runs there). The holder is a validated reference,
		// not a raw pointer, because the character dies on respawn and travel.
		// ---------------------------------------------------------------------
		constexpr float kHeatTagRecheckSeconds = 1.0f;

		SDK::FGameplayTag                       g_unlimitedHeatTag = {};
		ObjectRef<SDK::ACrCharacterPlayerBase>  g_heatTagHolder;   // the character we added the tag to
		bool                                    g_heatTagWanted    = false;
		float                                   g_heatTagTimer     = 0.0f;

		// ---------------------------------------------------------------------
		// Mining tool stats. The tool's item data is a UCrWeaponItemDataBase,
		// the same native class every gun's is, so its stats are the same
		// data-asset fields the Weapons panel reads. There is one mining tool
		// (upgraded in place), so one CDO: Default__BP_MiningTool_C. Overheat
		// cooling lives on the passive cooling ability's CDO instead, one
		// instance shared by every tool, and is reached by reflected name.
		//
		// Every row is a multiplier on the value the CDO held before we
		// touched it, written into the CDO only when the slider moves (or the
		// CDO is found again after unloading) -- never every tick -- and
		// handed back when the row returns to 1.00x, the world ends or the
		// plugin shuts down. Upgrades and buffs the game layers on top of the
		// CDO still apply on top of ours.
		// ---------------------------------------------------------------------
		enum StatIndex : int
		{
			kStatMiningDamage = 0, kStatWeakpoint, kStatEnemyDamage, kStatRange,
			kStatHitRate, kStatAggro, kStatCooling, kStatCount
		};

		// The UCrWeaponAttributeSet multiplier the game layers on top of a
		// stat while the tool is in hand, read for the readout only (never
		// written) -- the same attributes the Weapons panel composes for guns.
		enum class ModsSource { None, Damage, FireRate, Aggro };

		struct StatDef
		{
			const char* label;
			const char* key;       // SessionConfig / preset field key
			const char* tooltip;
			float       minValue, maxValue, step;
			ModsSource  mods;
		};

		const StatDef kStats[kStatCount] = {
			{ "Mining Damage", "miningDamageMult",
			  "Multiplies the mining tool's base damage against ore. Weak spots,\n"
			  "upgrades and buffs still apply on top. One Hit Kill Laser overrides it.",
			  0.10f, 25.0f, 0.05f, ModsSource::None },
			{ "Weak Spot Multiplier", "miningWeakpoint",
			  "Multiplies the tool's bonus multiplier for hitting a weak spot.",
			  0.10f, 10.0f, 0.05f, ModsSource::None },
			{ "Enemy Damage", "miningEnemyDamage",
			  "Multiplies the tool's base damage against creatures. Upgrades and\n"
			  "buffs still add on top.",
			  0.10f, 25.0f, 0.05f, ModsSource::Damage },
			{ "Range", "miningRange",
			  "Multiplies how far the mining beam reaches.",
			  0.50f, 5.0f, 0.05f, ModsSource::None },
			{ "Hit Rate", "miningHitRate",
			  "Multiplies how many hits a second the beam lands. Higher is faster.\n"
			  "Shown in hits per second.",
			  0.25f, 5.0f, 0.05f, ModsSource::FireRate },
			{ "Aggro", "miningAggro",
			  "Multiplies how likely mining is to draw nearby enemies. Lower is\n"
			  "stealthier; 0.00x never draws them.",
			  0.00f, 2.0f, 0.05f, ModsSource::Aggro },
			{ "Overheat Cooldown", "miningCooling",
			  "Multiplies how long one heat stack takes to cool off once you stop\n"
			  "mining. Lower cools faster, so the drill overheats less. Shown in\n"
			  "seconds per heat stack.",
			  0.05f, 5.0f, 0.05f, ModsSource::None },
		};

		constexpr const char* kStatFormat = "%.2fx";

		constexpr const char* kToolCdoName        = "Default__BP_MiningTool_C";
		constexpr const char* kCoolingClassName   = "GA_MiningToolPassiveCooling_C";
		constexpr const char* kCoolingPropertyName = "SingleStackDuration";

		// Safety clamp on the value written, not the slider's own range.
		constexpr float kStatFinalMin = 0.0f;
		constexpr float kStatFinalMax = 1000000.0f;

		// Slider values, written from RenderImGui and read from Tick.
		std::atomic<float> g_statValue[kStatCount] = { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };

		// Game-thread only.
		BetterCheats::ComposedAttribute       g_composed[kStatCount];
		ObjectRef<SDK::UCrWeaponItemDataBase> g_toolCdo;
		bool                                  g_toolCdoHeld      = false;   // g_toolCdo was set, so losing it needs a Forget
		bool                                  g_toolCdoMissLogged = false;
		float                                 g_toolCdoRetry     = 0.0f;
		CdoFieldHandle                        g_coolingField;
		bool                                  g_statApplied[kStatCount] = {};
		float                                 g_appliedAmount[kStatCount] = {};

		// Readout, published by Tick for RenderImGui.
		std::atomic<bool>  g_statResolved[kStatCount];
		std::atomic<float> g_statOriginal[kStatCount];     // the field before our change, in its own units
		std::atomic<float> g_statCurrent[kStatCount];      // the field right now
		// The game's multiplier over the field, 1 = none. Only refreshed while
		// the tool is in hand, so it doubles as "last seen" for the estimate.
		std::atomic<float> g_statMods[kStatCount] = { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
		std::atomic<bool>  g_toolEquipped{ false };

		// UCrMiningBoostAttributeSet on the character, a plain attribute write
		// every tick while it differs from 1.0 -- the attribute belongs to the
		// character, which the game re-creates on respawn and travel.
		std::atomic<float> g_miningBoostValue{ 1.0f };
		std::atomic<float> g_boostGame{ -1.0f };         // CurrentValue before this tick's write, -1 = no character
		std::atomic<float> g_boostUnmodified{ -1.0f };   // what the game held before we took over
		bool               g_boostWasActive = false;     // game-thread only

		bool StatActive(float amount) { return BetterCheats::DiffersFromDefault(amount, 1.0f); }

		std::string ValueKey(int s)   { return std::string("playerTools.") + kStats[s].key + ".value"; }
		std::string ComposeKey(int s) { return std::string("playerTools.compose.") + kStats[s].key; }

		float SliderValue(int s)
		{
			return BetterCheats::SafeValue(g_statValue[s].load(), 1.0f, kStats[s].minValue, kStats[s].maxValue);
		}

		// The data-asset field behind each row the mining tool's CDO holds
		// (everything except Overheat Cooldown).
		float* ToolField(SDK::UCrWeaponItemDataBase* cdo, int s)
		{
			switch (s)
			{
			case kStatMiningDamage: return &cdo->MiningTypeDamage;
			case kStatWeakpoint:    return &cdo->WeakpointDamageMultiplier;
			case kStatEnemyDamage:  return &cdo->BaseDamage.Value;
			case kStatRange:        return &cdo->BaseRange.Value;
			case kStatHitRate:      return &cdo->RoundsPerMinute.Value;
			case kStatAggro:        return &cdo->ShotAggro;
			default:                return nullptr;
			}
		}

		// Hit Rate's slider is a rate, but RoundsPerMinute.Value holds the
		// seconds between hits despite its name (see player_weapons.cpp's
		// RealBaseForAttr), so a faster rate is a shorter interval.
		float FieldAmount(int s, float amount) { return (s == kStatHitRate) ? 1.0f / amount : amount; }

		void MarkUnapplied(int s)
		{
			g_statApplied[s] = false;
			g_statResolved[s].store(false);
		}

		// Composes row `s` onto `field`, only when its slider has moved since
		// the last write or its CDO is newly found. Returns true if it ran.
		bool ApplyStatIfChanged(int s, const void* owner, float& field)
		{
			const float amount = SliderValue(s);
			if (g_statApplied[s] && amount == g_appliedAmount[s])
				return false;

			const bool active = StatActive(amount);
			BetterCheats::ApplyComposedRow(g_composed[s], owner, field, ComposeKey(s), active,
				FieldAmount(s, amount), BetterCheats::ComposedAttribute::Mode::Multiply, kStatFinalMin, kStatFinalMax);

			g_statApplied[s]   = true;
			g_appliedAmount[s] = amount;
			g_statOriginal[s].store(active ? g_composed[s].GetGame() : field);
			g_statResolved[s].store(true);
			return true;
		}

		SDK::UCrWeaponItemDataBase* ResolveToolCdo(float deltaSeconds)
		{
			if (SDK::UCrWeaponItemDataBase* cdo = g_toolCdo.Get())
				return cdo;

			if (g_toolCdoHeld)
			{
				// Unloaded since it was found: nothing left to hand back.
				for (int s = 0; s < kStatCount; ++s)
				{
					if (s == kStatCooling) continue;
					g_composed[s].Forget();
					MarkUnapplied(s);
				}
				g_toolCdoHeld  = false;
				g_toolCdoRetry = 0.0f;
			}

			if (!BetterCheats::CdoRetryDue(g_toolCdoRetry, deltaSeconds))
				return nullptr;

			auto* object = static_cast<SDK::UObject*>(BetterCheats::FindObjectByName(kToolCdoName));
			if (!object || !object->IsA(SDK::UCrWeaponItemDataBase::StaticClass()))
			{
				if (!g_toolCdoMissLogged)
				{
					LOG_WARN("Tools: '%s' not found (yet) -- mining tool stats stay unapplied until it resolves.", kToolCdoName);
					g_toolCdoMissLogged = true;
				}
				return nullptr;
			}

			auto* cdo = static_cast<SDK::UCrWeaponItemDataBase*>(object);
			g_toolCdo.Set(cdo);
			g_toolCdoHeld = true;
			LOG_INFO("Tools: resolved '%s' -- mining damage %.2f, weak spot x%.2f, enemy damage %.2f, "
			         "range %.2f, %.3f s between hits, aggro %.3f.",
				kToolCdoName, cdo->MiningTypeDamage, cdo->WeakpointDamageMultiplier, cdo->BaseDamage.Value,
				cdo->BaseRange.Value, cdo->RoundsPerMinute.Value, cdo->ShotAggro);
			return cdo;
		}

		void ApplyToolStats(SDK::UCrWeaponItemDataBase* cdo)
		{
			for (int s = 0; s < kStatCount; ++s)
			{
				if (s == kStatCooling) continue;
				float& field = *ToolField(cdo, s);
				ApplyStatIfChanged(s, cdo, field);
				g_statCurrent[s].store(field);
			}
		}

		// Overheat Cooldown: a double on an ability CDO, reached by name.
		void ApplyCooling(float deltaSeconds)
		{
			IPluginHooks* hooks = GetHooks();
			IPluginObjectProperties* props = hooks ? hooks->ObjectProperties : nullptr;
			if (!props)
				return;

			if (g_coolingField.ok && !g_coolingField.object.Get())
			{
				// Unloaded since it was found: nothing left to hand back.
				g_composed[kStatCooling].Forget();
				MarkUnapplied(kStatCooling);
			}

			SDK::UObject* object = BetterCheats::ResolveCdoField(g_coolingField, kCoolingClassName,
				kCoolingPropertyName, deltaSeconds, "Tools");
			if (!object)
				return;

			double raw = 0.0;
			if (!props->GetFloatProperty(object, g_coolingField.property, &raw))
				return;

			float value = static_cast<float>(raw);
			const float before = value;
			if (ApplyStatIfChanged(kStatCooling, object, value) && value != before)
				props->SetFloatProperty(object, g_coolingField.property, static_cast<double>(value));
			g_statCurrent[kStatCooling].store(value);
		}

		float ModsMultiplier(SDK::UCrWeaponAttributeSet* weapons, ModsSource source)
		{
			const SDK::FGameplayAttributeData* attr = nullptr;
			switch (source)
			{
			case ModsSource::Damage:   attr = &weapons->DamageModMultiplier;       break;
			case ModsSource::FireRate: attr = &weapons->FireRateModMultiplier;     break;
			case ModsSource::Aggro:    attr = &weapons->AggroPerShotModMultiplier; break;
			case ModsSource::None:     return 1.0f;
			}
			// Whatever the game adds over the attribute's own base, the same
			// "buffed - base" delta the Weapons panel reads for guns.
			return 1.0f + (attr->CurrentValue - attr->BaseValue);
		}

		// Hands every stat back to the value the game held before we changed
		// it -- or, off the game thread (the loader's RELOAD button runs
		// shutdown on the render thread), forgets it without touching any
		// UObject; ApplyComposedRow's RestoreIfStale then undoes the leftover
		// on the next activation. Drops every cached CDO either way.
		void ReleaseAllStats()
		{
			const bool onGameThread = BetterCheats::IsGameThread();

			SDK::UCrWeaponItemDataBase* cdo = onGameThread ? g_toolCdo.Get() : nullptr;
			for (int s = 0; s < kStatCount; ++s)
			{
				if (s == kStatCooling) continue;
				if (cdo)
				{
					g_composed[s].Release(cdo, *ToolField(cdo, s));
					BetterCheats::ClearComposeState(ComposeKey(s));
				}
				else
				{
					g_composed[s].Forget();
				}
				MarkUnapplied(s);
			}

			IPluginHooks* hooks = GetHooks();
			IPluginObjectProperties* props = hooks ? hooks->ObjectProperties : nullptr;
			SDK::UObject* cooling = (onGameThread && g_coolingField.ok) ? g_coolingField.object.Get() : nullptr;
			double raw = 0.0;
			if (props && cooling && props->GetFloatProperty(cooling, g_coolingField.property, &raw))
			{
				float value = static_cast<float>(raw);
				const float before = value;
				g_composed[kStatCooling].Release(cooling, value);
				if (value != before)
					props->SetFloatProperty(cooling, g_coolingField.property, static_cast<double>(value));
				BetterCheats::ClearComposeState(ComposeKey(kStatCooling));
			}
			else
			{
				g_composed[kStatCooling].Forget();
			}
			MarkUnapplied(kStatCooling);

			g_toolCdo.Reset();
			g_toolCdoHeld  = false;
			g_toolCdoRetry = 0.0f;
			g_coolingField = BetterCheats::CdoFieldHandle{};
			g_toolEquipped.store(false);
		}

		// How many copies of the unlimited heat tag the character carries.
		int32_t HeatTagCount(SDK::ACrCharacterPlayerBase* character)
		{
			if (g_unlimitedHeatTag.TagName.IsNone())
				g_unlimitedHeatTag.TagName = SDK::BasicFilesImplUtils::StringToName(L"Cheat.UnlimitedWeaponHeat");

			SDK::UCrAbilitySystemComponent* asc = character ? character->GetCrAbilitySystemComponent() : nullptr;
			return (asc && !g_unlimitedHeatTag.TagName.IsNone()) ? asc->GetGameplayTagCount(g_unlimitedHeatTag) : 0;
		}

		void SetHeatTag(SDK::ACrCharacterPlayerBase* character, bool present)
		{
			SDK::FGameplayTagContainer tags;
			tags.GameplayTags.Add(g_unlimitedHeatTag);

			if (present)
				SDK::UAbilitySystemBlueprintLibrary::AddLooseGameplayTags(character, tags, false);
			else
				SDK::UAbilitySystemBlueprintLibrary::RemoveLooseGameplayTags(character, tags, false);
		}

		// Takes the tag back off the character we added it to, if it is still
		// there. Off the game thread (the stock loader's RELOAD button runs
		// shutdown on the render thread) nothing is touched; the reference is
		// just dropped.
		void ReleaseHeatTag()
		{
			if (!BetterCheats::IsGameThread())
			{
				g_heatTagHolder.Reset();
				return;
			}

			SDK::ACrCharacterPlayerBase* held = g_heatTagHolder.Get();
			g_heatTagHolder.Reset();
			if (!held)
				return;

			try
			{
				if (HeatTagCount(held) > 0)
					SetHeatTag(held, false);
			}
			catch (...) {}
		}

		// Keeps the tag on `character` while the toggle is on, and off every
		// other character. Toggling is picked up on the next tick; a new
		// character, or a tag something else removed, within a second.
		void UpdateHeatTag(float deltaSeconds, SDK::ACrCharacterPlayerBase* character)
		{
			const bool wanted = g_noDrillOverheat.load();
			if (wanted != g_heatTagWanted)
			{
				g_heatTagWanted = wanted;
				g_heatTagTimer  = 0.0f;
			}

			SDK::ACrCharacterPlayerBase* held = g_heatTagHolder.Get();
			if (held && (held != character || !wanted))
			{
				ReleaseHeatTag();
				held = nullptr;
			}
			if (!wanted || !character)
				return;

			g_heatTagTimer -= deltaSeconds;
			if (g_heatTagTimer > 0.0f)
				return;
			g_heatTagTimer = kHeatTagRecheckSeconds;

			try
			{
				// A count above zero on a character we did not tag means the
				// game's own cheat holds it: nothing to add, nothing to remove.
				const int32_t count = HeatTagCount(character);
				if (held)
				{
					if (count == 0)
						SetHeatTag(character, true);
				}
				else if (count == 0)
				{
					SetHeatTag(character, true);
					g_heatTagHolder.Set(character);
				}
			}
			catch (...) {}
		}

		void OnBeforeWorldEndPlay(SDK::UWorld* /*world*/, const char* worldName)
		{
			if (!worldName || std::strcmp(worldName, kChimeraMainWorldName) != 0)
				return;

			ReleaseHeatTag();
			ReleaseAllStats();
			g_boostGame.store(-1.0f);
			g_boostWasActive = false;
		}

		float __fastcall Detour_GetMiningDamage(void* self, bool isHittingWeakSpot)
		{
			if (g_overload.load())
				return kOneHitKillDamage;

			return g_originalGetMiningDamage(self, isHittingWeakSpot);
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

		// ---------------------------------------------------------------------
		// Built-in presets, as multipliers of the tool's own stats. Entries end
		// at stat < 0.
		// ---------------------------------------------------------------------
		struct PresetVal { int stat; float value; };

		struct Preset
		{
			const char* label;
			const char* tooltip;
			const char* credit;
			PresetVal   vals[kStatCount + 1];
		};

		// The mod gives aggro only as "less" and "even less", so half and a
		// quarter are our own reading of it.
		const Preset kPresets[] = {
			{ "Better Mining",
			  "Mining damage x2.5 (130 to 325), enemy damage x1.2 (5 to 6), range\n"
			  "35 to 40, 20 to 25 hits a second, and half the aggro.",
			  "Modelled on 'Better Mining Tool' by astroboy314,\nNexusMod #105 for StarRupture",
			  { {kStatMiningDamage, 2.5f}, {kStatEnemyDamage, 1.2f}, {kStatRange, 40.0f / 35.0f},
			    {kStatHitRate, 1.25f}, {kStatAggro, 0.5f}, {-1, 0.0f} } },
			{ "Better Mining OP",
			  "Mining damage x4 (130 to 520), enemy damage x2 (5 to 10), range\n"
			  "35 to 50, 20 to 25 hits a second, and a quarter of the aggro.",
			  "Modelled on 'Better Mining Tool' (OP version) by astroboy314,\nNexusMod #105 for StarRupture",
			  { {kStatMiningDamage, 4.0f}, {kStatEnemyDamage, 2.0f}, {kStatRange, 50.0f / 35.0f},
			    {kStatHitRate, 1.25f}, {kStatAggro, 0.25f}, {-1, 0.0f} } },
		};
		constexpr int kPresetCount = static_cast<int>(sizeof(kPresets) / sizeof(kPresets[0]));

		void SetStat(int s, float value)
		{
			g_statValue[s].store(value);
			SessionConfig::Set(ValueKey(s), value);
		}

		void RenderPresetButtons(IModLoaderImGui* imgui)
		{
			imgui->TextDisabled("Presets:");
			for (int i = 0; i < kPresetCount; ++i)
			{
				const Preset& preset = kPresets[i];
				imgui->SameLine(0.0f, -1.0f);

				if (imgui->SmallButton(preset.label))
				{
					// A preset is a starting point, not a mode: clear every stat
					// first so leftovers from a previous preset don't survive.
					for (int s = 0; s < kStatCount; ++s)
						SetStat(s, 1.0f);
					for (const PresetVal& v : preset.vals)
					{
						if (v.stat < 0) break;
						SetStat(v.stat, v.value);
					}
					LOG_INFO("Tools: applied preset '%s'", preset.label);
				}
				if (imgui->IsItemHovered())
				{
					char tip[384];
					snprintf(tip, sizeof(tip), "%s%s%s",
						preset.tooltip,
						preset.credit ? "\n\n" : "",
						preset.credit ? preset.credit : "");
					imgui->SetTooltip(tip);
				}
			}
		}

		void RenderSavedPresets(IModLoaderImGui* imgui)
		{
			static BetterCheats::UI::SavedPresetRowState s_presetRow;
			constexpr int kFieldCount = kStatCount + 3;   // + One Hit Kill Laser, No Overheat, Mining Boost
			BetterCheats::PresetStore::Field fields[kFieldCount];

			auto getLive = [](BetterCheats::PresetStore::Field* out)
			{
				for (int s = 0; s < kStatCount; ++s)
					out[s] = { kStats[s].key, g_statValue[s].load() };
				out[kStatCount]     = { "overloadMining",  g_overload.load()        ? 1.0f : 0.0f };
				out[kStatCount + 1] = { "noDrillOverheat", g_noDrillOverheat.load() ? 1.0f : 0.0f };
				out[kStatCount + 2] = { "miningBoost",     g_miningBoostValue.load() };
			};
			auto applyFields = [](const BetterCheats::PresetStore::Field* f, int count)
			{
				for (int s = 0; s < kStatCount && s < count; ++s)
					SetStat(s, f[s].value);
				if (count > kStatCount)
				{
					g_overload.store(f[kStatCount].value != 0.0f);
					SessionConfig::Set("playerTools.overloadMining", g_overload.load());
				}
				if (count > kStatCount + 1)
				{
					g_noDrillOverheat.store(f[kStatCount + 1].value != 0.0f);
					SessionConfig::Set("playerTools.noDrillOverheat", g_noDrillOverheat.load());
				}
				if (count > kStatCount + 2)
				{
					g_miningBoostValue.store(f[kStatCount + 2].value);
					SessionConfig::Set("playerTools.miningBoost.value", f[kStatCount + 2].value);
				}
			};
			// Only the built-ins above should be guarded against; a saved
			// preset can't take one of their names.
			auto isBuiltin = [](const char* name)
			{
				for (int i = 0; i < kPresetCount; ++i)
					if (strcmp(kPresets[i].label, name) == 0)
						return true;
				return false;
			};
			// "<built-in> Custom" only when every stat the preset touches
			// matches, every other stat is still 1.00x, and the rows presets
			// never set are at rest too -- same rule the weapon presets use.
			auto computeSuggest = [](char* out, int cap)
			{
				for (int i = 0; i < kPresetCount; ++i)
				{
					const Preset& preset = kPresets[i];

					bool touched[kStatCount] = {};
					bool matches = true;
					for (const PresetVal& v : preset.vals)
					{
						if (v.stat < 0) break;
						if (std::fabs(g_statValue[v.stat].load() - v.value) > BetterCheats::kActiveEpsilon)
						{
							matches = false;
							break;
						}
						touched[v.stat] = true;
					}
					for (int s = 0; s < kStatCount && matches; ++s)
						if (!touched[s] && StatActive(g_statValue[s].load()))
							matches = false;
					if (matches && (g_overload.load() || g_noDrillOverheat.load() || StatActive(g_miningBoostValue.load())))
						matches = false;

					if (matches)
					{
						snprintf(out, cap, "%s Custom", preset.label);
						return;
					}
				}
				snprintf(out, cap, "Custom");
			};

			BetterCheats::UI::RenderSavedPresetsRow(imgui, "tools_saved_presets", "Tools",
				fields, kFieldCount, getLive, applyFields, isBuiltin, computeSuggest, s_presetRow);
		}

		void RenderStatRow(IModLoaderImGui* imgui, int s, bool equipped, float labelReserve)
		{
			const StatDef& def = kStats[s];
			float value = g_statValue[s].load();
			const bool overridden = (s == kStatMiningDamage && g_overload.load());
			const bool active     = StatActive(value) && !overridden;

			imgui->PushIDInt(s);
			imgui->TableNextRow(0, 0.0f);

			char changeDesc[24];
			BetterCheats::UI::FormatChangeDesc(changeDesc, sizeof(changeDesc),
				overridden ? BetterCheats::ComposedAttribute::Mode::Absolute : BetterCheats::ComposedAttribute::Mode::Multiply,
				overridden ? kOneHitKillDamage : value);

			BetterCheats::UI::RowSpec spec;
			spec.label         = def.label;
			spec.tooltip       = def.tooltip;
			spec.value         = &value;
			spec.minValue      = def.minValue;
			spec.maxValue      = def.maxValue;
			spec.step          = def.step;
			spec.format        = kStatFormat;
			spec.resetValue    = 1.0f;
			spec.active        = active;
			spec.disableSlider = overridden;
			spec.composing     = active || overridden;
			spec.changeDesc    = changeDesc;
			spec.tagWord       = "mods";
			spec.baseLabel     = "Base (no upgrades)";
			spec.estimateNote  = "Estimated -- equip the mining tool to see live values.";
			spec.labelReserve  = labelReserve;

			// Nothing to show until Tick has found the CDO and read the field.
			const float original = g_statOriginal[s].load();
			if (g_statResolved[s].load() && original > 0.0f)
			{
				// Multiplier space, relative to the field before our change --
				// Hit Rate's field is an interval, so its ratio is inverted.
				const float current    = g_statCurrent[s].load();
				const float fieldRatio = (s == kStatHitRate) ? (current > 0.0f ? original / current : 0.0f)
				                                             : current / original;
				const float mods   = g_statMods[s].load();
				const float amount = active ? value : 1.0f;

				BetterCheats::UI::MultiplyReadout readout;
				readout.live       = equipped;
				readout.base       = 1.0f;
				// The game's own total without our change: the mods multiply
				// the whole field, ours included, so theirs is what's left.
				readout.unmodified = amount * mods - (amount - 1.0f);
				readout.game       = fieldRatio * mods;
				// unmodified - base in SetMultiplyReadout's terms, using the current
				// slider value, not the one in effect when the mods were read.
				readout.cachedMods = amount * (mods - 1.0f);
				readout.amount     = value;
				readout.active     = active;
				readout.realBase   = (s == kStatHitRate) ? 1.0f / original : original;
				BetterCheats::UI::SetMultiplyReadout(spec, readout);

				if (overridden)
				{
					spec.hasOurs  = false;
					spec.ours     = 0.0f;
					spec.expected = kOneHitKillDamage;
					spec.game     = kOneHitKillDamage;
				}
			}

			if (BetterCheats::UI::BuildRow(imgui, spec).changed)
				SetStat(s, value);

			imgui->PopID();
		}

		constexpr const char* kBoostLabel = "Mining Boost";

		void RenderBoostRow(IModLoaderImGui* imgui, float labelReserve)
		{
			float value = g_miningBoostValue.load();
			const bool active = BetterCheats::DiffersFromDefault(value, 1.0f);

			imgui->PushIDInt(kStatCount);
			imgui->TableNextRow(0, 0.0f);

			char changeDesc[24];
			snprintf(changeDesc, sizeof(changeDesc), "set to x%.2f", value);

			BetterCheats::UI::RowSpec spec;
			spec.label        = kBoostLabel;
			spec.tooltip      = "UCrMiningBoostAttributeSet::CurrentBoostMultiplierValue, set directly.\n"
			                    "An attribute write rather than a hook, so a game update cannot break it.";
			spec.value        = &value;
			spec.minValue     = 0.10f;
			spec.maxValue     = 10.0f;
			spec.step         = 0.05f;
			spec.format       = kStatFormat;
			spec.resetValue   = 1.0f;
			spec.active       = active;
			spec.composing    = active;
			spec.changeDesc   = changeDesc;
			spec.labelReserve = labelReserve;

			const float game = g_boostGame.load();
			if (game >= 0.0f)
			{
				const float unmodified = g_boostUnmodified.load();
				spec.showLive   = true;
				spec.game       = game;
				spec.expected   = active ? value : game;
				spec.unmodified = (unmodified >= 0.0f) ? unmodified : game;
			}

			if (BetterCheats::UI::BuildRow(imgui, spec).changed)
			{
				g_miningBoostValue.store(value);
				SessionConfig::Set("playerTools.miningBoost.value", value);
			}

			imgui->PopID();
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

		if (IPluginSelf* self = GetSelf())
			self->hooks->World->RegisterOnBeforeWorldEndPlay(&OnBeforeWorldEndPlay);
	}

	void Shutdown()
	{
		if (IPluginSelf* self = GetSelf())
			self->hooks->World->UnregisterOnBeforeWorldEndPlay(&OnBeforeWorldEndPlay);

		RemoveHook(&g_hookGetMiningDamage, reinterpret_cast<void**>(&g_originalGetMiningDamage), "GetMiningDamage");

		ReleaseHeatTag();
		try { ReleaseAllStats(); }
		catch (...) {}
	}

	void Tick(float deltaSeconds)
	{
		BetterCheats::RecordGameThread();

		SDK::UCrWeaponItemDataBase* cdo = nullptr;
		try
		{
			cdo = ResolveToolCdo(deltaSeconds);
			if (cdo)
				ApplyToolStats(cdo);
			ApplyCooling(deltaSeconds);
		}
		catch (...) {}

		SDK::ACrCharacterPlayerBase* character = GetLocalCharacter();
		UpdateHeatTag(deltaSeconds, character);
		if (!character)
		{
			g_toolEquipped.store(false);
			g_boostGame.store(-1.0f);
			return;
		}

		try
		{
			// The data asset the weapon component last equipped IS this CDO
			// while the mining tool is in hand (the Weapons panel keys its tabs
			// off the same pointer).
			SDK::UCrWeaponComponent* weaponSystem = character->WeaponSystem;
			const bool equipped = cdo && weaponSystem && weaponSystem->LastEquippedWeaponData == cdo;
			g_toolEquipped.store(equipped);

			SDK::UCrWeaponAttributeSet* weapons = character->WeaponAttributes;
			if (equipped && weapons)
			{
				for (int s = 0; s < kStatCount; ++s)
					g_statMods[s].store(ModsMultiplier(weapons, kStats[s].mods));
			}
		}
		catch (...) {}

		// Mining boost. MaxBoost is raised alongside Current because the game
		// clamps Current against it.
		const float boostValue  = g_miningBoostValue.load();
		const bool  boostActive = BetterCheats::DiffersFromDefault(boostValue, 1.0f);
		try
		{
			if (SDK::UCrMiningBoostAttributeSet* boost = character->MiningBoostAttributes)
			{
				const float before = boost->CurrentBoostMultiplierValue.CurrentValue;
				g_boostGame.store(before);
				if (!boostActive || !g_boostWasActive)
					g_boostUnmodified.store(before);

				if (boostActive)
				{
					if (boost->MaxBoostMultiplierValue.CurrentValue < boostValue)
					{
						boost->MaxBoostMultiplierValue.BaseValue    = boostValue;
						boost->MaxBoostMultiplierValue.CurrentValue = boostValue;
					}
					boost->CurrentBoostMultiplierValue.BaseValue    = boostValue;
					boost->CurrentBoostMultiplierValue.CurrentValue = boostValue;
				}
				g_boostWasActive = boostActive;
			}
		}
		catch (...) {}
	}

	void ApplySavedConfig()
	{
		if (!SessionConfig::IsLoaded())
			return;

		g_overload.store(SessionConfig::Get("playerTools.overloadMining", false));
		g_noDrillOverheat.store(SessionConfig::Get("playerTools.noDrillOverheat", false));
		for (int s = 0; s < kStatCount; ++s)
			g_statValue[s].store(SessionConfig::Get(ValueKey(s), 1.0f));
		g_miningBoostValue.store(SessionConfig::Get("playerTools.miningBoost.value", 1.0f));

		LOG_INFO("Tools: applied saved config for session '%s'.", SessionConfig::GetSessionName().c_str());
	}

	void RenderImGui(IModLoaderImGui* imgui)
	{
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
				                  "Overrides the Mining Damage row below while enabled.");
			imgui->TableSetColumnIndex(1);
			bool overload = g_overload.load();
			if (imgui->Checkbox("##overload_mining", &overload))
			{
				g_overload.store(overload);
				SessionConfig::Set("playerTools.overloadMining", overload);
			}

			imgui->TableNextRow(0, 0.0f);
			imgui->TableSetColumnIndex(0);
			imgui->Text("No Handheld Drill Overheat");
			imgui->TableSetColumnIndex(1);
			bool noDrillOverheat = g_noDrillOverheat.load();
			if (imgui->Checkbox("##no_drill_overheat", &noDrillOverheat))
			{
				g_noDrillOverheat.store(noDrillOverheat);
				SessionConfig::Set("playerTools.noDrillOverheat", noDrillOverheat);
			}

			imgui->EndTable();
		}

		imgui->Spacing();
		imgui->SeparatorText("Mining Power");

		// Built-ins, then saved presets, then the rows they set -- the same
		// order every other group uses.
		RenderPresetButtons(imgui);
		imgui->Spacing();
		RenderSavedPresets(imgui);
		imgui->Spacing();

		imgui->TextDisabled("Multipliers apply to the tool's base stats. Reset a row to turn it off.");

		const float labelReserve = BetterCheats::UI::PrescanLabelWidth(imgui, kStatCount + 1,
			[](int i) { return (i < kStatCount) ? kStats[i].label : kBoostLabel; });
		const bool equipped = g_toolEquipped.load();

		if (imgui->BeginTable("##mining_power_table", 3, kToolsTableFlags))
		{
			imgui->TableSetupColumn("Attribute", BetterCheats::UI::kColumnWidthFixed,
				BetterCheats::UI::GetReadoutColumnWidth(imgui, labelReserve));
			imgui->TableSetupColumn("Value",     0, 0.54f);
			imgui->TableSetupColumn("",          0, 0.10f);

			for (int s = 0; s < kStatCount; ++s)
				RenderStatRow(imgui, s, equipped, labelReserve);
			RenderBoostRow(imgui, labelReserve);

			imgui->EndTable();
		}

		// Always reserved, so equipping or holstering the tool never shifts
		// anything below this line.
		imgui->TextDisabled(equipped ? "" : "Estimated. Live values show once the mining tool is equipped.");

		imgui->Spacing();
		imgui->Separator();
		imgui->TextDisabled("Presets modelled on astroboy314's Better Mining Tool pak mod (NexusMod #105).");
	}
}
