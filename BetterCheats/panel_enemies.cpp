#include "panel_enemies.h"
#include "enemies.h"

#include "map_fields.h"
#include "server_link.h"

#include <string>

namespace BetterCheats::Panels
{
	namespace
	{
		// A server may not report a server-wide setting, so each has an On and an
		// Off button and no checkbox.
		void RenderServerSetting(IModLoaderImGui* imgui, const ServerLink::Result& last,
			const char* label, const char* key)
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
			if (const std::string now = last.Value(key); last.readBack && (now == "on" || now == "off"))
			{
				imgui->SameLine(0.0f, 8.0f);
				imgui->TextDisabled(now == "on" ? "Now on" : "Now off");
			}
			imgui->PopID();
		}

		void RenderServerEnemies(IModLoaderImGui* imgui)
		{
			imgui->SeparatorText("Enemies");
			const ServerLink::Result last = ServerLink::LastResult(ServerLink::Op::Enemy);
			RenderServerSetting(imgui, last, "Disable enemy AI", "ai");
			RenderServerSetting(imgui, last, "One-hit kill", "onehit");
		}
	}

	void RenderEnemies(IModLoaderImGui* imgui)
	{
		ServerLink::Section(imgui, Features::Enemies, ServerLink::Op::Enemy,
			[&](bool greyed) { Enemies::RenderImGui(imgui, greyed); },
			[&] { RenderServerEnemies(imgui); });
	}
}
