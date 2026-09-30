#pragma once

#include <atomic>

#include "plugin_interface.h"
#include "plugin_helpers.h"

namespace BetterCheats
{
	enum class MenuCategory
	{
		// WORLD
		World_Environment = 0,
		World_Corporations,

		// PLAYER
		Player_Self,
		Player_ItemSpawner,
		Player_Inventory,
		Player_Weapon,
		Player_Movement,
		Player_Teleport,
		Player_Building,
		Player_Skills,
		Player_Tools,

		// ENEMIES
		Enemies_Enemies,

		// MACHINERY
		Machinery_Crafters,
		Machinery_Power,
		Machinery_LogisticDrones,
		Machinery_RailDrones,

		// MISC
		Misc,

#if BETTERCHEATS_DEV_BUILD
		// DEV — the game's own cheat manager, debug builds only
		Dev_CheatManager,
#endif

		COUNT
	};

	class CheatMenu
	{
	public:
		static void Initialize(IPluginSelf* self);
		static void Shutdown();

		// The deliberate user action (the toggle key): flips which of
		// OpenMenu/CloseMenu applies. Every OTHER close source (Escape/Q, the
		// loader's own OnPanelClosed notification, shutdown) must call
		// CloseMenu directly, never this -- see OpenMenu/CloseMenu below.
		static void Toggle();
		static bool IsOpen() { return s_open; }

		// Escape/Q keybind callback flags a close; TickPendingClose consumes it
		// on the next game tick (OnEngineTick), not from the render callback --
		// see TickPendingClose's definition for why.
		static void RequestClose();

		// Applies a pending Escape/Q close request. Must be called from the
		// game tick, never from OnRender -- see its definition for why.
		static void TickPendingClose();

	private:
		static void OpenMenu();
		static void CloseMenu();

		// Applies "closed" to this plugin's own state (s_open false, capture
		// token released, any in-progress rebind capture cancelled)
		// idempotently. Every close path funnels through this, never a raw
		// flip, so a second close signal for an already-closed menu is always
		// a no-op instead of reopening it.
		static void ApplyMenuClosed(const char* reason);

		static void OnPanelClosed(PanelHandle handle);
		static void OnRender(IModLoaderImGui* imgui);
		static void RenderSidebar(IModLoaderImGui* imgui, float navWidth);
		static void RenderContent(IModLoaderImGui* imgui);
		static void RenderUnavailableMessage(IModLoaderImGui* imgui, float avail_x, float avail_y, const char* message);
		static void NavItem(IModLoaderImGui* imgui, const char* label, MenuCategory cat, float navWidth);

		static IPluginSelf*      s_self;
		static PanelHandle       s_panelHandle;
		static std::atomic<bool> s_open;   // toggled from keybind callbacks, read on the render thread
		static MenuCategory      s_activeCategory;
		static std::atomic<bool> s_closeRequested;
	};
}
