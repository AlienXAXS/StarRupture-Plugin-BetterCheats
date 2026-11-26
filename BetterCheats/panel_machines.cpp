#include "panel_machines.h"
#include "machine_power.h"

#include "map_fields.h"

namespace BetterCheats::Panels
{
	void RenderMachines_Crafters(IModLoaderImGui* imgui)
	{
		imgui->SeparatorText("Crafters");
		imgui->TextDisabled("No options yet.");
	}

	void RenderMachines_Power(IModLoaderImGui* imgui)
	{
		Features::Section(imgui, Features::Power, [&](bool greyed) { Power::RenderImGui(imgui, greyed); });
	}

	void RenderMachines_LogisticDrones(IModLoaderImGui* imgui)
	{
		imgui->SeparatorText("Logistic Drones");
		imgui->TextDisabled("Drone audio and movement settings are configured in BetterDrone.ini");
	}

	void RenderMachines_RailDrones(IModLoaderImGui* imgui)
	{
		imgui->SeparatorText("Rail Drones");
		imgui->TextDisabled("No options yet.");
	}
}
