#include "player_inventory.h"
#include "plugin_helpers.h"
#include "game_context.h"
#include "session_config.h"
#include "player_lookup.h"
#include "item_registry.h"
#include "attribute_compose.h"
#include "cheat_math.h"
#include "ui_widgets.h"
#include "game_thread.h"

#include "Chimera_classes.hpp"
#include "ChimeraUI_classes.hpp"
#include "WBP_InventorySlot_classes.hpp"
#include "AuItems_classes.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// Resizing the player inventory is generated-SDK only: no AOB patterns, no
// detours. The grid itself is one UFUNCTION call; the slot widgets are plain
// UMG field writes.
//
// Two halves that have to agree. The inventory frame has no scroll box anywhere
// in it — the whole WBP_Inventory tree is GridPanel, Border and SizeBox — so a
// grid with more rows than the frame was authored for simply runs off the
// bottom of the pane. Widening instead of lengthening keeps the shape, and
// shrinking every slot widget by the same factor keeps the footprint. Do one
// without the other and the grid overflows sideways instead of downwards.
//
// Everything that touches a UObject runs on the game thread — Tick(), or the
// console handler, which is registered with gameThread = true. RenderImGui()
// runs on the render thread and only ever reads the snapshot.

namespace BetterCheats::Panels::Inventory
{
	namespace
	{
		// The game refuses to build a grid smaller than this, so neither the
		// panel nor the console command offers one.
		constexpr int kMinGridColumns = 8;
		constexpr int kMinGridRows    = 8;
		constexpr int kMaxGridColumns = 40;
		constexpr int kMaxGridRows    = 40;

		// Below this the icons and stack counts stop being readable, so the grid
		// is allowed to overflow the frame rather than shrink any further.
		constexpr float kMinSlotScale = 0.40f;

		// Neither half stays applied on its own: the game rebuilds every slot
		// widget whenever the inventory resizes, so the sizes have to be put back
		// afterwards, and the widgets only exist once the inventory is opened.
		constexpr float kMaintainInterval = 0.5f;

		// ResizeInventory can refuse (see ResizeGrid). Retrying an RPC forever at
		// 2 Hz is worse than leaving the grid the shape it is.
		constexpr int kMaxResizeAttempts = 3;

		constexpr const char* kCommandName  = "bc_invsize";
		constexpr const char* kCommandAlias = "invsize";

		// ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp
		constexpr int kTableFlags  = (1 << 6) | (1 << 9) | (3 << 13);
		constexpr int kColumnFixed = 1 << 4; // ImGuiTableColumnFlags_WidthFixed

		bool g_commandRegistered = false;

		int ClampColumns(int columns)
		{
			if (columns < kMinGridColumns) return kMinGridColumns;
			if (columns > kMaxGridColumns) return kMaxGridColumns;
			return columns;
		}

		int ClampRows(int rows)
		{
			if (rows < kMinGridRows) return kMinGridRows;
			if (rows > kMaxGridRows) return kMaxGridRows;
			return rows;
		}

		// ---------------------------------------------------------------------
		// Lookups. GetLocalCharacter (player_lookup.h) is class-checked: the local
		// pawn is replicated, and during a level transition or a multiplayer join
		// it is briefly some other class — reading InventoryComponent off the
		// wrong object reads past the end of it.
		// ---------------------------------------------------------------------
		SDK::UCrInventoryComponent* GetLocalInventory()
		{
			SDK::ACrCharacterPlayerBase* character = GetLocalCharacter();
			return character ? character->InventoryComponent : nullptr;
		}

		// =====================================================================
		// The resize itself.
		//
		// UCrInventoryComponent::ServerResizeInventory is NetServer with no
		// _Validate. The native ResizeInventory forwards to the RPC whenever the
		// caller is not the authority, so one call covers both cases: on a host
		// it runs straight away, on a client the server performs it and
		// replicates the new Slots array back.
		//
		// It is silent about two refusals, both inside the engine:
		//   * Rows*Columns == Slots.Num()          -> no-op, nothing to do
		//   * new size < number of occupied slots  -> refused, so items are safe
		//
		// The first one matters here, because reshaping at a constant slot count
		// is a normal request: 16x8 and 8x16 are both 128 slots, so asking for
		// the other shape does nothing at all. Passing through a one-row-taller
		// grid first gives the engine a size change to act on, and lands on the
		// shape that was actually asked for.
		// =====================================================================
		bool ResizeGrid(SDK::UCrInventoryComponent* inv, int columns, int rows)
		{
			if (!inv || columns <= 0 || rows <= 0)
				return false;

			if (inv->GridColumns == columns && inv->GridRows == rows)
				return true;

			if (inv->Slots.Num() == columns * rows)
				inv->ServerResizeInventory(columns, rows + 1);

			inv->ServerResizeInventory(columns, rows);
			return true;
		}

		// =====================================================================
		// Slot scale — shrink the widgets so the requested grid still fits the
		// frame the minimum grid was drawn for.
		//
		// The reference is the 8x8 minimum, NOT the component's InitialGridRows:
		// that is smaller than 8 on the player inventory, so measuring against it
		// shrank the slots at 8x8 and left the grid sitting in the corner of an
		// otherwise empty frame. Against 8x8, 8x8 is 100%, 16x16 is 50%, and so
		// on down.
		//
		// One factor for both axes so the slots stay square: the limiting axis
		// wins, and the grid keeps its proportions instead of turning into a
		// stretched strip. A grid that is wide but short therefore leaves space
		// below it rather than stretching to fill the frame.
		// =====================================================================
		float SolveSlotScale(int columns, int rows)
		{
			if (columns <= 0 || rows <= 0)
				return 1.0f;

			const float byWidth  = static_cast<float>(kMinGridColumns) / static_cast<float>(columns);
			const float byHeight = static_cast<float>(kMinGridRows)    / static_cast<float>(rows);

			float scale = byWidth < byHeight ? byWidth : byHeight;

			if (scale > 1.0f)          scale = 1.0f;
			if (scale < kMinSlotScale) scale = kMinSlotScale;
			return scale;
		}

		// ---------------------------------------------------------------------
		// The live inventory widget. Game thread only.
		//
		// GObjects has to be walked to find it — there is no path to the
		// inventory widget from the player controller — so the result is cached
		// and revalidated by index rather than by dereferencing a pointer that
		// may belong to a destroyed widget. If the object at the cached index is
		// still the same pointer then it is live, and only then is it safe to
		// look at.
		// ---------------------------------------------------------------------
		SDK::UCrUW_InventoryContainer* g_container      = nullptr;
		int32_t                        g_containerIndex = -1;

		SDK::UCrUW_InventoryContainer* ValidateContainer()
		{
			if (!g_container || g_containerIndex < 0)
				return nullptr;

			SDK::TUObjectArray* arr = SDK::UObject::GObjects.GetTypedPtr();
			if (!arr || g_containerIndex >= arr->Num())
				return nullptr;

			if (arr->GetByIndex(g_containerIndex) != g_container)
				return nullptr;

			SDK::UClass* containerClass = SDK::UCrUW_InventoryContainer::StaticClass();
			if (!containerClass || !g_container->IsA(containerClass))
				return nullptr;

			return g_container;
		}

		void RescanContainer()
		{
			g_container      = nullptr;
			g_containerIndex = -1;

			SDK::TUObjectArray* arr = SDK::UObject::GObjects.GetTypedPtr();
			if (!arr)
				return;

			SDK::UClass* inventoryClass = SDK::UCrUW_Inventory::StaticClass();
			if (!inventoryClass)
				return;

			// Newest first: a widget from a previous session sticks around until
			// GC runs, and the live one is always the more recently created.
			for (int i = arr->Num() - 1; i >= 0; --i)
			{
				SDK::UObject* obj = arr->GetByIndex(i);
				if (!obj || !obj->Class) continue;
				if (obj->IsDefaultObject()) continue;
				if (!obj->IsA(inventoryClass)) continue;

				SDK::UCrUW_InventoryContainer* container =
					static_cast<SDK::UCrUW_Inventory*>(obj)->ItemsContainer;

				if (!container || !container->ItemGridPanel)
					continue;

				g_container      = container;
				g_containerIndex = container->Index;
				return;
			}
		}

		// The designer slot size, captured from the first slot seen before
		// anything has been written to it. Every scaled size is a fraction of it.
		float g_baseSlotWidth  = 0.0f;
		float g_baseSlotHeight = 0.0f;

