#include "world_corporations.h"
#include "plugin_helpers.h"

#include "Chimera_classes.hpp"

#include <atomic>
#include <cstdio>
#include <mutex>

namespace BetterCheats::Panels::Corporations
{
	namespace
	{
		// -------------------------------------------------------------------------
		// Snapshot — populated on game thread (Tick), read on ImGui render thread.
		// Never touch SDK objects from RenderImGui().
		// -------------------------------------------------------------------------
		struct CorporationsSnapshot
		{
			bool ownerFound  = false;
			int  dataPoints  = 0;
		};

		std::mutex            g_snapshotMutex;
		CorporationsSnapshot  g_snapshot;

		SDK::ACrCorporationsOwner* GetCorporationsOwner()
		{
			try
			{
				SDK::UWorld* world = SDK::UWorld::GetWorld();
				if (!world || !world->GameState) return nullptr;

				return static_cast<SDK::ACrGameStateBase*>(world->GameState)->CorporationsOwner;
			}
			catch (...)
			{
				return nullptr;
			}
		}

		void RefreshSnapshot()
		{
			CorporationsSnapshot snap;
			if (SDK::ACrCorporationsOwner* owner = GetCorporationsOwner())
			{
				snap.ownerFound = true;
				snap.dataPoints = owner->DataPoints;
			}

			std::lock_guard<std::mutex> lock(g_snapshotMutex);
			g_snapshot = snap;
		}

		// -------------------------------------------------------------------------
		// Pending edit — queued from the ImGui render thread, applied on the game
		// thread in Tick(). Last write wins within a single frame.
		// -------------------------------------------------------------------------
		std::atomic<bool> g_pending{ false };
		std::atomic<int>  g_pendingValue{ 0 };

		void ApplyPendingEdit()
		{
			if (!g_pending.exchange(false))
				return;

			try
			{
				if (SDK::ACrCorporationsOwner* owner = GetCorporationsOwner())
					owner->DataPoints = g_pendingValue.load();
			}
			catch (...)
			{
				LOG_ERROR("Corporations: exception setting DataPoints.");
			}
		}

		// -------------------------------------------------------------------------
		// Table layout — matches world_wave.cpp / player_attributes.cpp
		// ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp
		// -------------------------------------------------------------------------
		constexpr int kTableFlags  = (1 << 6) | (1 << 9) | (3 << 13);
		constexpr int kColumnFixed = 1 << 4; // ImGuiTableColumnFlags_WidthFixed

		void SetupCorporationsTableColumns(IModLoaderImGui* imgui)
		{
			imgui->TableSetupColumn("Property", kColumnFixed, 130.0f);
			imgui->TableSetupColumn("Value",    0,            0.0f);
			imgui->TableSetupColumn("Action",   kColumnFixed, 80.0f);
		}
	}

	void Initialize() {}
	void Shutdown()   {}

	void Tick(float /*deltaSeconds*/)
	{
		ApplyPendingEdit();
		RefreshSnapshot();
	}

	void RenderImGui(IModLoaderImGui* imgui)
	{
		CorporationsSnapshot snap;
		{
			std::lock_guard<std::mutex> lock(g_snapshotMutex);
			snap = g_snapshot;
		}

		imgui->SeparatorText("Data Points");

		if (!snap.ownerFound)
		{
			imgui->TextDisabled("Corporations owner not found.");
			return;
		}

		static int  editValue = 0;
		static bool  editValueInit = false;
		if (!editValueInit)
		{
			editValue = snap.dataPoints;
			editValueInit = true;
		}

		if (imgui->BeginTable("##corporations_table", 3, kTableFlags))
		{
			SetupCorporationsTableColumns(imgui);

			imgui->TableNextRow(0, 0.0f);
			imgui->TableSetColumnIndex(0); imgui->Text("Data Points");
			imgui->TableSetColumnIndex(1);
			imgui->SetNextItemWidth(-1.0f);
			char fmt[32];
			snprintf(fmt, sizeof(fmt), "Current: %d", snap.dataPoints);
			imgui->InputInt("##data_points", &editValue, 1, 100);
			// Typing a value beyond INT_MAX wraps negative inside InputInt's own
			// text parsing before we ever see it — clamp back to 0 every frame so
			// the field never holds (or applies) a negative DataPoints value.
			if (editValue < 0)
				editValue = 0;
			imgui->SetItemTooltip(fmt);
			imgui->TableSetColumnIndex(2);
			if (imgui->SmallButton("Apply"))
			{
				g_pendingValue.store(editValue);
				g_pending.store(true);
			}

			imgui->EndTable();
		}

		imgui->Spacing();
		imgui->TextColored(1.0f, 0.3f, 0.3f, 1.0f,
			"Note: Setting Data Points here will persist in the player save file, "
			"even if BetterCheats is later removed.");
	}
}
