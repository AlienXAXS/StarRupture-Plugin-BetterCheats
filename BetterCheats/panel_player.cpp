#include "panel_player.h"
#include "player_attributes.h"
#include "player_building.h"
#include "player_inventory.h"
#include "player_items.h"
#include "player_movement.h"
#include "player_skills.h"
#include "player_tools.h"
#include "player_weapons.h"

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

	void RenderPlayer_Inventory(IModLoaderImGui* imgui)
	{
		Inventory::RenderImGui(imgui);
	}

	void RenderPlayer_Weapon(IModLoaderImGui* imgui)
	{
		Weapons::RenderImGui(imgui);
	}

	void RenderPlayer_Movement(IModLoaderImGui* imgui)
	{
		Movement::RenderImGui(imgui);
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