		// WBP_InventorySlot_C::SetSlotSize's parameter block, which carries the
		// Blueprint graph's own locals after the argument — ProcessEvent writes
		// through all of it, so the whole 0x28 has to be there.
		struct SetSlotSizeParams
		{
			SDK::FVector2D InSize;
			double         BreakVector2D_X;
			double         BreakVector2D_Y;
			float          WidthOverrideCast;
			float          HeightOverrideCast;
		};
		static_assert(sizeof(SetSlotSizeParams) == 0x28,
			"WBP_InventorySlot_C::SetSlotSize parameter block changed");

		// Prefer the widget's own resize entry point: it drives the SizeBox the
		// same way we would, and whatever else the Blueprint does to keep the
		// slot's insides in proportion comes along with it. The direct SizeBox
		// write below still runs, so a stubbed-out event is not a silent failure.
		void CallSetSlotSize(SDK::UWBP_InventorySlot_C* slot, float width, float height)
		{
			static SDK::UFunction* function = nullptr;

			if (!function)
			{
				if (!slot->Class) return;
				function = slot->Class->GetFunction("WBP_InventorySlot_C", "SetSlotSize");
				if (!function) return;
			}

			SetSlotSizeParams params{};
			params.InSize = SDK::FVector2D(static_cast<double>(width), static_cast<double>(height));

			slot->ProcessEvent(function, &params);
		}

		// Returns the scale actually in force, or 0 when there was nothing to
		// apply it to — the slot widgets only exist once the inventory is opened.
		float ApplySlotScale(float scale)
		{
			SDK::UCrUW_InventoryContainer* container = ValidateContainer();
			if (!container) return 0.0f;

			SDK::UGridPanel* grid = container->ItemGridPanel;
			if (!grid) return 0.0f;

			SDK::UClass* slotClass = SDK::UWBP_InventorySlot_C::StaticClass();
			if (!slotClass) return 0.0f;

			int touched = 0;

			for (int i = 0; i < grid->Slots.Num(); ++i)
			{
				SDK::UPanelSlot* panelSlot = grid->Slots[i];
				if (!panelSlot || !panelSlot->Content) continue;
				if (!panelSlot->Content->IsA(slotClass)) continue;

				SDK::UWBP_InventorySlot_C* slot = static_cast<SDK::UWBP_InventorySlot_C*>(panelSlot->Content);
				SDK::USizeBox* box = slot->SizeBox;
				if (!box) continue;

				// Captured once, from a slot nothing has written to yet. Slot
				// widgets are rebuilt from the designer template on every resize,
				// so a fresh one always carries the real base size.
				if (g_baseSlotWidth <= 0.0f)
				{
					if (box->bOverride_WidthOverride && box->WidthOverride > 0.0f)
					{
						g_baseSlotWidth  = box->WidthOverride;
						g_baseSlotHeight = (box->bOverride_HeightOverride && box->HeightOverride > 0.0f)
							? box->HeightOverride
							: box->WidthOverride;
					}
					else
					{
						// No override authored — fall back to what Slate measured,
						// which is zero until the widget has been laid out once.
						const SDK::FVector2D desired = slot->GetDesiredSize();
						if (desired.X <= 0.0) continue;

						g_baseSlotWidth  = static_cast<float>(desired.X);
						g_baseSlotHeight = static_cast<float>(desired.Y > 0.0 ? desired.Y : desired.X);
					}
				}

				const float width  = g_baseSlotWidth  * scale;
				const float height = g_baseSlotHeight * scale;

				// Every write invalidates layout, so skip the ones that would not
				// change anything — this runs twice a second, forever.
				if (box->WidthOverride == width && box->HeightOverride == height)
				{
					++touched;
					continue;
				}

				CallSetSlotSize(slot, width, height);

				box->SetWidthOverride(width);
				box->SetHeightOverride(height);
				++touched;
			}

			return touched > 0 ? scale : 0.0f;
		}

		// ---------------------------------------------------------------------
		// Wanted state — written from the render thread and the console handler,
		// read on the game thread.
		// ---------------------------------------------------------------------
		// Zero means "not decided yet": no saved grid for this session and the
		// inventory component has not reported one either. The maintenance pass
		// adopts whatever the game built rather than forcing a minimum onto a
		// save that never asked for one.
		std::atomic<int>  g_wantColumns{ 0 };
		std::atomic<int>  g_wantRows{ 0 };
		std::atomic<bool> g_wantFitToPanel{ true };
		std::atomic<bool> g_pendingResize{ false };

		// ---------------------------------------------------------------------
		// Applied state — game thread only.
		// ---------------------------------------------------------------------
		float g_maintainTimer    = 0.0f;
		float g_appliedSlotScale = 1.0f;
		int   g_resizeAttempts   = 0;
		int   g_resizeTarget     = 0; // the shape the attempts were counted for

		// A miss costs a full GObjects walk, and the widget does not exist at all
		// until the inventory is opened for the first time, so a miss is the
		// normal case for most of a session. Back off hard between attempts —
		// see enemies.cpp, where walking GObjects too often was itself the
		// framerate drop it looked like it was diagnosing.
		constexpr float kRescanCooldown = 5.0f;
		float g_rescanCooldown = 0.0f;

		void ApplyPendingResize()
		{
			if (!g_pendingResize.load())
				return;

			// Already clamped at every site that writes them.
			const int columns = g_wantColumns.load();
			const int rows    = g_wantRows.load();

			if (columns <= 0 || rows <= 0)
			{
				g_pendingResize.store(false);
				return;
			}

			try
			{
				SDK::UCrInventoryComponent* inv = GetLocalInventory();
				if (!inv)
					return; // stays pending: the pawn may not be possessed yet

				g_pendingResize.store(false);
				g_resizeAttempts = 0;
				g_resizeTarget   = columns * 1000 + rows;

				if (ResizeGrid(inv, columns, rows))
					LOG_INFO("Inventory: grid set to %d x %d (%d slots).", columns, rows, columns * rows);
			}
			catch (...)
			{
				g_pendingResize.store(false);
				LOG_ERROR("Inventory: exception while resizing the inventory.");
			}
		}

		// Re-asserts both halves. The grid can be reshaped underneath us — the
		// corporation-reward unlock path calls ResizeInventory itself — and the
		// slot widgets are rebuilt at their designer size every time that happens.
		void MaintainGrid(float deltaSeconds)
		{
			g_maintainTimer += deltaSeconds;
			if (g_maintainTimer < kMaintainInterval)
				return;
			g_maintainTimer = 0.0f;

			SDK::UCrInventoryComponent* inv = GetLocalInventory();
			if (!inv)
				return;

			const bool fit = g_wantFitToPanel.load();

			// Nothing to find the widget for while the slots are already the size
			// the game made them.
			if ((fit || g_appliedSlotScale != 1.0f) && !ValidateContainer())
			{
				g_rescanCooldown -= kMaintainInterval;
				if (g_rescanCooldown <= 0.0f)
				{
					g_rescanCooldown = kRescanCooldown;
					RescanContainer();
				}
			}

			int columns = g_wantColumns.load();
			int rows    = g_wantRows.load();

			// Nothing has asked for a size, so the game's own grid becomes the
			// target — and stays untouched.
			if (columns <= 0 || rows <= 0)
			{
				if (inv->GridColumns <= 0 || inv->GridRows <= 0)
					return;

				columns = inv->GridColumns;
				rows    = inv->GridRows;
				g_wantColumns.store(columns);
				g_wantRows.store(rows);
			}

			// Keyed on the shape, not the slot count: 16x8 and 8x16 are the same
			// total but different requests, and the second one deserves its own
			// attempts rather than inheriting the first one's.
			if (g_resizeTarget != columns * 1000 + rows)
			{
				g_resizeTarget   = columns * 1000 + rows;
				g_resizeAttempts = 0;
			}

			if (inv->GridColumns != columns || inv->GridRows != rows)
			{
				if (g_resizeAttempts < kMaxResizeAttempts)
				{
					++g_resizeAttempts;
					ResizeGrid(inv, columns, rows);

					if (g_resizeAttempts == kMaxResizeAttempts)
					{
						LOG_WARN("Inventory: the game would not resize the grid to %d x %d "
							"(currently %d x %d, %d slots) — it will not shrink below the "
							"slots that are in use.",
							columns, rows, inv->GridColumns, inv->GridRows, inv->Slots.Num());
					}
				}
			}
			else
			{
				g_resizeAttempts = 0;
			}

			// Scaled against the grid the game actually built, not the one that
			// was asked for — a refused resize should not shrink the slots.
			const float scale = fit
				? SolveSlotScale(inv->GridColumns, inv->GridRows)
				: 1.0f;

			const float applied = ApplySlotScale(scale);
			if (applied > 0.0f)
				g_appliedSlotScale = applied;
		}

