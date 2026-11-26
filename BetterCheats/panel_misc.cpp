#include "panel_misc.h"
#include "map_fields.h"
#include "world_wind.h"

namespace BetterCheats::Panels
{
	void RenderMisc(IModLoaderImGui* imgui)
	{
		// The wind option is our own setting, so it draws the same either way.
		Features::Section(imgui, Features::WindMute, [&](bool /*greyed*/)
		{
			imgui->SeparatorText("Audio");
			Wind::RenderImGui(imgui);
		});
	}
}
