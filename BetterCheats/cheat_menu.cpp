#include "cheat_menu.h"
#include "plugin_helpers.h"
#include "panel_world.h"
#include "panel_player.h"
#include "panel_machines.h"
#include "panel_enemies.h"
#include "panel_misc.h"

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

	// ImGuiCond
	constexpr int Cond_FirstUseEver = 8;

	// Layout
	constexpr float kNavWidth  = 160.0f;
	constexpr float kSepWidth  =   1.0f;

	// Colours
	constexpr float kAccR = 0.12f, kAccG = 0.30f, kAccB = 0.58f; // active item
	constexpr float kNavBgR = 0.10f, kNavBgG = 0.10f, kNavBgB = 0.13f; // sidebar bg
	constexpr float kSepR = 0.22f,  kSepG = 0.22f,   kSepB = 0.28f;   // separator
	constexpr float kGrpR = 0.48f,  kGrpG = 0.52f,   kGrpB = 0.62f;   // group header
}

namespace BetterCheats
{
	IPluginSelf* CheatMenu::s_self           = nullptr;
	WidgetHandle CheatMenu::s_widgetHandle   = nullptr;
	bool         CheatMenu::s_open           = false;
	MenuCategory CheatMenu::s_activeCategory = MenuCategory::World_Environment;

	// -------------------------------------------------------------------------

	void CheatMenu::Initialize(IPluginSelf* self)
	{
		s_self = self;

		static PluginWindowHints hints{};
		hints.width     = 900.0f;
		hints.height    = 530.0f;
		hints.pos_x     = -1.0f;
		hints.pos_y     = -1.0f;
		hints.size_cond = 8; // ImGuiCond_FirstUseEver — allows user to resize freely after first open
		hints.pos_cond  = 8;

		static PluginWidgetDesc desc{};
		desc.name        = "BetterCheats";
		desc.renderFn    = &CheatMenu::OnRender;
		desc.windowHints = &hints;

		s_widgetHandle = self->hooks->UI->RegisterWidget(&desc);
		self->hooks->UI->SetWidgetVisible(s_widgetHandle, false);
	}

	void CheatMenu::Shutdown()
	{
		if (s_widgetHandle && s_self)
		{
			s_self->hooks->UI->SetWidgetVisible(s_widgetHandle, false);
			s_self->hooks->UI->UnregisterWidget(s_widgetHandle);
			s_widgetHandle = nullptr;
		}
		s_self = nullptr;
	}

	void CheatMenu::Toggle()
	{
		if (!s_widgetHandle || !s_self) return;
		s_open = !s_open;
		s_self->hooks->UI->SetWidgetVisible(s_widgetHandle, s_open);
	}

	// -------------------------------------------------------------------------
	// Top-level render — sidebar | 1px separator | content
	// -------------------------------------------------------------------------

	void CheatMenu::OnRender(IModLoaderImGui* imgui)
	{
		float avail_x, avail_y;
		imgui->GetContentRegionAvail(&avail_x, &avail_y);

		// Sidebar
		imgui->PushStyleColor(Col_ChildBg, kNavBgR, kNavBgG, kNavBgB, 1.0f);
		imgui->PushStyleVarVec2(Var_WindowPadding, 0.0f, 6.0f);
		imgui->PushStyleVarVec2(Var_ItemSpacing,   0.0f, 1.0f);
		if (imgui->BeginChild("##nav", kNavWidth, avail_y, false))
			RenderSidebar(imgui);
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
		if (imgui->BeginChild("##content", 0.0f, avail_y, false))
			RenderContent(imgui);
		imgui->EndChild();
	}

	// -------------------------------------------------------------------------
	// Sidebar
	// -------------------------------------------------------------------------

	void CheatMenu::NavItem(IModLoaderImGui* imgui, const char* label, MenuCategory cat)
	{
		const bool active = (s_activeCategory == cat);

		if (active)
		{
			imgui->PushStyleColor(Col_Header,        kAccR,         kAccG,         kAccB,         1.0f);
			imgui->PushStyleColor(Col_HeaderHovered, kAccR + 0.05f, kAccG + 0.05f, kAccB + 0.07f, 1.0f);
			imgui->PushStyleColor(Col_HeaderActive,  kAccR - 0.03f, kAccG - 0.03f, kAccB - 0.05f, 1.0f);
		}

		imgui->PushIDStr(label);
		if (imgui->SelectableFull(label, active, 0, kNavWidth, 0.0f))
			s_activeCategory = cat;
		imgui->PopID();

		if (active)
			imgui->PopStyleColor(3);
	}

	void CheatMenu::RenderSidebar(IModLoaderImGui* imgui)
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
		NavItem(imgui, "  Environment",         MenuCategory::World_Environment);

		NavGroup("PLAYER");
		NavItem(imgui, "  Self",                MenuCategory::Player_Self);
		NavItem(imgui, "  Weapon",              MenuCategory::Player_Weapon);
		NavItem(imgui, "  Movement",            MenuCategory::Player_Movement);
		NavItem(imgui, "  Teleport",            MenuCategory::Player_Teleport);
		NavItem(imgui, "  Building",            MenuCategory::Player_Building);
		NavItem(imgui, "  Skills",              MenuCategory::Player_Skills);

		NavGroup("ENEMIES");
		NavItem(imgui, "  Enemies",             MenuCategory::Enemies_Enemies);

		NavGroup("MACHINERY");
		NavItem(imgui, "  Crafters",            MenuCategory::Machinery_Crafters);
		NavItem(imgui, "  Power",               MenuCategory::Machinery_Power);
		NavItem(imgui, "  Logistic Drones",     MenuCategory::Machinery_LogisticDrones);
		NavItem(imgui, "  Rail Drones",         MenuCategory::Machinery_RailDrones);

		NavGroup("MISC");
		NavItem(imgui, "  Misc",                MenuCategory::Misc);
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
		case MenuCategory::Player_Self:            Panels::RenderPlayer_Self(imgui);             break;
		case MenuCategory::Player_Weapon:          Panels::RenderPlayer_Weapon(imgui);           break;
		case MenuCategory::Player_Movement:        Panels::RenderPlayer_Movement(imgui);         break;
		case MenuCategory::Player_Teleport:        Panels::RenderPlayer_Teleport(imgui);         break;
		case MenuCategory::Player_Building:        Panels::RenderPlayer_Building(imgui);         break;
		case MenuCategory::Player_Skills:          Panels::RenderPlayer_Skills(imgui);           break;
		case MenuCategory::Enemies_Enemies:        Panels::RenderEnemies(imgui);                 break;
		case MenuCategory::Machinery_Crafters:     Panels::RenderMachines_Crafters(imgui);       break;
		case MenuCategory::Machinery_Power:        Panels::RenderMachines_Power(imgui);          break;
		case MenuCategory::Machinery_LogisticDrones: Panels::RenderMachines_LogisticDrones(imgui); break;
		case MenuCategory::Machinery_RailDrones:   Panels::RenderMachines_RailDrones(imgui);     break;
		case MenuCategory::Misc:                   Panels::RenderMisc(imgui);                    break;
		default: break;
		}
	}
}