		// Drops every cached pointer into the game's widgets.
		//
		// Deliberately does NOT put the slot sizes back. Unload runs on the game
		// thread but says nothing about what state the UI is in, and executing a
		// Blueprint function (SetSlotSize) through a widget tree that may already
		// be tearing down is not worth the risk of faulting: the loader wraps
		// PluginShutdown in SEH, so a fault here is swallowed and the DLL is
		// freed anyway, taking the rest of the shutdown with it.
		//
		// The cost of leaving them is cosmetic and self-correcting — the game
		// rebuilds every slot widget at its designer size the next time the
		// inventory is resized. Turning the fit toggle off restores them properly,
		// on a tick, while the plugin is still alive.
		void ForgetWidgets()
		{
			g_appliedSlotScale = 1.0f;
			g_baseSlotWidth    = 0.0f;
			g_baseSlotHeight   = 0.0f;
			g_container        = nullptr;
			g_containerIndex   = -1;
		}

		// ---------------------------------------------------------------------
		// Snapshot — populated on the game thread (Tick), read on the ImGui
		// render thread. Never touch SDK objects from RenderImGui().
		// ---------------------------------------------------------------------
		struct InventorySnapshot
		{
			bool  inventoryFound = false;
			int   columns        = 0;
			int   rows           = 0;
			int   slots          = 0;
			bool  widgetFound    = false;
			float slotScale      = 1.0f;
		};

		std::mutex        g_snapshotMutex;
		InventorySnapshot g_snapshot;

		void RefreshSnapshot()
		{
			InventorySnapshot snap;

			try
			{
				if (SDK::UCrInventoryComponent* inv = GetLocalInventory())
				{
					snap.inventoryFound = true;
					snap.columns        = inv->GridColumns;
					snap.rows           = inv->GridRows;
					snap.slots          = inv->Slots.Num();
				}

				snap.widgetFound = ValidateContainer() != nullptr;
				snap.slotScale   = g_appliedSlotScale;
			}
			catch (...)
			{
				return;
			}

			std::lock_guard<std::mutex> lock(g_snapshotMutex);
			g_snapshot = snap;
		}

		// ---------------------------------------------------------------------
		// Console command — "bc_invsize <columns> <rows>" (alias "invsize").
		//
		// gameThread = true, so the handler runs on the next engine tick rather
		// than on the console thread that typed it.
		// ---------------------------------------------------------------------
		bool ParseInt(const char* text, int& out)
		{
			if (!text || !*text) return false;

			int value = 0;
			for (const char* p = text; *p; ++p)
			{
				if (*p < '0' || *p > '9') return false;
				value = value * 10 + (*p - '0');
				if (value > 100000) return false;
			}

			out = value;
			return true;
		}

		void HandleInvSize(const char* const* argv, int argc, PluginConsoleSink sink, void* userData)
		{
			IPluginSelf* self = static_cast<IPluginSelf*>(userData);
			if (!self || !self->hooks || !self->hooks->Console) return;

			IPluginConsole* console = self->hooks->Console;

			if (!GameContext::AreCheatsAllowed())
			{
				console->Write(sink, PluginConsoleLineKind::Error,
					"Cheats are only available in single player.");
				return;
			}

			try
			{
				SDK::UCrInventoryComponent* inv = GetLocalInventory();
				if (!inv)
				{
					console->Write(sink, PluginConsoleLineKind::Error,
						"No player inventory available yet -- load into a world first.");
					return;
				}

				if (argc < 3)
				{
					console->Printf(sink, PluginConsoleLineKind::Output,
						"Grid is %d x %d (%d slots). Usage: bc_invsize <columns> <rows>, minimum %d x %d.",
						inv->GridColumns, inv->GridRows, inv->Slots.Num(), kMinGridColumns, kMinGridRows);
					return;
				}

				int columns = 0, rows = 0;
				if (!ParseInt(argv[1], columns) || !ParseInt(argv[2], rows))
				{
					console->Write(sink, PluginConsoleLineKind::Error,
						"Usage: bc_invsize <columns> <rows>");
					return;
				}

				columns = ClampColumns(columns);
				rows    = ClampRows(rows);

				g_wantColumns.store(columns);
				g_wantRows.store(rows);
				g_pendingResize.store(true);

				SessionConfig::Set("playerInventory.columns", columns);
				SessionConfig::Set("playerInventory.rows", rows);

				console->Printf(sink, PluginConsoleLineKind::Output,
					"Inventory grid set to %d x %d (%d slots).", columns, rows, columns * rows);
			}
			catch (...)
			{
				console->Write(sink, PluginConsoleLineKind::Error,
					"Exception while resizing the inventory.");
			}
		}

		// ---------------------------------------------------------------------
		// "Columns  [-] 12 [+]" stepper. Returns true on the frame the value
		// changed, so the caller can resize on the click rather than behind an
		// Apply button.
		//
		// Laid out on absolute offsets from the start of the line so the [+]
		// button does not shuffle sideways as the number gains a digit.
		// ---------------------------------------------------------------------
		bool RenderStepper(IModLoaderImGui* imgui, const char* id, const char* label,
		                   int* value, int minValue, int maxValue)
		{
			constexpr float kMinusX = 90.0f;
			constexpr float kValueX = 126.0f;
			constexpr float kValueW = 28.0f;
			constexpr float kPlusX  = 162.0f;

			bool changed = false;

			imgui->PushIDStr(id);

			imgui->AlignTextToFramePadding();
			imgui->Text(label);

			imgui->SameLine(kMinusX, 0.0f);
			if (imgui->Button("-") && *value > minValue)
			{
				--(*value);
				changed = true;
			}

			char text[16];
			snprintf(text, sizeof(text), "%d", *value);

			float textWidth = 0.0f, textHeight = 0.0f;
			imgui->CalcTextSize(text, &textWidth, &textHeight, false, 0.0f);

			imgui->SameLine(kValueX + (kValueW - textWidth) * 0.5f, 0.0f);
			imgui->AlignTextToFramePadding();
			imgui->Text(text);

			imgui->SameLine(kPlusX, 0.0f);
			if (imgui->Button("+") && *value < maxValue)
			{
				++(*value);
				changed = true;
			}

			imgui->PopID();

			return changed;
		}

		// =====================================================================
		// Item stack sizes. UAuItemDataBase::MaxStack is a plain int32
		// on the item TYPE's own CDO (AuItems_classes.hpp:290) -- same per-
		// type-CDO shape as the weapon magazine/grenade fields in
		// player_weapons.cpp, so it uses the exact same capture/write/restore/
		// persist machinery (ComposedAttribute + ApplyComposedRow, now shared
		// via attribute_compose.h). Enumeration reuses item_registry.h's
		// asset-registry pass -- the same one player_items.cpp uses -- rather
		// than re-scanning independently.
		//
		// Two lists, deliberately not one: g_stackItems is render-thread owned
		// (adopted only from RenderImGui, like player_items.cpp's g_items) for
		// the UI; g_stackItemsGameThread is a separate copy written only by the
		// game-thread scan callback and read only from Tick()/ApplyStackSizes()
		// -- the apply pass writes real UObject fields and must never touch the
		// render-thread copy without a lock it doesn't otherwise need.
		// =====================================================================

		struct StackEntry
		{
			SDK::UAuItemDataBase* item             = nullptr;
			SDK::FAssetData         assetData;         // re-resolve fresh before a write -- item_registry.h
			std::string             uniqueName;        // item->UniqueItemName, the identity key
			std::string             name;              // display name
			int                     originalMaxStack = 1;  // captured at scan time, the true base
			bool                    canStack         = true; // StackingType != DoNotStack
			SDK::EUIItemType        category = SDK::EUIItemType::None;  // item->UIItemType -- the
			                                                            // game's own UI category
			                                                            // (AuItems_classes.hpp:320,
			                                                            // AuItems_structs.hpp:44) --
			                                                            // already what the game itself
			                                                            // uses to color/label items
			                                                            // per type (ChimeraUI's
			                                                            // UIItemTypesColors data asset
			                                                            // keys off the same enum).
		};

