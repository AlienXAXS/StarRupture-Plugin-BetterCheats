#include "panel_world.h"

namespace BetterCheats::Panels
{
	void RenderWorld_Environment(IModLoaderImGui* imgui)
	{
		static bool  freezeTime = false;
		static float timeScale  = 1.0f;

		imgui->SeparatorText("Time");
		imgui->Checkbox("Freeze Time", &freezeTime);
		imgui->SliderFloat("Time Scale", &timeScale, 0.0f, 5.0f, "%.2fx");
	}
}
