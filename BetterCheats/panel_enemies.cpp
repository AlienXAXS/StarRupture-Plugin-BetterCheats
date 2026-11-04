#include "panel_enemies.h"
#include "enemies.h"

#include "map_fields.h"

namespace BetterCheats::Panels
{
	void RenderEnemies(IModLoaderImGui* imgui)
	{
		if (Features::Enemies.On())
			Enemies::RenderImGui(imgui);
		else
			Features::RenderOff(imgui, Features::Enemies);
	}
}
