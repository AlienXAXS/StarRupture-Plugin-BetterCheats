#include "panel_machines.h"

namespace BetterCheats::Panels
{
	void RenderMachines_Crafters(IModLoaderImGui* imgui)
	{
		static bool instantBuild   = false;
		static bool noResourceCost = false;

		imgui->SeparatorText("Crafters");
		imgui->TextDisabled("No options yet.");
	}

	void RenderMachines_Power(IModLoaderImGui* imgui)
	{
		static bool noPowerDrain = false;

		imgui->SeparatorText("Power");
		imgui->TextDisabled("No options yet.");
	}

	void RenderMachines_LogisticDrones(IModLoaderImGui* imgui)
	{
		imgui->SeparatorText("Logistic Flight Drones");
		imgui->TextDisabled("No options yet.");
	}

	void RenderMachines_RailDrones(IModLoaderImGui* imgui)
	{
		imgui->SeparatorText("Rail Drones");
		imgui->TextDisabled("No options yet.");
	}
}
