#include "player_items.h"
#include "plugin_helpers.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "aob_resolver.h"
#include "item_registry.h"
#include "Chimera_classes.hpp"
#include "AuItems_classes.hpp"
#include "Engine_classes.hpp"
#include "plugin_map.h"
#include "map_fields.h"
#include "item_icons.h"

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
			SDK::FAssetData       assetData;        // re-resolve the CDO fresh at give-time — a cached CDO can be garbage-collected
			                                         // if its Blueprint class/package gets unloaded before "Give Item" is clicked.
			std::string           name;
			std::string           uniqueName;     // item->UniqueItemName — passed to ServerDebugAddItems
			int                   maxStack        = 1;
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

		// Item icons load the first time the list draws them, a few per tick.
		ItemIcons::Loader      g_icons;

		// The rows (positions in g_filteredIndices) that were on screen last frame.
		// Icons are also asked for a few rows past them, so they are ready by the
		// time scrolling brings those rows in.
		constexpr int          kIconMarginRows = 3;
		int                    g_visibleFirst  = -1;
		int                    g_visibleLast   = -1;

		// A row asks for its icon only after staying in that range for kIconDwell,
		// so a fast scrollbar drag queues nothing it sweeps past. g_iconSince: rows
		// in range still waiting (pruned each frame); g_iconAsked: rows that asked.
		using IconClock = std::chrono::steady_clock;
		constexpr std::chrono::milliseconds                    kIconDwell{ 200 };
		std::unordered_map<std::string, IconClock::time_point> g_iconSince;
		std::unordered_set<std::string>                        g_iconAsked;
		IconClock::time_point                                  g_iconFrameAt;

		std::string IconUniqueNameOf(SDK::UAuItemDataBase* item)
		{
			return BetterCheats::Fields::UniqueNameOf(item);
		}

		const SDK::FSlateBrush* IconBrushOf(SDK::UAuItemDataBase* item)
		{
			return BetterCheats::Fields::AuItemDataBase_ItemIcon.In(static_cast<const void*>(item));
		}

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
			g_visibleFirst = -1;
			g_visibleLast  = -1;

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
			try { world = PluginSdkRuntime::GetWorld(); }
			catch (...) { return nullptr; }
			if (!world) return nullptr;

			SDK::APlayerController* pc = SDK::UGameplayStatics::GetPlayerController(world, 0);
			if (!pc) return nullptr;

			// See player_skills.cpp's GetLocalController — the controller isn't a
			// Chimera one for the whole life of the world.
			SDK::UClass* controllerClass = SDK::ACrPlayerControllerBase::StaticClass();
			if (!controllerClass || !pc->IsA(controllerClass)) return nullptr;

			return static_cast<SDK::ACrPlayerControllerBase*>(pc);
		}

		// UAuItemsComponent::AddNewItem is the real server-authoritative "give item" path
		// (HaveSpace check, OwnedItems insertion, replication dirty-marking, OnItemAdded
		// broadcasts) — found by tracing ServerDebugAddItems in IDA. Calling it directly
		// avoids the RPC entirely and gives us the actual result: an empty array means
		// the add was rejected (no space / not authority), a populated one lists what
		// was actually added.
		// `result` is a TArray<FAuAddedItem>, stepped by the live size (PluginMap::ElementAt).
		using AddNewItemFn = void*(__fastcall*)(
			SDK::UAuItemsComponent* self, void* result, const SDK::UAuItemDataBase* newItem, uint32_t amount);

		AddNewItemFn g_addNewItem            = nullptr;
		bool         g_addNewItemResolveTried = false;

		AddNewItemFn ResolveAddNewItem()
		{
			if (g_addNewItemResolveTried)
				return g_addNewItem;

			g_addNewItemResolveTried = true;

			uintptr_t address = AOB::Resolved().AddNewItem;
			if (!address)
			{
				LOG_WARN("Item Spawner: UAuItemsComponent::AddNewItem unresolved.");
				return nullptr;
			}

			g_addNewItem = reinterpret_cast<AddNewItemFn>(address);
			return g_addNewItem;
		}

		// Returns the resource backing an item's icon brush — typically a UTexture2D —
		// or nullptr if the item has none configured. Only the scan's filter reads it;
		// the pointer is not kept, and ItemIcons::Loader loads the picture itself.
		SDK::UTexture2D* GetItemIconResource(SDK::UAuItemDataBase* item)
		{
			// ResourceObject might be UTexture, UMaterialInterface, etc. — the IsA check
			// at render time is the safe gate; just return it as UTexture2D* for storage.
			const SDK::FSlateBrush* brush = BetterCheats::Fields::AuItemDataBase_ItemIcon.In(static_cast<const void*>(item));
			return brush ? reinterpret_cast<SDK::UTexture2D*>(brush->ResourceObject) : nullptr;
		}

		// Enumerates every item type by querying the asset registry for
		// UAuItemBlueprint assets (mirrors UCrUW_CheatItemsTab::DebugGatherAllItems)
		// and force-loading each one down to its UAuItemDataBase CDO. Display names
		// are resolved via Kismet (UAuItemDataBase::ItemName is an FText —
		// Conv_TextToString mirrors how the game's own UI resolves it).
		//
		// Runs on the game thread (posted via RequestRefreshItemList) and publishes
		// its result through g_pendingItems for RenderImGui() to adopt.
		void RefreshItemListOnGameThread(void* /*context*/)
		{
			std::vector<ItemEntry> items;

			IPluginSelf* self = GetSelf();
			IPluginSplash* splash = (self && self->hooks) ? self->hooks->Splash : nullptr;
			if (splash && splash->IsVisible())
			{
				splash->SetSubStatus("Building item list...");
				splash->SetSubProgress(0.0f);
			}

			LOG_INFO("Item Spawner: refreshing item list from the asset registry...");

			try
			{
				SDK::IAssetRegistry* registry = BetterCheats::ItemRegistry::GetAssetRegistry();
				if (!registry)
				{
					LOG_WARN("Item Spawner: could not resolve the asset registry.");
				}
				else
				{
					SDK::TArray<SDK::FAssetData> assetData;
					if (!BetterCheats::ItemRegistry::GetAssetsByClass(registry,
						BetterCheats::ItemRegistry::ItemBlueprintClassPath(), assetData))
					{
						LOG_WARN("Item Spawner: IAssetRegistry::GetAssetsByClass failed.");
					}

					const int rawCount = assetData.Num();
					LOG_INFO("Item Spawner: asset registry reports %d AuItemBlueprint asset(s).", rawCount);

					int unresolvedCount = 0;
					int noIconCount     = 0;
					constexpr int kMaxVerboseLogs = 25;
					for (int i = 0; i < rawCount; ++i)
					{
#ifdef _DEBUG
						const bool verbose = (i < kMaxVerboseLogs) || (unresolvedCount < kMaxVerboseLogs);
#else
						const bool verbose = false;
#endif
						if (verbose)
							LOG_DEBUG("Item Spawner: [%d/%d] resolving '%s' (PackageName '%s', PackagePath '%s', AssetClass '%s')", i + 1, rawCount,
								assetData[i].AssetName.ToString().c_str(), assetData[i].PackageName.GetRawString().c_str(),
								assetData[i].PackagePath.GetRawString().c_str(), assetData[i].AssetClass.GetRawString().c_str());

						SDK::UAuItemDataBase* item = BetterCheats::ItemRegistry::ResolveItemFromBlueprintAsset(assetData[i], verbose);
						if (!item)
						{
							++unresolvedCount;
							continue;
						}

						ItemEntry entry;
						entry.assetData  = assetData[i];
						SDK::UTexture2D* icon = GetItemIconResource(item);
						bool iconIsTexture2D = false;
						if (icon) { try { iconIsTexture2D = icon->IsA(SDK::UTexture2D::StaticClass()); } catch (...) {} }
						const int stackSize = BetterCheats::Fields::ValueOr(BetterCheats::Fields::AuItemDataBase_MaxStack, item);
						entry.maxStack   = stackSize > 0 ? stackSize : 1;
						entry.uniqueName = BetterCheats::Fields::UniqueNameOf(item);
						entry.name       = BetterCheats::Fields::DisplayNameOf(item);
						if (entry.name.empty() || entry.name == "<MISSING STRING TABLE ENTRY>")
							entry.name = entry.uniqueName;

						// Skip placeholder/stub items that have no real presence in the game.
						if (entry.name == "None")
							continue;
						// Skip raw "...Blueprint" entries — these are the Blueprint assets
						// themselves, not usable items.
						if (entry.name.size() >= 9 && entry.name.compare(entry.name.size() - 9, 9, "Blueprint") == 0)
							continue;
						if (icon)
						{
							const std::string iconName = icon->GetName();
							if (iconName.find("WhiteSquareTexture") != std::string::npos)
								continue;
						}

						if (!icon || !iconIsTexture2D)
						{
							++noIconCount;
							continue;
						}

						items.push_back(std::move(entry));
					}

					if (unresolvedCount > 0)
						LOG_INFO("Item Spawner: %d Blueprint asset(s) could not be resolved to an item.", unresolvedCount);
					if (noIconCount > 0)
						LOG_INFO("Item Spawner: %d item(s) have no icon resource.", noIconCount);

					std::sort(items.begin(), items.end(),
						[](const ItemEntry& a, const ItemEntry& b) { return a.name < b.name; });

					LOG_INFO("Item Spawner: loaded %d item(s) from the asset registry.", static_cast<int>(items.size()));
				}
			}
			catch (...)
			{
				LOG_DEBUG("Item Spawner: exception while resolving items via the asset registry.");
			}

			{
				std::lock_guard<std::mutex> lock(g_pendingMutex);
				g_pendingItems      = std::move(items);
				g_pendingItemsReady = true;
				g_refreshInFlight.store(false, std::memory_order_release);
				LOG_DEBUG("Item Spawner: published %d pending item(s) for adoption.", static_cast<int>(g_pendingItems.size()));
			}

			if (splash)
			{
				splash->ClearSubBar();
				splash->ReleaseSplashHold();
			}
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

			LOG_INFO("{POSTING_TO_GAME_THREAD} Item Spawner: posting item list refresh to game thread.");
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
		// callback that triggers it. The context carries only the unique name;
		// the item is re-resolved once we're safely on the game thread.
		// -------------------------------------------------------------------------
		struct GiveItemContext
		{
			std::string     uniqueName;
			int32_t         amount;
			int             stacks;
			int             maxStack;
			std::string     displayName;
			SDK::FAssetData assetData;
		};

		void GiveItemOnGameThread(void* context)
		{
			std::unique_ptr<GiveItemContext> ctx(static_cast<GiveItemContext*>(context));

			try
			{
				SDK::ACrPlayerControllerBase* controller = GetLocalController();
				if (!controller)
				{
					LOG_WARN("Item Spawner: no local player controller found — cannot give item.");
					return;
				}
				LOG_DEBUG("Item Spawner: controller=%p Pawn=%p", controller, controller->Pawn);

				auto* character = static_cast<SDK::ACrCharacterPlayerBase*>(controller->Pawn);
				SDK::UCrInventoryComponent* inventory = character ? BetterCheats::Fields::ValueOr(BetterCheats::Fields::CrCharacterPlayerBase_InventoryComponent, character) : nullptr;
				if (!inventory)
				{
					LOG_WARN("Item Spawner: no local inventory component found — cannot give item.");
					return;
				}
				LOG_DEBUG("Item Spawner: character=%p InventoryComponent=%p", character, inventory);

				// Re-resolve the item CDO fresh, on the game thread, right before use — the CDO cached
				// at scan-time can be garbage-collected if its Blueprint class/package gets unloaded
				// in the meantime, leaving a dangling pointer.
				SDK::UAuItemDataBase* itemData = BetterCheats::ItemRegistry::ResolveItemFromBlueprintAsset(ctx->assetData, false);
				if (!itemData)
				{
					LOG_WARN("Item Spawner: could not re-resolve item data for \"%s\" — cannot give item.", ctx->uniqueName.c_str());
					return;
				}
				LOG_DEBUG("Item Spawner: itemData=%p uniqueName=\"%s\" amount=%d", itemData, ctx->uniqueName.c_str(), ctx->amount);

				AddNewItemFn addNewItem = ResolveAddNewItem();
				if (!addNewItem)
				{
					LOG_WARN("Item Spawner: AddNewItem unavailable — cannot give item.");
					return;
				}
				LOG_DEBUG("Item Spawner: addNewItem=%p", reinterpret_cast<void*>(addNewItem));

				SDK::TArray<SDK::FAuAddedItem> added{};
				LOG_DEBUG("Item Spawner: calling AddNewItem(self=%p, result=%p, item=%p, amount=%u)...",
					inventory, &added, itemData, static_cast<uint32_t>(ctx->amount));
				addNewItem(inventory, &added, itemData, static_cast<uint32_t>(ctx->amount));
				LOG_DEBUG("Item Spawner: AddNewItem returned, added.Num()=%d", added.Num());

				if (added.Num() == 0)
				{
					LOG_INFO("Item Spawner: failed to give %d x \"%s\" [%s] — AddNewItem returned no items "
						"(not enough inventory space, or not authoritative).",
						ctx->amount, ctx->displayName.c_str(), ctx->uniqueName.c_str());
					return;
				}

				int32_t totalAdded = 0;
				for (int32_t i = 0; i < added.Num(); ++i)
					totalAdded += BetterCheats::Fields::ValueOr(BetterCheats::Fields::AuAddedItem_Amount,
						PluginMap::ElementAt(added, i, BetterCheats::Fields::AuAddedItem_Size));

				LOG_INFO("Item Spawner: gave %d x \"%s\" [%s] (%d stack(s) of %d, %d entr%s added).",
					totalAdded, ctx->displayName.c_str(), ctx->uniqueName.c_str(), ctx->stacks, ctx->maxStack,
					added.Num(), added.Num() == 1 ? "y" : "ies");
			}
			catch (...)
			{
				LOG_DEBUG("Item Spawner: exception while giving item.");
			}
		}

		void RequestGiveItem(const std::string& uniqueName, int32_t amount, int stacks, int maxStack, const std::string& displayName, const SDK::FAssetData& assetData)
		{
			IPluginHooks* hooks = GetHooks();
			if (!hooks)
				return;

			hooks->Engine->PostToGameThread(&GiveItemOnGameThread, new GiveItemContext{ uniqueName, amount, stacks, maxStack, displayName, assetData });
		}
	}

	void Initialize()
	{
		ResolveAddNewItem();

		IPluginSelf* self = GetSelf();
		g_icons.Init(self, "BetterCheats", { &IconUniqueNameOf, &IconBrushOf });
		IPluginSplash* splash = (self && self->hooks) ? self->hooks->Splash : nullptr;
		if (splash && splash->IsVisible())
			splash->AcquireSplashHold();

#ifndef _DEBUG
		RequestRefreshItemList();
#else
		self->hooks->Splash->ReleaseSplashHold();
#endif
	}

	void RenderImGui(IModLoaderImGui* imgui)
	{
		imgui->SeparatorText("Item Spawner");

		if (!ResolveAddNewItem())
		{
			imgui->TextWrapped("Item Spawner is unavailable — UAuItemsComponent::AddNewItem could not be "
				"located in this game build.");
			return;
		}

		AdoptPendingItemsIfReady();

		if (!g_itemsLoaded)
			RequestRefreshItemList();

		imgui->TextWrapped("Pick an item and a number of stacks, then give it to your "
			"local player. The total amount given is stacks \xC3\x97 the item's stack size.");
		imgui->Spacing();

		if (g_items.empty())
		{
			imgui->TextDisabled("No items found — open a world and wait for items to load.");
			return;
		}

		const bool hasSelection = g_selectedIndex >= 0 && g_selectedIndex < static_cast<int>(g_items.size());
		const ItemEntry* selected = hasSelection ? &g_items[g_selectedIndex] : nullptr;

		const int maxStack    = selected ? selected->maxStack : 1;
		const int totalAmount = (g_stacks > 0 ? g_stacks : 1) * maxStack;

		// Search bar
		char searchHint[64];
		snprintf(searchHint, sizeof(searchHint), "Search %d items...", static_cast<int>(g_items.size()));
		imgui->SetNextItemWidth(-1.0f);
		if (imgui->InputTextWithHint("##item_search", searchHint, g_searchBuf, sizeof(g_searchBuf)))
			RefreshFilteredItems();

		imgui->Spacing();

		// --- Bottom controls height estimate (stacks row + give button + spacing) ---
		// Reserve enough room so the list fills everything above the controls.
		const float lineH    = imgui->GetFrameHeightWithSpacing();
		const float bottomH  = lineH * 3.0f + imgui->GetTextLineHeight() + 8.0f;

		float availX, availY;
		imgui->GetContentRegionAvail(&availX, &availY);
		const float listH = availY - bottomH;

		// --- Scrollable item list ---
		// ImGuiWindowFlags_NoScrollbar = 0x800 (we manage our own scroll via BeginChild default)
		if (imgui->BeginChild("##item_list", -1.0f, listH > 40.0f ? listH : 40.0f, false))
		{
			const float rowH   = 48.0f;
			const float iconSz = 36.0f;
			const float padX   = 6.0f;

			int visibleFirst = -1;
			int visibleLast  = -1;

			// A frame gap over the dwell means the list was not drawn: nothing waits.
			const IconClock::time_point now = IconClock::now();
			if (now - g_iconFrameAt > kIconDwell)
				g_iconSince.clear();
			g_iconFrameAt = now;
			std::unordered_map<std::string, IconClock::time_point> iconSince;

			for (int pos = 0; pos < static_cast<int>(g_filteredIndices.size()); ++pos)
			{
				const int filteredIndex = g_filteredIndices[pos];
				const ItemEntry& entry = g_items[filteredIndex];

				// The cursor is at the row's top here. IsRectVisible says whether the
				// list's clip rect shows any of the row; scrolled-off rows ask for no icon.
				const bool onScreen = imgui->IsRectVisible(1.0f, rowH);
				if (onScreen)
				{
					if (visibleFirst < 0)
						visibleFirst = pos;
					visibleLast = pos;
				}
				const bool wantIcon = onScreen
					|| (g_visibleFirst <= g_visibleLast
						&& pos >= g_visibleFirst - kIconMarginRows
						&& pos <= g_visibleLast + kIconMarginRows);
				const bool isSelected  = filteredIndex == g_selectedIndex;

				imgui->PushIDInt(filteredIndex);

				// Highlight selected row — ImGuiCol_Header (21) is the correct target for
				// selectables; ImGuiCol_ChildBg only tints child windows, not their contents.
				if (isSelected)
					imgui->PushStyleColor(21 /*ImGuiCol_Header*/, 0.26f, 0.59f, 0.98f, 0.35f);

				// Record Y before the selectable so we can restore it for custom content.
				const float rowTopY = imgui->GetCursorPosY();

				// ImGuiSelectableFlags_AllowOverlap (1<<4 in this ImGui build) lets the selectable
				// register clicks even though the icon, text, and button widgets are drawn on top of it.
				// Width must be 0.0f (not -1) so ImGui expands it to full available width.
				if (imgui->SelectableFull("##row", isSelected, 1 << 4, 0.0f, rowH))
					g_selectedIndex = filteredIndex;

				// Restore cursor to top of row + small top padding so text sits inside the row.
				const float textLineH = imgui->GetTextLineHeight();
				imgui->SetCursorPosY(rowTopY + (rowH - textLineH * 2.0f) * 0.5f);

				// Icon — loaded on first draw by the shared helper, only for rows on
				// screen or just past it that have stayed there for kIconDwell.
				imgui->SetCursorPosX(imgui->GetCursorPosX() + padX);
				IPluginHooks* texHooks = GetHooks();
				bool drawIcon = wantIcon && g_icons.Has(entry.uniqueName);
				if (drawIcon && !g_iconAsked.count(entry.uniqueName))
				{
					const auto seen  = g_iconSince.find(entry.uniqueName);
					const auto since = seen != g_iconSince.end() ? seen->second : now;
					drawIcon = now - since >= kIconDwell;
					if (drawIcon)
						g_iconAsked.insert(entry.uniqueName);
					else
						iconSince.emplace(entry.uniqueName, since);
				}
				if (drawIcon)
					g_icons.Draw(imgui, texHooks ? texHooks->ImGuiTextures : nullptr, entry.uniqueName, iconSz, padX);
				else
				{
					imgui->Dummy(iconSz, iconSz);
					imgui->SameLine(0.0f, padX);
				}

				// Name + stack size info, vertically centred beside the icon
				imgui->BeginGroup();
				{
					// Bold via slight colour pop — real bold requires a second font push
					imgui->TextColored(1.0f, 1.0f, 1.0f, 1.0f, entry.name.c_str());

					char sub[64];
					snprintf(sub, sizeof(sub), "Stack size: %d", entry.maxStack);
					imgui->TextColored(0.6f, 0.6f, 0.6f, 1.0f, sub);
				}
				imgui->EndGroup();

				if (isSelected)
					imgui->PopStyleColor(1);

				// Restore cursor to the row's true bottom so layout isn't corrupted by the
				// manually-positioned overlapping widgets (which leave the cursor above rowTopY+rowH).
				imgui->SetCursorPosY(rowTopY + rowH);

				// Faded separator between rows
				imgui->PushStyleColor(27 /*ImGuiCol_Separator*/, 1.0f, 1.0f, 1.0f, 0.08f);
				imgui->Separator();
				imgui->PopStyleColor(1);

				imgui->PopID();
			}

			g_visibleFirst = visibleFirst;
			g_visibleLast  = visibleLast;
			g_iconSince.swap(iconSince);
		}
		imgui->EndChild();

		imgui->Spacing();

		// Stacks input + give button on same line
		imgui->SetNextItemWidth(80.0f);
		imgui->InputInt("Stacks##stacks_input", &g_stacks, 1, 10);
		if (g_stacks < 1)
			g_stacks = 1;

		imgui->SameLine(0.0f, 8.0f);

		if (!selected)
		{
			imgui->BeginDisabled(true);
			imgui->Button("Give Item");
			imgui->EndDisabled();
			imgui->SameLine(0.0f, 6.0f);
			imgui->TextDisabled("(select an item)");
		}
		else
		{
			if (imgui->Button("Give Item"))
				RequestGiveItem(selected->uniqueName, totalAmount, g_stacks, maxStack, selected->name, selected->assetData);
			imgui->SameLine(0.0f, 6.0f);
			char totalLabel[64];
			snprintf(totalLabel, sizeof(totalLabel), "x%d  (stack %d)", totalAmount, maxStack);
			imgui->TextDisabled(totalLabel);
		}

		// Loot-box-at-feet spawning would land here once the relevant pickup-container
		// class (the in-world "loot box" actor) is identified in the SDK.
	}

	void Tick()
	{
		g_icons.Tick();
	}

	void Shutdown()
	{
		g_icons.Shutdown();
		g_items.clear();
		g_itemsLoaded = false;
	}
}
