#include "player_skills.h"
#include "plugin_helpers.h"

#include <cstdint>
#include <cstdio>
#include <string>

#include "Chimera_classes.hpp"

namespace BetterCheats::Panels::Skills
{
	namespace
	{
		SDK::ACrPlayerControllerBase* GetLocalController()
		{
			SDK::UWorld* world = nullptr;
			try { world = SDK::UWorld::GetWorld(); }
			catch (...) { return nullptr; }
			if (!world) return nullptr;

			SDK::APlayerController* pc = SDK::UGameplayStatics::GetPlayerController(world, 0);
			return static_cast<SDK::ACrPlayerControllerBase*>(pc);
		}

		// FTextKey is just a 4-byte interned-string-table index (confirmed in IDA —
		// not reflected in the dumped SDK, so we mirror its layout locally).
		struct FTextKey { int32_t Index; };

		using MakeTextKeyFn           = void (__fastcall*)(FTextKey*, const wchar_t*);
		using AsLocalizableAdvancedFn = SDK::FText* (__fastcall*)(SDK::FText*, const FTextKey*, const FTextKey*, const wchar_t*);

		// Loc info confirmed via UCrUW_PlayerProgressionSkill::InitSkill — namespace is
		// always "CrPlayerProgression"; keys/fallbacks below are keyed by raw skill ID
		// (ECrPlayerProgressionSkill itself carries no named values in the SDK).
		struct SkillLocInfo
		{
			const wchar_t* locKey;
			const wchar_t* fallback;
			const char*    englishName; // used if the text trampolines are unavailable
		};

		constexpr SkillLocInfo kSkillLoc[] =
		{
			{ L"MovementSkill", L"MOVEMENT", "Movement" },
			{ L"CombatSkill",   L"COMBAT",   "Combat"   },
			{ L"SurvivalSkill", L"SURVIVAL", "Survival" },
		};
		constexpr int kSkillLocCount = sizeof(kSkillLoc) / sizeof(kSkillLoc[0]);

		// Localized names only need to be resolved once — cache per skill ID.
		std::string g_localizedNames[kSkillLocCount];
		bool        g_localizedReady[kSkillLocCount] = {};

		const std::string& LocalizedSkillName(uint8_t id)
		{
			static const std::string unknown = "Unknown";
			if (id >= kSkillLocCount)
				return unknown;

			if (g_localizedReady[id])
				return g_localizedNames[id];

			g_localizedNames[id] = kSkillLoc[id].englishName; // default until/unless localized
			g_localizedReady[id] = true;

			IPluginTextUtils* text = GetHooks() ? GetHooks()->Text : nullptr;
			if (!text)
				return g_localizedNames[id];

			const uintptr_t makeKeyAddr  = text->MakeTextKey();
			const uintptr_t localizeAddr = text->AsLocalizable_Advanced();
			if (!makeKeyAddr || !localizeAddr)
				return g_localizedNames[id];

			auto makeKey  = reinterpret_cast<MakeTextKeyFn>(makeKeyAddr);
			auto localize = reinterpret_cast<AsLocalizableAdvancedFn>(localizeAddr);

			FTextKey ns{};
			FTextKey key{};
			makeKey(&ns,  L"CrPlayerProgression");
			makeKey(&key, kSkillLoc[id].locKey);

			SDK::FText result{};
			if (localize(&result, &ns, &key, kSkillLoc[id].fallback) && result.TextData)
				g_localizedNames[id] = result.ToString();

			return g_localizedNames[id];
		}
	}

	void RenderImGui(IModLoaderImGui* imgui)
	{
		imgui->SeparatorText("Skill Progression");

		SDK::ACrPlayerControllerBase* pc = GetLocalController();
		if (!pc)
		{
			imgui->TextDisabled("No local player controller found.");
			return;
		}

		SDK::TArray<SDK::FCrSkillData>& skills = pc->PlayerSkills;
		const int count = skills.Num();
		if (count <= 0)
		{
			imgui->TextDisabled("Player has no skill data yet.");
			return;
		}

		SDK::FCrSkillData* data = const_cast<SDK::FCrSkillData*>(skills.GetDataPtr());

		if (imgui->Button("Max All Skills"))
		{
			for (int i = 0; i < count; ++i)
			{
				data[i].Level      = 999;
				data[i].Experience = 0.0f;
			}
		}

		imgui->Spacing();
		imgui->SeparatorText("Per-Skill Overrides");

		char idBuf[16];
		char label[48];
		for (int i = 0; i < count; ++i)
		{
			const uint8_t skillId = static_cast<uint8_t>(data[i].Skill);
			const std::string& name = LocalizedSkillName(skillId);

			snprintf(idBuf, sizeof(idBuf), "skill_%d", i);
			imgui->PushIDStr(idBuf);

			snprintf(label, sizeof(label), "%s Level", name.c_str());
			imgui->SliderInt(label, &data[i].Level, 0, 100, "%d");

			imgui->PopID();
		}

		imgui->Spacing();
		imgui->TextColored(1.0f, 0.3f, 0.3f, 1.0f,
			"Note: Setting skills here will persist in the player save file, "
			"even if BetterCheats is later removed.");
	}
}
