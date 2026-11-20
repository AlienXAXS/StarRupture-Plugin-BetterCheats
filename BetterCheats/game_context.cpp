#include "game_context.h"
#include "plugin_config.h"
#include "plugin_helpers.h"

#include "Chimera_classes.hpp"

#include "plugin_map.h"

namespace BetterCheats
{
	IPluginSelf*      GameContext::s_self          = nullptr;
	std::atomic<bool> GameContext::s_inChimeraMain{ false };
	std::atomic<EPluginNetMode> GameContext::s_netMode{ EPluginNetMode::Unknown };

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
		s_netMode = EPluginNetMode::Unknown;
	}

	void GameContext::OnUIStateChanged(uint64_t /*oldState*/, uint64_t newState, void* /*userData*/)
	{
		s_inChimeraMain = (newState & PLUGIN_UI_IN_GAMEPLAY) != 0;

		// A new world starts refused until the game thread has asked for its
		// net mode, so the menu never shows a previous session's answer.
		if (!s_inChimeraMain)
			s_netMode = EPluginNetMode::Unknown;
	}

	EPluginNetMode GameContext::CurrentNetMode()
	{
		if (!s_self || !s_self->hooks->NetMode)
			return EPluginNetMode::Unknown;

		if (!s_self->hooks->Engine->IsGameThread())
			return s_netMode;

		const EPluginNetMode mode = s_self->hooks->NetMode->GetNetMode();
		if (s_netMode.exchange(mode) != mode)
		{
			const char* verdict = "allowed";
			switch (ClassifyCheatGate(false, mode))
			{
			case CheatGate::SinglePlayerOnly: verdict = "refused (single player only)"; break;
			case CheatGate::WaitingForWorld:  verdict = "waiting for the world"; break;
			default: break;
			}
			LOG_INFO("GameContext: net mode %d, cheats %s.", static_cast<int>(mode), verdict);
		}
		return mode;
	}

	bool GameContext::AreCheatsAllowed()
	{
		return GetCheatGate() == CheatGate::Allowed;
	}

	CheatGate GameContext::GetCheatGate()
	{
		if (BetterCheatsConfig::Config::IsCheatsInMultiplayerEnabled())
			return CheatGate::Allowed;

		return ClassifyCheatGate(false, CurrentNetMode());
	}
}
