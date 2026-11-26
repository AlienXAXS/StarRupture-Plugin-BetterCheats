#include "panel_world.h"
#include "world_wave.h"
#include "world_corporations.h"

#include "map_fields.h"

namespace BetterCheats::Panels
{
	void RenderWorld_Environment(IModLoaderImGui* imgui)
	{
		Features::Section(imgui, Features::Wave, [&](bool greyed) { Wave::RenderImGui(imgui, greyed); });
	}

	void RenderWorld_Corporations(IModLoaderImGui* imgui)
	{
		Features::Section(imgui, Features::Corporations, [&](bool greyed) { Corporations::RenderImGui(imgui, greyed); });
	}
}
