#include "plugin.h"
#include "plugin_helpers.h"
#include "plugin_config.h"
#include "session_config.h"
#include "game_context.h"
#include "cheat_menu.h"
#include "player_attributes.h"
#include "player_building.h"
#include "player_items.h"
#include "player_skills.h"
#include "player_tools.h"
#include "world_wave.h"
#include "machine_power.h"

static IPluginSelf* g_self = nullptr;

IPluginSelf* GetSelf() { return g_self; }

#ifndef MODLOADER_BUILD_TAG
#define MODLOADER_BUILD_TAG "dev"
#endif

static PluginInfo s_pluginInfo = {
	"BetterCheats",
	MODLOADER_BUILD_TAG,
	"AlienX",
	"Modular in-game cheat menu for StarRupture",
	PLUGIN_INTERFACE_VERSION,
	PLUGIN_TARGET_CLIENT
};

// Keybind callback — fires on key press to toggle the menu
static void OnToggleMenuPressed(EModKey /*key*/, EModKeyEvent /*event*/)
{
	BetterCheats::CheatMenu::Toggle();
}

// Fires once a save is fully loaded into the world — reload this session's
// JSON config and re-apply any persisted cheat settings.
static void OnExperienceLoadComplete()
{
	if (!BetterCheats::SessionConfig::Reload())
	{
		return;
	}

	BetterCheats::Panels::Attributes::ApplySavedConfig();
	BetterCheats::Panels::Building::ApplySavedConfig();
	BetterCheats::Panels::Tools::ApplySavedConfig();
	BetterCheats::Panels::Power::ApplySavedConfig();
	BetterCheats::Panels::Wave::ApplySavedConfig();
}

// Engine tick — drives continuous cheat effects (e.g. God Mode) regardless of
// whether the menu is currently open.
static void OnEngineTick(float deltaSeconds)
{
	if (!BetterCheats::GameContext::IsInChimeraMain())
		return;

	BetterCheats::Panels::Attributes::Tick(deltaSeconds);
	BetterCheats::Panels::Building::Tick(deltaSeconds);
	BetterCheats::Panels::Skills::Tick(deltaSeconds);
	BetterCheats::Panels::Tools::Tick(deltaSeconds);
	BetterCheats::Panels::Wave::Tick(deltaSeconds);
}

extern "C" {

	__declspec(dllexport) PluginInfo* GetPluginInfo()
	{
		return &s_pluginInfo;
	}

	__declspec(dllexport) bool PluginInit(IPluginSelf* self)
	{
		g_self = self;

		LOG_INFO("BetterCheats initializing...");

		if (!BetterCheatsConfig::Config::IsEnabled())
		{
			LOG_WARN("BetterCheats is disabled in config");
			return true;
		}

		LOG_INFO("Initializing config...");
		BetterCheatsConfig::Config::Initialize(self);
		BetterCheats::SessionConfig::Initialize(self);

		LOG_INFO("Initializing game context and panels...");
		BetterCheats::GameContext::Initialize(self);

		LOG_INFO("Initializing Attribute panel...");
		BetterCheats::Panels::Attributes::Initialize();

		LOG_INFO("Initializing Building panel...");
		BetterCheats::Panels::Building::Initialize();

		LOG_INFO("Initializing Item Spawner panel...");
		BetterCheats::Panels::Items::Initialize();

		LOG_INFO("Initializing Tools panel...");
		BetterCheats::Panels::Tools::Initialize();

		LOG_INFO("Initializing Wave panel...");
		BetterCheats::Panels::Wave::Initialize();

		LOG_INFO("Initializing Power panel...");
		BetterCheats::Panels::Power::Initialize();

		// Register the cheat menu widget
		BetterCheats::CheatMenu::Initialize(self);

		// Register the toggle keybind — modloader tracks rebinds automatically
		const char* toggleKey = BetterCheatsConfig::Config::GetToggleKey();
		self->hooks->Input->RegisterKeybindByName(toggleKey, EModKeyEvent::Pressed, &OnToggleMenuPressed);

		self->hooks->Engine->RegisterOnTick(&OnEngineTick);
		self->hooks->World->RegisterOnExperienceLoadComplete(&OnExperienceLoadComplete);

		// Hot-reload: experience-load-complete may have already fired before we
		// registered, so if a session is already in progress, run the same setup now.
		if (BetterCheats::GameContext::IsInChimeraMain())
		{
			LOG_INFO("BetterCheats: hot-reloaded into an active session — running experience-load setup now.");
			OnExperienceLoadComplete();
		}

		LOG_INFO("BetterCheats initialized — toggle key: %s", toggleKey);

		return true;
	}

	__declspec(dllexport) void PluginShutdown()
	{
		LOG_INFO("BetterCheats shutting down...");

		if (g_self)
		{
			const char* toggleKey = BetterCheatsConfig::Config::GetToggleKey();
			g_self->hooks->Input->UnregisterKeybindByName(toggleKey, EModKeyEvent::Pressed, &OnToggleMenuPressed);
			g_self->hooks->Engine->UnregisterOnTick(&OnEngineTick);
			g_self->hooks->World->UnregisterOnExperienceLoadComplete(&OnExperienceLoadComplete);
		}

		BetterCheats::CheatMenu::Shutdown();
		BetterCheats::GameContext::Shutdown();
		BetterCheats::Panels::Attributes::Shutdown();
		BetterCheats::Panels::Building::Shutdown();
		BetterCheats::Panels::Items::Shutdown();
		BetterCheats::Panels::Tools::Shutdown();
		BetterCheats::Panels::Wave::Shutdown();
		BetterCheats::SessionConfig::Shutdown();

		g_self = nullptr;
	}

} // extern "C"
