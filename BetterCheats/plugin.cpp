#include "plugin.h"
#include "plugin_helpers.h"
#include "plugin_config.h"
#include "aob_resolver.h"
#include "session_config.h"
#include "game_context.h"
#include "cheat_menu.h"
#include "player_attributes.h"
#include "player_building.h"
#include "player_inventory.h"
#include "player_items.h"
#include "player_movement.h"
#include "player_skills.h"
#include "player_tools.h"
#include "world_wave.h"
#include "world_corporations.h"
#include "machine_power.h"
#include "enemies.h"
#include "dev_menus.h"

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
	if (!BetterCheats::GameContext::AreCheatsAllowed())
	{
		return;
	}

	if (!BetterCheats::SessionConfig::Reload())
	{
		return;
	}

	BetterCheats::Panels::Attributes::ApplySavedConfig();
	BetterCheats::Panels::Building::ApplySavedConfig();
	BetterCheats::Panels::Inventory::ApplySavedConfig();
	BetterCheats::Panels::Movement::ApplySavedConfig();
	BetterCheats::Panels::Tools::ApplySavedConfig();
	BetterCheats::Panels::Power::ApplySavedConfig();
	BetterCheats::Panels::Wave::ApplySavedConfig();
	BetterCheats::Panels::Enemies::ApplySavedConfig();
}

// Engine tick — drives continuous cheat effects (e.g. God Mode) regardless of
// whether the menu is currently open.
static void OnEngineTick(float deltaSeconds)
{
	if (!BetterCheats::GameContext::IsInChimeraMain())
		return;

	// A connected client loads ChimeraMain too, so without this the whole fan-out
	// below ran against server-replicated state on join — crashing the client even
	// with the menu correctly refusing to open.
	if (!BetterCheats::GameContext::AreCheatsAllowed())
		return;

	BetterCheats::Panels::Attributes::Tick(deltaSeconds);
	BetterCheats::Panels::Building::Tick(deltaSeconds);
	BetterCheats::Panels::Inventory::Tick(deltaSeconds);
	BetterCheats::Panels::Movement::Tick(deltaSeconds);
	BetterCheats::Panels::Skills::Tick(deltaSeconds);
	BetterCheats::Panels::Tools::Tick(deltaSeconds);
	BetterCheats::Panels::Wave::Tick(deltaSeconds);
	BetterCheats::Panels::Corporations::Tick(deltaSeconds);
	BetterCheats::Panels::Enemies::Tick(deltaSeconds);

#if BETTERCHEATS_DEV_BUILD
	BetterCheats::Panels::DevMenus::Tick(deltaSeconds);
#endif
}

extern "C" {

	__declspec(dllexport) PluginInfo* GetPluginInfo()
	{
		return &s_pluginInfo;
	}

	// Runs after GetPluginInfo and before PluginInit — the only window in which the
	// loader lets a plugin pattern scan. Resolve every AOB here and install nothing:
	// self->hooks is null for the duration, and a plugin that misses a required
	// pattern is unloaded before PluginInit ever runs.
	__declspec(dllexport) void OnPluginLoadHooks(IPluginSelf* self, IPluginHookScanner* scanner)
	{
		// Publish self early so the logging macros work inside the resolver; the
		// loader hands PluginInit the same pointer.
		g_self = self;

		BetterCheats::AOB::ResolveAll(self, scanner);
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

		LOG_INFO("Initializing Inventory panel...");
		BetterCheats::Panels::Inventory::Initialize();

		LOG_INFO("Initializing Movement panel...");
		BetterCheats::Panels::Movement::Initialize();

		LOG_INFO("Initializing Tools panel...");
		BetterCheats::Panels::Tools::Initialize();

		LOG_INFO("Initializing Wave panel...");
		BetterCheats::Panels::Wave::Initialize();

		LOG_INFO("Initializing Power panel...");
		BetterCheats::Panels::Power::Initialize();

		LOG_INFO("Initializing Corporations panel...");
		BetterCheats::Panels::Corporations::Initialize();

		LOG_INFO("Initializing Enemies panel...");
		BetterCheats::Panels::Enemies::Initialize();

#if BETTERCHEATS_DEV_BUILD
		LOG_INFO("Initializing Dev Cheat Manager panel (debug build)...");
		BetterCheats::Panels::DevMenus::Initialize();
#endif

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

		// Detours come out FIRST, before anything that touches game objects.
		//
		// The loader wraps PluginShutdown in SEH: a fault anywhere in here is
		// swallowed, the remaining shutdowns are skipped, and the DLL is freed
		// regardless. With hook removal at the end of the list that turned any
		// shutdown fault into a guaranteed second crash — the game kept calling
		// detours that no longer existed, and ACrCharacterPlayerBase::Tick calls
		// two of them (UpdateRepHarvesterHeatStack, GetMiningDamage) every frame.
		// Detached first, a fault below is survivable.
		BetterCheats::Panels::Tools::Shutdown();
		BetterCheats::Panels::Building::Shutdown();

		BetterCheats::CheatMenu::Shutdown();
		BetterCheats::GameContext::Shutdown();
		BetterCheats::Panels::Attributes::Shutdown();
		BetterCheats::Panels::Items::Shutdown();
		BetterCheats::Panels::Inventory::Shutdown();
		BetterCheats::Panels::Movement::Shutdown();
		BetterCheats::Panels::Wave::Shutdown();
		BetterCheats::Panels::Corporations::Shutdown();
		BetterCheats::Panels::Enemies::Shutdown();
#if BETTERCHEATS_DEV_BUILD
		BetterCheats::Panels::DevMenus::Shutdown();
#endif
		BetterCheats::SessionConfig::Shutdown();

		g_self = nullptr;
	}

} // extern "C"