		std::mutex               g_pendingStackMutex;
		std::vector<StackEntry>  g_pendingStackItems;
		bool                     g_pendingStackReady = false;
		std::atomic<bool>        g_stackRefreshInFlight{ false };

		std::vector<StackEntry>  g_stackItems;          // render-thread owned (UI)
		bool                     g_stackItemsLoaded  = false;
		std::vector<StackEntry>  g_stackItemsGameThread; // game-thread owned (apply pass)

		char                     g_stackSearchBuf[128] = {};
		std::vector<int>         g_filteredStackIndices;
		bool                     g_onlyShowChangedStacks = false;

		constexpr int   kStackFloor      = 1;
		constexpr int   kStackCeiling    = 1000000;  // sanity clamp on the multiplier's result --
		                                              // same spirit as player_weapons.cpp's kMagazineCeiling
		constexpr float kStackMultDefault = 1.0f;
		constexpr float kStackMultMin     = 0.10f;
		constexpr float kStackMultMax     = 20.0f;

		std::atomic<float> g_stackMultiplier{ kStackMultDefault };

		// Per-item override, keyed by the item's true UniqueItemName (an exact
		// in-memory key, distinct from the sanitized string used for
		// SessionConfig paths below). Absent = no override, follow the global
		// multiplier. Guarded because RenderImGui (render thread) writes it and
		// Tick() (game thread) reads it every tick.
		std::mutex                           g_overrideMutex;
		std::unordered_map<std::string, int> g_stackOverrides;

		// Per-item compose state, game-thread only -- never touched from
		// RenderImGui. `item` is set the first time ApplyStackSizes() visits an
		// entry, so Shutdown() can Release() without depending on
		// g_stackItemsGameThread still matching by the time it runs.
		struct StackComposeState
		{
			BetterCheats::ComposedAttribute composed;
			SDK::UAuItemDataBase*           item = nullptr;
		};
		std::unordered_map<std::string, StackComposeState> g_stackComposed;

		std::string ToLowerAsciiStack(const std::string& s)
		{
			std::string out = s;
			for (char& c : out)
				if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
			return out;
		}

		// Config-path-safe key -- UniqueItemName is expected to already be a
		// plain identifier, but sanitizing (mirroring player_weapons.cpp's own
		// Sanitize()) means a stray '.' can never split a SessionConfig path in
		// two. The in-memory maps above key on the exact name instead: they
		// don't build dot-paths, so nothing to protect there.
		std::string SanitizeStackKey(const std::string& raw)
		{
			std::string out;
			for (char c : ToLowerAsciiStack(raw))
				if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) out += c;
			return out.empty() ? "unknown" : out;
		}

		int ClampStackValue(int value)
		{
			if (value < kStackFloor)   return kStackFloor;
			if (value > kStackCeiling) return kStackCeiling;
			return value;
		}

		int MultiplierDefaultFor(int originalMaxStack, float multiplier)
		{
			return ClampStackValue(static_cast<int>(static_cast<float>(originalMaxStack) * multiplier + 0.5f));
		}

		// EUIItemType's own enum names, not a raw/crafted/building/consumable
		// split -- the game has no "crafted" or "building" bucket in this
		// field, so relabelling would just invent a mapping that doesn't
		// exist. None/EmptyItem/BlueprintItem/Count are
		// real enum values but never expected to carry actual stackable items
		// through the scan's own filtering; kept here only so the switch is
		// exhaustive and a stray one still gets a readable label instead of
		// "Other".
		const char* CategoryLabel(SDK::EUIItemType t)
		{
			switch (t)
			{
			case SDK::EUIItemType::Combat:        return "Combat";
			case SDK::EUIItemType::Resource:      return "Resource";
			case SDK::EUIItemType::Consumable:    return "Consumable";
			case SDK::EUIItemType::StoryItem:     return "Story Item";
			case SDK::EUIItemType::AlienItem:     return "Alien Item";
			case SDK::EUIItemType::Valuable:      return "Valuable";
			case SDK::EUIItemType::GemMovement:   return "Gem (Movement)";
			case SDK::EUIItemType::GemCombat:     return "Gem (Combat)";
			case SDK::EUIItemType::GemSurvival:   return "Gem (Survival)";
			case SDK::EUIItemType::None:          return "Uncategorized";
			case SDK::EUIItemType::EmptyItem:     return "Empty Item";
			case SDK::EUIItemType::BlueprintItem: return "Blueprint Item";
			default:                              return "Other";
			}
		}

		// Inverse of CategoryLabel, for reloading persisted category overrides
		// (stored by label, not raw enum value -- see PersistCategoryOverrides).
		bool CategoryFromLabel(const std::string& label, SDK::EUIItemType& out)
		{
			for (int i = 0; i <= static_cast<int>(SDK::EUIItemType::BlueprintItem); ++i)
			{
				const auto t = static_cast<SDK::EUIItemType>(i);
				if (label == CategoryLabel(t)) { out = t; return true; }
			}
			return false;
		}

		// Per-item override, keyed by the item's true UniqueItemName -- the
		// innermost, most-specific tier. Absent = no item-level override,
		// inherit the category tier below.
		int EffectiveStackFor(const StackEntry& e, float globalMultiplier,
			const std::unordered_map<int, float>& categoryOverrides,
			const std::unordered_map<std::string, int>& itemOverrides)
		{
			auto itemIt = itemOverrides.find(e.uniqueName);
			if (itemIt != itemOverrides.end())
				return ClampStackValue(itemIt->second);

			auto catIt = categoryOverrides.find(static_cast<int>(e.category));
			const float categoryMultiplier = (catIt != categoryOverrides.end()) ? catIt->second : globalMultiplier;
			return MultiplierDefaultFor(e.originalMaxStack, categoryMultiplier);
		}

		// Persisted as one array of {uniqueName, value} objects -- the same
		// shape player_weapons.cpp's PersistKnownWeapons uses for its "known
		// weapons" list -- rather than a nested object keyed by item name.
		// SessionConfig's Set/Get only do dot-path traversal (session_config.cpp
		// naively swaps '.' for '/' building a JSON pointer, with no escaping),
		// so a raw UniqueItemName used AS a path segment would corrupt the tree
		// if it ever contained a '.'; keeping it as a plain string VALUE inside
		// an array sidesteps that entirely, and round-trips the exact name
		// (a sanitized-for-path key could never recover it losslessly).
		void PersistStackOverrides()
		{
			std::unordered_map<std::string, int> snapshot;
			{
				std::lock_guard<std::mutex> lock(g_overrideMutex);
				snapshot = g_stackOverrides;
			}

			nlohmann::json arr = nlohmann::json::array();
			for (const auto& kv : snapshot)
				arr.push_back({ {"uniqueName", kv.first}, {"value", kv.second} });
			SessionConfig::Set("playerInventory.stacks.overrides", arr);
		}

		void SetStackOverride(const std::string& uniqueName, int value)
		{
			{
				std::lock_guard<std::mutex> lock(g_overrideMutex);
				g_stackOverrides[uniqueName] = value;
			}
			PersistStackOverrides();
		}

		void ClearStackOverride(const std::string& uniqueName)
		{
			{
				std::lock_guard<std::mutex> lock(g_overrideMutex);
				g_stackOverrides.erase(uniqueName);
			}
			PersistStackOverrides();
		}

		// Category tier -- same array-of-objects shape as the item overrides
		// above, keyed by label (a stable string) rather than the raw enum
		// value, so a future game patch that reorders EUIItemType doesn't
		// silently remap a saved override onto the wrong category.
		std::mutex                     g_categoryMutex;
		std::unordered_map<int, float> g_categoryOverrides;   // key: static_cast<int>(EUIItemType)

		void PersistCategoryOverrides()
		{
			std::unordered_map<int, float> snapshot;
			{
				std::lock_guard<std::mutex> lock(g_categoryMutex);
				snapshot = g_categoryOverrides;
			}

			nlohmann::json arr = nlohmann::json::array();
			for (const auto& kv : snapshot)
				arr.push_back({ {"category", CategoryLabel(static_cast<SDK::EUIItemType>(kv.first))}, {"value", kv.second} });
			SessionConfig::Set("playerInventory.stacks.categoryOverrides", arr);
		}

		void SetCategoryOverride(SDK::EUIItemType category, float value)
		{
			{
				std::lock_guard<std::mutex> lock(g_categoryMutex);
				g_categoryOverrides[static_cast<int>(category)] = value;
			}
			PersistCategoryOverrides();
		}

