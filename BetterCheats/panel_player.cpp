#include "panel_player.h"
#include "player_attributes.h"
#include "player_building.h"
#include "player_inventory.h"
#include "player_items.h"
#include "player_movement.h"
#include "player_skills.h"
#include "player_tools.h"
#include "player_weapons.h"

#include "map_fields.h"

namespace BetterCheats::Panels
{
	void RenderPlayer_Self(IModLoaderImGui* imgui)
	{
		if (Features::Stats.On())
			Attributes::RenderImGui(imgui);
		else
			Features::RenderOff(imgui, Features::Stats);
	}

	void RenderPlayer_ItemSpawner(IModLoaderImGui* imgui)
	{
		if (Features::Items.On())
			Items::RenderImGui(imgui);
		else
			Features::RenderOff(imgui, Features::Items);
	}

	void RenderPlayer_Inventory(IModLoaderImGui* imgui)
	{
		if (Features::Inventory.On())
			Inventory::RenderImGui(imgui);
		else
			Features::RenderOff(imgui, Features::Inventory);
	}

	void RenderPlayer_Weapon(IModLoaderImGui* imgui)
	{
		if (Features::Weapons.On())
			Weapons::RenderImGui(imgui);
		else
			Features::RenderOff(imgui, Features::Weapons);
	}

	void RenderPlayer_Movement(IModLoaderImGui* imgui)
	{
		if (Features::Movement.On())
			Movement::RenderImGui(imgui);
		else
			Features::RenderOff(imgui, Features::Movement);
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
		if (Features::Skills.On())
			Skills::RenderImGui(imgui);
		else
			Features::RenderOff(imgui, Features::Skills);
	}

	void RenderPlayer_Tools(IModLoaderImGui* imgui)
	{
		if (Features::Mining.On())
			Tools::RenderImGui(imgui);
		else
			Features::RenderOff(imgui, Features::Mining);
	}
}
