#include "game_context.h"
#include "plugin_config.h"

#include "Chimera_classes.hpp"

#include "plugin_map.h"

namespace BetterCheats
{
	IPluginSelf*      GameContext::s_self          = nullptr;
	std::atomic<bool> GameContext::s_inChimeraMain{ false };

	void GameContext::Initialize(IPluginSelf* self)
	{
		s_self = self;

		if (!self->hooks->GameMenu)
			return;

		// Hot-reload: the world may already be running, and PluginInit asks
		// IsInChimeraMain right after this, so read the state once now. The
		// callback keeps it current from here on.
		s_inChimeraMain = (self->hooks->GameMenu->GetUIState() & PLUGIN_UI_IN_GAMEPLAY) != 0;
		self->hooks->GameMenu->RegisterOnUIStateChanged(PLUGIN_UI_IN_GAMEPLAY, &GameContext::OnUIStateChanged, nullptr);
	}

	void GameContext::Shutdown()
	{
		if (s_self && s_self->hooks->GameMenu)
			s_self->hooks->GameMenu->UnregisterOnUIStateChanged(&GameContext::OnUIStateChanged, nullptr);
		s_self = nullptr;
		s_inChimeraMain = false;
	}

	void GameContext::OnUIStateChanged(uint64_t /*oldState*/, uint64_t newState, void* /*userData*/)
	{
		s_inChimeraMain = (newState & PLUGIN_UI_IN_GAMEPLAY) != 0;
	}

	bool GameContext::IsSinglePlayer()
	{
		if (!s_self || !s_self->hooks->NetMode)
			return false;

		return s_self->hooks->NetMode->GetNetMode() == EPluginNetMode::Standalone;
	}

	bool GameContext::AreCheatsAllowed()
	{
		if (BetterCheatsConfig::Config::IsCheatsInMultiplayerEnabled())
			return true;

		return IsSinglePlayer();
	}
}
