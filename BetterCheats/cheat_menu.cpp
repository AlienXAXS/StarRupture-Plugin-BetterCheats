#include "cheat_menu.h"
#include "game_context.h"
#include "plugin_helpers.h"
#include "panel_world.h"
#include "panel_player.h"
#include "panel_machines.h"
#include "panel_enemies.h"
#include "panel_misc.h"
#include "panel_dev.h"
#include "keybind_picker.h"
#include "ui_widgets.h"

// ---------------------------------------------------------------------------
// ImGui style constant aliases — mirror imgui.h, must match modloader's ImGui
// ---------------------------------------------------------------------------
namespace
{
	// ImGuiCol
	constexpr int Col_Text          = 0;
	constexpr int Col_ChildBg       = 3;
	constexpr int Col_Header        = 24;
	constexpr int Col_HeaderHovered = 25;
	constexpr int Col_HeaderActive  = 26;

	// ImGuiStyleVar
	constexpr int Var_WindowPadding = 2;  // vec2
	constexpr int Var_ItemSpacing   = 14; // vec2

	// Layout
	constexpr float kSepWidth     =   1.0f;
	constexpr float kContentPadX  =  14.0f;
	constexpr float kContentPadY  =  10.0f;

	// Colours
	constexpr float kAccR = 0.12f, kAccG = 0.30f, kAccB = 0.58f; // active item
	constexpr float kNavBgR = 0.10f, kNavBgG = 0.10f, kNavBgB = 0.13f; // sidebar bg
	constexpr float kSepR = 0.22f,  kSepG = 0.22f,   kSepB = 0.28f;   // separator
	constexpr float kGrpR = 0.48f,  kGrpG = 0.52f,   kGrpB = 0.62f;   // group header

	// Every nav item and group header, so the sidebar's width (computed in
	// OnRender, see PrescanLabelWidth) fits the widest one of either kind --
	// group headers ("MACHINERY") can be as wide as an item label. A fixed
	// 160px clipped "Logistic Drones"/"MACHINERY" once FontScale went much
	// past 1.0.
	const char* kSidebarLabels[] = {
		"WORLD", "  Environment", "  Corporations",
		"PLAYER", "  Self", "  Item Spawner", "  Inventory", "  Weapon",
		"  Movement", "  Teleport", "  Building", "  Skills", "  Tools",
		"ENEMIES", "  Enemies",
		"MACHINERY", "  Crafters", "  Power", "  Logistic Drones", "  Rail Drones",
		"MISC", "  Misc",
#if BETTERCHEATS_DEV_BUILD
		"DEV", "  Cheat Manager",
#endif
	};
	constexpr int kSidebarLabelCount = static_cast<int>(sizeof(kSidebarLabels) / sizeof(kSidebarLabels[0]));
}

namespace BetterCheats
{
	IPluginSelf*      CheatMenu::s_self           = nullptr;
	PanelHandle       CheatMenu::s_panelHandle    = nullptr;
	std::atomic<bool> CheatMenu::s_open           { false };
	MenuCategory      CheatMenu::s_activeCategory = MenuCategory::World_Environment;
	std::atomic<bool> CheatMenu::s_closeRequested { false };
	void* g_inputCaptureToken				 = nullptr;

	// -------------------------------------------------------------------------


	// The loader's own notification that our panel closed -- its titlebar X,
	// or (redundantly, harmlessly) our own SetPanelClose call cascading back
	// here. Never touches the registry itself; only reconciles our side.
	void CheatMenu::OnPanelClosed(PanelHandle handle)
	{
		if (handle != s_panelHandle)
			return;

		LOG_DEBUG("CheatMenu::OnPanelClosed: loader reports the panel closed");
		ApplyMenuClosed("OnPanelClosed");
	}

	void CheatMenu::Initialize(IPluginSelf* self)
	{
		s_self = self;

		static PluginPanelDesc desc{};
		desc.buttonLabel = "BetterCheats";
		desc.windowTitle = "BetterCheats";
		desc.renderFn    = &CheatMenu::OnRender;

		s_panelHandle = self->hooks->UI->RegisterPanel(&desc);
		s_self->hooks->UI->RegisterOnPanelWindowClosed(OnPanelClosed);
	}

