#pragma once

#include "plugin_interface.h"

#include <atomic>

namespace BetterCheats
{
	// Plugin-wide game session state — world tracking and net-mode queries.
	// Call Initialize/Shutdown from PluginInit/PluginShutdown.
	class GameContext
	{
	public:
		static void Initialize(IPluginSelf* self);
		static void Shutdown();

		// Any thread: the menu reads it on the render thread.
		static bool IsInChimeraMain() { return s_inChimeraMain; }

		// Any thread. On the game thread it asks the loader for the net mode
		// now; elsewhere (the menu on the render thread, keybind callbacks) it
		// returns the game thread's last answer, since the query reads UObjects.
		static bool IsSinglePlayer();

		// Single gate for "may this build touch game state?". Every path that reads
		// or writes SDK objects — the menu and every per-tick feature module — must
		// check this, not IsInChimeraMain() alone: a connected client also loads
		// ChimeraMain, so world tracking on its own does not keep cheats out of
		// multiplayer.
		static bool AreCheatsAllowed();

	private:
		static void OnUIStateChanged(uint64_t oldState, uint64_t newState, void* userData);

		static IPluginSelf*      s_self;
		static std::atomic<bool> s_inChimeraMain;
		static std::atomic<bool> s_singlePlayer;
	};
}
