#pragma once

#include "plugin_interface.h"

namespace BetterCheats
{
	// Why the cheat menu is, or is not, usable right now.
	enum class CheatGate
	{
		Allowed,          // the menu may run
		SinglePlayerOnly, // a real multiplayer session, refused by config
		WaitingForWorld   // the net mode is not known yet (no world, or no answer)
	};

	// Unknown is not a refusal: it is the answer before the world is up, and it
	// changes once the game thread can read the net mode.
	inline CheatGate ClassifyCheatGate(EPluginNetMode mode)
	{
		if (mode == EPluginNetMode::Standalone)
			return CheatGate::Allowed;

		return mode == EPluginNetMode::Unknown ? CheatGate::WaitingForWorld : CheatGate::SinglePlayerOnly;
	}
}
