#pragma once

#include "plugin_interface.h"

namespace BetterCheats
{
	enum class MenuCategory
	{
		// WORLD
		World_Environment = 0,

		// PLAYER
		Player_Self,
		Player_Weapon,
		Player_Movement,
		Player_Teleport,
		Player_Building,
		Player_Skills,

		// ENEMIES
		Enemies_Enemies,

		// MACHINERY
		Machinery_Crafters,
		Machinery_Power,
		Machinery_LogisticDrones,
		Machinery_RailDrones,

		// MISC
		Misc,

		COUNT
	};

	class CheatMenu
	{
	public:
		static void Initialize(IPluginSelf* self);
		static void Shutdown();
		static void Toggle();
		static bool IsOpen() { return s_open; }

	private:
		static void OnRender(IModLoaderImGui* imgui);
		static void RenderSidebar(IModLoaderImGui* imgui);
		static void RenderContent(IModLoaderImGui* imgui);
		static void NavItem(IModLoaderImGui* imgui, const char* label, MenuCategory cat);

		static IPluginSelf*  s_self;
		static WidgetHandle  s_widgetHandle;
		static bool          s_open;
		static MenuCategory  s_activeCategory;
	};
}