	void CheatMenu::Shutdown()
	{
		if (s_panelHandle && s_self)
		{
			s_self->hooks->UI->SetPanelClose(s_panelHandle);
			ApplyMenuClosed("Shutdown");

			s_self->hooks->UI->UnregisterOnPanelWindowClosed(OnPanelClosed);
			s_self->hooks->UI->UnregisterPanel(s_panelHandle);
			s_panelHandle = nullptr;
		}
		s_self = nullptr;
	}

	// Idempotent: no-op if already open. Never call this from a close path.
	void CheatMenu::OpenMenu()
	{
		if (!s_panelHandle || !s_self) return;

		if (s_open.exchange(true))
		{
			LOG_DEBUG("CheatMenu::OpenMenu: already open, ignoring");
			return;
		}

		// Drop any close request left over from before the menu opened, so it
		// can't be consumed on the very first frame it's visible.
		s_closeRequested.store(false);
		s_self->hooks->UI->SetPanelOpen(s_panelHandle);
		g_inputCaptureToken = s_self->hooks->UI->AcquireInputCapture();
		LOG_DEBUG("CheatMenu::OpenMenu: opened, capture token %p", g_inputCaptureToken);
	}

	// Idempotent: no-op if already closed (ApplyMenuClosed's own guard). Tells
	// the loader first -- SetPanelClose synchronously cascades into
	// OnPanelClosed/ApplyMenuClosed when the registry agrees the panel was
	// open, but calling ApplyMenuClosed here too covers the panel already
	// having gone stale in the registry for any reason.
	void CheatMenu::CloseMenu()
	{
		if (!s_panelHandle || !s_self) return;
		if (!s_open.load()) return;

		LOG_DEBUG("CheatMenu::CloseMenu: requesting SetPanelClose");
		s_self->hooks->UI->SetPanelClose(s_panelHandle);
		ApplyMenuClosed("CloseMenu");
	}

	void CheatMenu::ApplyMenuClosed(const char* reason)
	{
		if (!s_open.exchange(false))
		{
			LOG_DEBUG("CheatMenu::ApplyMenuClosed(%s): already closed, ignoring", reason);
			return;
		}

		LOG_DEBUG("CheatMenu::ApplyMenuClosed(%s): closing", reason);

		// A rebind picker left waiting on a panel that is closing would never
		// get the chance to finish.
		Keybind::CancelCapture();

		if (g_inputCaptureToken)
		{
			LOG_DEBUG("CheatMenu::ApplyMenuClosed(%s): releasing capture token %p", reason, g_inputCaptureToken);
			if (s_self && s_self->hooks && s_self->hooks->UI)
				s_self->hooks->UI->ReleaseInputCapture(g_inputCaptureToken);
			g_inputCaptureToken = nullptr;
		}
	}

	void CheatMenu::Toggle()
	{
		if (s_open.load())
			CloseMenu();
		else
			OpenMenu();
	}

	void CheatMenu::RequestClose()
	{
		// Runs on whatever thread fires the Escape/Q keybind — no ImGui/SDK calls
		// here, just flag the request for TickPendingClose to act on the next
		// game tick.
		LOG_DEBUG("CheatMenu::RequestClose: close key received (open: %s)", s_open.load() ? "yes" : "no");
		if (s_open)
			s_closeRequested.store(true);
	}

	// Applies a pending Escape/Q close request. Called from the game tick
	// (OnEngineTick, plugin.cpp) rather than from OnRender: the loader's
	// RenderPanelWindows snapshots each panel's isOpen into a local bool
	// BEFORE calling our render function and writes that same stale local
	// back to the registry AFTER it returns -- so a SetPanelClose called from
	// inside our own render callback closes the panel for an instant and the
	// loader's own snapshot silently reopens it one statement later. That's
	// the flicker: the window closing and immediately springing back open,
	// left with a released capture token but a panel the registry still
	// considers open, which is what stopped Escape/Q from working again
	// afterwards. Applying the close from the tick instead means it lands on
	// a call stack RenderPanelWindows is never nested inside, so there is
	// nothing left for it to stomp.
	void CheatMenu::TickPendingClose()
	{
		if (s_closeRequested.exchange(false))
		{
			LOG_DEBUG("CheatMenu::TickPendingClose: applying a pending close request");
			CloseMenu();
		}
	}

	// -------------------------------------------------------------------------
	// Centered placeholder shown when the menu can't be used right now
	// -------------------------------------------------------------------------

