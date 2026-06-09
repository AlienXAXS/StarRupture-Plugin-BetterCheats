#include "game_context.h"

#include "Chimera_classes.hpp"

#include <cstring>

namespace
{
	constexpr const char* kChimeraMainWorldName = "ChimeraMain";
}

namespace BetterCheats
{
	IPluginSelf* GameContext::s_self          = nullptr;
	bool         GameContext::s_inChimeraMain = false;

	void GameContext::Initialize(IPluginSelf* self)
	{
		s_self = self;

		self->hooks->World->RegisterOnWorldBeginPlay(&GameContext::OnWorldBeginPlay);
		self->hooks->World->RegisterOnAfterWorldEndPlay(&GameContext::OnWorldEndPlay);

		// Hot-reload: world begin-play may have already fired before we registered,
		// so probe the current world directly to pick up an in-progress session.
		try
		{
			SDK::UWorld* world = SDK::UWorld::GetWorld();
			if (world && world->GetName() == kChimeraMainWorldName)
				s_inChimeraMain = true;
		}
		catch (...) {}
	}

	void GameContext::Shutdown()
	{
		if (s_self)
		{
			s_self->hooks->World->UnregisterOnWorldBeginPlay(&GameContext::OnWorldBeginPlay);
			s_self->hooks->World->UnregisterOnAfterWorldEndPlay(&GameContext::OnWorldEndPlay);
		}
		s_self = nullptr;
		s_inChimeraMain = false;
	}

	void GameContext::OnWorldBeginPlay(SDK::UWorld* /*world*/)
	{
		s_inChimeraMain = true;
	}

	void GameContext::OnWorldEndPlay(SDK::UWorld* /*world*/, const char* worldName)
	{
		if (worldName && std::strcmp(worldName, kChimeraMainWorldName) == 0)
			s_inChimeraMain = false;
	}

	bool GameContext::IsSinglePlayer()
	{
		if (!s_self || !s_self->hooks->NetMode)
			return false;

		return s_self->hooks->NetMode->GetNetMode() == EPluginNetMode::Standalone;
	}
}
