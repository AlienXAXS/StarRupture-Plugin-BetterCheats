#pragma once

// The cheats a joined player asks the server to run. Requests are queued from
// the menu, sent from the engine tick and answered on the loader's network
// callback, so everything here is behind one lock.

#include "plugin_interface.h"
#include "map_fields.h"
#include "server_protocol.h"

#include <string>

namespace BetterCheats::ServerLink
{
	enum class Op { Give, Wave, Stack, Corp, Count };

	// What the last request of an op came to. `text` may be the server's own
	// words, so it is never a format string.
	struct Result
	{
		std::string text;
		std::string waveState;   // "", "idle", "running" or "paused"
		int         value = -1;  // corp points or stack size the server reported
	};

	// PluginInit / PluginShutdown.
	void Initialize(IPluginSelf* self);
	void Shutdown();

	// Game thread, once per engine tick, behind the cheats gate. The only place
	// that sends.
	void Tick();

	// Any thread. Drops what is queued but not sent.
	void DropQueued();

	// Any thread. A world ended or the connection changed: forgets what the
	// server can do, every waiting request and every result.
	void Reset();

	// Any thread. Why the server can't run the op, or empty when it can.
	std::string Blocker(Op op);

	// Any thread. Ignored while the op is blocked.
	void Queue(Op op, ServerProtocol::Fields fields);

	// Any thread.
	Result LastResult(Op op);

	// Any thread. Text that may hold anything the server sent.
	void Say(IModLoaderImGui* imgui, const std::string& text);

	// Features::Section for a section the server can run: on a joined client it
	// draws `remote` when the server handles the op, and otherwise the greyed
	// section under the reason. A denied feature never reaches either.
	template <class Draw, class Remote>
	void Section(IModLoaderImGui* imgui, const PluginMap::Feature& feature, Op op, Draw draw, Remote remote,
		Features::Why saidAbove = Features::Why::Live)
	{
		if (Features::StateOf(feature) != Features::Why::ServerOnly)
		{
			Features::Section(imgui, feature, draw, saidAbove);
			return;
		}

		const std::string blocker = Blocker(op);
		if (!blocker.empty())
		{
			Say(imgui, blocker);
			imgui->BeginDisabled(true);
			draw(true);
			imgui->EndDisabled();
			return;
		}

		remote();
		const std::string result = LastResult(op).text;
		if (!result.empty())
			Say(imgui, result);
	}
}
