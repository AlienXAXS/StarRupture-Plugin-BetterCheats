#include "panel_player.h"
#include "player_attributes.h"
#include "player_building.h"
#include "player_items.h"
#include "player_skills.h"
#include "player_tools.h"

namespace BetterCheats::Panels
{
	void RenderPlayer_Self(IModLoaderImGui* imgui)
	{
		Attributes::RenderImGui(imgui);
	}

	void RenderPlayer_ItemSpawner(IModLoaderImGui* imgui)
	{
		Items::RenderImGui(imgui);
	}

	void RenderPlayer_Weapon(IModLoaderImGui* imgui)
	{
		static bool infiniteAmmo    = false;
		static bool noReload        = false;
		static bool instantCooldown = false;

		imgui->SeparatorText("Weapons");
		imgui->TextDisabled("No options yet.");
	}

	void RenderPlayer_Movement(IModLoaderImGui* imgui)
	{
		static bool  noClip     = false;
		static float speedMul   = 1.0f;

		imgui->SeparatorText("Movement");
		imgui->TextDisabled("No options yet.");
	}

	void RenderPlayer_Teleport(IModLoaderImGui* imgui)
	{
		imgui->SeparatorText("Teleport");
		imgui->TextDisabled("No teleport options yet.");
	}

	void RenderPlayer_Building(IModLoaderImGui* imgui)
	{
		Building::RenderImGui(imgui);
	}

	void RenderPlayer_Skills(IModLoaderImGui* imgui)
	{
		Skills::RenderImGui(imgui);
	}

	void RenderPlayer_Tools(IModLoaderImGui* imgui)
	{
		Tools::RenderImGui(imgui);
	}
}
