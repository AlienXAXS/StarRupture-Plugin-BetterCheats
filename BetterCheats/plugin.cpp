#include "plugin.h"
#include "plugin_helpers.h"
#include "plugin_config.h"
#include "aob_resolver.h"
#include "session_config.h"
#include "game_context.h"
#include "cheat_menu.h"
#include "player_attributes.h"
#include "player_building.h"
#include "defer.h"
#include "player_inventory.h"
#include "player_items.h"
#include "player_movement.h"
#include "player_skills.h"
#include "player_tools.h"
#include "player_weapons.h"
#include "world_wave.h"
#include "world_wind.h"
#include "world_corporations.h"
#include "machine_power.h"
#include "enemies.h"
#include "dev_menus.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include "Engine_classes.hpp"
#include "plugin_map.h"
#include "map_fields.h"

static IPluginSelf* g_self = nullptr;
static bool g_enabled = true;

// Which sections PluginInit actually initialised (installed hooks, registered
// keybinds). A host can switch a section's flag off after that, so PluginShutdown
// undoes what ran, not what the flag says now: gating on the live flag skipped the
// unhook and left detours pointing into the freed DLL.
static struct
{
	bool Inventory, Movement, Weapons, Mining, Wave, WindMute, Corp, Enemies, Items, StackSize;
	bool Stats, PowerSection, BuildFree, BuildUnlock, BuildAnywhere;
} g_initRan;

// True once SessionConfig holds the world that is running now: set when load
// complete has reloaded it, cleared when a world ends. Until then it still holds
// the previous world's data, which a re-apply must not read.
static std::atomic<bool> g_sessionCurrent{ false };

// The cheats gate, as load complete and the tick last found it. Game thread only.
// Open: sections may have written values. Released: the gate shut after that and
// they were taken back. Load waiting: a world finished loading while it was shut.
static bool g_gateOpen     = false;
static bool g_gateReleased = false;
static bool g_loadWaiting  = false;

// Each section's flag as the tick last saw it, and whether it still owes a
// re-apply. A host (or a version-map miss) can switch a flag off while the game
// runs: the tick notices the edge, gives back what the section wrote, and on the
// way back on applies the session's saved settings again, which a section that
// was off at world load never got.
struct FlagEdge
{
	bool was     = false;
	bool pending = false;
};

static struct
{
	FlagEdge Stats, Inventory, Movement, Mining, Weapons, Wave, Enemies;
	FlagEdge BuildFree, BuildUnlock, BuildAnywhere, PowerSection, WindMute, StackSize;
} g_wasOn;

static void SeedWasOn()
{
	using namespace BetterCheats;
	g_wasOn.Stats.was         = Features::Stats.On();
	g_wasOn.Inventory.was     = Features::Inventory.On();
	g_wasOn.Movement.was      = Features::Movement.On();
	g_wasOn.Mining.was        = Features::Mining.On();
	g_wasOn.Weapons.was       = Features::Weapons.On();
	g_wasOn.Wave.was          = Features::Wave.On();
	g_wasOn.Enemies.was       = Features::Enemies.On();
	g_wasOn.BuildFree.was     = Features::BuildFree.On();
	g_wasOn.BuildUnlock.was   = Features::BuildUnlock.On();
	g_wasOn.BuildAnywhere.was = Features::BuildAnywhere.On();
	g_wasOn.PowerSection.was  = Features::Power.On();
	g_wasOn.WindMute.was      = Features::WindMute.On();
	g_wasOn.StackSize.was     = Features::StackSize.On();
}

// Follows one section's flag. Game thread only. A section whose Initialize never
// ran (its flag was off at PluginInit) stays inactive until RELOAD: it has no hooks
// or world callbacks, so neither its tick nor an edge may touch it. `released` takes
// back what the section wrote when the flag goes off; `rearm` applies the session's
// saved settings once the flag is back on and the session config is the current
// world's. Either may be null. Returns whether the section should tick.
static bool FollowFlag(bool initRan, bool now, FlagEdge& edge, void (*released)(), void (*rearm)())
{
	if (!initRan)
		return false;

	if (now != edge.was)
	{
		edge.was = now;
		if (now)
		{
			edge.pending = true;
		}
		else
		{
			edge.pending = false;
			if (released)
				released();
		}
	}

	if (edge.pending && g_sessionCurrent.load())
	{
		edge.pending = false;
		if (rearm)
			rearm();
	}
	return now;
}