		void ClearCategoryOverride(SDK::EUIItemType category)
		{
			{
				std::lock_guard<std::mutex> lock(g_categoryMutex);
				g_categoryOverrides.erase(static_cast<int>(category));
			}
			PersistCategoryOverrides();
		}

		// Distinct categories actually present in the scanned item list, sorted
		// by label -- built once when the list is adopted (AdoptPendingStackItemsIfReady),
		// never per frame. Render-thread owned, like g_stackItems.
		std::vector<SDK::EUIItemType> g_stackCategories;

		void RebuildStackCategories()
		{
			g_stackCategories.clear();
			for (const StackEntry& e : g_stackItems)
			{
				bool seen = false;
				for (SDK::EUIItemType c : g_stackCategories)
					if (c == e.category) { seen = true; break; }
				if (!seen)
					g_stackCategories.push_back(e.category);
			}
			std::sort(g_stackCategories.begin(), g_stackCategories.end(),
				[](SDK::EUIItemType a, SDK::EUIItemType b) { return std::string(CategoryLabel(a)) < CategoryLabel(b); });
		}

		void RefreshFilteredStackItems()
		{
			g_filteredStackIndices.clear();

			const std::string needle     = ToLowerAsciiStack(g_stackSearchBuf);
			const float       multiplier = g_stackMultiplier.load();

			std::unordered_map<std::string, int> overrides;
			{
				std::lock_guard<std::mutex> lock(g_overrideMutex);
				overrides = g_stackOverrides;
			}
			std::unordered_map<int, float> categoryOverrides;
			{
				std::lock_guard<std::mutex> lock(g_categoryMutex);
				categoryOverrides = g_categoryOverrides;
			}

			for (int i = 0; i < static_cast<int>(g_stackItems.size()); ++i)
			{
				const StackEntry& e = g_stackItems[i];
				if (!needle.empty() && ToLowerAsciiStack(e.name).find(needle) == std::string::npos)
					continue;

				if (g_onlyShowChangedStacks)
				{
					const int effective = EffectiveStackFor(e, multiplier, categoryOverrides, overrides);
					if (effective == e.originalMaxStack)
						continue;
				}

				g_filteredStackIndices.push_back(i);
			}
		}

		// Mirrors player_items.cpp's own scan (same asset-registry pass, via
		// item_registry.h) but without icon resolution -- Inventory only cares
		// about MaxStack/StackingType/UniqueItemName, and every item type
		// should be configurable here regardless of whether Item Spawner would
		// want to display it (icon-less items still stack).
		void RefreshStackListOnGameThread(void* /*context*/)
		{
			std::vector<StackEntry> items;

			LOG_INFO("Inventory: refreshing item list for stack sizes from the asset registry...");

			try
			{
				SDK::IAssetRegistry* registry = BetterCheats::ItemRegistry::GetAssetRegistry();
				if (!registry)
				{
					LOG_WARN("Inventory: could not resolve the asset registry for stack sizes.");
				}
				else
				{
					SDK::TArray<SDK::FAssetData> assetData;
					if (!BetterCheats::ItemRegistry::GetAssetsByClass(registry,
						BetterCheats::ItemRegistry::ItemBlueprintClassPath(), assetData))
					{
						LOG_WARN("Inventory: IAssetRegistry::GetAssetsByClass failed for stack sizes.");
					}

					const int rawCount = assetData.Num();
					int unresolvedCount = 0;

					for (int i = 0; i < rawCount; ++i)
					{
						SDK::UAuItemDataBase* item = BetterCheats::ItemRegistry::ResolveItemFromBlueprintAsset(assetData[i], false);
						if (!item) { ++unresolvedCount; continue; }

						StackEntry entry;
						entry.item       = item;
						entry.assetData  = assetData[i];
						entry.uniqueName = item->UniqueItemName.ToString();
						entry.name       = SDK::UKismetTextLibrary::Conv_TextToString(item->ItemName).ToString();
						if (entry.name.empty() || entry.name == "<MISSING STRING TABLE ENTRY>")
							entry.name = entry.uniqueName;

						// Same placeholder/Blueprint-asset filtering player_items.cpp uses.
						if (entry.name == "None") continue;
						if (entry.name.size() >= 9 && entry.name.compare(entry.name.size() - 9, 9, "Blueprint") == 0)
							continue;
						if (entry.uniqueName.empty()) continue;   // nothing to key an override against

						entry.originalMaxStack = item->MaxStack > 0 ? item->MaxStack : 1;
						entry.canStack          = item->StackingType != SDK::ENxItemStackType::DoNotStack;
						entry.category          = item->UIItemType;

						items.push_back(std::move(entry));
					}

					if (unresolvedCount > 0)
						LOG_INFO("Inventory: %d Blueprint asset(s) could not be resolved for stack sizes.", unresolvedCount);

					std::sort(items.begin(), items.end(),
						[](const StackEntry& a, const StackEntry& b) { return a.name < b.name; });

					LOG_INFO("Inventory: loaded %d item type(s) for stack sizes.", static_cast<int>(items.size()));
				}
			}
			catch (...)
			{
				LOG_DEBUG("Inventory: exception while resolving items for stack sizes.");
			}

			// Game-thread copy for the apply pass, ahead of the render-thread
			// hand-off below -- Tick() must never block on g_pendingStackMutex.
			g_stackItemsGameThread = items;

			std::lock_guard<std::mutex> lock(g_pendingStackMutex);
			g_pendingStackItems = std::move(items);
			g_pendingStackReady = true;
			g_stackRefreshInFlight.store(false, std::memory_order_release);
		}

		void RequestRefreshStackList()
		{
			bool expected = false;
			if (!g_stackRefreshInFlight.compare_exchange_strong(expected, true))
				return;

			IPluginHooks* hooks = GetHooks();
			if (!hooks)
			{
				g_stackRefreshInFlight.store(false, std::memory_order_release);
				return;
			}

			hooks->Engine->PostToGameThread(&RefreshStackListOnGameThread, nullptr);
		}

		void AdoptPendingStackItemsIfReady()
		{
			std::vector<StackEntry> incoming;
			{
				std::lock_guard<std::mutex> lock(g_pendingStackMutex);
				if (!g_pendingStackReady)
					return;
				incoming = std::move(g_pendingStackItems);
				g_pendingStackItems.clear();
				g_pendingStackReady = false;
			}

			g_stackItems       = std::move(incoming);
			g_stackItemsLoaded = true;
			RebuildStackCategories();
			RefreshFilteredStackItems();
		}

		// Game thread only. Writes MaxStack for every stackable item whose
		// effective value (override, or the global multiplier applied to its
		// captured original) differs from that original -- never touches
		// DoNotStack items at all. Cheap: this walks the already-resolved list,
		// no asset-registry work here, so running it every tick is fine.
		void ApplyStackSizes()
		{
			if (g_stackItemsGameThread.empty())
				return;

			const float multiplier = g_stackMultiplier.load();
			std::unordered_map<std::string, int> overrides;
			{
				std::lock_guard<std::mutex> lock(g_overrideMutex);
				overrides = g_stackOverrides;
			}
			std::unordered_map<int, float> categoryOverrides;
			{
				std::lock_guard<std::mutex> lock(g_categoryMutex);
				categoryOverrides = g_categoryOverrides;
			}

			try
			{
				for (const StackEntry& e : g_stackItemsGameThread)
				{
					if (!e.item || !e.canStack)
						continue;

					const int  desired = EffectiveStackFor(e, multiplier, categoryOverrides, overrides);
					const bool active  = desired != e.originalMaxStack;

					StackComposeState& state = g_stackComposed[e.uniqueName];
					state.item = e.item;

					float valueF = static_cast<float>(e.item->MaxStack);
					const std::string key = "playerInventory.stacks." + SanitizeStackKey(e.uniqueName);

					BetterCheats::ApplyComposedRow(state.composed, e.item, valueF, key, active,
						static_cast<float>(desired), BetterCheats::ComposedAttribute::Mode::Absolute,
						static_cast<float>(kStackFloor), static_cast<float>(kStackCeiling));

					const int newValue = static_cast<int>(valueF + 0.5f);
					if (e.item->MaxStack != newValue)
						e.item->MaxStack = newValue;
				}
			}
			catch (...) {}
		}