	void CheatMenu::RenderUnavailableMessage(IModLoaderImGui* imgui, float avail_x, float avail_y, const char* message)
	{
		float text_x, text_y;
		imgui->CalcTextSize(message, &text_x, &text_y, false, 0.0f);

		float cursor_x = imgui->GetCursorPosX();
		float cursor_y = imgui->GetCursorPosY();
		imgui->SetCursorPos(cursor_x + (avail_x - text_x) * 0.5f, cursor_y + (avail_y - text_y) * 0.5f);
		imgui->Text(message);
	}

	// -------------------------------------------------------------------------
	// Top-level render — sidebar | 1px separator | content
	// -------------------------------------------------------------------------

	void CheatMenu::OnRender(IModLoaderImGui* imgui)
	{
		// A pending Escape/Q close is applied from TickPendingClose (the game
		// tick), not here -- see its comment for why closing from inside this
		// render callback caused the close to silently undo itself one frame
		// later (the loader's RenderPanelWindows snapshots isOpen before this
		// call and writes that stale snapshot back after it returns). By the
		// time this runs, the loader will simply not have called it at all for
		// a frame where the close already landed.
		float avail_x, avail_y;
		imgui->GetContentRegionAvail(&avail_x, &avail_y);

		if (!GameContext::IsInChimeraMain())
		{
			RenderUnavailableMessage(imgui, avail_x, avail_y, "Cheat menu can only be used in game");
			return;
		}

		if (!GameContext::AreCheatsAllowed())
		{
			RenderUnavailableMessage(imgui, avail_x, avail_y, "Cheat menu can only be used in single player");
			return;
		}

		// Sidebar
		const float navWidth = BetterCheats::UI::PrescanLabelWidth(imgui, kSidebarLabelCount,
			[](int i) { return kSidebarLabels[i]; });

		imgui->PushStyleColor(Col_ChildBg, kNavBgR, kNavBgG, kNavBgB, 1.0f);
		imgui->PushStyleVarVec2(Var_WindowPadding, 0.0f, 6.0f);
		imgui->PushStyleVarVec2(Var_ItemSpacing,   0.0f, 1.0f);
		if (imgui->BeginChild("##nav", navWidth, avail_y, false))
			RenderSidebar(imgui, navWidth);
		imgui->EndChild();
		imgui->PopStyleVar(2);
		imgui->PopStyleColor(1);

		// Separator
		imgui->SameLine(0.0f, 0.0f);
		imgui->PushStyleColor(Col_ChildBg, kSepR, kSepG, kSepB, 1.0f);
		imgui->BeginChild("##vsep", kSepWidth, avail_y, false);
		imgui->EndChild();
		imgui->PopStyleColor(1);

		// Content
		imgui->SameLine(0.0f, 0.0f);
		imgui->PushStyleVarVec2(Var_WindowPadding, kContentPadX, kContentPadY);
		if (imgui->BeginChild("##content", 0.0f, avail_y, false))
			RenderContent(imgui);
		imgui->EndChild();
		imgui->PopStyleVar(1);
	}

	// -------------------------------------------------------------------------
	// Sidebar
	// -------------------------------------------------------------------------

	void CheatMenu::NavItem(IModLoaderImGui* imgui, const char* label, MenuCategory cat, float navWidth)
	{
		const bool active = (s_activeCategory == cat);

		if (active)
		{
			imgui->PushStyleColor(Col_Header,        kAccR,         kAccG,         kAccB,         1.0f);
			imgui->PushStyleColor(Col_HeaderHovered, kAccR + 0.05f, kAccG + 0.05f, kAccB + 0.07f, 1.0f);
			imgui->PushStyleColor(Col_HeaderActive,  kAccR - 0.03f, kAccG - 0.03f, kAccB - 0.05f, 1.0f);
		}

		imgui->PushIDStr(label);
		if (imgui->SelectableFull(label, active, 0, navWidth, 0.0f))
		{
			// A rebind picker left waiting on the panel we are navigating away
			// from would never get the chance to finish.
			if (!active)
				Keybind::CancelCapture();

			s_activeCategory = cat;
		}
		imgui->PopID();

		if (active)
			imgui->PopStyleColor(3);
	}

