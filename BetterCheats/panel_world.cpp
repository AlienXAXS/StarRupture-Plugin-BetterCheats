#include "panel_world.h"
#include "world_wave.h"
#include "world_corporations.h"

namespace BetterCheats::Panels
{
	void RenderWorld_Environment(IModLoaderImGui* imgui)
	{
		Wave::RenderImGui(imgui);
	}

	void RenderWorld_Corporations(IModLoaderImGui* imgui)
	{
		Corporations::RenderImGui(imgui);
	}
}
