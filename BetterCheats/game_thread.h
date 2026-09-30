#pragma once

#include <atomic>
#include <Windows.h>

// The loader's own RELOAD button runs PluginShutdown from inside its D3D
// Present hook -- the render thread, not the game thread every Tick() and
// GetLocalCharacter() call assumes. A shutdown path that might run there
// needs to know before it touches any UObject, since GetLocalCharacter()
// intermittently crashes off the game thread (player_lookup.h).
namespace BetterCheats
{
	inline std::atomic<unsigned long> g_gameThreadId{ 0 };

	// Call once per Tick -- cheap, and self-correcting if the game ever moves
	// its tick to a different thread across a hot-reload.
	inline void RecordGameThread()
	{
		g_gameThreadId.store(GetCurrentThreadId());
	}

	// False until the first Tick has run, or when called from any thread other
	// than the one that called RecordGameThread.
	inline bool IsGameThread()
	{
		const unsigned long recorded = g_gameThreadId.load();
		return recorded != 0 && recorded == GetCurrentThreadId();
	}
}