	void CheatMenu::RenderSidebar(IModLoaderImGui* imgui, float navWidth)
	{
		auto NavGroup = [&](const char* label)
		{
			imgui->Spacing();
			imgui->Separator();
			imgui->Spacing();
			imgui->SetCursorPosX(imgui->GetCursorPosX() + 8.0f);
			imgui->PushStyleColor(Col_Text, kGrpR, kGrpG, kGrpB, 1.0f);
			imgui->Text(label);
			imgui->PopStyleColor(1);
			imgui->Spacing();
		};

		NavGroup("WORLD");
		NavItem(imgui, "  Environment",         MenuCategory::World_Environment,      navWidth);
		NavItem(imgui, "  Corporations",        MenuCategory::World_Corporations,     navWidth);

		NavGroup("PLAYER");
		NavItem(imgui, "  Self",                MenuCategory::Player_Self,            navWidth);
		NavItem(imgui, "  Item Spawner",        MenuCategory::Player_ItemSpawner,     navWidth);
		NavItem(imgui, "  Inventory",           MenuCategory::Player_Inventory,       navWidth);
		NavItem(imgui, "  Weapon",              MenuCategory::Player_Weapon,          navWidth);
		NavItem(imgui, "  Movement",            MenuCategory::Player_Movement,        navWidth);
		NavItem(imgui, "  Teleport",            MenuCategory::Player_Teleport,        navWidth);
		NavItem(imgui, "  Building",            MenuCategory::Player_Building,        navWidth);
		NavItem(imgui, "  Skills",              MenuCategory::Player_Skills,          navWidth);
		NavItem(imgui, "  Tools",               MenuCategory::Player_Tools,           navWidth);

		NavGroup("ENEMIES");
		NavItem(imgui, "  Enemies",             MenuCategory::Enemies_Enemies,        navWidth);

		NavGroup("MACHINERY");
		NavItem(imgui, "  Crafters",            MenuCategory::Machinery_Crafters,     navWidth);
		NavItem(imgui, "  Power",               MenuCategory::Machinery_Power,        navWidth);
		NavItem(imgui, "  Logistic Drones",     MenuCategory::Machinery_LogisticDrones, navWidth);
		NavItem(imgui, "  Rail Drones",         MenuCategory::Machinery_RailDrones,   navWidth);

		NavGroup("MISC");
		NavItem(imgui, "  Misc",                MenuCategory::Misc,                   navWidth);

#if BETTERCHEATS_DEV_BUILD
		NavGroup("DEV");
		NavItem(imgui, "  Cheat Manager",       MenuCategory::Dev_CheatManager,       navWidth);
#endif
	}

	// -------------------------------------------------------------------------
	// Content dispatch
	// -------------------------------------------------------------------------

	void CheatMenu::RenderContent(IModLoaderImGui* imgui)
	{
		imgui->Spacing();

		switch (s_activeCategory)
		{
		case MenuCategory::World_Environment:      Panels::RenderWorld_Environment(imgui);       break;
		case MenuCategory::World_Corporations:     Panels::RenderWorld_Corporations(imgui);      break;
		case MenuCategory::Player_Self:            Panels::RenderPlayer_Self(imgui);             break;
		case MenuCategory::Player_ItemSpawner:     Panels::RenderPlayer_ItemSpawner(imgui);      break;
		case MenuCategory::Player_Inventory:       Panels::RenderPlayer_Inventory(imgui);        break;
		case MenuCategory::Player_Weapon:          Panels::RenderPlayer_Weapon(imgui);           break;
		case MenuCategory::Player_Movement:        Panels::RenderPlayer_Movement(imgui);         break;
		case MenuCategory::Player_Teleport:        Panels::RenderPlayer_Teleport(imgui);         break;
		case MenuCategory::Player_Building:        Panels::RenderPlayer_Building(imgui);         break;
		case MenuCategory::Player_Skills:          Panels::RenderPlayer_Skills(imgui);           break;
		case MenuCategory::Player_Tools:           Panels::RenderPlayer_Tools(imgui);            break;
		case MenuCategory::Enemies_Enemies:        Panels::RenderEnemies(imgui);                 break;
		case MenuCategory::Machinery_Crafters:     Panels::RenderMachines_Crafters(imgui);       break;
		case MenuCategory::Machinery_Power:        Panels::RenderMachines_Power(imgui);          break;
		case MenuCategory::Machinery_LogisticDrones: Panels::RenderMachines_LogisticDrones(imgui); break;
		case MenuCategory::Machinery_RailDrones:   Panels::RenderMachines_RailDrones(imgui);     break;
		case MenuCategory::Misc:                   Panels::RenderMisc(imgui);                    break;
#if BETTERCHEATS_DEV_BUILD
		case MenuCategory::Dev_CheatManager:       Panels::RenderDev_CheatManager(imgui);        break;
#endif
		default: break;
		}
	}
}
