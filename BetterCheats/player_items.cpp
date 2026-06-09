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
#include "Engine_classes.hpp"
#include "AssetRegistry_classes.hpp"
#include "AssetRegistry_parameters.hpp"

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
			SDK::UAuItemDataBase* item              = nullptr;
			SDK::UTexture2D*      icon              = nullptr;  // UTexture2D from ItemIcon.ResourceObject
			bool                  iconIsTexture2D   = false;    // cached IsA(UTexture2D) result
			PluginTextureHandle   cachedTexHandle   = nullptr;  // persistent SRV wrapping engine GPU resource
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

		void FreeItemTexHandles(std::vector<ItemEntry>& items)
		{
			IPluginHooks* hooks = GetHooks();
			if (!hooks || !hooks->ImGuiTextures) return;
			for (ItemEntry& e : items)
			{
				if (e.cachedTexHandle)
				{
					hooks->ImGuiTextures->FreeTexture(e.cachedTexHandle);
					e.cachedTexHandle = nullptr;
				}
			}
		}

		// Attempts to register UTexture2D icons with ImGui for every entry that has
		// no handle yet.  Safe to call from any thread — LoadFromUTexture2D returns
		// NULL (without throwing) when D3D12 is not yet ready, so entries that miss
		// here are retried the next time this function is called.
		void LoadItemTexHandles(std::vector<ItemEntry>& items, IPluginSplash* splash = nullptr)
		{
			IPluginHooks* hooks = GetHooks();
			if (!hooks || !hooks->ImGuiTextures) return;

			const int total = static_cast<int>(items.size());
			int loaded = 0;

			if (splash && splash->IsVisible())
				splash->SetSubStatus("(BetterCheats) Copying Item Textures... Wait!");

			for (ItemEntry& e : items)
			{
				if (e.iconIsTexture2D && e.icon && !e.cachedTexHandle)
				{
					const std::string texName = e.icon->GetName();
					e.cachedTexHandle = hooks->ImGuiTextures->LoadFromUTexture2D(e.icon, texName.c_str());
				}

				if (splash && splash->IsVisible() && total > 0)
				{
					++loaded;
					splash->SetSubProgress(static_cast<float>(loaded) / static_cast<float>(total));
				}
			}

			if (splash && splash->IsVisible())
				splash->ClearSubBar();
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

		// ACrPlayerControllerBase::ServerDebugAddItems is the RPC that actually works in
		// single-player: it takes a plain name string (the item's UniqueItemName) and an
		// amount, and the server-side handler inserts the item directly. This mirrors what
		// other working cheat mods do — the inventory-component path (ServerDebugAddItem)
		// does nothing observable in this build.
		void CallServerDebugAddItems(SDK::ACrPlayerControllerBase* controller, const std::string& uniqueName, int32_t amount)
		{
			static SDK::UFunction* func = nullptr;
			if (!func)
				func = controller->Class->GetFunction("CrPlayerControllerBase", "ServerDebugAddItems");
			if (!func)
			{
				LOG_WARN("Item Spawner: could not resolve ACrPlayerControllerBase::ServerDebugAddItems.");
				return;
			}

			const std::wstring wname(uniqueName.begin(), uniqueName.end());
			SDK::Params::CrPlayerControllerBase_ServerDebugAddItems parms{};
			parms.Name_0 = SDK::FString(wname.c_str());
			parms.Amount = amount;

			const auto flags = func->FunctionFlags;
			func->FunctionFlags |= 0x400; // FUNC_NetServer — force the call through on a listen-server / SP host
			controller->ProcessEvent(func, &parms);
			func->FunctionFlags = flags;
		}

		// Returns the resource backing an item's icon brush — typically a UTexture2D —
		// or nullptr if the item has none configured. Not rendered anywhere yet; this
		// just resolves and stores it so an icon column can be wired into ImGui later
		// once texture binding is figured out.
		SDK::UTexture2D* GetItemIconResource(SDK::UAuItemDataBase* item)
		{
			// ResourceObject might be UTexture, UMaterialInterface, etc. — the IsA check
			// at render time is the safe gate; just return it as UTexture2D* for storage.
			return item ? reinterpret_cast<SDK::UTexture2D*>(item->ItemIcon.ResourceObject) : nullptr;
		}

		// Every item type is defined as a Blueprint (UAuItemBlueprint asset whose
		// GeneratedClass's CDO is the actual UAuItemDataBase) — mirroring how the
		// game's own UCrUW_CheatItemsTab::DebugGatherAllItems enumerates the full
		// item list via the asset registry rather than any runtime "seen so far"
		// cache. /Script/AuItems.AuItemBlueprint is the class path to query.
		SDK::FTopLevelAssetPath MakeItemBlueprintClassPath()
		{
			SDK::FString packagePath(L"/Script/AuItems");
			SDK::FString className(L"AuItemBlueprint");
			return SDK::UKismetSystemLibrary::MakeTopLevelAssetPath(packagePath, className);
		}

		// UAssetRegistryHelpers::GetAssetRegistry — AssetRegistry_functions.cpp isn't
		// compiled into the project, so this (and the helpers below) must be invoked
		// via ProcessEvent against the resolved UFunction.
		SDK::UObject* CallGetAssetRegistry()
		{
			SDK::UAssetRegistryHelpers* cdo = SDK::UAssetRegistryHelpers::GetDefaultObj();
			if (!cdo) return nullptr;

			static SDK::UFunction* func = nullptr;
			if (!func)
				func = SDK::UAssetRegistryHelpers::StaticClass()->GetFunction("AssetRegistryHelpers", "GetAssetRegistry");
			if (!func)
			{
				LOG_WARN("Item Spawner: could not resolve UAssetRegistryHelpers::GetAssetRegistry.");
				return nullptr;
			}

			SDK::Params::AssetRegistryHelpers_GetAssetRegistry parms{};
			const auto flags = func->FunctionFlags;
			func->FunctionFlags |= 0x400;
			cdo->ProcessEvent(func, &parms);
			func->FunctionFlags = flags;

			return parms.ReturnValue.GetObjectRef();
		}

		// IAssetRegistry::GetAssetsByClass — resolved from the interface's own UClass
		// (IAssetRegistry::StaticClass), since the runtime UAssetRegistryImpl class
		// chain doesn't expose the interface UFunction by that name.
		bool CallGetAssetsByClass(SDK::IAssetRegistry* registry, const SDK::FTopLevelAssetPath& classPath, SDK::TArray<SDK::FAssetData>& outAssets)
		{
			SDK::UObject* registryObject = registry ? registry->AsUObject() : nullptr;
			if (!registryObject) return false;

			static SDK::UFunction* func = nullptr;
			if (!func)
				func = SDK::IAssetRegistry::StaticClass()->GetFunction("AssetRegistry", "GetAssetsByClass");
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

		// Raw CoreUObject object/package lookup and loading functions — these are
		// not UFunctions, so ProcessEvent can't reach them. The Kismet soft-object-
		// path / LoadAsset_Blocking route (FStreamableManager::LoadSynchronous)
		// reliably returns null for these off-disk Blueprint packages in this
		// build (verified: a well-formed soft path like
		// "/Game/Chimera/Items/I_AquaBar.I_AquaBar" still resolves to nullptr), so
		// resolution instead mirrors UCrUW_CheatItemsTab::DebugGatherAllItems's own
		// force-load sequence: FindPackage/LoadPackage + StaticFindObject by the
		// generated "<AssetName>_C" class name. The modloader AOB-resolves these
		// addresses at startup and exposes them via IPluginEngineEvents.
		using StaticFindObjectByNameFn = SDK::UObject*  (__fastcall*)(SDK::UClass*, SDK::UObject*, const wchar_t*, bool);
		using FindPackageFn            = SDK::UPackage* (__fastcall*)(SDK::UObject*, const wchar_t*);
		using PackageFullyLoadFn       = void           (__fastcall*)(SDK::UPackage*);
		using LoadPackageFn            = SDK::UPackage* (__fastcall*)(SDK::UPackage*, const wchar_t*, uint32_t, void*, const void*);

		StaticFindObjectByNameFn g_staticFindObjectByName = nullptr;
		FindPackageFn            g_findPackage             = nullptr;
		PackageFullyLoadFn       g_packageFullyLoad        = nullptr;
		LoadPackageFn            g_loadPackage             = nullptr;
		bool                     g_engineFnsResolveTried   = false;

		bool ResolveEngineLookupFunctions()
		{
			if (g_engineFnsResolveTried)
				return g_staticFindObjectByName && g_findPackage && g_packageFullyLoad && g_loadPackage;

			g_engineFnsResolveTried = true;

			IPluginHooks* hooks = GetHooks();
			IPluginEngineEvents* engine = hooks ? hooks->Engine : nullptr;
			if (!engine)
			{
				LOG_WARN("Item Spawner: engine events interface unavailable — cannot resolve object/package lookup functions.");
				return false;
			}

			if (uintptr_t address = engine->GetStaticFindObjectByNameAddress())
				g_staticFindObjectByName = reinterpret_cast<StaticFindObjectByNameFn>(address);
			if (uintptr_t address = engine->GetFindPackageAddress())
				g_findPackage = reinterpret_cast<FindPackageFn>(address);
			if (uintptr_t address = engine->GetPackageFullyLoadAddress())
				g_packageFullyLoad = reinterpret_cast<PackageFullyLoadFn>(address);
			if (uintptr_t address = engine->GetLoadPackageAddress())
				g_loadPackage = reinterpret_cast<LoadPackageFn>(address);

			if (!g_staticFindObjectByName || !g_findPackage || !g_packageFullyLoad || !g_loadPackage)
			{
				LOG_WARN("Item Spawner: failed to resolve object/package lookup functions "
					"(StaticFindObject=%p FindPackage=%p FullyLoad=%p LoadPackage=%p) — item list will be incomplete.",
					reinterpret_cast<void*>(g_staticFindObjectByName), reinterpret_cast<void*>(g_findPackage),
					reinterpret_cast<void*>(g_packageFullyLoad), reinterpret_cast<void*>(g_loadPackage));
				return false;
			}

			return true;
		}

		// Forces the Blueprint's package to load and resolves it down to the
		// UAuItemDataBase CDO that actually holds the item's data:
		// package -> generated "<AssetName>_C" class -> ClassDefaultObject.
		// Mirrors UCrUW_CheatItemsTab::DebugGatherAllItems exactly (minus the
		// FAssetData::FastGetAsset already-loaded fast path, which is just an
		// optimization — force-loading covers both cases).
		SDK::UAuItemDataBase* ResolveItemFromBlueprintAsset(const SDK::FAssetData& assetData, bool verbose)
		{
			if (!ResolveEngineLookupFunctions())
				return nullptr;

			const std::string assetName   = assetData.AssetName.ToString();
			const std::string packageName = assetData.PackageName.GetRawString();
			if (packageName.empty())
				return nullptr;

			const std::wstring packageNameW(packageName.begin(), packageName.end());
			const std::wstring generatedClassNameW = std::wstring(assetName.begin(), assetName.end()) + L"_C";

			SDK::UPackage* package = g_findPackage(nullptr, packageNameW.c_str());
			if (package)
				g_packageFullyLoad(package);
			else
				package = g_loadPackage(nullptr, packageNameW.c_str(), 0, nullptr, nullptr);

			if (!package)
			{
				if (verbose)
					LOG_DEBUG("Item Spawner:   [%s] could not find or load package '%s'.", assetName.c_str(), packageName.c_str());
				return nullptr;
			}
			if (verbose)
				LOG_DEBUG("Item Spawner:   [%s] package '%s' loaded ('%s')", assetName.c_str(), packageName.c_str(),
					package->GetName().c_str());

			SDK::UObject* generatedObj = g_staticFindObjectByName(SDK::UClass::StaticClass(), package, generatedClassNameW.c_str(), false);
			if (!generatedObj)
			{
				if (verbose)
					LOG_DEBUG("Item Spawner:   [%s] StaticFindObject could not find generated class '%s_C' in package '%s'.",
						assetName.c_str(), assetName.c_str(), packageName.c_str());
				return nullptr;
			}

			SDK::UClass* generatedClass = static_cast<SDK::UClass*>(generatedObj);
			if (verbose)
				LOG_DEBUG("Item Spawner:   [%s] generated class '%s'", assetName.c_str(), generatedClass->GetName().c_str());

			SDK::UObject* cdo = generatedClass->ClassDefaultObject;
			if (!cdo)
			{
				if (verbose)
					LOG_DEBUG("Item Spawner:   [%s] ClassDefaultObject is null.", assetName.c_str());
				return nullptr;
			}
			if (!cdo->IsA(SDK::UAuItemDataBase::StaticClass()))
			{
				if (verbose)
					LOG_DEBUG("Item Spawner:   [%s] CDO '%s' is not a UAuItemDataBase (class '%s').", assetName.c_str(),
						cdo->GetName().c_str(), cdo->Class ? cdo->Class->GetName().c_str() : "<none>");
				return nullptr;
			}

			return static_cast<SDK::UAuItemDataBase*>(cdo);
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
				SDK::UObject* registryObject = CallGetAssetRegistry();
				SDK::IAssetRegistry* registry = registryObject ? reinterpret_cast<SDK::IAssetRegistry*>(registryObject) : nullptr;
				if (!registry)
				{
					LOG_WARN("Item Spawner: could not resolve the asset registry.");
				}
				else
				{
					SDK::TArray<SDK::FAssetData> assetData;
					if (!CallGetAssetsByClass(registry, MakeItemBlueprintClassPath(), assetData))
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

						SDK::UAuItemDataBase* item = ResolveItemFromBlueprintAsset(assetData[i], verbose);
						if (!item)
						{
							++unresolvedCount;
							continue;
						}

						ItemEntry entry;
						entry.item       = item;
						entry.icon       = GetItemIconResource(item);
						if (entry.icon) { try { entry.iconIsTexture2D = entry.icon->IsA(SDK::UTexture2D::StaticClass()); } catch (...) {} }
						entry.maxStack   = item->MaxStack > 0 ? item->MaxStack : 1;
						entry.uniqueName = item->UniqueItemName.ToString();
						entry.name       = SDK::UKismetTextLibrary::Conv_TextToString(item->ItemName).ToString();
						if (entry.name.empty() || entry.name == "<MISSING STRING TABLE ENTRY>")
							entry.name = entry.uniqueName;

						// Skip placeholder/stub items that have no real presence in the game.
						if (entry.name == "None")
							continue;
						if (entry.icon)
						{
							const std::string iconName = entry.icon->GetName();
							if (iconName.find("WhiteSquareTexture") != std::string::npos)
								continue;
						}

						if (!entry.icon || !entry.iconIsTexture2D)
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

			// Pre-load texture handles while still on the game thread.  D3D12 may
			// not be ready yet (returns NULL), in which case the fallback in
			// AdoptPendingItemsIfReady will pick up the remainder on first render.
			LoadItemTexHandles(items, splash);

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

			FreeItemTexHandles(g_items);
			g_items         = std::move(incoming);
			g_itemsLoaded   = true;
			g_selectedIndex = -1;
			LoadItemTexHandles(g_items);  // retry any entries that returned NULL on the game thread
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
			std::string uniqueName;
			int32_t     amount;
			int         stacks;
			int         maxStack;
			std::string displayName;
		};

		void GiveItemOnGameThread(void* context)
		{
			std::unique_ptr<GiveItemContext> ctx(static_cast<GiveItemContext*>(context));

			try
			{
				if (SDK::ACrPlayerControllerBase* controller = GetLocalController())
				{
					CallServerDebugAddItems(controller, ctx->uniqueName, ctx->amount);
					LOG_INFO("Item Spawner: gave %d x \"%s\" [%s] (%d stack(s) of %d).",
						ctx->amount, ctx->displayName.c_str(), ctx->uniqueName.c_str(), ctx->stacks, ctx->maxStack);
				}
				else
				{
					LOG_WARN("Item Spawner: no local player controller found — cannot give item.");
				}
			}
			catch (...)
			{
				LOG_DEBUG("Item Spawner: exception while giving item.");
			}
		}

		void RequestGiveItem(const std::string& uniqueName, int32_t amount, int stacks, int maxStack, const std::string& displayName)
		{
			IPluginHooks* hooks = GetHooks();
			if (!hooks)
				return;

			hooks->Engine->PostToGameThread(&GiveItemOnGameThread, new GiveItemContext{ uniqueName, amount, stacks, maxStack, displayName });
		}
	}

	void Initialize()
	{
		IPluginSelf* self = GetSelf();
		IPluginSplash* splash = (self && self->hooks) ? self->hooks->Splash : nullptr;
		if (splash && splash->IsVisible())
			splash->AcquireSplashHold();

#ifndef _DEBUG
		RequestRefreshItemList();
#endif
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

			for (int filteredIndex : g_filteredIndices)
			{
				const ItemEntry& entry = g_items[filteredIndex];
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

				// Icon — use the persistent handle cached when items were adopted.
				imgui->SetCursorPosX(imgui->GetCursorPosX() + padX);
				{
					IPluginHooks* texHooks = GetHooks();
					const ItemEntry& e = g_items[filteredIndex];
					if (e.cachedTexHandle && texHooks && texHooks->ImGuiTextures)
						texHooks->ImGuiTextures->Image(e.cachedTexHandle, iconSz, iconSz);
					else
						imgui->Dummy(iconSz, iconSz);
				}

				// Name + stack size info, vertically centred beside the icon
				imgui->SameLine(0.0f, padX);
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
				RequestGiveItem(selected->uniqueName, totalAmount, g_stacks, maxStack, selected->name);
			imgui->SameLine(0.0f, 6.0f);
			char totalLabel[64];
			snprintf(totalLabel, sizeof(totalLabel), "x%d  (stack %d)", totalAmount, maxStack);
			imgui->TextDisabled(totalLabel);
		}

		// Loot-box-at-feet spawning would land here once the relevant pickup-container
		// class (the in-world "loot box" actor) is identified in the SDK.
	}

	void Shutdown()
	{
		FreeItemTexHandles(g_items);
		g_items.clear();
		g_itemsLoaded = false;
	}
}