		// Render thread. Global multiplier row, search + "only show changed"
		// filter, then a scrolled table of every filtered item -- follows the
		// same shared-row-helper shape as player_weapons.cpp's tables, and the
		// same scrolled-list-with-a-search-box shape as player_items.cpp's Item
		// Spawner. g_filteredStackIndices is only ever rebuilt on an actual
		// input change (search text, filter toggle, multiplier, an override),
		// never every frame -- "hundreds of items" is fine to filter on demand,
        // not fine to re-filter every single frame for no reason.
		void RenderItemStackSizes(IModLoaderImGui* imgui)
		{
			imgui->Spacing();
			imgui->Separator();
			imgui->SeparatorText("Item Stack Sizes");
			imgui->TextDisabled("Applies to new stacks only -- items already stacked keep their\n"
			                    "current cap until picked up, split, or merged again. Items that\n"
			                    "don't stack at all are shown but can't be changed here.");
			imgui->Spacing();

			AdoptPendingStackItemsIfReady();
			if (!g_stackItemsLoaded)
				RequestRefreshStackList();

			if (!g_stackItemsLoaded)
			{
				imgui->TextDisabled("Loading item list from the asset registry...");
				return;
			}

			// No built-in presets here, so saved presets sit at the very top --
			// same "above every control" position every group uses. One "Inventory"
			// group covers all three tiers in one save/load: the global multiplier,
			// every category (keyed by label, same future-proofing as
			// PersistCategoryOverrides -- a game patch reordering EUIItemType must
			// not remap a saved override onto the wrong category), and every item
			// (keyed by its exact UniqueItemName, same as g_stackOverrides itself).
			// Every category/item is included on every save, not just the
			// currently-overridden ones, with -1 meaning "no override" -- a sparse
			// field set would leave an override in place on load just because that
			// item happened not to be part of the saved preset.
			{
				static BetterCheats::UI::SavedPresetRowState s_presetRow;

				std::vector<std::string> categoryKeys(g_stackCategories.size());
				for (size_t i = 0; i < g_stackCategories.size(); ++i)
					categoryKeys[i] = std::string("category:") + CategoryLabel(g_stackCategories[i]);

				std::vector<std::string> itemKeys(g_stackItems.size());
				for (size_t i = 0; i < g_stackItems.size(); ++i)
					itemKeys[i] = "item:" + g_stackItems[i].uniqueName;

				const int fieldCount = 1 + static_cast<int>(g_stackCategories.size()) + static_cast<int>(g_stackItems.size());
				std::vector<BetterCheats::PresetStore::Field> fields(fieldCount);

				auto getLive = [&](BetterCheats::PresetStore::Field* out)
				{
					std::unordered_map<int, float> categoryOverrides;
					{
						std::lock_guard<std::mutex> lock(g_categoryMutex);
						categoryOverrides = g_categoryOverrides;
					}
					std::unordered_map<std::string, int> itemOverrides;
					{
						std::lock_guard<std::mutex> lock(g_overrideMutex);
						itemOverrides = g_stackOverrides;
					}

					int idx = 0;
					out[idx++] = { "multiplier", g_stackMultiplier.load() };

					for (size_t i = 0; i < g_stackCategories.size(); ++i)
					{
						const auto it = categoryOverrides.find(static_cast<int>(g_stackCategories[i]));
						out[idx++] = { categoryKeys[i].c_str(), it != categoryOverrides.end() ? it->second : -1.0f };
					}
					for (size_t i = 0; i < g_stackItems.size(); ++i)
					{
						const auto it = itemOverrides.find(g_stackItems[i].uniqueName);
						out[idx++] = { itemKeys[i].c_str(), it != itemOverrides.end() ? static_cast<float>(it->second) : -1.0f };
					}
				};
				auto applyFields = [&](const BetterCheats::PresetStore::Field* f, int count)
				{
					int idx = 0;
					if (idx < count)
					{
						const float mult = f[idx].value;
						g_stackMultiplier.store(mult);
						SessionConfig::Set("playerInventory.stacks.multiplier", mult);
					}
					++idx;

					for (SDK::EUIItemType cat : g_stackCategories)
					{
						if (idx >= count) break;
						const float v = f[idx].value;
						if (v < 0.0f) ClearCategoryOverride(cat);
						else          SetCategoryOverride(cat, v);
						++idx;
					}
					for (const StackEntry& e : g_stackItems)
					{
						if (idx >= count) break;
						const float v = f[idx].value;
						if (v < 0.0f) ClearStackOverride(e.uniqueName);
						else          SetStackOverride(e.uniqueName, static_cast<int>(v + 0.5f));
						++idx;
					}
					RefreshFilteredStackItems();
				};
				auto isBuiltin      = [](const char*) { return false; };
				auto computeSuggest = [](char* out, int cap) { snprintf(out, cap, "Custom"); };

				BetterCheats::UI::RenderSavedPresetsRow(imgui, "inventory_saved_presets", "Inventory",
					fields.data(), fieldCount, getLive, applyFields, isBuiltin, computeSuggest, s_presetRow);
			}
			imgui->Spacing();

			// ---- global multiplier -------------------------------------------
			float multiplier = g_stackMultiplier.load();
			if (imgui->BeginTable("##stack_mult_table", 3, kTableFlags))
			{
				const float multLabelReserve = BetterCheats::UI::PrescanLabelWidth(imgui, 1,
					[](int) { return "Stack size multiplier"; });

				imgui->TableSetupColumn("Attribute", kColumnFixed,
					BetterCheats::UI::GetReadoutColumnWidth(imgui, multLabelReserve));
				imgui->TableSetupColumn("Value", 0, 0.54f);
				imgui->TableSetupColumn("",      0, 0.10f);

				imgui->TableNextRow(0, 0.0f);

				BetterCheats::UI::RowSpec spec;
				spec.label        = "Stack size multiplier";
				spec.tooltip      = "Multiplies every stackable item's captured original MaxStack.\n"
				                    "A per-item override below replaces this for that one item.";
				spec.value        = &multiplier;
				spec.minValue     = kStackMultMin;
				spec.maxValue     = kStackMultMax;
				spec.step         = 0.10f;
				spec.format       = "%.2fx";
				spec.resetValue   = kStackMultDefault;
				spec.active       = BetterCheats::DiffersFromDefault(multiplier, kStackMultDefault);
				spec.labelReserve = multLabelReserve;

				if (BetterCheats::UI::BuildRow(imgui, spec).changed)
				{
					g_stackMultiplier.store(multiplier);
					SessionConfig::Set("playerInventory.stacks.multiplier", multiplier);
					RefreshFilteredStackItems();
				}

				imgui->EndTable();
			}

			imgui->Spacing();

			// ---- category tier --------------------------------------------------
			// One row per category actually present in the scan (EUIItemType --
			// the game's own item-type field, see the StackEntry::category
			// comment). Inherits the global multiplier until explicitly
			// overridden; resetting a category drops it back to inheriting.
			if (!g_stackCategories.empty() && imgui->BeginTable("##stack_category_table", 3, kTableFlags))
			{
				const float catLabelReserve = BetterCheats::UI::PrescanLabelWidth(imgui,
					static_cast<int>(g_stackCategories.size()),
					[](int i) { return CategoryLabel(g_stackCategories[i]); });

				imgui->TableSetupColumn("Attribute", kColumnFixed,
					BetterCheats::UI::GetReadoutColumnWidth(imgui, catLabelReserve));
				imgui->TableSetupColumn("Value", 0, 0.54f);
				imgui->TableSetupColumn("",      0, 0.10f);

				std::unordered_map<int, float> categorySnapshot;
				{
					std::lock_guard<std::mutex> lock(g_categoryMutex);
					categorySnapshot = g_categoryOverrides;
				}

				for (SDK::EUIItemType cat : g_stackCategories)
				{
					const auto  catIt        = categorySnapshot.find(static_cast<int>(cat));
					const bool  hasOverride  = catIt != categorySnapshot.end();
					float       catValue     = hasOverride ? catIt->second : multiplier;

					int itemCount = 0;
					for (const StackEntry& e : g_stackItems)
						if (e.category == cat) ++itemCount;

					imgui->PushIDStr(CategoryLabel(cat));
					imgui->TableNextRow(0, 0.0f);

					char tooltip[192];
					snprintf(tooltip, sizeof(tooltip),
						"%d item%s. %s", itemCount, itemCount == 1 ? "" : "s",
						hasOverride ? "Overrides the global multiplier for this category."
						            : "Inherits the global multiplier until you change this row.");

					BetterCheats::UI::RowSpec spec;
					spec.label        = CategoryLabel(cat);
					spec.tooltip      = tooltip;
					spec.value        = &catValue;
					spec.minValue     = kStackMultMin;
					spec.maxValue     = kStackMultMax;
					spec.step         = 0.10f;
					spec.format       = "%.2fx";
					spec.resetValue   = multiplier;   // reset = "go back to inheriting global"
					spec.active       = hasOverride;
					spec.labelReserve = catLabelReserve;

					const BetterCheats::UI::RowResult result = BetterCheats::UI::BuildRow(imgui, spec);
					if (result.changed)
					{
						if (result.resetClicked || BetterCheats::DiffersFromDefault(catValue, multiplier) == false)
							ClearCategoryOverride(cat);
						else
							SetCategoryOverride(cat, catValue);
						RefreshFilteredStackItems();
					}

					imgui->PopID();
				}

				imgui->EndTable();
			}

			imgui->Spacing();

			// ---- search + filter ----------------------------------------------
			char searchHint[64];
			snprintf(searchHint, sizeof(searchHint), "Search %d items...", static_cast<int>(g_stackItems.size()));
			imgui->SetNextItemWidth(-1.0f);
			if (imgui->InputTextWithHint("##stack_search", searchHint, g_stackSearchBuf, sizeof(g_stackSearchBuf)))
				RefreshFilteredStackItems();

			bool onlyChanged = g_onlyShowChangedStacks;
			if (imgui->Checkbox("Only show changed", &onlyChanged))
			{
				g_onlyShowChangedStacks = onlyChanged;
				RefreshFilteredStackItems();
			}

			imgui->Spacing();

			if (g_stackItems.empty())
			{
				imgui->TextDisabled("No stackable item types found.");
				return;
			}
			if (g_filteredStackIndices.empty())
			{
				imgui->TextDisabled("No items match.");
				return;
			}

			// ---- per-item list --------------------------------------------------
			float availX = 0.0f, availY = 0.0f;
			imgui->GetContentRegionAvail(&availX, &availY);
			const float listH = availY > 150.0f ? availY : 150.0f;

			std::unordered_map<std::string, int> overrides;
			{
				std::lock_guard<std::mutex> lock(g_overrideMutex);
				overrides = g_stackOverrides;
			}
			std::unordered_map<int, float> categoryOverrides;
			{
				std::lock_guard<std::mutex> lock(g_categoryMutex);
				categoryOverrides = g_categoryOverrides;
			}
			const float mult = g_stackMultiplier.load();

			if (imgui->BeginChild("##stack_item_list", -1.0f, listH, false))
			{
				const float itemLabelReserve = BetterCheats::UI::PrescanLabelWidth(imgui,
					static_cast<int>(g_filteredStackIndices.size()),
					[](int i) { return g_stackItems[g_filteredStackIndices[i]].name.c_str(); });

				// RefreshFilteredStackItems() rebuilds g_filteredStackIndices in
				// place -- calling it while the range-for below is still iterating
				// that same vector would invalidate the iterator mid-loop. Defer to
				// after the loop instead.
				bool needsRefilter = false;

				if (imgui->BeginTable("##stack_item_table", 3, kTableFlags))
				{
					imgui->TableSetupColumn("Attribute", kColumnFixed,
						BetterCheats::UI::GetReadoutColumnWidth(imgui, itemLabelReserve));
					imgui->TableSetupColumn("Value", 0, 0.54f);
					imgui->TableSetupColumn("",      0, 0.10f);

					for (int filteredIndex : g_filteredStackIndices)
					{
						StackEntry& e = g_stackItems[filteredIndex];

						// The category tier's own effective result -- what this row
						// falls back to if its item-level override is reset. May
						// itself be inheriting global (no category override) or an
						// explicit category override.
						const auto  catIt           = categoryOverrides.find(static_cast<int>(e.category));
						const bool  categoryOverridden = catIt != categoryOverrides.end();
						const float categoryMultiplier = categoryOverridden ? catIt->second : mult;
						const int   inherited        = MultiplierDefaultFor(e.originalMaxStack, categoryMultiplier);

						const auto  overrideIt   = overrides.find(e.uniqueName);
						const bool  hasOverride  = overrideIt != overrides.end();
						float       rowValue     = static_cast<float>(hasOverride ? overrideIt->second : inherited);

						imgui->PushIDStr(e.uniqueName.c_str());
						imgui->TableNextRow(0, 0.0f);

						char tooltip[384];
						int n = snprintf(tooltip, sizeof(tooltip), "Original: %d", e.originalMaxStack);
						n += snprintf(tooltip + n, sizeof(tooltip) - n, "\nGlobal: x%.2f -> %d",
							mult, MultiplierDefaultFor(e.originalMaxStack, mult));
						n += snprintf(tooltip + n, sizeof(tooltip) - n, "\nCategory (%s): %s -> %d",
							CategoryLabel(e.category),
							categoryOverridden ? "overridden" : "inherits global",
							inherited);
						n += snprintf(tooltip + n, sizeof(tooltip) - n, "\nThis item: %s",
							hasOverride ? "overridden (winning)" : "inherits category");
						if (!e.canStack)
							snprintf(tooltip + n, sizeof(tooltip) - n,
								"\n\nThis item does not stack (StackingType is DoNotStack) --\nMaxStack is left untouched regardless of any tier above.");

						BetterCheats::UI::RowSpec spec;
						spec.label        = e.name.c_str();
						spec.tooltip      = tooltip;
						spec.value        = &rowValue;
						spec.minValue     = static_cast<float>(kStackFloor);
						spec.maxValue     = static_cast<float>(kStackCeiling);
						spec.step         = 1.0f;
						spec.format       = "%.0f";
						spec.resetValue   = static_cast<float>(inherited);
						spec.active       = e.canStack && hasOverride;
						spec.disableSlider = !e.canStack;
						spec.labelReserve = itemLabelReserve;

						const BetterCheats::UI::RowResult result = BetterCheats::UI::BuildRow(imgui, spec);
						if (result.changed && e.canStack)
						{
							const int newInt = ClampStackValue(static_cast<int>(rowValue + 0.5f));
							if (result.resetClicked || newInt == inherited)
								ClearStackOverride(e.uniqueName);
							else
								SetStackOverride(e.uniqueName, newInt);
							needsRefilter = true;
						}

						imgui->PopID();
					}

					imgui->EndTable();
				}

				if (needsRefilter)
					RefreshFilteredStackItems();
			}
			imgui->EndChild();
		}
	}

	void Initialize()
	{
		IPluginSelf* self = GetSelf();
		if (!self || !self->hooks || !self->hooks->Console)
		{
			LOG_WARN("Inventory: console unavailable, '%s' not registered.", kCommandName);
			return;
		}

		PluginConsoleCommandDesc desc{};
		desc.name       = kCommandName;
		desc.aliases    = kCommandAlias;
		desc.usage      = "bc_invsize <columns> <rows>";
		desc.help       = "Resize the player inventory grid. Minimum 8 x 8.";
		desc.handler    = &HandleInvSize;
		desc.userData   = self;
		desc.gameThread = true;

		g_commandRegistered = self->hooks->Console->RegisterCommand(self, &desc);

		// Command names are global across every plugin, so a taken alias sinks the
		// whole registration — retry on the prefixed name alone before giving up.
		if (!g_commandRegistered)
		{
			desc.aliases = nullptr;
			g_commandRegistered = self->hooks->Console->RegisterCommand(self, &desc);
		}

		if (!g_commandRegistered)
			LOG_WARN("Inventory: console command '%s' is already taken.", kCommandName);
	}

	void Shutdown()
	{
		IPluginSelf* self = GetSelf();
		if (g_commandRegistered && self && self->hooks && self->hooks->Console)
			self->hooks->Console->UnregisterCommand(self, kCommandName);

		g_commandRegistered = false;

		ForgetWidgets();

		// Item stack sizes: same render-thread-Shutdown hazard as the weapon
		// panel's own composed fields -- the loader's own RELOAD button runs
		// PluginShutdown from its D3D Present hook, not the
		// game thread Tick() recorded, and every UObject touch below
		// intermittently crashes there. Forget instead of Release off-thread:
		// SessionConfig still has whatever RestoreIfStale needs to undo a
		// leftover write on the next activation, off-thread or not.
		if (!BetterCheats::IsGameThread())
		{
			for (auto& kv : g_stackComposed)
				kv.second.composed.Forget();
			return;
		}

		try
		{
			for (auto& kv : g_stackComposed)
			{
				StackComposeState& state = kv.second;
				if (!state.item) continue;

				float valueF = static_cast<float>(state.item->MaxStack);
				state.composed.Release(state.item, valueF);
				const int newValue = static_cast<int>(valueF + 0.5f);
				if (state.item->MaxStack != newValue)
					state.item->MaxStack = newValue;

				BetterCheats::ClearComposeState("playerInventory.stacks." + SanitizeStackKey(kv.first));
			}
		}
		catch (...) {}
	}

	void Tick(float deltaSeconds)
	{
		BetterCheats::RecordGameThread();

		ApplyPendingResize();
		MaintainGrid(deltaSeconds);
		RefreshSnapshot();

		ApplyStackSizes();
	}

	void ApplySavedConfig()
	{
		if (!SessionConfig::IsLoaded())
			return;

		const int  savedColumns = SessionConfig::Get("playerInventory.columns", 0);
		const int  savedRows    = SessionConfig::Get("playerInventory.rows", 0);
		const bool savedFit     = SessionConfig::Get("playerInventory.fitToPanel", true);

		g_wantFitToPanel.store(savedFit);

		if (savedColumns > 0 && savedRows > 0)
		{
			g_wantColumns.store(ClampColumns(savedColumns));
			g_wantRows.store(ClampRows(savedRows));
			g_pendingResize.store(true);
		}
		else
		{
			// This save has never been resized. Leave the grid alone and let the
			// maintenance pass adopt it as the target.
			g_wantColumns.store(0);
			g_wantRows.store(0);
			g_pendingResize.store(false);
		}

		// A new session means new widgets, so the size captured from the old ones
		// is no longer something we know to be unscaled.
		g_baseSlotWidth  = 0.0f;
		g_baseSlotHeight = 0.0f;
		g_container      = nullptr;
		g_containerIndex = -1;

		// Item stack sizes.
		g_stackMultiplier.store(SessionConfig::Get("playerInventory.stacks.multiplier", kStackMultDefault));

		{
			std::unordered_map<std::string, int> loaded;
			const nlohmann::json arr = SessionConfig::Get("playerInventory.stacks.overrides", nlohmann::json::array());
			if (arr.is_array())
			{
				for (const auto& e : arr)
				{
					const std::string uniqueName = e.value("uniqueName", std::string());
					if (uniqueName.empty() || !e.contains("value") || !e["value"].is_number())
						continue;
					loaded[uniqueName] = e["value"].get<int>();
				}
			}
			std::lock_guard<std::mutex> lock(g_overrideMutex);
			g_stackOverrides = std::move(loaded);
		}

		{
			std::unordered_map<int, float> loaded;
			const nlohmann::json arr = SessionConfig::Get("playerInventory.stacks.categoryOverrides", nlohmann::json::array());
			if (arr.is_array())
			{
				for (const auto& e : arr)
				{
					const std::string label = e.value("category", std::string());
					SDK::EUIItemType  cat{};
					if (label.empty() || !e.contains("value") || !e["value"].is_number() || !CategoryFromLabel(label, cat))
						continue;
					loaded[static_cast<int>(cat)] = e["value"].get<float>();
				}
			}
			std::lock_guard<std::mutex> lock(g_categoryMutex);
			g_categoryOverrides = std::move(loaded);
		}

		// A new session's item CDOs are the same static assets, but a fresh
		// scan is the only way to know their true original MaxStack before
		// this session's Tick() writes anything -- ApplyStackSizes() will
		// RestoreIfStale-recover from a leftover write left by an unclean
		// reload of a PREVIOUS session, but never from a genuine session
		// change, which is exactly what just happened.
		g_stackItemsGameThread.clear();
		g_stackComposed.clear();
		RequestRefreshStackList();
	}

	void RenderImGui(IModLoaderImGui* imgui)
	{
		InventorySnapshot snap;
		{
			std::lock_guard<std::mutex> lock(g_snapshotMutex);
			snap = g_snapshot;
		}

		imgui->SeparatorText("Inventory Grid");

		if (!snap.inventoryFound)
		{
			imgui->TextDisabled("Player inventory not found.");
			return;
		}

		char buffer[192];

		if (imgui->BeginTable("##inventory_grid_table", 2, kTableFlags))
		{
			imgui->TableSetupColumn("Property", kColumnFixed, 150.0f);
			imgui->TableSetupColumn("Value",    0,            0.0f);

			imgui->TableNextRow(0, 0.0f);
			imgui->TableSetColumnIndex(0); imgui->Text("Current grid");
			imgui->TableSetColumnIndex(1);
			snprintf(buffer, sizeof(buffer), "%d x %d  (%d slots)", snap.columns, snap.rows, snap.slots);
			imgui->Text(buffer);

			imgui->TableNextRow(0, 0.0f);
			imgui->TableSetColumnIndex(0); imgui->Text("Reference grid");
			imgui->TableSetColumnIndex(1);
			snprintf(buffer, sizeof(buffer), "%d x %d  (100%% slot size)", kMinGridColumns, kMinGridRows);
			imgui->Text(buffer);

			imgui->TableNextRow(0, 0.0f);
			imgui->TableSetColumnIndex(0); imgui->Text("Slot size");
			imgui->TableSetColumnIndex(1);
			if (snap.widgetFound)
			{
				snprintf(buffer, sizeof(buffer), "%.0f%%", snap.slotScale * 100.0f);
				imgui->Text(buffer);
			}
			else
			{
				imgui->TextDisabled("Open the inventory once to scale it.");
			}

			imgui->EndTable();
		}

		imgui->Spacing();

		// Seeded from the wanted values, which the console command also writes,
		// so the steppers always show the grid that was last asked for.
		static int columns     = kMinGridColumns;
		static int rows        = kMinGridRows;
		static int seenColumns = -1;
		static int seenRows    = -1;

		const int loadedColumns = g_wantColumns.load();
		const int loadedRows    = g_wantRows.load();

		const int wantColumns = loadedColumns > 0 ? loadedColumns : snap.columns;
		const int wantRows    = loadedRows    > 0 ? loadedRows    : snap.rows;

		if (wantColumns != seenColumns) { columns = wantColumns; seenColumns = wantColumns; }
		if (wantRows    != seenRows)    { rows    = wantRows;    seenRows    = wantRows; }

		const bool columnsChanged =
			RenderStepper(imgui, "grid_columns", "Columns", &columns, kMinGridColumns, kMaxGridColumns);
		const bool rowsChanged =
			RenderStepper(imgui, "grid_rows", "Rows", &rows, kMinGridRows, kMaxGridRows);

		if (columnsChanged || rowsChanged)
		{
			g_wantColumns.store(columns);
			g_wantRows.store(rows);
			g_pendingResize.store(true);

			// Match what the panel is already showing, or the next frame's seed
			// would snap the fields back to the last value the game reported.
			seenColumns = columns;
			seenRows    = rows;

			SessionConfig::Set("playerInventory.columns", columns);
			SessionConfig::Set("playerInventory.rows", rows);
		}

		imgui->Spacing();

		bool fit = g_wantFitToPanel.load();
		if (imgui->Checkbox("Shrink slots to fit the inventory frame", &fit))
		{
			g_wantFitToPanel.store(fit);
			SessionConfig::Set("playerInventory.fitToPanel", fit);
		}

		imgui->Spacing();

		const float preview = fit ? SolveSlotScale(columns, rows) : 1.0f;

		snprintf(buffer, sizeof(buffer), "%d x %d = %d slots, drawn at %.0f%% slot size.  Minimum %d x %d.",
			columns, rows, columns * rows, preview * 100.0f, kMinGridColumns, kMinGridRows);
		imgui->TextDisabled(buffer);

		imgui->Spacing();
		imgui->TextWrapped("The inventory frame does not scroll, so a taller grid runs off the bottom "
			"of it. Slot size is measured against the 8 x 8 minimum: 8 x 8 draws at 100%, 16 x 16 at "
			"50%, 20 x 20 at 40%. Slots stop shrinking there -- past 20 x 20 the grid overflows "
			"rather than becoming unreadable. Slots stay square, so a wide, short grid leaves space "
			"below it instead of stretching.");

		imgui->Spacing();
		imgui->TextColored(1.0f, 0.3f, 0.3f, 1.0f,
			"The game will not shrink the grid below the slots you are already using. "
			"Empty it out first if a smaller grid is refused.");

		imgui->Spacing();
		imgui->TextDisabled("Console: bc_invsize <columns> <rows>");

		RenderItemStackSizes(imgui);
	}
}