// Takes back what the sections wrote once the cheats gate shuts. Each section
// reads as switched off, so FollowFlag applies its saved settings again when the
// gate opens. Game thread only.
static void ReleaseForShutGate()
{
	using namespace BetterCheats;
	FollowFlag(g_initRan.Stats, false, g_wasOn.Stats, nullptr, nullptr);
	FollowFlag(g_initRan.BuildFree, false, g_wasOn.BuildFree, nullptr, nullptr);
	FollowFlag(g_initRan.BuildAnywhere, false, g_wasOn.BuildAnywhere, nullptr, nullptr);
	FollowFlag(g_initRan.BuildUnlock, false, g_wasOn.BuildUnlock, nullptr, nullptr);
	Panels::Building::ForgetSettings();
	FollowFlag(g_initRan.Inventory, false, g_wasOn.Inventory, &Panels::Inventory::ReleaseValues, nullptr);
	FollowFlag(g_initRan.StackSize, false, g_wasOn.StackSize, &Panels::Inventory::ReleaseStackSizes, nullptr);
	FollowFlag(g_initRan.Movement, false, g_wasOn.Movement,
		[] { Panels::Movement::ReleaseValues(true); }, nullptr);
	FollowFlag(g_initRan.Mining, false, g_wasOn.Mining, nullptr, nullptr);
	if (g_initRan.Mining)
		Panels::Tools::ForgetSettings();
	FollowFlag(g_initRan.Weapons, false, g_wasOn.Weapons, &Panels::Weapons::ReleaseValues, nullptr);
	FollowFlag(g_initRan.Wave, false, g_wasOn.Wave, &Panels::Wave::ReleaseValues, nullptr);
	FollowFlag(g_initRan.Enemies, false, g_wasOn.Enemies, &Panels::Enemies::ReleaseValues, nullptr);
	FollowFlag(g_initRan.PowerSection, false, g_wasOn.PowerSection, &Panels::Power::ReleaseValues, nullptr);
}

IPluginSelf* GetSelf() { return g_self; }

PluginPresets::Store BetterCheats::g_presets([](const char* msg) { LOG_WARN("%s", msg); });

// Mirrors BetterDrone's own GetPresetsFilePath (drone_ui.cpp) -- resolves
// this DLL's own directory (not the working directory, which the loader
// doesn't guarantee) and appends config\BetterCheats-Presets.ini, matching
// the sibling BetterDrone-Presets.ini path convention.
static void GetPresetsFilePath(char* outPath, size_t outSize)
{
	HMODULE module = nullptr;
	GetModuleHandleExA(
		GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		reinterpret_cast<LPCSTR>(&GetPresetsFilePath), &module);

	char path[MAX_PATH] = {};
	GetModuleFileNameA(module, path, MAX_PATH);

	char* lastSlash = strrchr(path, '\\');
	if (lastSlash)
		*lastSlash = '\0';

	snprintf(outPath, outSize, "%s\\config\\BetterCheats-Presets.ini", path);
}

#ifndef MODLOADER_BUILD_TAG
#define MODLOADER_BUILD_TAG "dev"
#endif

static PluginInfo s_pluginInfo = {
	"BetterCheats",
	MODLOADER_BUILD_TAG,
	"AlienX",
	"Modular in-game cheat menu for StarRupture",
	PLUGIN_DECLARED_INTERFACE,
	PLUGIN_TARGET_CLIENT,
	PLUGIN_GAME_BUILT_CL,
	PLUGIN_MAP_MIGRATED
};

