#include "panel_enemies.h"
#include "enemies.h"

#include "map_fields.h"
#include "server_link.h"

namespace BetterCheats::Panels
{
	namespace
	{
		// A server-wide setting reads as unknown after a join, so each has an On
		// and an Off button and no checkbox.
		void RenderServerSetting(IModLoaderImGui* imgui, const char* label, const char* key)
		{
			imgui->PushIDStr(key);
			imgui->AlignTextToFramePadding();
			imgui->Text(label);
			imgui->SameLine(0.0f, 8.0f);
			if (imgui->Button("On"))
				ServerLink::Queue(ServerLink::Op::Enemy, { { key, "on" } });
			imgui->SameLine(0.0f, 8.0f);
			if (imgui->Button("Off"))
				ServerLink::Queue(ServerLink::Op::Enemy, { { key, "off" } });
			imgui->PopID();
		}

		void RenderServerEnemies(IModLoaderImGui* imgui)
		{
			imgui->SeparatorText("Enemies");
			RenderServerSetting(imgui, "Disable enemy AI", "ai");
			RenderServerSetting(imgui, "One-hit kill", "onehit");
			imgui->TextWrapped("The other enemy options are not on a server yet.");
		}
	}

	void RenderEnemies(IModLoaderImGui* imgui)
	{
		ServerLink::Section(imgui, Features::Enemies, ServerLink::Op::Enemy,
			[&](bool greyed) { Enemies::RenderImGui(imgui, greyed); },
			[&] { RenderServerEnemies(imgui); });
	}
}
