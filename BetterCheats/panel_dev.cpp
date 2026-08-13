#include "panel_dev.h"

#if BETTERCHEATS_DEV_BUILD

#include "dev_menus.h"

namespace BetterCheats::Panels
{
	void RenderDev_CheatManager(IModLoaderImGui* imgui)
	{
		DevMenus::RenderImGui(imgui);
	}
}

#endif // BETTERCHEATS_DEV_BUILD
