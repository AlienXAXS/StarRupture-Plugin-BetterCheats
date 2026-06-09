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
		Player_ItemSpawner,
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
		static void RenderUnavailableMessage(IModLoaderImGui* imgui, float avail_x, float avail_y, const char* message);
		static void NavItem(IModLoaderImGui* imgui, const char* label, MenuCategory cat);

		// Gates the menu to single-player ChimeraMain sessions only.
		static void OnWorldBeginPlay(SDK::UWorld* world);
		static void OnWorldEndPlay(SDK::UWorld* world, const char* worldName);
		static bool IsSinglePlayer();

		static IPluginSelf*  s_self;
		static PanelHandle   s_panelHandle;
		static bool          s_open;
		static bool          s_inChimeraMain;
		static MenuCategory  s_activeCategory;
	};
}
