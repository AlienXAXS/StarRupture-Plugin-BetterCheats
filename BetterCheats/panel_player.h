#pragma once

#include "plugin_interface.h"

namespace BetterCheats::Panels
{
	void RenderPlayer_Self(IModLoaderImGui* imgui);
	void RenderPlayer_ItemSpawner(IModLoaderImGui* imgui);
	void RenderPlayer_Weapon(IModLoaderImGui* imgui);
	void RenderPlayer_Movement(IModLoaderImGui* imgui);
	void RenderPlayer_Teleport(IModLoaderImGui* imgui);
	void RenderPlayer_Building(IModLoaderImGui* imgui);
	void RenderPlayer_Skills(IModLoaderImGui* imgui);
}
