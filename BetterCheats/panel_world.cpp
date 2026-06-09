#include "panel_world.h"
#include "world_wave.h"

namespace BetterCheats::Panels
{
	void RenderWorld_Environment(IModLoaderImGui* imgui)
	{
		Wave::RenderImGui(imgui);
	}
}