// Keybind callback — fires on key press to toggle the menu
static void OnToggleMenuPressed(EModKey /*key*/, EModKeyEvent /*event*/)
{
	BetterCheats::CheatMenu::Toggle();
}

// Escape and Q (the game's own close/cancel key) both close the menu like any
// other overlay's dismiss keys. Registered by enum, not RegisterKeybindByName:
// they're a universal convention, not a user rebind, so neither should show up
// as a setting on the loader's config page. Plain RegisterKeybind is
// non-blocking by default (see keybind_registry.cpp's blocking map), so Q still
// reaches the game whenever the panel is closed.
static void OnCloseKeyPressed(EModKey /*key*/, EModKeyEvent /*event*/)
{
	BetterCheats::CheatMenu::RequestClose();
}

// Fires once a save is fully loaded into the world — reload this session's
// JSON config and re-apply any persisted cheat settings.
static void OnExperienceLoadComplete()
{
	// A joining client has no server rules yet, so a shut gate only postpones
	// this: the tick runs it when the gate opens.
	g_loadWaiting = !BetterCheats::GameContext::AreCheatsAllowed();
	if (g_loadWaiting)
		return;

	g_gateOpen = true;

	if (!BetterCheats::SessionConfig::Reload())
	{
		g_sessionCurrent = false;
		return;
	}

	if (g_initRan.Stats && BetterCheats::Features::Stats.On())
		BetterCheats::Panels::Attributes::ApplySavedConfig();
	BetterCheats::Panels::Building::ApplySavedConfig();
	if (g_initRan.Inventory && BetterCheats::Features::Inventory.On())
		BetterCheats::Panels::Inventory::ApplySavedConfig();
	if (g_initRan.StackSize && BetterCheats::Features::StackSize.On())
		BetterCheats::Panels::Inventory::ApplySavedStackConfig();
	if (g_initRan.Movement && BetterCheats::Features::Movement.On())
		BetterCheats::Panels::Movement::ApplySavedConfig();
	if (g_initRan.Mining && BetterCheats::Features::Mining.On())
		BetterCheats::Panels::Tools::ApplySavedConfig();
	if (g_initRan.Weapons && BetterCheats::Features::Weapons.On())
		BetterCheats::Panels::Weapons::ApplySavedConfig();
	if (g_initRan.PowerSection && BetterCheats::Features::Power.On())
		BetterCheats::Panels::Power::ApplySavedConfig();
	if (g_initRan.Wave && BetterCheats::Features::Wave.On())
		BetterCheats::Panels::Wave::ApplySavedConfig();
	if (g_initRan.Enemies && BetterCheats::Features::Enemies.On())
		BetterCheats::Panels::Enemies::ApplySavedConfig();

	// Last, so a section re-armed by the tick never reads the previous world's data.
	g_sessionCurrent = true;
}

// A world is gone: SessionConfig still holds its data until load complete reloads
// it for the next one.
static void OnAfterWorldEndPlay(SDK::UWorld* /*world*/, const char* /*name*/)
{
	g_sessionCurrent = false;
	g_loadWaiting    = false;
}

// PluginGameThreadCallback wrapper for the hot-reload path above.
static void OnExperienceLoadCompleteOnGameThread(void* /*context*/)
{
	OnExperienceLoadComplete();
}

// Whether the named plugin is loaded and enabled. Loaders below interface 80 have
// no registry, so this answers false there.
static bool IsPluginRunning(const char* name)
{
	IPluginHooks* hooks = GetHooks();
	IPluginRegistry* registry = PluginOptRegistry(hooks);
	return registry && registry->GetPluginVersion(name, nullptr, 0);
}

// The loader's declared-features text, or -1 before it has finished starting.
static int ReadDeclaredFeatures(char* buf, size_t size)
{
	IPluginHooks* hooks = GetHooks();
	IPluginRegistry* registry = PluginOptRegistry(hooks);
	return registry ? registry->GetDeclaredFeatures(buf, size) : -1;
}

