#include "panel_world.h"
#include "world_wave.h"
#include "world_corporations.h"

#include "map_fields.h"
#include "server_link.h"

#include <algorithm>
#include <string>

namespace BetterCheats::Panels
{
	namespace
	{
		void RenderServerWave(IModLoaderImGui* imgui)
		{
			using ServerLink::Op;
			const auto ask = [](const char* what) { ServerLink::Queue(Op::Wave, { { "do", what } }); };

			imgui->SeparatorText("Wave Controls");

			const std::string state = ServerLink::LastResult(Op::Wave).Value("state");
			if (!state.empty())
				ServerLink::Say(imgui, "State: " + state);

			if (imgui->SmallButton("Start"))
				ask("start");
			imgui->SameLine(0.0f, 8.0f);
			if (imgui->SmallButton("Pause"))
				ask("pause");
			imgui->SameLine(0.0f, 8.0f);
			if (imgui->SmallButton("Resume"))
				ask("resume");
			imgui->SameLine(0.0f, 8.0f);
			if (imgui->SmallButton("Cancel"))
				ask("cancel");

			imgui->Spacing();
			imgui->TextWrapped("Not on a server yet:");
			imgui->BeginDisabled(true);
			imgui->SmallButton("Skip");
			imgui->SameLine(0.0f, 8.0f);
			imgui->SmallButton("Pause Waves");
			imgui->SameLine(0.0f, 8.0f);
			imgui->SmallButton("Restart Timer");
			imgui->SameLine(0.0f, 8.0f);
			imgui->SmallButton("Enable Waves");
			imgui->SameLine(0.0f, 8.0f);
			imgui->SmallButton("Disable Waves");
			imgui->EndDisabled();
		}

		void RenderServerCorporations(IModLoaderImGui* imgui)
		{
			// Apply sets the server's total, so the untouched 0 is never sent.
			static int  points  = 0;
			static bool entered = false;

			imgui->SeparatorText("Data Points");
			if (imgui->InputInt("##server_data_points", &points, 1, 100))
				entered = true;
			points = std::clamp(points, 0, 999999999);
			imgui->SameLine(0.0f, 8.0f);
			imgui->BeginDisabled(!entered);
			if (imgui->SmallButton("Apply##server_data_points"))
				ServerLink::Queue(ServerLink::Op::Corp, { { "points", std::to_string(points) } });
			imgui->EndDisabled();
			if (!entered)
				imgui->TextWrapped("Enter a value to turn on Apply. It replaces the server's current Data Points.");
		}
	}

	void RenderWorld_Environment(IModLoaderImGui* imgui)
	{
		ServerLink::Section(imgui, Features::Wave, ServerLink::Op::Wave,
			[&](bool greyed) { Wave::RenderImGui(imgui, greyed); },
			[&] { RenderServerWave(imgui); });
	}

	void RenderWorld_Corporations(IModLoaderImGui* imgui)
	{
		ServerLink::Section(imgui, Features::Corporations, ServerLink::Op::Corp,
			[&](bool greyed) { Corporations::RenderImGui(imgui, greyed); },
			[&] { RenderServerCorporations(imgui); });
	}
}
