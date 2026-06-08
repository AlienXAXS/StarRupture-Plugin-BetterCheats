#pragma once

#include "plugin_interface.h"

namespace BetterCheats::Panels::Skills
{
	void RenderImGui(IModLoaderImGui* imgui);

	// Refreshes the skill snapshot RenderImGui() displays and applies edits it
	// queued — call once per engine tick.
	void Tick(float deltaSeconds);
}
