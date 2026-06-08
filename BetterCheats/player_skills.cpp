#include "player_skills.h"
#include "plugin_helpers.h"

#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>

#include "Chimera_classes.hpp"

namespace BetterCheats::Panels::Skills
{
	namespace
	{
		// ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp
		constexpr int kSkillTableFlags = (1 << 6) | (1 << 9) | (3 << 13);
		// ImGuiTableColumnFlags_WidthFixed
		constexpr int kColumnWidthFixed = 1 << 4;

		// ECrPlayerProgressionSkill currently only defines Movement/Combat/Survival —
		// generous headroom for future skills without growing the snapshot dynamically.
		constexpr int kMaxSkills = 16;

		void SetupSkillTableColumns(IModLoaderImGui* imgui)
		{
			imgui->TableSetupColumn("Skill", kColumnWidthFixed, 110.0f);
			imgui->TableSetupColumn("Level", 0, 0.0f);
		}

		// Only ever called from Tick() (the engine-tick callback, which runs on the
		// game thread) — never from RenderImGui(), which runs on a different thread
		// where UWorld::GetWorld() and UObject access intermittently crash inside the
		// renderer (FD3D12DynamicRHI::HandleFailedD3D12Result, no useful callstack).
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

		// -------------------------------------------------------------------------
		// Snapshot — plain data describing the controller's current skill levels.
		// Refreshed on the game thread by Tick() and read by RenderImGui(), which
		// runs on a different thread and must never touch SDK/UObjects directly
		// (doing so intermittently crashes inside the renderer with no useful
		// callstack — FD3D12DynamicRHI::HandleFailedD3D12Result).
		// -------------------------------------------------------------------------
		struct SkillsSnapshot
		{
			bool    hasController = false;
			int     count = 0;
			uint8_t skillIds[kMaxSkills] = {};
			int32_t levels[kMaxSkills]   = {};
		};

		std::mutex     g_snapshotMutex;
		SkillsSnapshot g_snapshot;

		// Edits queued by RenderImGui() (UI thread) for Tick() (game thread) to apply.
		struct PendingEdit { bool pending = false; int32_t level = 0; };

		std::mutex  g_pendingMutex;
		PendingEdit g_pendingLevel[kMaxSkills];
		bool        g_pendingMaxAll = false;

		void QueueLevelEdit(int index, int32_t level)
		{
			if (index < 0 || index >= kMaxSkills)
				return;

			std::lock_guard<std::mutex> lock(g_pendingMutex);
			g_pendingLevel[index] = { true, level };
		}

		void QueueMaxAllSkills()
		{
			std::lock_guard<std::mutex> lock(g_pendingMutex);
			g_pendingMaxAll = true;
		}

		// Copies the controller's current skill state into the snapshot — must run
		// on the game thread.
		void RefreshSnapshot(SDK::ACrPlayerControllerBase* pc)
		{
			SkillsSnapshot snapshot;
			snapshot.hasController = pc != nullptr;

			if (pc)
			{
				SDK::TArray<SDK::FCrSkillData>& skills = pc->PlayerSkills;
				const SDK::FCrSkillData* data = skills.GetDataPtr();

				const int rawCount = skills.Num();
				snapshot.count = rawCount < kMaxSkills ? rawCount : kMaxSkills;
				for (int i = 0; i < snapshot.count; ++i)
				{
					snapshot.skillIds[i] = static_cast<uint8_t>(data[i].Skill);
					snapshot.levels[i]   = data[i].Level;
				}
			}

			std::lock_guard<std::mutex> lock(g_snapshotMutex);
			g_snapshot = snapshot;
		}

		// Applies any edits RenderImGui() queued since the last tick — must run on
		// the game thread.
		void ApplyPendingEdits(SDK::ACrPlayerControllerBase* pc)
		{
			if (!pc)
				return;

			SDK::TArray<SDK::FCrSkillData>& skills = pc->PlayerSkills;
			const int count = skills.Num();
			SDK::FCrSkillData* data = const_cast<SDK::FCrSkillData*>(skills.GetDataPtr());

			std::lock_guard<std::mutex> lock(g_pendingMutex);

			if (g_pendingMaxAll)
			{
				for (int i = 0; i < count; ++i)
				{
					data[i].Level      = 999;
					data[i].Experience = 0.0f;
				}
				g_pendingMaxAll = false;
			}

			for (int i = 0; i < count && i < kMaxSkills; ++i)
			{
				PendingEdit& edit = g_pendingLevel[i];
				if (!edit.pending)
					continue;

				data[i].Level = edit.level;
				edit.pending  = false;
			}
		}
	}

	void Tick(float /*deltaSeconds*/)
	{
		SDK::ACrPlayerControllerBase* pc = GetLocalController();

		// Keep RenderImGui()'s snapshot fresh and apply any edits it queued —
		// both must happen here, on the game thread.
		RefreshSnapshot(pc);
		ApplyPendingEdits(pc);
	}

	void RenderImGui(IModLoaderImGui* imgui)
	{
		imgui->SeparatorText("Skill Progression");

		// RenderImGui() runs on the ImGui render thread, not the game thread —
		// it must only ever read the snapshot Tick() refreshed, never touch
		// SDK/UObjects directly (see GetLocalController()'s comment for why).
		SkillsSnapshot snapshot;
		{
			std::lock_guard<std::mutex> lock(g_snapshotMutex);
			snapshot = g_snapshot;
		}

		if (!snapshot.hasController)
		{
			imgui->TextDisabled("No local player controller found.");
			return;
		}

		if (snapshot.count <= 0)
		{
			imgui->TextDisabled("Player has no skill data yet.");
			return;
		}

		if (imgui->Button("Max All Skills"))
			QueueMaxAllSkills();

		imgui->Spacing();
		imgui->SeparatorText("Per-Skill Overrides");

		char idBuf[16];
		if (imgui->BeginTable("##skills_table", 2, kSkillTableFlags))
		{
			SetupSkillTableColumns(imgui);

			for (int i = 0; i < snapshot.count; ++i)
			{
				const std::string& name = LocalizedSkillName(snapshot.skillIds[i]);

				snprintf(idBuf, sizeof(idBuf), "skill_%d", i);
				imgui->PushIDStr(idBuf);

				imgui->TableNextRow(0, 0.0f);

				imgui->TableSetColumnIndex(0);
				imgui->Text(name.c_str());

				imgui->TableSetColumnIndex(1);
				imgui->SetNextItemWidth(-1.0f);
				int level = snapshot.levels[i];
				if (imgui->SliderInt("##level", &level, 0, 100, "%d"))
					QueueLevelEdit(i, level);

				imgui->PopID();
			}

			imgui->EndTable();
		}

		imgui->Spacing();
		imgui->TextColored(1.0f, 0.3f, 0.3f, 1.0f,
			"Note: Setting skills here will persist in the player save file, "
			"even if BetterCheats is later removed.");
	}
}
