#include "player_items.h"
#include "plugin_helpers.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "Chimera_classes.hpp"
#include "AuItems_classes.hpp"
#include "Engine_classes.hpp"

namespace BetterCheats::Panels::Items
{
	namespace
	{
		// ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp
		constexpr int kItemTableFlags = (1 << 6) | (1 << 9) | (3 << 13);
		// ImGuiTableColumnFlags_WidthFixed
		constexpr int kColumnWidthFixed = 1 << 4;

		struct ItemEntry
		{
			SDK::UAuItemDataBase* item = nullptr;
			std::string           name;
		};

		std::vector<ItemEntry> g_items;
		bool                   g_itemsLoaded   = false;
		int                    g_selectedIndex = -1;
		int                    g_stacks        = 1;

		void SetupItemTableColumns(IModLoaderImGui* imgui)
		{
			imgui->TableSetupColumn("Property", kColumnWidthFixed, 110.0f);
			imgui->TableSetupColumn("Value", 0, 0.0f);
		}

		SDK::ACrPlayerControllerBase* GetLocalController()
		{
			SDK::UWorld* world = nullptr;
			try { world = SDK::UWorld::GetWorld(); }
			catch (...) { return nullptr; }
			if (!world) return nullptr;

			SDK::APlayerController* pc = SDK::UGameplayStatics::GetPlayerController(world, 0);
			return static_cast<SDK::ACrPlayerControllerBase*>(pc);
		}

		// Pulls the live item registry from the world's item database subsystem and
		// resolves each entry's display name via Kismet (UAuItemDataBase::ItemName is
		// an FText — Conv_TextToString mirrors how the game's own UI resolves it).
		void RefreshItemList()
		{
			g_items.clear();
			g_selectedIndex = -1;
			g_itemsLoaded   = true;

			try
			{
				SDK::UWorld* world = SDK::UWorld::GetWorld();
				if (!world)
					return;

				auto* subsystem = static_cast<SDK::UAuItemDataBaseSubsystem*>(
					SDK::USubsystemBlueprintLibrary::GetWorldSubsystem(world, SDK::UAuItemDataBaseSubsystem::StaticClass()));
				if (!subsystem)
					return;

				SDK::UAuItemDataBase* const* data = subsystem->ItemDataBaseArray.GetDataPtr();
				for (int i = 0; i < subsystem->ItemDataBaseArray.Num(); ++i)
				{
					SDK::UAuItemDataBase* item = data[i];
					if (!item)
						continue;

					ItemEntry entry;
					entry.item = item;
					entry.name = SDK::UKismetTextLibrary::Conv_TextToString(item->ItemName).ToString();
					if (entry.name.empty())
						entry.name = item->UniqueItemName.ToString();

					g_items.push_back(std::move(entry));
				}

				std::sort(g_items.begin(), g_items.end(),
					[](const ItemEntry& a, const ItemEntry& b) { return a.name < b.name; });

				LOG_DEBUG("Item Spawner: loaded %d item(s) from UAuItemDataBaseSubsystem.", static_cast<int>(g_items.size()));
			}
			catch (...)
			{
				LOG_DEBUG("Item Spawner: exception while resolving the item database subsystem.");
			}
		}
	}

	void RenderImGui(IModLoaderImGui* imgui)
	{
		if (!g_itemsLoaded)
			RefreshItemList();

		imgui->SeparatorText("Item Spawner");
		imgui->TextWrapped("Pick an item and a number of stacks, then give it to your "
			"local player. The total amount given is stacks \xC3\x97 the item's stack size.");
		imgui->Spacing();

		if (imgui->Button("Refresh Item List"))
			RefreshItemList();

		imgui->SameLine(0.0f, -1.0f);
		char countLabel[64];
		snprintf(countLabel, sizeof(countLabel), "%d item(s) loaded", static_cast<int>(g_items.size()));
		imgui->TextDisabled(countLabel);

		imgui->Spacing();

		if (g_items.empty())
		{
			imgui->TextDisabled("No items found — open a world and try Refresh Item List.");
			return;
		}

		const bool hasSelection = g_selectedIndex >= 0 && g_selectedIndex < static_cast<int>(g_items.size());
		const ItemEntry* selected = hasSelection ? &g_items[g_selectedIndex] : nullptr;

		const int maxStack     = (selected && selected->item->MaxStack > 0) ? selected->item->MaxStack : 1;
		const int totalAmount  = (g_stacks > 0 ? g_stacks : 1) * maxStack;

		if (imgui->BeginTable("##item_spawner_table", 2, kItemTableFlags))
		{
			SetupItemTableColumns(imgui);

			// Item dropdown
			imgui->TableNextRow(0, 0.0f);
			imgui->TableSetColumnIndex(0);
			imgui->Text("Item");

			imgui->TableSetColumnIndex(1);
			imgui->SetNextItemWidth(-1.0f);
			const char* preview = selected ? selected->name.c_str() : "Select an item...";
			if (imgui->BeginCombo("##item_select", preview))
			{
				for (int i = 0; i < static_cast<int>(g_items.size()); ++i)
				{
					imgui->PushIDInt(i);
					if (imgui->Selectable(g_items[i].name.c_str(), i == g_selectedIndex))
						g_selectedIndex = i;
					imgui->PopID();
				}
				imgui->EndCombo();
			}

			// Stacks input
			imgui->TableNextRow(0, 0.0f);
			imgui->TableSetColumnIndex(0);
			imgui->Text("Stacks");

			imgui->TableSetColumnIndex(1);
			imgui->SetNextItemWidth(-1.0f);
			imgui->InputInt("##stacks", &g_stacks, 1, 10);
			if (g_stacks < 1)
				g_stacks = 1;

			// Stack size (read-only info)
			imgui->TableNextRow(0, 0.0f);
			imgui->TableSetColumnIndex(0);
			imgui->Text("Stack Size");

			imgui->TableSetColumnIndex(1);
			char stackSizeText[32];
			snprintf(stackSizeText, sizeof(stackSizeText), "%d", maxStack);
			imgui->Text(selected ? stackSizeText : "-");

			// Total amount (read-only info)
			imgui->TableNextRow(0, 0.0f);
			imgui->TableSetColumnIndex(0);
			imgui->Text("Total Amount");

			imgui->TableSetColumnIndex(1);
			char totalText[32];
			snprintf(totalText, sizeof(totalText), "%d", totalAmount);
			imgui->Text(selected ? totalText : "-");

			imgui->EndTable();
		}

		imgui->Spacing();

		if (!selected)
		{
			imgui->TextDisabled("Select an item to give it to your local player.");
			return;
		}

		if (imgui->Button("Give Item"))
		{
			if (SDK::ACrPlayerControllerBase* controller = GetLocalController())
			{
				controller->ServerCheatGiveItem(selected->item, totalAmount);
				LOG_INFO("Item Spawner: gave %d x \"%s\" (%d stack(s) of %d).",
					totalAmount, selected->name.c_str(), g_stacks, maxStack);
			}
			else
			{
				LOG_WARN("Item Spawner: no local player controller found — cannot give item.");
			}
		}

		// Loot-box-at-feet spawning would land here once the relevant pickup-container
		// class (the in-world "loot box" actor) is identified in the SDK.
	}
}
