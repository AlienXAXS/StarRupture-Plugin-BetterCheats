#include "panel_enemies.h"
#include "enemies.h"

#include "map_fields.h"

namespace BetterCheats::Panels
{
	void RenderEnemies(IModLoaderImGui* imgui)
	{
		Features::Section(imgui, Features::Enemies, [&](bool greyed) { Enemies::RenderImGui(imgui, greyed); });
	}
}
