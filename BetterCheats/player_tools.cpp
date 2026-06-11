#include "player_tools.h"
#include "plugin_helpers.h"
#include "aob_patterns.h"
#include "session_config.h"

#include "Chimera_classes.hpp"

#include <cstdint>

namespace BetterCheats::Panels::Tools
{
	namespace
	{
		// ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp
		constexpr int kToolsTableFlags = (1 << 6) | (1 << 9) | (3 << 13);
		// ImGuiTableColumnFlags_WidthFixed
		constexpr int kColumnWidthFixed = 1 << 4;

		using GetMiningDamageFn            = float(__fastcall*)(void* self, bool isHittingWeakSpot);
		using UpdateRepHarvesterHeatStackFn = void(__fastcall*)(void* self);

		GetMiningDamageFn             g_originalGetMiningDamage            = nullptr;
		HookHandle                    g_hookGetMiningDamage                 = nullptr;

		UpdateRepHarvesterHeatStackFn g_originalUpdateRepHarvesterHeatStack = nullptr;
		HookHandle                    g_hookUpdateRepHarvesterHeatStack      = nullptr;

		bool     g_overload        = false;
		bool     g_noDrillOverheat = false;
		int32_t  g_overheatTickCounter = 0;

		float __fastcall Detour_GetMiningDamage(void* self, bool isHittingWeakSpot)
		{
			if (g_overload)
				return 10000.0f;

			return g_originalGetMiningDamage(self, isHittingWeakSpot);
		}

		void __fastcall Detour_UpdateRepHarvesterHeatStack(void* self)
		{
			if (g_noDrillOverheat)
				return;

			g_originalUpdateRepHarvesterHeatStack(self);
		}

		void InstallHook(const char* pattern, void* detour, void** original, HookHandle* outHandle, const char* name)
		{
			IPluginScanner* scanner = GetScanner();
			if (!scanner) { LOG_WARN("Tools: scanner unavailable, %s hook skipped", name); return; }

			uintptr_t addr = scanner->FindPatternInMainModule(pattern);
			if (!addr) { LOG_WARN("Tools: %s pattern not found", name); return; }

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

		SDK::ACrCharacterPlayerBase* GetLocalCharacter()
		{
			SDK::UWorld* world = nullptr;
			try { world = SDK::UWorld::GetWorld(); }
			catch (...) { return nullptr; }
			if (!world) return nullptr;

			SDK::ACrPlayerControllerBase* pc = reinterpret_cast<SDK::ACrPlayerControllerBase*>(SDK::UGameplayStatics::GetPlayerController(world, 0));
			if (!pc || !pc->Pawn) return nullptr;

			return static_cast<SDK::ACrCharacterPlayerBase*>(pc->Pawn);
		}
	}

	void Initialize()
	{
		InstallHook(
			AOB::GetMiningDamage,
			reinterpret_cast<void*>(&Detour_GetMiningDamage),
			reinterpret_cast<void**>(&g_originalGetMiningDamage),
			&g_hookGetMiningDamage,
			"GetMiningDamage");

		InstallHook(
			AOB::UpdateRepHarvesterHeatStack,
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

		g_overload        = SessionConfig::Get("playerTools.overloadMining", false);
		g_noDrillOverheat = SessionConfig::Get("playerTools.noDrillOverheat", false);

		LOG_INFO("Tools: applied saved config for session '%s'.", SessionConfig::GetSessionName().c_str());
	}

	void RenderImGui(IModLoaderImGui* imgui)
	{
		imgui->SeparatorText("Mining");

		if (imgui->BeginTable("##mining_table", 2, kToolsTableFlags))
		{
			imgui->TableSetupColumn("Option", kColumnWidthFixed, 500.0f);
			imgui->TableSetupColumn("Enabled", 0, 0.0f);

			imgui->TableNextRow(0, 0.0f);
			imgui->TableSetColumnIndex(0);
			imgui->Text("Overload Handheld Mining Laser");
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
	}
}
