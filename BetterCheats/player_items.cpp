#include "player_items.h"
#include "plugin_helpers.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "Chimera_classes.hpp"
#include "Chimera_parameters.hpp"
#include "AuItems_classes.hpp"
#include "AssetRegistry_classes.hpp"
#include "AssetRegistry_parameters.hpp"
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
			SDK::UAuItemDataBase* item     = nullptr;
			SDK::UObject*         icon     = nullptr;
			std::string           name;
			int                   maxStack = 1;
		};

		// -------------------------------------------------------------------------
		// RenderImGui() runs on the ImGui render thread, not the game thread —
		// resolving the world, querying the asset registry, and invoking UFunctions
		// via ProcessEvent from there intermittently crashes inside the renderer
		// with no useful callstack (FD3D12DynamicRHI::HandleFailedD3D12Result). All
		// of that is therefore posted to the game thread; results are handed back
		// through g_pendingItems for RenderImGui() to adopt.
		// -------------------------------------------------------------------------
		std::mutex             g_pendingMutex;
		std::vector<ItemEntry> g_pendingItems;
		bool                   g_pendingItemsReady = false;
		std::atomic<bool>      g_refreshInFlight{ false };

		// Owned and accessed exclusively by RenderImGui() (the render thread) —
		// no locking needed once AdoptPendingItemsIfReady() has copied data in.
		std::vector<ItemEntry> g_items;
		bool                   g_itemsLoaded   = false;
		int                    g_selectedIndex = -1;
		int                    g_stacks        = 1;

		char                   g_searchBuf[128] = {};
		std::vector<int>       g_filteredIndices;

		// Lower-cases ASCII letters; item names are plain English so this is sufficient
		// for case-insensitive substring matching without pulling in locale machinery.
		std::string ToLowerAscii(const std::string& s)
		{
			std::string out = s;
			for (char& c : out)
				if (c >= 'A' && c <= 'Z')
					c = static_cast<char>(c - 'A' + 'a');
			return out;
		}

		void RefreshFilteredItems()
		{
			g_filteredIndices.clear();

			const std::string needle = ToLowerAscii(g_searchBuf);
			for (int i = 0; i < static_cast<int>(g_items.size()); ++i)
			{
				if (needle.empty() || ToLowerAscii(g_items[i].name).find(needle) != std::string::npos)
					g_filteredIndices.push_back(i);
			}
		}

		void SetupItemTableColumns(IModLoaderImGui* imgui)
		{
			imgui->TableSetupColumn("Property", kColumnWidthFixed, 110.0f);
			imgui->TableSetupColumn("Value", 0, 0.0f);
		}

		// Only ever called on the game thread (from the posted callbacks below) —
		// never from RenderImGui().
		SDK::ACrPlayerControllerBase* GetLocalController()
		{
			SDK::UWorld* world = nullptr;
			try { world = SDK::UWorld::GetWorld(); }
			catch (...) { return nullptr; }
			if (!world) return nullptr;

			SDK::APlayerController* pc = SDK::UGameplayStatics::GetPlayerController(world, 0);
			return static_cast<SDK::ACrPlayerControllerBase*>(pc);
		}

		SDK::ACrCharacterPlayerBase* GetLocalCharacter()
		{
			SDK::ACrPlayerControllerBase* controller = GetLocalController();
			if (!controller || !controller->Pawn)
				return nullptr;

			return static_cast<SDK::ACrCharacterPlayerBase*>(controller->Pawn);
		}

		// ServerCheatGiveItem only does something useful when routed through real client/server
		// replication: ACrPlayerControllerBase::CheatGiveItem shows the authority (single-player
		// host) path resolves the pawn's inventory but never actually inserts the item — the
		// give-item logic for that path lives in UCrInventoryComponent::ServerDebugAddItem
		// instead, which walks the inventory's slots and calls AddNewItemToInventorySlot
		// directly. Call that on the local pawn's inventory component so single-player works.
		void CallServerDebugAddItem(SDK::UCrInventoryComponent* inventory, SDK::UAuItemDataBase* item, int32_t amount)
		{
			static SDK::UFunction* func = nullptr;
			if (!func)
				func = inventory->Class->GetFunction("CrInventoryComponent", "ServerDebugAddItem");
			if (!func)
			{
				LOG_WARN("Item Spawner: could not resolve CrInventoryComponent::ServerDebugAddItem.");
				return;
			}

			SDK::Params::CrInventoryComponent_ServerDebugAddItem parms{};
			parms.InItem = item;
			parms.Amount = amount;

			const auto flags = func->FunctionFlags;
			func->FunctionFlags |= 0x400; // FUNC_NetServer — force the call through even if flagged client-only locally
			inventory->ProcessEvent(func, &parms);
			func->FunctionFlags = flags;
		}

		// Returns the resource backing an item's icon brush — typically a UTexture2D —
		// or nullptr if the item has none configured. Not rendered anywhere yet; this
		// just resolves and stores it so an icon column can be wired into ImGui later
		// once texture binding is figured out.
		SDK::UObject* GetItemIconResource(SDK::UAuItemDataBase* item)
		{
			return item ? item->ItemIcon.ResourceObject : nullptr;
		}

		// AuItems.AuItemDataBase as a class path ("/Script/AuItems", "AuItemDataBase"),
		// used to query the asset registry for every item type — including ones the
		// player has never encountered, unlike UAuItemDataBaseSubsystem::ItemDataBaseArray.
		SDK::FTopLevelAssetPath MakeItemDataBaseClassPath()
		{
			SDK::FString packagePath(L"/Script/AuItems");
			SDK::FString className(L"AuItemDataBase");
			return SDK::UKismetSystemLibrary::MakeTopLevelAssetPath(packagePath, className);
		}

		// AssetRegistry_functions.cpp isn't compiled into this project, so IAssetRegistry's
		// methods (which the SDK only declares, not defines, here) have to be invoked
		// through ProcessEvent directly — same approach as CallServerDebugAddItem above.
		bool CallGetAssetsByClass(SDK::IAssetRegistry* registry, const SDK::FTopLevelAssetPath& classPath, SDK::TArray<SDK::FAssetData>& outAssets)
		{
			SDK::UObject* registryObject = registry ? registry->AsUObject() : nullptr;
			if (!registryObject)
				return false;

			static SDK::UFunction* func = nullptr;
			if (!func)
				func = registryObject->Class->GetFunction("AssetRegistry", "GetAssetsByClass");
			if (!func)
			{
				LOG_WARN("Item Spawner: could not resolve IAssetRegistry::GetAssetsByClass.");
				return false;
			}

			SDK::Params::AssetRegistry_GetAssetsByClass parms{};
			parms.ClassPathName    = classPath;
			parms.bSearchSubClasses = true;

			const auto flags = func->FunctionFlags;
			func->FunctionFlags |= 0x400;
			registryObject->ProcessEvent(func, &parms);
			func->FunctionFlags = flags;

			outAssets = std::move(parms.OutAssetData);
			return parms.ReturnValue;
		}

		SDK::UObject* CallGetAsset(const SDK::FAssetData& assetData)
		{
			SDK::UAssetRegistryHelpers* cdo = SDK::UAssetRegistryHelpers::GetDefaultObj();
			if (!cdo)
				return nullptr;

			static SDK::UFunction* func = nullptr;
			if (!func)
				func = SDK::UAssetRegistryHelpers::StaticClass()->GetFunction("AssetRegistryHelpers", "GetAsset");
			if (!func)
			{
				LOG_WARN("Item Spawner: could not resolve UAssetRegistryHelpers::GetAsset.");
				return nullptr;
			}

			SDK::Params::AssetRegistryHelpers_GetAsset parms{};
			parms.InAssetData = assetData;

			const auto flags = func->FunctionFlags;
			func->FunctionFlags |= 0x400;
			cdo->ProcessEvent(func, &parms);
			func->FunctionFlags = flags;

			return parms.ReturnValue;
		}

		// Enumerates every UAuItemDataBase (and subclass) asset known to the asset
		// registry — on disk or in memory — rather than relying on
		// UAuItemDataBaseSubsystem::ItemDataBaseArray, which only ever contains items
		// the local player has already encountered at least once. Display names are
		// resolved via Kismet (UAuItemDataBase::ItemName is an FText —
		// Conv_TextToString mirrors how the game's own UI resolves it).
		//
		// Runs on the game thread (posted via RequestRefreshItemList) and publishes
		// its result through g_pendingItems for RenderImGui() to adopt.
		void RefreshItemListOnGameThread(void* /*context*/)
		{
			std::vector<ItemEntry> items;

			try
			{
				SDK::IAssetRegistry* registry = SDK::IAssetRegistry::GetDefaultObj();
				if (!registry)
				{
					LOG_WARN("Item Spawner: could not resolve the asset registry.");
				}
				else
				{
					SDK::TArray<SDK::FAssetData> assetData{};
					if (!CallGetAssetsByClass(registry, MakeItemDataBaseClassPath(), assetData))
					{
						LOG_WARN("Item Spawner: GetAssetsByClass(AuItemDataBase) returned no results.");
					}
					else
					{
						const int rawCount = assetData.Num();
						LOG_DEBUG("Item Spawner: asset registry reports %d AuItemDataBase asset(s).", rawCount);

						int unresolvedCount = 0;
						for (int i = 0; i < rawCount; ++i)
						{
							SDK::UObject* asset = CallGetAsset(assetData[i]);
							SDK::UAuItemDataBase* item = asset ? static_cast<SDK::UAuItemDataBase*>(asset) : nullptr;
							if (!item)
							{
								++unresolvedCount;
								continue;
							}

							ItemEntry entry;
							entry.item     = item;
							entry.icon     = GetItemIconResource(item);
							entry.maxStack = item->MaxStack > 0 ? item->MaxStack : 1;
							entry.name     = SDK::UKismetTextLibrary::Conv_TextToString(item->ItemName).ToString();
							if (entry.name.empty())
								entry.name = item->UniqueItemName.ToString();

							LOG_DEBUG("Item Spawner: [%d] %s (unique=%s)", i, entry.name.c_str(), item->UniqueItemName.ToString().c_str());

							items.push_back(std::move(entry));
						}

						if (unresolvedCount > 0)
							LOG_DEBUG("Item Spawner: skipped %d unresolved asset(s).", unresolvedCount);

						std::sort(items.begin(), items.end(),
							[](const ItemEntry& a, const ItemEntry& b) { return a.name < b.name; });

						LOG_DEBUG("Item Spawner: loaded %d item(s) from the asset registry.", static_cast<int>(items.size()));
					}
				}
			}
			catch (...)
			{
				LOG_DEBUG("Item Spawner: exception while resolving items via the asset registry.");
			}

			std::lock_guard<std::mutex> lock(g_pendingMutex);
			g_pendingItems      = std::move(items);
			g_pendingItemsReady = true;
			g_refreshInFlight.store(false, std::memory_order_release);
		}

		// Triggers an asynchronous item-list reload on the game thread. Safe to call
		// repeatedly from RenderImGui() — coalesces into a single in-flight request.
		void RequestRefreshItemList()
		{
			bool expected = false;
			if (!g_refreshInFlight.compare_exchange_strong(expected, true))
				return;

			IPluginHooks* hooks = GetHooks();
			if (!hooks)
			{
				g_refreshInFlight.store(false, std::memory_order_release);
				return;
			}

			hooks->Engine->PostToGameThread(&RefreshItemListOnGameThread, nullptr);
		}

		// Adopts a freshly-loaded item list, if one is ready, into g_items — called
		// only from RenderImGui(), so g_items needs no locking once adopted.
		void AdoptPendingItemsIfReady()
		{
			std::vector<ItemEntry> incoming;
			{
				std::lock_guard<std::mutex> lock(g_pendingMutex);
				if (!g_pendingItemsReady)
					return;

				incoming = std::move(g_pendingItems);
				g_pendingItems.clear();
				g_pendingItemsReady = false;
			}

			g_items         = std::move(incoming);
			g_itemsLoaded   = true;
			g_selectedIndex = -1;
			RefreshFilteredItems();
		}

		// -------------------------------------------------------------------------
		// "Give Item" — resolving the local character/inventory and invoking the
		// inventory RPC must happen on the game thread, not from the ImGui render
		// callback that triggers it. The selected item pointer is just a value here;
		// it's only dereferenced once we're safely on the game thread.
		// -------------------------------------------------------------------------
		struct GiveItemContext
		{
			SDK::UAuItemDataBase* item;
			int32_t               amount;
			int                   stacks;
			int                   maxStack;
			std::string           name;
		};

		void GiveItemOnGameThread(void* context)
		{
			std::unique_ptr<GiveItemContext> ctx(static_cast<GiveItemContext*>(context));

			try
			{
				if (SDK::ACrCharacterPlayerBase* character = GetLocalCharacter())
				{
					if (SDK::UCrInventoryComponent* inventory = character->InventoryComponent)
					{
						CallServerDebugAddItem(inventory, ctx->item, ctx->amount);
						LOG_INFO("Item Spawner: gave %d x \"%s\" (%d stack(s) of %d).",
							ctx->amount, ctx->name.c_str(), ctx->stacks, ctx->maxStack);
					}
					else
					{
						LOG_WARN("Item Spawner: local player has no inventory component — cannot give item.");
					}
				}
				else
				{
					LOG_WARN("Item Spawner: no local player character found — cannot give item.");
				}
			}
			catch (...)
			{
				LOG_DEBUG("Item Spawner: exception while giving item.");
			}
		}

		void RequestGiveItem(SDK::UAuItemDataBase* item, int32_t amount, int stacks, int maxStack, const std::string& name)
		{
			IPluginHooks* hooks = GetHooks();
			if (!hooks)
				return;

			hooks->Engine->PostToGameThread(&GiveItemOnGameThread, new GiveItemContext{ item, amount, stacks, maxStack, name });
		}
	}

	void RenderImGui(IModLoaderImGui* imgui)
	{
		AdoptPendingItemsIfReady();

		if (!g_itemsLoaded)
			RequestRefreshItemList();

		imgui->SeparatorText("Item Spawner");
		imgui->TextWrapped("Pick an item and a number of stacks, then give it to your "
			"local player. The total amount given is stacks \xC3\x97 the item's stack size.");
		imgui->Spacing();

		if (imgui->Button("Refresh Item List"))
			RequestRefreshItemList();

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

		const int maxStack    = selected ? selected->maxStack : 1;
		const int totalAmount = (g_stacks > 0 ? g_stacks : 1) * maxStack;

		imgui->Text("Search");
		imgui->SameLine(0.0f, -1.0f);
		imgui->SetNextItemWidth(-1.0f);
		if (imgui->InputTextWithHint("##item_search", "Filter by name...", g_searchBuf, sizeof(g_searchBuf)))
			RefreshFilteredItems();

		imgui->Spacing();

		char filteredLabel[64];
		snprintf(filteredLabel, sizeof(filteredLabel), "%d match(es)", static_cast<int>(g_filteredIndices.size()));
		imgui->TextDisabled(filteredLabel);

		if (imgui->BeginChild("##item_select_list", -1.0f, 180.0f, true))
		{
			for (int filteredIndex : g_filteredIndices)
			{
				imgui->PushIDInt(filteredIndex);
				if (imgui->Selectable(g_items[filteredIndex].name.c_str(), filteredIndex == g_selectedIndex))
					g_selectedIndex = filteredIndex;
				imgui->PopID();
			}
		}
		imgui->EndChild();

		imgui->Spacing();

		if (imgui->BeginTable("##item_spawner_table", 2, kItemTableFlags))
		{
			SetupItemTableColumns(imgui);

			// Selected item (read-only info)
			imgui->TableNextRow(0, 0.0f);
			imgui->TableSetColumnIndex(0);
			imgui->Text("Item");

			imgui->TableSetColumnIndex(1);
			imgui->Text(selected ? selected->name.c_str() : "-");

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
			RequestGiveItem(selected->item, totalAmount, g_stacks, maxStack, selected->name);

		// Loot-box-at-feet spawning would land here once the relevant pickup-container
		// class (the in-world "loot box" actor) is identified in the SDK.
	}
}
