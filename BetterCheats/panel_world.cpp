#include "panel_world.h"
#include "world_wave.h"
#include "world_corporations.h"

#include "map_fields.h"

namespace BetterCheats::Panels
{
	void RenderWorld_Environment(IModLoaderImGui* imgui)
	{
		if (Features::Wave.On())
			Wave::RenderImGui(imgui);
		else
			Features::RenderOff(imgui, Features::Wave);
	}

	void RenderWorld_Corporations(IModLoaderImGui* imgui)
	{
		if (Features::Corporations.On())
			Corporations::RenderImGui(imgui);
		else
			Features::RenderOff(imgui, Features::Corporations);
	}
}
