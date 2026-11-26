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

		// Any thread. True from a gameplay world's end until the next one begins
		// play. The loader can run a queued task in between, mid-travel, so a task
		// that force-loads assets does nothing then. Not tied to a loaded save: a
		// joined world may never report one.
		static bool IsBetweenWorlds() { return s_betweenWorlds; }
		static void SetBetweenWorlds(bool between) { s_betweenWorlds = between; }

		// Any thread. On the game thread it asks the loader for the net mode
		// now; elsewhere (the menu on the render thread, keybind callbacks) it
		// returns the game thread's last answer, since the query reads UObjects.
		static bool IsSinglePlayer();

		// Any thread. The world is one joined on a server, as the game thread last found it.
		static bool IsJoinedClient() { return s_joinedClient; }

		// Game thread. Asks the loader for the net mode; an unknown one keeps the last answer.
		static void RefreshJoinedClient();

		// Single gate for "may this build touch game state?". Every path that reads
		// or writes SDK objects — the menu and every per-tick feature module — must
		// check this, not IsInChimeraMain() alone: a connected client also loads
		// ChimeraMain, so world tracking on its own does not keep cheats out of
		// multiplayer.
		static bool AreCheatsAllowed();

		// Any thread. The joined server sent a rule set. The loader has then
		// switched off the features it denies. False on a loader below interface 81.
		static bool ServerSetsRules();

		// Game thread. Cheats are refused and the net mode is known. An unknown
		// net mode decides nothing.
		static bool IsRefusalDecided();

	private:
		static void OnUIStateChanged(uint64_t oldState, uint64_t newState, void* userData);

		static IPluginSelf*      s_self;
		static std::atomic<bool> s_inChimeraMain;
		static std::atomic<bool> s_singlePlayer;
		static std::atomic<bool> s_betweenWorlds;
		static std::atomic<bool> s_joinedClient;
	};
}
