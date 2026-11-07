#include "panel_misc.h"
#include "map_fields.h"
#include "world_wind.h"

namespace BetterCheats::Panels
{
	void RenderMisc(IModLoaderImGui* imgui)
	{
		if (!Features::WindMute.On())
		{
			Features::RenderOff(imgui, Features::WindMute);
			return;
		}

		imgui->SeparatorText("Audio");
		Wind::RenderImGui(imgui);
	}
}
