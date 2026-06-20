#include "panel_enemies.h"
#include "enemies.h"

namespace BetterCheats::Panels
{
	void RenderEnemies(IModLoaderImGui* imgui)
	{
		Enemies::RenderImGui(imgui);
	}
}