// Lets a loader that has interface 80 report its level, which turns the registry on.
PLUGIN_EXPORT_HOST_INFO()

// Engine tick — drives continuous cheat effects (e.g. God Mode) regardless of
// whether the menu is currently open.
static void OnEngineTick(float deltaSeconds)
{
	// Applies a pending Escape/Q close request. Must run here, on the game
	// tick, never from inside the panel's own render callback -- see
	// CheatMenu::TickPendingClose for why. Runs before the ChimeraMain/cheats
	// gate below: closing the menu must still work even if the player left
	// single-player or the world unloaded out from under an open panel.
	// TickPendingOpen applies a click on the pause menu's BETTER CHEATS row,
	// on the same tick for the same reason.
	BetterCheats::CheatMenu::TickPendingClose();
	BetterCheats::CheatMenu::TickPendingOpen();

	// Asks whether Best Builder is running and has each building feature
	// working in this game build, so those can be left to it. Game thread only; the answer is cached for
	// the menu and the detours.
	BetterCheats::Defer::Tick(GetTickCount64(), &IsPluginRunning, &ReadDeclaredFeatures);

	// An audio option, not a cheat: it does not wait for the cheats gate, and it
	// gives the sound back itself when the world goes away.
	if (FollowFlag(g_initRan.WindMute, BetterCheats::Features::WindMute.On(), g_wasOn.WindMute,
			&BetterCheats::Panels::Wind::ReleaseValues, nullptr))
		BetterCheats::Panels::Wind::Tick(deltaSeconds);

	if (!BetterCheats::GameContext::IsInChimeraMain())
		return;

	// A connected client loads ChimeraMain too, so without this the whole fan-out
	// below ran against server-replicated state on join — crashing the client even
	// with the menu correctly refusing to open.
	if (!BetterCheats::GameContext::AreCheatsAllowed())
	{
		if (g_gateOpen && BetterCheats::GameContext::IsRefusalDecided())
		{
			g_gateOpen     = false;
			g_gateReleased = true;
			ReleaseForShutGate();
		}
		return;
	}

	g_gateOpen = true;
	if (g_loadWaiting)
		OnExperienceLoadComplete();
	else if (g_gateReleased && g_sessionCurrent.load())
		BetterCheats::Panels::Building::ApplySavedConfig();
	g_gateReleased = false;

	// After the gates: the release and the re-apply both touch game objects, which
	// only a cheats world has. A flag that changed while gated is picked up on
	// the first tick back.
	{
		using namespace BetterCheats;
		if (FollowFlag(g_initRan.Stats, Features::Stats.On(), g_wasOn.Stats,
				nullptr, &Panels::Attributes::ApplySavedConfig))
			Panels::Attributes::Tick(deltaSeconds);

		// Three flags share the Building panel: its saved settings are read per flag,
		// so any of them coming back on re-reads them all. Only Unlock All Buildings
		// wrote something that has to be taken back, and only it has a tick.
		FollowFlag(g_initRan.BuildFree, Features::BuildFree.On(), g_wasOn.BuildFree,
			nullptr, &Panels::Building::ApplySavedConfig);
		FollowFlag(g_initRan.BuildAnywhere, Features::BuildAnywhere.On(), g_wasOn.BuildAnywhere,
			nullptr, &Panels::Building::ApplySavedConfig);
		if (FollowFlag(g_initRan.BuildUnlock, Features::BuildUnlock.On(), g_wasOn.BuildUnlock,
				&Panels::Building::ReleaseValues, &Panels::Building::ApplySavedConfig))
			Panels::Building::Tick(deltaSeconds);

		if (FollowFlag(g_initRan.Inventory, Features::Inventory.On(), g_wasOn.Inventory,
				&Panels::Inventory::ReleaseValues, &Panels::Inventory::ApplySavedConfig))
			Panels::Inventory::Tick(deltaSeconds);
		if (FollowFlag(g_initRan.StackSize, Features::StackSize.On(), g_wasOn.StackSize,
				&Panels::Inventory::ReleaseStackSizes, &Panels::Inventory::ApplySavedStackConfig))
			Panels::Inventory::TickStackSizes();
		if (FollowFlag(g_initRan.Movement, Features::Movement.On(), g_wasOn.Movement,
				[] { Panels::Movement::ReleaseValues(true); }, &Panels::Movement::ApplySavedConfig))
			Panels::Movement::Tick(deltaSeconds);
		if (Features::Skills.On())
			Panels::Skills::Tick(deltaSeconds);
		if (FollowFlag(g_initRan.Mining, Features::Mining.On(), g_wasOn.Mining,
				&Panels::Tools::ReleaseValues, &Panels::Tools::ApplySavedConfig))
			Panels::Tools::Tick(deltaSeconds);
		if (FollowFlag(g_initRan.Weapons, Features::Weapons.On(), g_wasOn.Weapons,
				&Panels::Weapons::ReleaseValues, &Panels::Weapons::ApplySavedConfig))
			Panels::Weapons::Tick(deltaSeconds);
		if (FollowFlag(g_initRan.Wave, Features::Wave.On(), g_wasOn.Wave,
				&Panels::Wave::ReleaseValues, &Panels::Wave::ApplySavedConfig))
			Panels::Wave::Tick(deltaSeconds);
		if (g_initRan.Corp && Features::Corporations.On())
			Panels::Corporations::Tick(deltaSeconds);
		if (FollowFlag(g_initRan.Enemies, Features::Enemies.On(), g_wasOn.Enemies,
				&Panels::Enemies::ReleaseValues, &Panels::Enemies::ApplySavedConfig))
			Panels::Enemies::Tick(deltaSeconds);
		FollowFlag(g_initRan.PowerSection, Features::Power.On(), g_wasOn.PowerSection,
			&Panels::Power::ReleaseValues, &Panels::Power::ApplySavedConfig);
	}

#if BETTERCHEATS_DEV_BUILD
	BetterCheats::Panels::DevMenus::Tick(deltaSeconds);
#endif

	// Debounced: almost every call here is a no-op (nothing dirty, or the
	// debounce window hasn't elapsed yet). See session_config.cpp -- this is
	// what turns a slider held down for a second, or a stack-size apply
	// pass touching every item, into at most one disk write per window
	// instead of one per Set() call.
	BetterCheats::SessionConfig::Commit();
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

		BetterCheats::Features::Declare(self, scanner);
		BetterCheats::AOB::ResolveAll(self, scanner);
		BetterCheats::Fields::Resolve(self, scanner);
		BetterCheats::Features::ResolveSlotSize(self, scanner);
	}

	__declspec(dllexport) bool PluginInit(IPluginSelf* self)
	{
		// First, before any early return (a disabled plugin included): off its game
		// build the loader refuses a plugin whose PluginInit did not take the globals.
		PluginSdkRuntime::Init(self->hooks);

		g_self = self;

		LOG_INFO("BetterCheats initializing...");

		LOG_INFO("Initializing config...");
		BetterCheatsConfig::Config::Initialize(self);

		// Read only after Initialize: IsEnabled() has no plugin handle before it.
		if (!BetterCheatsConfig::Config::IsEnabled())
		{
			LOG_WARN("BetterCheats is disabled in config");
			g_enabled = false;
			return true;
		}

		BetterCheats::SessionConfig::Initialize(self);

		LOG_INFO("Initializing game context and panels...");
		BetterCheats::GameContext::Initialize(self);

		LOG_INFO("Initializing Attribute panel...");
		if (BetterCheats::Features::Stats.On())
		{
			BetterCheats::Panels::Attributes::Initialize();
			g_initRan.Stats = true;
		}

		LOG_INFO("Initializing Building panel...");
		BetterCheats::Panels::Building::Initialize();
		g_initRan.BuildFree     = BetterCheats::Features::BuildFree.On();
		g_initRan.BuildUnlock   = BetterCheats::Features::BuildUnlock.On();
		g_initRan.BuildAnywhere = BetterCheats::Features::BuildAnywhere.On();

		LOG_INFO("Initializing Item Spawner panel...");
		if (BetterCheats::Features::Items.On())
		{
			BetterCheats::Panels::Items::Initialize();
			g_initRan.Items = true;
		}

		LOG_INFO("Initializing Inventory panel...");
		if (BetterCheats::Features::Inventory.On())
		{
			BetterCheats::Panels::Inventory::Initialize();
			g_initRan.Inventory = true;
		}

		// Stack sizes have no hooks or keybinds to set up: the section only ticks.
		g_initRan.StackSize = BetterCheats::Features::StackSize.On();

		LOG_INFO("Initializing Movement panel...");
		if (BetterCheats::Features::Movement.On())
		{
			BetterCheats::Panels::Movement::Initialize();
			g_initRan.Movement = true;
		}

		LOG_INFO("Initializing Weapons panel...");
		if (BetterCheats::Features::Weapons.On())
		{
			BetterCheats::Panels::Weapons::Initialize();
			g_initRan.Weapons = true;
		}

		LOG_INFO("Initializing Tools panel...");
		if (BetterCheats::Features::Mining.On())
		{
			BetterCheats::Panels::Tools::Initialize();
			g_initRan.Mining = true;
		}

		LOG_INFO("Initializing Wave panel...");
		if (BetterCheats::Features::Wave.On())
		{
			BetterCheats::Panels::Wave::Initialize();
			g_initRan.Wave = true;
		}

		LOG_INFO("Initializing Wind mute...");
		if (BetterCheats::Features::WindMute.On())
		{
			BetterCheats::Panels::Wind::Initialize();
			g_initRan.WindMute = true;
		}

		LOG_INFO("Initializing Power panel...");
		if (BetterCheats::Features::Power.On())
		{
			BetterCheats::Panels::Power::Initialize();
			g_initRan.PowerSection = true;
		}

		LOG_INFO("Initializing Corporations panel...");
		if (BetterCheats::Features::Corporations.On())
		{
			BetterCheats::Panels::Corporations::Initialize();
			g_initRan.Corp = true;
		}

		LOG_INFO("Initializing Enemies panel...");
		if (BetterCheats::Features::Enemies.On())
		{
			BetterCheats::Panels::Enemies::Initialize();
			g_initRan.Enemies = true;
		}

#if BETTERCHEATS_DEV_BUILD
		LOG_INFO("Initializing Dev Cheat Manager panel (debug build)...");
		BetterCheats::Panels::DevMenus::Initialize();
#endif

		LOG_INFO("Initializing saved presets store...");
		char presetsPath[MAX_PATH] = {};
		GetPresetsFilePath(presetsPath, sizeof(presetsPath));
		BetterCheats::g_presets.LoadFile(presetsPath);

		// Register the cheat menu widget
		BetterCheats::CheatMenu::Initialize(self);

		// Register the toggle keybind — modloader tracks rebinds automatically
		const char* toggleKey = BetterCheatsConfig::Config::GetToggleKey();
		self->hooks->Input->RegisterKeybindByName(toggleKey, EModKeyEvent::Pressed, &OnToggleMenuPressed);
		self->hooks->Input->RegisterKeybind(EModKey::Escape, EModKeyEvent::Pressed, &OnCloseKeyPressed);
		self->hooks->Input->RegisterKeybind(EModKey::Q, EModKeyEvent::Pressed, &OnCloseKeyPressed);

		SeedWasOn();
		self->hooks->Engine->RegisterOnTick(&OnEngineTick);
		self->hooks->World->RegisterOnExperienceLoadComplete(&OnExperienceLoadComplete);
		self->hooks->World->RegisterOnAfterWorldEndPlay(&OnAfterWorldEndPlay);

		// Hot-reload: experience-load-complete may have already fired before we
		// registered, so if a session is already in progress, run the same setup now.
		//
		// It has to go through the game thread. SessionConfig::Reload() resolves the
		// save name via UCrSaveSubsystem, which only works there -- called inline from
		// PluginInit it returns false, OnExperienceLoadComplete() bails, and every
		// panel's ApplySavedConfig is skipped. The failure is silent, and because
		// SessionConfig::Set() no-ops while unloaded, NOTHING persists for the rest of
		// the session. Same PostToGameThread pattern used by machine_power.cpp,
		// player_attributes.cpp and player_items.cpp.
		if (BetterCheats::GameContext::IsInChimeraMain())
		{
			LOG_INFO("BetterCheats: hot-reloaded into an active session — posting experience-load setup to the game thread.");
			self->hooks->Engine->PostToGameThread(&OnExperienceLoadCompleteOnGameThread, nullptr);
		}

		LOG_INFO("BetterCheats initialized — toggle key: %s", toggleKey);

		return true;
	}

	__declspec(dllexport) void PluginShutdown()
	{
		LOG_INFO("BetterCheats shutting down...");

		// Disabled in config: PluginInit registered nothing, so there is nothing to undo.
		if (!g_enabled)
		{
			g_self = nullptr;
			return;
		}

		if (g_self)
		{
			const char* toggleKey = BetterCheatsConfig::Config::GetToggleKey();
			g_self->hooks->Input->UnregisterKeybindByName(toggleKey, EModKeyEvent::Pressed, &OnToggleMenuPressed);
			g_self->hooks->Input->UnregisterKeybind(EModKey::Escape, EModKeyEvent::Pressed, &OnCloseKeyPressed);
			g_self->hooks->Input->UnregisterKeybind(EModKey::Q, EModKeyEvent::Pressed, &OnCloseKeyPressed);
			g_self->hooks->Engine->UnregisterOnTick(&OnEngineTick);
			g_self->hooks->World->UnregisterOnExperienceLoadComplete(&OnExperienceLoadComplete);
			g_self->hooks->World->UnregisterOnAfterWorldEndPlay(&OnAfterWorldEndPlay);
		}

		// Detours come out FIRST, before anything that touches game objects.
		//
		// The loader wraps PluginShutdown in SEH: a fault anywhere in here is
		// swallowed, the remaining shutdowns are skipped, and the DLL is freed
		// regardless. With hook removal at the end of the list that turned any
		// shutdown fault into a guaranteed second crash — the game kept calling
		// detours that no longer existed, and ACrCharacterPlayerBase::Tick calls
		// GetMiningDamage every frame.
		// Detached first, a fault below is survivable.
		if (g_initRan.Mining)
			BetterCheats::Panels::Tools::Shutdown();
		BetterCheats::Panels::Building::Shutdown();

		BetterCheats::CheatMenu::Shutdown();
		BetterCheats::GameContext::Shutdown();
		if (g_initRan.Items)
			BetterCheats::Panels::Items::Shutdown();
		if (g_initRan.Inventory)
			BetterCheats::Panels::Inventory::Shutdown();
		if (g_initRan.StackSize)
			BetterCheats::Panels::Inventory::ShutdownStackSizes();
		if (g_initRan.Movement)
			BetterCheats::Panels::Movement::Shutdown();
		if (g_initRan.Weapons)
			BetterCheats::Panels::Weapons::Shutdown();
		if (g_initRan.Wave)
			BetterCheats::Panels::Wave::Shutdown();
		if (g_initRan.WindMute)
			BetterCheats::Panels::Wind::Shutdown();
		if (g_initRan.Corp)
			BetterCheats::Panels::Corporations::Shutdown();
		if (g_initRan.Enemies)
			BetterCheats::Panels::Enemies::Shutdown();
#if BETTERCHEATS_DEV_BUILD
		BetterCheats::Panels::DevMenus::Shutdown();
#endif
		BetterCheats::SessionConfig::Shutdown();

		g_initRan = {};
		g_wasOn = {};
		g_gateOpen = g_gateReleased = g_loadWaiting = false;
		g_sessionCurrent = false;
		g_self = nullptr;
	}

} // extern "C"
