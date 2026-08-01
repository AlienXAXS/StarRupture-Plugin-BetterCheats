#pragma once

#include "plugin_interface.h"

namespace BetterCheats
{
	// Plugin-wide game session state — world tracking and net-mode queries.
	// Call Initialize/Shutdown from PluginInit/PluginShutdown.
	class GameContext
	{
	public:
		static void Initialize(IPluginSelf* self);
		static void Shutdown();

		static bool IsInChimeraMain() { return s_inChimeraMain; }
		static bool IsSinglePlayer();

		// Single gate for "may this build touch game state?". Every path that reads
		// or writes SDK objects — the menu and every per-tick feature module — must
		// check this, not IsInChimeraMain() alone: a connected client also loads
		// ChimeraMain, so world tracking on its own does not keep cheats out of
		// multiplayer.
		static bool AreCheatsAllowed();

	private:
		static void OnWorldBeginPlay(SDK::UWorld* world);
		static void OnWorldEndPlay(SDK::UWorld* world, const char* worldName);

		static IPluginSelf* s_self;
		static bool         s_inChimeraMain;
	};
}
