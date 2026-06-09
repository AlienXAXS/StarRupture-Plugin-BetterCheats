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

	private:
		static void OnWorldBeginPlay(SDK::UWorld* world);
		static void OnWorldEndPlay(SDK::UWorld* world, const char* worldName);

		static IPluginSelf* s_self;
		static bool         s_inChimeraMain;
	};
}
