#pragma once

#include "plugin_interface.h"

namespace BetterCheats::Panels::Corporations
{
	void Initialize();
	void Shutdown();
	void Tick(float deltaSeconds);
	void RenderImGui(IModLoaderImGui* imgui, bool greyed);

	// Any thread. Forgets an edit that has not been applied yet.
	void DropPendingEdit();
}
