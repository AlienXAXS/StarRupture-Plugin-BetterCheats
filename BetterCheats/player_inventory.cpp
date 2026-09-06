#include "player_inventory.h"
#include "plugin_helpers.h"
#include "game_context.h"
#include "session_config.h"

#include "Chimera_classes.hpp"
#include "ChimeraUI_classes.hpp"
#include "WBP_InventorySlot_classes.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>

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
		// Lookups. The cast is class-checked: the local pawn is replicated, and
		// during a level transition or a multiplayer join it is briefly some
		// other class — reading InventoryComponent off the wrong object reads
		// past the end of it.
		// ---------------------------------------------------------------------
		SDK::UWorld* GetWorldSafe()
		{
			try { return SDK::UWorld::GetWorld(); }
			catch (...) { return nullptr; }
		}

		SDK::ACrCharacterPlayerBase* GetLocalCharacter()
		{
			SDK::UWorld* world = GetWorldSafe();
			if (!world) return nullptr;

			SDK::APlayerController* pc = SDK::UGameplayStatics::GetPlayerController(world, 0);
			if (!pc || !pc->Pawn) return nullptr;

			SDK::UClass* characterClass = SDK::ACrCharacterPlayerBase::StaticClass();
			if (!characterClass || !pc->Pawn->IsA(characterClass)) return nullptr;

			return static_cast<SDK::ACrCharacterPlayerBase*>(pc->Pawn);
		}

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
	}

	void Tick(float deltaSeconds)
	{
		ApplyPendingResize();
		MaintainGrid(deltaSeconds);
		RefreshSnapshot();
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
	}
}
