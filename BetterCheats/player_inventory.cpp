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
#include "object_ref.h"
#include "aob_resolver.h"

#include "Chimera_classes.hpp"
#include "ChimeraUI_classes.hpp"
#include "WBP_InventorySlot_classes.hpp"
#include "AuItems_classes.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// The grid itself is one UFUNCTION call; the slot widgets are plain UMG field
// writes, made from a detour on UCrUW_InventoryContainer::InitInventorySlots so
// they land the moment the game builds the slots — on the inventory screen, on
// every machine screen (each one opens the same player inventory widget), and
// after every in-game resize.
//
// Two halves that have to agree. The inventory frame has no scroll box anywhere
// in it — the whole WBP_Inventory tree is GridPanel, Border and SizeBox — so a
// grid with more rows than the frame was authored for simply runs off the
// bottom of the pane. Widening instead of lengthening keeps the shape, and
// shrinking every slot widget by the same factor keeps the footprint. Do one
// without the other and the grid overflows sideways instead of downwards.
//
// Everything that touches a UObject runs on the game thread — Tick(), the
// detour, or the console handler, which is registered with gameThread = true. RenderImGui()
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
		// The player-inventory containers seen so far. Game thread only.
		//
		// Each one is handed to us by the InitInventorySlots detour, which is
		// what scales it. The list exists so a change made from the panel (the
		// fit toggle) reaches containers that are already built, without waiting
		// for them to be opened again. The main inventory and the machine
		// screens are separate widget instances, so there is more than one.
		// ObjectRef, because a widget is freed with its screen.
		// ---------------------------------------------------------------------
		std::vector<ObjectRef<SDK::UCrUW_InventoryContainer>> g_containers;

		void TrackContainer(SDK::UCrUW_InventoryContainer* container)
		{
			bool known = false;
			g_containers.erase(std::remove_if(g_containers.begin(), g_containers.end(),
				[&](const ObjectRef<SDK::UCrUW_InventoryContainer>& ref)
				{
					SDK::UCrUW_InventoryContainer* live = ref.Get();
					if (live == container) known = true;
					return live == nullptr;
				}), g_containers.end());

			if (!known)
				g_containers.emplace_back(container);
		}

		bool AnyContainerLive()
		{
			for (const auto& ref : g_containers)
				if (ref.Get()) return true;
			return false;
		}

		// UCrUW_InventoryContainer::InventoryComponent is a private
		// TWeakObjectPtr<UCrInventoryComponent> the SDK dumps as padding —
		// InitInventorySlots opens on `lea r12, [rcx+468h]` to read it. Only the
		// index is compared, never resolved: InitPlayer can bind the character's
		// hidden inventory instead, and that grid is not the one being resized.
		constexpr size_t kContainerInventoryOffset = 0x468;

		bool IsBoundTo(const SDK::UCrUW_InventoryContainer* container, const SDK::UCrInventoryComponent* inv)
		{
			const auto* weak = reinterpret_cast<const SDK::FWeakObjectPtr*>(
				reinterpret_cast<const uint8_t*>(container) + kContainerInventoryOffset);
			return weak->ObjectIndex == inv->Index;
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
		//
		// SetSlotSize is a Blueprint function, not a native one: it is freed with
		// its class when the widget Blueprint unloads (back to the main menu, for
		// one), and a reload makes a new one. So the cached function is an
		// ObjectRef, looked up again once it's gone.
		void CallSetSlotSize(SDK::UWBP_InventorySlot_C* slot, float width, float height)
		{
			static ObjectRef<SDK::UFunction> s_function;

			SDK::UFunction* function = s_function.Get();
			if (!function)
			{
				if (!slot->Class) return;
				function = slot->Class->GetFunction("WBP_InventorySlot_C", "SetSlotSize");
				if (!function) return;
				s_function.Set(function);
			}

			SetSlotSizeParams params{};
			params.InSize = SDK::FVector2D(static_cast<double>(width), static_cast<double>(height));

			slot->ProcessEvent(function, &params);
		}

		// Returns the scale actually in force, or 0 when there was nothing to
		// apply it to.
		float ApplySlotScale(SDK::UCrUW_InventoryContainer* container, float scale)
		{
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
				// change anything — a reopened screen may reuse slots already sized.
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
		// Zero means "not decided yet": no saved grid for this session, so the
		// grid is left as the game built it rather than forcing a minimum onto a
		// save that never asked for one.
		std::atomic<int>  g_wantColumns{ 0 };
		std::atomic<int>  g_wantRows{ 0 };
		std::atomic<bool> g_wantFitToPanel{ true };
		std::atomic<bool> g_pendingResize{ false };

		// Set by the fit toggle: the slots already built need resizing without
		// the game rebuilding them.
		std::atomic<bool> g_rescaleBuilt{ false };

		// ---------------------------------------------------------------------
		// Applied state — game thread only.
		// ---------------------------------------------------------------------
		float g_appliedSlotScale = 1.0f;

		float WantedSlotScale(const SDK::UCrInventoryComponent* inv)
		{
			// Scaled against the grid the game actually built, not the one that
			// was asked for — a refused resize should not shrink the slots.
			return g_wantFitToPanel.load() ? SolveSlotScale(inv->GridColumns, inv->GridRows) : 1.0f;
		}

		// Game thread, straight after the game has built a container's slots.
		void OnSlotsBuilt(SDK::UCrUW_InventoryContainer* container)
		{
			if (!container || !container->ItemGridPanel)
				return;

			SDK::UCrInventoryComponent* inv = GetLocalInventory();
			if (!inv || !IsBoundTo(container, inv))
				return;

			TrackContainer(container);

			// Freshly built slots are already at their designer size.
			const float scale = WantedSlotScale(inv);
			if (scale == 1.0f && g_baseSlotWidth <= 0.0f)
				return;

			const float applied = ApplySlotScale(container, scale);
			if (applied > 0.0f)
				g_appliedSlotScale = applied;
		}

		void RescaleBuiltContainers()
		{
			if (!g_rescaleBuilt.exchange(false))
				return;

			// No pawn, no grid to measure. Nothing is lost: the detour scales
			// every container again as it is built.
			SDK::UCrInventoryComponent* inv = GetLocalInventory();
			if (!inv)
				return;

			const float scale = WantedSlotScale(inv);
			for (const auto& ref : g_containers)
			{
				const float applied = ApplySlotScale(ref.Get(), scale);
				if (applied > 0.0f)
					g_appliedSlotScale = applied;
			}
		}

		// ---------------------------------------------------------------------
		// UCrUW_InventoryContainer::InitInventorySlots detour.
		// ---------------------------------------------------------------------
		using InitInventorySlotsFn = void(__fastcall*)(SDK::UCrUW_InventoryContainer* self);

		InitInventorySlotsFn g_originalInitInventorySlots = nullptr;
		HookHandle           g_hookInitInventorySlots     = nullptr;

		void __fastcall Detour_InitInventorySlots(SDK::UCrUW_InventoryContainer* self)
		{
			g_originalInitInventorySlots(self);

			try { OnSlotsBuilt(self); }
			catch (...) { LOG_ERROR("Inventory: exception while scaling the inventory slots."); }
		}

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

				if (!ResizeGrid(inv, columns, rows))
					return;

				// On a host the resize has already run; on a client it is still
				// in flight, so a mismatch there means nothing yet.
				SDK::AActor* owner = inv->GetOwner();
				if (owner && owner->HasAuthority() && (inv->GridColumns != columns || inv->GridRows != rows))
				{
					LOG_WARN("Inventory: the game would not resize the grid to %d x %d "
						"(currently %d x %d, %d slots) — it will not shrink below the "
						"slots that are in use.",
						columns, rows, inv->GridColumns, inv->GridRows, inv->Slots.Num());
					return;
				}

				LOG_INFO("Inventory: grid set to %d x %d (%d slots).", columns, rows, columns * rows);
			}
			catch (...)
			{
				g_pendingResize.store(false);
				LOG_ERROR("Inventory: exception while resizing the inventory.");
			}
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
			g_containers.clear();
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

				snap.widgetFound = AnyContainerLive();
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

		// `item` is an ObjectRef, checked before every read or write: the scan
		// force-loads each item's package and nothing keeps it loaded, so a
		// garbage collection (a world travel always runs one) can free the CDO
		// while this list still holds it.
		struct StackEntry
		{
			ObjectRef<SDK::UAuItemDataBase> item;
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

		// Every tier sets an absolute stack size, and a size only ever RAISES
		// an item: one whose own MaxStack is already higher keeps it, so no
		// setting can shrink a cap below what a player is already holding.
		// That makes 1 the global "off" value. MaxStack is a plain int32
		// with no engine-side limit; 9999 keeps the slider usable and the
		// count readable in an inventory slot.
		constexpr int kStackFloor   = 1;
		constexpr int kStackCeiling = 9999;
		constexpr int kStackSizeOff = kStackFloor;

		std::atomic<int> g_stackSize{ kStackSizeOff };

		// Per-item override, keyed by the item's true UniqueItemName (an exact
		// in-memory key, distinct from the sanitized string used for
		// SessionConfig paths below). Absent = no override, follow the category
		// and global tiers. Guarded because RenderImGui (render thread) writes it and
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
			ObjectRef<SDK::UAuItemDataBase> item;
		};
		std::unordered_map<std::string, StackComposeState> g_stackComposed;

		// Each item's true MaxStack, keyed by UniqueItemName, game-thread only.
		// Captured once per plugin lifetime and never dropped, so a rescan or a
		// new session can never recapture one of our own writes as the base
		// (which would compound 3x into 9x).
		std::unordered_map<std::string, int> g_stackOriginals;

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

		std::string StackComposeKey(const std::string& uniqueName)
		{
			return "playerInventory.stacks." + SanitizeStackKey(uniqueName);
		}

		// Game thread only. The base of a count we scale, from the value it
		// holds now: a previous plugin instance unloaded by RELOAD (Shutdown on
		// the render thread) may have left its write in place, and
		// RestoreIfStale maps that back to the base it saved. Returns false
		// while no session config is loaded, since the saved pair can't be read
		// yet and a leftover would be taken for the base.
		bool RecoverOriginal(const std::string& key, int current, int& original)
		{
			if (!SessionConfig::IsLoaded())
				return false;

			float value = static_cast<float>(current);
			BetterCheats::RestoreIfStale(key, value);
			original = static_cast<int>(value + 0.5f);
			return true;
		}

		int ClampStackValue(int value)
		{
			if (value < kStackFloor)   return kStackFloor;
			if (value > kStackCeiling) return kStackCeiling;
			return value;
		}

		// What a tier's size means for one item: the size, but never below the
		// item's own original.
		int RaiseOnly(int originalMaxStack, int size)
		{
			return (std::max)(originalMaxStack, ClampStackValue(size));
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

		// The category tier's size for one item, or the global one while the
		// category has no override.
		int CategorySizeFor(const StackEntry& e, int globalSize,
			const std::unordered_map<int, int>& categoryOverrides)
		{
			auto catIt = categoryOverrides.find(static_cast<int>(e.category));
			return (catIt != categoryOverrides.end()) ? catIt->second : globalSize;
		}

		// Per-item override, keyed by the item's true UniqueItemName -- the
		// innermost, most-specific tier. Absent = no item-level override,
		// inherit the category tier below.
		int EffectiveStackFor(const StackEntry& e, int globalSize,
			const std::unordered_map<int, int>& categoryOverrides,
			const std::unordered_map<std::string, int>& itemOverrides)
		{
			auto itemIt = itemOverrides.find(e.uniqueName);
			const int size = (itemIt != itemOverrides.end()) ? itemIt->second
			                                                 : CategorySizeFor(e, globalSize, categoryOverrides);
			return RaiseOnly(e.originalMaxStack, size);
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
		std::unordered_map<int, int> g_categoryOverrides;   // key: static_cast<int>(EUIItemType)

		void PersistCategoryOverrides()
		{
			std::unordered_map<int, int> snapshot;
			{
				std::lock_guard<std::mutex> lock(g_categoryMutex);
				snapshot = g_categoryOverrides;
			}

			nlohmann::json arr = nlohmann::json::array();
			for (const auto& kv : snapshot)
				arr.push_back({ {"category", CategoryLabel(static_cast<SDK::EUIItemType>(kv.first))}, {"value", kv.second} });
			SessionConfig::Set("playerInventory.stacks.categorySizes", arr);
		}

		void SetCategoryOverride(SDK::EUIItemType category, int value)
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
			const int         globalSize = g_stackSize.load();

			std::unordered_map<std::string, int> overrides;
			{
				std::lock_guard<std::mutex> lock(g_overrideMutex);
				overrides = g_stackOverrides;
			}
			std::unordered_map<int, int> categoryOverrides;
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
					const int effective = EffectiveStackFor(e, globalSize, categoryOverrides, overrides);
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
						entry.item.Set(item);
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

						auto known = g_stackOriginals.find(entry.uniqueName);
						if (known != g_stackOriginals.end())
						{
							entry.originalMaxStack = known->second;
						}
						else
						{
							// Before the session config loads nothing is scaled
							// (every size and override is still a default),
							// so the current value serves until the rescan that
							// ApplySavedConfig requests captures the real one.
							int original = item->MaxStack;
							const bool recovered = RecoverOriginal(StackComposeKey(entry.uniqueName), item->MaxStack, original);
							entry.originalMaxStack = original > 0 ? original : 1;
							if (recovered)
								g_stackOriginals[entry.uniqueName] = entry.originalMaxStack;
						}
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
		// effective value (the most specific tier's size, never below the
		// captured original) differs from that original -- never touches
		// DoNotStack items at all. Cheap: this walks the already-resolved list,
		// no asset-registry work here, so running it every tick is fine.
		void ApplyStackSizes()
		{
			if (g_stackItemsGameThread.empty())
				return;

			const int globalSize = g_stackSize.load();
			std::unordered_map<std::string, int> overrides;
			{
				std::lock_guard<std::mutex> lock(g_overrideMutex);
				overrides = g_stackOverrides;
			}
			std::unordered_map<int, int> categoryOverrides;
			{
				std::lock_guard<std::mutex> lock(g_categoryMutex);
				categoryOverrides = g_categoryOverrides;
			}

			try
			{
				for (const StackEntry& e : g_stackItemsGameThread)
				{
					if (!e.canStack)
						continue;

					StackComposeState& state = g_stackComposed[e.uniqueName];

					// Unloaded since the scan. Whatever we wrote went with it, so
					// there is nothing to hand back either.
					SDK::UAuItemDataBase* item = e.item.Get();
					if (!item)
					{
						state.composed.Forget();
						state.item.Reset();
						continue;
					}

					const int  desired = EffectiveStackFor(e, globalSize, categoryOverrides, overrides);
					const bool active  = desired != e.originalMaxStack;

					state.item.Set(item);

					float valueF = static_cast<float>(item->MaxStack);
					const std::string key = StackComposeKey(e.uniqueName);

					BetterCheats::ApplyComposedRow(state.composed, item, valueF, key, active,
						static_cast<float>(desired), BetterCheats::ComposedAttribute::Mode::Absolute,
						static_cast<float>(kStackFloor), static_cast<float>(kStackCeiling));

					const int newValue = static_cast<int>(valueF + 0.5f);
					if (item->MaxStack != newValue)
						item->MaxStack = newValue;
				}
			}
			catch (...) {}
		}

		// Game thread only. Hands every live item its original MaxStack back
		// and drops the persisted compose state.
		void RestoreStackSizes()
		{
			for (auto& kv : g_stackComposed)
			{
				StackComposeState& state = kv.second;
				SDK::UAuItemDataBase* item = state.item.Get();
				if (!item)
				{
					state.composed.Forget();
					continue;
				}

				float valueF = static_cast<float>(item->MaxStack);
				state.composed.Release(item, valueF);
				auto known = g_stackOriginals.find(kv.first);
				const int newValue = known != g_stackOriginals.end() ? known->second : static_cast<int>(valueF + 0.5f);
				if (item->MaxStack != newValue)
					item->MaxStack = newValue;

				BetterCheats::ClearComposeState(StackComposeKey(kv.first));
			}
		}

		// Render thread. Global size row, category rows, search + "only show changed"
		// filter, then a scrolled table of every filtered item -- follows the
		// same shared-row-helper shape as player_weapons.cpp's tables, and the
		// same scrolled-list-with-a-search-box shape as player_items.cpp's Item
		// Spawner. g_filteredStackIndices is only ever rebuilt on an actual
		// input change (search text, filter toggle, a size, an override),
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
			imgui->TextDisabled("A size only raises a stack: an item that already stacks higher\n"
			                    "keeps its own size.");
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
			// group covers all three tiers in one save/load: the global size,
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
					categoryKeys[i] = std::string("categorySize:") + CategoryLabel(g_stackCategories[i]);

				std::vector<std::string> itemKeys(g_stackItems.size());
				for (size_t i = 0; i < g_stackItems.size(); ++i)
					itemKeys[i] = "item:" + g_stackItems[i].uniqueName;

				const int fieldCount = 1 + static_cast<int>(g_stackCategories.size()) + static_cast<int>(g_stackItems.size());
				std::vector<BetterCheats::PresetStore::Field> fields(fieldCount);

				auto getLive = [&](BetterCheats::PresetStore::Field* out)
				{
					std::unordered_map<int, int> categoryOverrides;
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
					out[idx++] = { "size", static_cast<float>(g_stackSize.load()) };

					for (size_t i = 0; i < g_stackCategories.size(); ++i)
					{
						const auto it = categoryOverrides.find(static_cast<int>(g_stackCategories[i]));
						out[idx++] = { categoryKeys[i].c_str(), it != categoryOverrides.end() ? static_cast<float>(it->second) : -1.0f };
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
						const int size = ClampStackValue(static_cast<int>(f[idx].value + 0.5f));
						g_stackSize.store(size);
						SessionConfig::Set("playerInventory.stacks.globalSize", size);
					}
					++idx;

					for (SDK::EUIItemType cat : g_stackCategories)
					{
						if (idx >= count) break;
						const float v = f[idx].value;
						if (v < 0.0f) ClearCategoryOverride(cat);
						else          SetCategoryOverride(cat, ClampStackValue(static_cast<int>(v + 0.5f)));
						++idx;
					}
					for (const StackEntry& e : g_stackItems)
					{
						if (idx >= count) break;
						const float v = f[idx].value;
						if (v < 0.0f) ClearStackOverride(e.uniqueName);
						else          SetStackOverride(e.uniqueName, ClampStackValue(static_cast<int>(v + 0.5f)));
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

			// ---- global size -----------------------------------------------------
			int globalSize = g_stackSize.load();
			if (imgui->BeginTable("##stack_size_table", 3, kTableFlags))
			{
				const float sizeLabelReserve = BetterCheats::UI::PrescanLabelWidth(imgui, 1,
					[](int) { return "Stack size"; });

				imgui->TableSetupColumn("Attribute", kColumnFixed,
					BetterCheats::UI::GetReadoutColumnWidth(imgui, sizeLabelReserve));
				imgui->TableSetupColumn("Value", 0, 0.54f);
				imgui->TableSetupColumn("",      0, 0.10f);

				imgui->TableNextRow(0, 0.0f);

				float sizeValue = static_cast<float>(globalSize);

				BetterCheats::UI::RowSpec spec;
				spec.label        = "Stack size";
				spec.tooltip      = "Every stackable item stacks to at least this many.\n"
				                    "Items that already stack higher keep their own size; 1 = off.\n"
				                    "A category or per-item size below replaces this.";
				spec.value        = &sizeValue;
				spec.minValue     = static_cast<float>(kStackFloor);
				spec.maxValue     = static_cast<float>(kStackCeiling);
				spec.step         = 1.0f;
				spec.format       = "%.0f";
				spec.wholeNumbers = true;
				spec.resetValue   = static_cast<float>(kStackSizeOff);
				spec.active       = globalSize != kStackSizeOff;
				spec.labelReserve = sizeLabelReserve;

				if (BetterCheats::UI::BuildRow(imgui, spec).changed)
				{
					globalSize = static_cast<int>(sizeValue);
					g_stackSize.store(globalSize);
					SessionConfig::Set("playerInventory.stacks.globalSize", globalSize);
					RefreshFilteredStackItems();
				}

				imgui->EndTable();
			}

			imgui->Spacing();

			// ---- category tier --------------------------------------------------
			// One row per category actually present in the scan (EUIItemType --
			// the game's own item-type field, see the StackEntry::category
			// comment). Inherits the global size until explicitly
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

				std::unordered_map<int, int> categorySnapshot;
				{
					std::lock_guard<std::mutex> lock(g_categoryMutex);
					categorySnapshot = g_categoryOverrides;
				}

				for (SDK::EUIItemType cat : g_stackCategories)
				{
					const auto  catIt        = categorySnapshot.find(static_cast<int>(cat));
					const bool  hasOverride  = catIt != categorySnapshot.end();
					float       catValue     = static_cast<float>(hasOverride ? catIt->second : globalSize);

					int itemCount = 0;
					for (const StackEntry& e : g_stackItems)
						if (e.category == cat) ++itemCount;

					imgui->PushIDStr(CategoryLabel(cat));
					imgui->TableNextRow(0, 0.0f);

					char tooltip[192];
					snprintf(tooltip, sizeof(tooltip),
						"%d item%s. %s", itemCount, itemCount == 1 ? "" : "s",
						hasOverride ? "Overrides the global stack size for this category."
						            : "Inherits the global stack size until you change this row.");

					BetterCheats::UI::RowSpec spec;
					spec.label        = CategoryLabel(cat);
					spec.tooltip      = tooltip;
					spec.value        = &catValue;
					spec.minValue     = static_cast<float>(kStackFloor);
					spec.maxValue     = static_cast<float>(kStackCeiling);
					spec.step         = 1.0f;
					spec.format       = "%.0f";
					spec.wholeNumbers = true;
					spec.resetValue   = static_cast<float>(globalSize);   // reset = "go back to inheriting global"
					spec.active       = hasOverride;
					spec.labelReserve = catLabelReserve;

					const BetterCheats::UI::RowResult result = BetterCheats::UI::BuildRow(imgui, spec);
					if (result.changed)
					{
						const int size = static_cast<int>(catValue);
						if (result.resetClicked || size == globalSize)
							ClearCategoryOverride(cat);
						else
							SetCategoryOverride(cat, size);
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
			std::unordered_map<int, int> categoryOverrides;
			{
				std::lock_guard<std::mutex> lock(g_categoryMutex);
				categoryOverrides = g_categoryOverrides;
			}
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
						const bool  categoryOverridden = categoryOverrides.count(static_cast<int>(e.category)) != 0;
						const int   inherited          = RaiseOnly(e.originalMaxStack,
							CategorySizeFor(e, globalSize, categoryOverrides));

						const auto  overrideIt   = overrides.find(e.uniqueName);
						const bool  hasOverride  = overrideIt != overrides.end();
						float       rowValue     = static_cast<float>(hasOverride
							? RaiseOnly(e.originalMaxStack, overrideIt->second) : inherited);

						imgui->PushIDStr(e.uniqueName.c_str());
						imgui->TableNextRow(0, 0.0f);

						char tooltip[384];
						int n = snprintf(tooltip, sizeof(tooltip), "Original: %d", e.originalMaxStack);
						n += snprintf(tooltip + n, sizeof(tooltip) - n, "\nGlobal: %d -> %d",
							globalSize, RaiseOnly(e.originalMaxStack, globalSize));
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
						spec.minValue     = static_cast<float>((std::min)(e.originalMaxStack, kStackCeiling));   // raise-only
						spec.maxValue     = static_cast<float>(kStackCeiling);
						spec.step         = 1.0f;
						spec.format       = "%.0f";
						spec.wholeNumbers = true;
						spec.resetValue   = static_cast<float>(inherited);
						spec.active       = e.canStack && hasOverride;
						spec.disableSlider = !e.canStack;
						spec.labelReserve = itemLabelReserve;

						const BetterCheats::UI::RowResult result = BetterCheats::UI::BuildRow(imgui, spec);
						if (result.changed && e.canStack)
						{
							const int newInt = static_cast<int>(rowValue);
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

		// =====================================================================
		// Plant Pickup (wild-plant gather multiplier)
		// =====================================================================
		// Scales ACrGatherableBaseActor::ResourceCount, the per-type count a
		// simple (non-crop) gatherable hands out, on each gatherable Blueprint
		// class's CDO -- the same kind of per-type field as MaxStack above. A
		// gatherable actor takes its ResourceCount from the CDO when it is
		// constructed. Crops (ACrGatherableCropActor) derive from ACrOreActor,
		// not this base, and are not covered.
		//
		// The classes are found in memory, not through the asset registry. A
		// Blueprint asset's registry class is Blueprint, not the actor class it
		// generates, so a registry query on the actor base finds nothing (the
		// item scan above only works because AuItemBlueprint is itself a
		// UBlueprint subclass). Walking loaded classes also means nothing here
		// force-loads a Blueprint the game isn't holding: a class only we had
		// loaded would be collected at the next GC and reloaded later with its
		// original count. The world loads the gatherable classes it spawns
		// from; one it loads later is picked up by the periodic rescan below.

		// `cdo` is an ObjectRef, checked before every read or write: a GC can
		// collect a class the world no longer needs. `original` is captured
		// once per plugin lifetime and entries are never dropped, so a rescan,
		// a new session, or a class the game reloaded can never recapture one
		// of our own writes as the base (which would compound 3x into 9x).
		struct GatherEntry
		{
			ObjectRef<SDK::ACrGatherableBaseActor> cdo;
			std::string                            key;           // SessionConfig path for the compose state
			int                                    original = 0;  // the Blueprint's own ResourceCount
			BetterCheats::ComposedAttribute        composed;
		};

		// Game thread only, keyed by the generated class name.
		std::unordered_map<std::string, GatherEntry> g_gather;

		// The UI reads these; only the game thread touches g_gather itself.
		std::atomic<int>  g_gatherTypeCount{ 0 };
		std::atomic<bool> g_gatherScanNow{ true };     // plugin start, new session
		std::atomic<bool> g_gatherPanelOpen{ false };  // keeps the rescan going while the panel shows

		// Capped at 5x. 1x is the floor -- this is a reward
		// multiplier, not a way to make plants worse. Whole steps only: a
		// fractional multiplier would round a one-item plant to a half.
		constexpr float kGatherMultDefault = 1.0f;
		constexpr float kGatherMultMin     = 1.0f;
		constexpr float kGatherMultMax     = 5.0f;

		// Sanity clamp on the multiplied result, same spirit as kStackCeiling
		// above -- ResourceCount is a plain int32 with no engine-side range
		// check of its own.
		constexpr int kGatherResourceCeiling = 1000000;

		// One full GObjects pass; enemies.cpp
		// walks GObjects every 0.25s, so this cadence is well inside budget.
		constexpr float kGatherRescanSeconds = 5.0f;
		float           g_gatherRescanTimer  = 0.0f;

		std::atomic<float> g_gatherMultiplier{ kGatherMultDefault };

		float ClampGatherMultiplier(float value)
		{
			if (!(value >= kGatherMultMin)) return kGatherMultMin;   // also catches NaN
			if (value > kGatherMultMax)     return kGatherMultMax;
			return std::round(value);
		}

		// Game thread only. Binds every loaded gatherable Blueprint class's
		// CDO, new or reloaded since the last pass.
		void ScanGatherables()
		{
			SDK::TUObjectArray* arr = SDK::UObject::GObjects.GetTypedPtr();
			SDK::UClass* generatedClass = SDK::UBlueprintGeneratedClass::StaticClass();
			SDK::UClass* gatherBase     = SDK::ACrGatherableBaseActor::StaticClass();
			if (!arr || !generatedClass || !gatherBase)
				return;

			int bound = 0;

			// Still being loaded (its CDO may not hold the Blueprint's value
			// yet), or on its way out.
			constexpr uint32_t kSkipFlags =
				static_cast<uint32_t>(SDK::EObjectFlags::NeedLoad) |
				static_cast<uint32_t>(SDK::EObjectFlags::NeedPostLoad) |
				static_cast<uint32_t>(SDK::EObjectFlags::NeedPostLoadSubobjects) |
				static_cast<uint32_t>(SDK::EObjectFlags::BeginDestroyed) |
				static_cast<uint32_t>(SDK::EObjectFlags::FinishDestroyed) |
				static_cast<uint32_t>(SDK::EObjectFlags::MirroredGarbage);

			for (int i = 0; i < arr->Num(); ++i)
			{
				// Pointer compare before any dereference: GObjects can hand back
				// half-registered entries whose Class is garbage (world_wave.cpp).
				SDK::UObject* obj = arr->GetByIndex(i);
				if (!obj || obj->Class != generatedClass)
					continue;

				SDK::UClass* cls = static_cast<SDK::UClass*>(obj);
				if (static_cast<uint32_t>(cls->Flags) & kSkipFlags)
					continue;
				if (!cls->IsSubclassOf(gatherBase))
					continue;

				SDK::UObject* cdoObj = cls->ClassDefaultObject;
				if (!cdoObj || (static_cast<uint32_t>(cdoObj->Flags) & kSkipFlags))
					continue;

				SDK::ACrGatherableBaseActor* cdo = static_cast<SDK::ACrGatherableBaseActor*>(cdoObj);
				const std::string name = cls->GetName();

				auto it = g_gather.find(name);
				if (it != g_gather.end())
				{
					GatherEntry& e = it->second;
					if (e.cdo.Get() == cdo)
						continue;

					// Reloaded since we bound it: a fresh CDO holding the
					// original again, so there is nothing of ours to hand back.
					e.composed.Forget();
					e.cdo.Set(cdo);
					++bound;
					continue;
				}

				// First sighting this plugin lifetime. Not bound until the
				// session config loads; ApplySavedConfig rescans then.
				const std::string key = "playerInventory.gather." + SanitizeStackKey(name);
				int original = 0;
				if (!RecoverOriginal(key, cdo->ResourceCount, original))
					continue;
				if (original <= 0)
					continue;   // no reward to scale

				GatherEntry& e = g_gather[name];
				e.cdo.Set(cdo);
				e.key      = key;
				e.original = original;
				++bound;
			}

			if (bound > 0)
				LOG_INFO("Inventory: bound %d gatherable type(s) for plant pickup (%d known).",
					bound, static_cast<int>(g_gather.size()));
			g_gatherTypeCount.store(static_cast<int>(g_gather.size()));
		}

		// Game thread only. Hands every live CDO its original back and drops
		// the persisted compose state.
		void RestoreGatherables()
		{
			for (auto& kv : g_gather)
			{
				GatherEntry& e = kv.second;
				e.composed.Forget();

				SDK::ACrGatherableBaseActor* cdo = e.cdo.Get();
				if (!cdo)
					continue;

				if (cdo->ResourceCount != e.original)
					cdo->ResourceCount = e.original;
				BetterCheats::ClearComposeState(e.key);
			}
		}

		// Game thread only. Rescans on start and on a new session, then every
		// few seconds while a multiplier is set or the panel is open. Writes
		// ResourceCount only when the value it should hold changes.
		void ApplyGatherMultiplier(float deltaSeconds)
		{
			const float multiplier = g_gatherMultiplier.load();

			g_gatherRescanTimer += deltaSeconds;
			const bool periodic = g_gatherRescanTimer >= kGatherRescanSeconds &&
				(multiplier != kGatherMultDefault || g_gatherPanelOpen.exchange(false));
			if (g_gatherScanNow.exchange(false) || periodic)
			{
				g_gatherRescanTimer = 0.0f;
				try { ScanGatherables(); }
				catch (...) { LOG_WARN("Inventory: exception scanning for gatherable classes."); }
			}

			try
			{
				for (auto& kv : g_gather)
				{
					GatherEntry& e = kv.second;

					SDK::ACrGatherableBaseActor* cdo = e.cdo.Get();
					if (!cdo)
					{
						e.composed.Forget();
						continue;
					}

					// Nearest whole item, never below the original (so never 0).
					int desired = static_cast<int>(static_cast<float>(e.original) * multiplier + 0.5f);
					if (desired < e.original)             desired = e.original;
					if (desired > kGatherResourceCeiling) desired = kGatherResourceCeiling;
					const bool active = desired != e.original;

					float valueF = static_cast<float>(cdo->ResourceCount);
					BetterCheats::ApplyComposedRow(e.composed, cdo, valueF, e.key, active,
						static_cast<float>(desired), BetterCheats::ComposedAttribute::Mode::Absolute,
						1.0f, static_cast<float>(kGatherResourceCeiling));

					const int newValue = static_cast<int>(valueF + 0.5f);
					if (cdo->ResourceCount != newValue)
						cdo->ResourceCount = newValue;
				}
			}
			catch (...) {}
		}

		// Render thread. Single global multiplier row, same BuildRow shape as
		// the stack-size rows.
		void RenderPlantPickup(IModLoaderImGui* imgui)
		{
			g_gatherPanelOpen.store(true);

			imgui->Spacing();
			imgui->Separator();
			imgui->SeparatorText("Plant Pickup");
			imgui->TextDisabled("Scales how many items a wild plant/mushroom/rock hands out when\n"
			                    "picked up. Crops (regrowing bushes) are not covered by this.");
			imgui->Spacing();

			float multiplier = g_gatherMultiplier.load();
			if (imgui->BeginTable("##gather_mult_table", 3, kTableFlags))
			{
				const float labelReserve = BetterCheats::UI::PrescanLabelWidth(imgui, 1,
					[](int) { return "Plant Pickup"; });

				imgui->TableSetupColumn("Attribute", kColumnFixed,
					BetterCheats::UI::GetReadoutColumnWidth(imgui, labelReserve));
				imgui->TableSetupColumn("Value", 0, 0.54f);
				imgui->TableSetupColumn("",      0, 0.10f);

				imgui->TableNextRow(0, 0.0f);

				const int typeCount = g_gatherTypeCount.load();
				char tooltip[192];
				snprintf(tooltip, sizeof(tooltip),
					"Multiplies every wild gatherable's original ResourceCount.\n"
					"%d gatherable type%s loaded. Capped at %.0fx.",
					typeCount, typeCount == 1 ? "" : "s", kGatherMultMax);

				BetterCheats::UI::RowSpec spec;
				spec.label        = "Plant Pickup";
				spec.tooltip      = tooltip;
				spec.value        = &multiplier;
				spec.minValue     = kGatherMultMin;
				spec.maxValue     = kGatherMultMax;
				spec.step         = 1.0f;
				spec.format       = "%.0fx";
				spec.wholeNumbers = true;
				spec.resetValue   = kGatherMultDefault;
				spec.active       = BetterCheats::DiffersFromDefault(multiplier, kGatherMultDefault);
				spec.labelReserve = labelReserve;

				// BuildRow rounds and clamps both the slider and the typed box.
				if (BetterCheats::UI::BuildRow(imgui, spec).changed)
				{
					g_gatherMultiplier.store(multiplier);
					SessionConfig::Set("playerInventory.gather.multiplier", static_cast<int>(multiplier));
				}

				imgui->EndTable();
			}
		}
	}

	void Initialize()
	{
		IPluginHookUtils* hookUtils = GetHooks() ? GetHooks()->Hooks : nullptr;
		const uintptr_t initSlotsAddr = AOB::Resolved().InventoryContainer_InitInventorySlots;

		if (!initSlotsAddr)
		{
			LOG_WARN("Inventory: InitInventorySlots unresolved - slots will not be scaled to fit.");
		}
		else if (!hookUtils)
		{
			LOG_WARN("Inventory: hook utils unavailable, InitInventorySlots hook skipped.");
		}
		else
		{
			g_hookInitInventorySlots = hookUtils->Install(
				initSlotsAddr,
				reinterpret_cast<void*>(&Detour_InitInventorySlots),
				reinterpret_cast<void**>(&g_originalInitInventorySlots));

			if (!g_hookInitInventorySlots)
				LOG_WARN("Inventory: failed to install InitInventorySlots hook.");
			else
				LOG_INFO("Inventory: InitInventorySlots hook installed.");
		}

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

		IPluginHookUtils* hookUtils = GetHooks() ? GetHooks()->Hooks : nullptr;
		if (hookUtils && g_hookInitInventorySlots)
		{
			hookUtils->Remove(g_hookInitInventorySlots);
			g_hookInitInventorySlots     = nullptr;
			g_originalInitInventorySlots = nullptr;
		}

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
			for (auto& kv : g_gather)
				kv.second.composed.Forget();
			return;
		}

		try { RestoreStackSizes(); }
		catch (...) {}

		// Plant Pickup: same render-thread-Shutdown hazard, same fix.
		try { RestoreGatherables(); }
		catch (...) {}
	}

	void Tick(float deltaSeconds)
	{
		BetterCheats::RecordGameThread();

		ApplyPendingResize();
		RescaleBuiltContainers();
		RefreshSnapshot();

		ApplyStackSizes();
		ApplyGatherMultiplier(deltaSeconds);
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
			// This save has never been resized. Leave the grid alone; the panel
			// shows whatever the game built.
			g_wantColumns.store(0);
			g_wantRows.store(0);
			g_pendingResize.store(false);
		}

		// A new session means new widgets, so the size captured from the old ones
		// is no longer something we know to be unscaled.
		g_baseSlotWidth  = 0.0f;
		g_baseSlotHeight = 0.0f;
		g_containers.clear();

		// Item stack sizes. Sizes replaced the old multipliers; a multiplier
		// has no single size it maps to, so the old keys are dropped, not
		// converted.
		SessionConfig::Remove("playerInventory.stacks.multiplier");
		SessionConfig::Remove("playerInventory.stacks.categoryOverrides");
		{
			const nlohmann::json saved = SessionConfig::Get("playerInventory.stacks.globalSize", kStackSizeOff);
			g_stackSize.store(ClampStackValue(saved.is_number() ? saved.get<int>() : kStackSizeOff));
		}

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
					loaded[uniqueName] = ClampStackValue(e["value"].get<int>());
				}
			}
			std::lock_guard<std::mutex> lock(g_overrideMutex);
			g_stackOverrides = std::move(loaded);
		}

		{
			std::unordered_map<int, int> loaded;
			const nlohmann::json arr = SessionConfig::Get("playerInventory.stacks.categorySizes", nlohmann::json::array());
			if (arr.is_array())
			{
				for (const auto& e : arr)
				{
					const std::string label = e.value("category", std::string());
					SDK::EUIItemType  cat{};
					if (label.empty() || !e.contains("value") || !e["value"].is_number() || !CategoryFromLabel(label, cat))
						continue;
					loaded[static_cast<int>(cat)] = ClampStackValue(e["value"].get<int>());
				}
			}
			std::lock_guard<std::mutex> lock(g_categoryMutex);
			g_categoryOverrides = std::move(loaded);
		}

		// Hand every item its original MaxStack back first, the same way as
		// Plant Pickup below, then rescan: the next tick captures from the true
		// base and persists into this session's config rather than the last
		// one's. Off the game thread (only before the first Tick, when nothing
		// has been written yet) dropping state is enough.
		if (BetterCheats::IsGameThread())
		{
			try { RestoreStackSizes(); }
			catch (...) {}
		}
		g_stackItemsGameThread.clear();
		g_stackComposed.clear();
		RequestRefreshStackList();

		// Plant Pickup. Hand every CDO its original back first, so the next
		// tick captures the true base and persists it into this session's
		// config rather than the last one's. This runs on the game thread;
		// IsGameThread() is only false before the first Tick, when nothing has
		// been written yet, so dropping state is enough there.
		{
			const nlohmann::json saved = SessionConfig::Get("playerInventory.gather.multiplier", kGatherMultDefault);
			g_gatherMultiplier.store(ClampGatherMultiplier(saved.is_number() ? saved.get<float>() : kGatherMultDefault));
		}
		if (BetterCheats::IsGameThread())
		{
			try { RestoreGatherables(); }
			catch (...) {}
		}
		else
		{
			for (auto& kv : g_gather)
				kv.second.composed.Forget();
		}
		g_gatherScanNow.store(true);
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
			g_rescaleBuilt.store(true);
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

		// Plant Pickup first: the stack list below fills the rest of the panel.
		RenderPlantPickup(imgui);
		RenderItemStackSizes(imgui);
	}
}
