#include "cheat_menu.h"
#include "game_context.h"
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

	// Layout
	constexpr float kNavWidth     = 160.0f;
	constexpr float kSepWidth     =   1.0f;
	constexpr float kContentPadX  =  14.0f;
	constexpr float kContentPadY  =  10.0f;

	// Colours
	constexpr float kAccR = 0.12f, kAccG = 0.30f, kAccB = 0.58f; // active item
	constexpr float kNavBgR = 0.10f, kNavBgG = 0.10f, kNavBgB = 0.13f; // sidebar bg
	constexpr float kSepR = 0.22f,  kSepG = 0.22f,   kSepB = 0.28f;   // separator
	constexpr float kGrpR = 0.48f,  kGrpG = 0.52f,   kGrpB = 0.62f;   // group header
}

namespace BetterCheats
{
	IPluginSelf* CheatMenu::s_self           = nullptr;
	PanelHandle  CheatMenu::s_panelHandle    = nullptr;
	bool         CheatMenu::s_open           = false;
	MenuCategory CheatMenu::s_activeCategory = MenuCategory::World_Environment;
	void* g_inputCaptureToken				 = nullptr;

	// -------------------------------------------------------------------------


	void CheatMenu::OnPanelClosed(PanelHandle handle)
	{
		if (handle == s_panelHandle)
		{
			s_open = false;
			if (s_self && g_inputCaptureToken)
			{
				s_self->hooks->UI->ReleaseInputCapture(g_inputCaptureToken);
				g_inputCaptureToken = nullptr;
			}
		}
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

			if (g_inputCaptureToken )
				s_self->hooks->UI->ReleaseInputCapture(g_inputCaptureToken);

			s_self->hooks->UI->UnregisterOnPanelWindowClosed(OnPanelClosed);
			s_self->hooks->UI->UnregisterPanel(s_panelHandle);
			s_panelHandle = nullptr;
		}
		s_self = nullptr;
	}

	void CheatMenu::Toggle()
	{
		if (!s_panelHandle || !s_self) return;

		s_open = !s_open;
		if (s_open)
		{
			s_self->hooks->UI->SetPanelOpen(s_panelHandle);
			g_inputCaptureToken = s_self->hooks->UI->AcquireInputCapture();
		}
		else
		{
			s_self->hooks->UI->SetPanelClose(s_panelHandle);
			s_self->hooks->UI->ReleaseInputCapture(g_inputCaptureToken);
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
		float avail_x, avail_y;
		imgui->GetContentRegionAvail(&avail_x, &avail_y);

		if (!GameContext::IsInChimeraMain())
		{
			RenderUnavailableMessage(imgui, avail_x, avail_y, "Cheat menu can only be used in game");
			return;
		}

		if (!GameContext::IsSinglePlayer())
		{
			RenderUnavailableMessage(imgui, avail_x, avail_y, "Cheat menu can only be used in single player");
			return;
		}

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
		imgui->PushStyleVarVec2(Var_WindowPadding, kContentPadX, kContentPadY);
		if (imgui->BeginChild("##content", 0.0f, avail_y, false))
			RenderContent(imgui);
		imgui->EndChild();
		imgui->PopStyleVar(1);
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
		NavItem(imgui, "  Corporations",        MenuCategory::World_Corporations);

		NavGroup("PLAYER");
		NavItem(imgui, "  Self",                MenuCategory::Player_Self);
		NavItem(imgui, "  Item Spawner",        MenuCategory::Player_ItemSpawner);
		NavItem(imgui, "  Weapon",              MenuCategory::Player_Weapon);
		NavItem(imgui, "  Movement",            MenuCategory::Player_Movement);
		NavItem(imgui, "  Teleport",            MenuCategory::Player_Teleport);
		NavItem(imgui, "  Building",            MenuCategory::Player_Building);
		NavItem(imgui, "  Skills",              MenuCategory::Player_Skills);
		NavItem(imgui, "  Tools",               MenuCategory::Player_Tools);

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
		case MenuCategory::World_Corporations:     Panels::RenderWorld_Corporations(imgui);      break;
		case MenuCategory::Player_Self:            Panels::RenderPlayer_Self(imgui);             break;
		case MenuCategory::Player_ItemSpawner:     Panels::RenderPlayer_ItemSpawner(imgui);      break;
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
		default: break;
		}
	}
}
