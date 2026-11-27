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
#include "server_link.h"

namespace BetterCheats::Panels
{
	void RenderPlayer_Self(IModLoaderImGui* imgui)
	{
		Features::Section(imgui, Features::Stats, [&](bool greyed) { Attributes::RenderImGui(imgui, greyed); });
	}

	void RenderPlayer_ItemSpawner(IModLoaderImGui* imgui)
	{
		ServerLink::Section(imgui, Features::Items, ServerLink::Op::Give,
			[&](bool greyed) { Items::RenderImGui(imgui, greyed); },
			[&] { Items::RenderImGui(imgui, false, true); });
	}

	void RenderPlayer_Inventory(IModLoaderImGui* imgui)
	{
		Features::Section(imgui, Features::Inventory, [&](bool greyed) { Inventory::RenderImGui(imgui, greyed); });

		Inventory::RenderStackSizes(imgui);
	}

	void RenderPlayer_Weapon(IModLoaderImGui* imgui)
	{
		Features::Section(imgui, Features::Weapons, [&](bool greyed) { Weapons::RenderImGui(imgui, greyed); });
	}

	void RenderPlayer_Movement(IModLoaderImGui* imgui)
	{
		Features::Section(imgui, Features::Movement, [&](bool greyed) { Movement::RenderImGui(imgui, greyed); });
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
		Features::Section(imgui, Features::Skills, [&](bool greyed) { Skills::RenderImGui(imgui, greyed); });
	}

	void RenderPlayer_Tools(IModLoaderImGui* imgui)
	{
		Features::Section(imgui, Features::Mining, [&](bool greyed) { Tools::RenderImGui(imgui, greyed); });
	}
}
