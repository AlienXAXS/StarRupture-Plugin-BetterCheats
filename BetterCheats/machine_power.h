#pragma once

#include "plugin_interface.h"

namespace BetterCheats::Panels::Power
{
	// Resolves AOB-pattern-based engine functions up front during PluginInit, so
	// scan failures are logged immediately at startup rather than on first Apply.
	void Initialize();

	void RenderImGui(IModLoaderImGui* imgui);
}
