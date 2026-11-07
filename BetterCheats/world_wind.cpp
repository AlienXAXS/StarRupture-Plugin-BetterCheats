#include "world_wind.h"
#include "plugin_helpers.h"
#include "plugin_config.h"
#include "game_context.h"
#include "map_fields.h"
#include "object_ref.h"

#include "Chimera_classes.hpp"
#include "Engine_classes.hpp"

#include <atomic>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace BetterCheats::Panels::Wind
{
	namespace
	{
		// The wind loops the game keeps playing behind its menus. The game drives
		// their volume from weather curves, so the mute is a volume multiplier of
		// zero that is held every tick, not a one-off write the game could undo.
		constexpr const char* kWindSounds[] = {
			"S_Wind1_Loop", "S_Wind2_Loop", "S_Wind3_Loop", "S_WindMountain1_Loop",
			"S_WindWave_Loop", "S_Wind_2d_loop_01_sweetener"
		};

		// A full pass over GObjects, so it runs when the menu opens and then on an
		// interval, not every frame. Short enough that a loop the game starts while the
		// menu is up is silenced within a second; components are caught whether or not
		// they are playing yet, so only one created and gone inside the interval slips by.
		constexpr float kRescanSeconds = 1.0f;

		// How long a closed answer must hold before the volumes go back. The pause
		// flag lags a frame and may blink while the menu moves between screens.
		constexpr float kReleaseDelaySeconds = 0.25f;

		constexpr float kRetryControllerSeconds = 1.0f;
		constexpr float kSilent = 0.001f;

		// Written by the config page and the Misc panel, read on the tick.
		std::atomic<bool> g_option{ false };

		struct Muted
		{
			ObjectRef<SDK::UAudioComponent> comp;
			// The last non-zero volume the game had on the component. Written back only
			// if the component still sits at zero, so a value the game set itself in the
			// meantime is never overwritten.
			float gameVolume = 1.0f;
		};

		// Game thread only.
		std::vector<Muted>                       g_muted;
		bool                                     g_active = false;
		float                                    g_rescanTimer = 0.0f;
		float                                    g_releaseTimer = 0.0f;
		ObjectRef<SDK::ACrPlayerControllerBase>  g_controller;
		float                                    g_controllerRetry = 0.0f;

		// By asset name alone: the SDK's GetName() on a package returns only its last
		// path segment, so a full-name test for the Ambience folder never matches. The
		// zipline and wind turbine sounds also carry "Wind" but are not listed.
		bool IsWindSound(const SDK::UObject* sound)
		{
			if (!sound)
				return false;
			const std::string name = sound->GetName();
			for (const char* wind : kWindSounds)
			{
				if (name == wind)
					return true;
			}
			return false;
		}

		// Dev diagnostic state. Game thread only.
		std::vector<std::string> g_devSeen;     // distinct sound assets already logged this menu session
		int                      g_devLoader = -1;
		int                      g_devController = -1;
		bool                     g_devFieldReported = false;
		int                      g_devTicks = 0;
		int                      g_devRewrites = 0;
		int                      g_devHeldPeak = 0;
		float                    g_devLogTimer = 0.0f;
		// Faster than the mute's own rescan, so a sound it would miss still shows in the log.
		constexpr float          kDevLogSeconds = 0.25f;

		bool ControllerInPauseMenu(float deltaSeconds);

		constexpr uint32_t kDevSkip =
			static_cast<uint32_t>(SDK::EObjectFlags::ClassDefaultObject) |
			static_cast<uint32_t>(SDK::EObjectFlags::ArchetypeObject) |
			static_cast<uint32_t>(SDK::EObjectFlags::BeginDestroyed) |
			static_cast<uint32_t>(SDK::EObjectFlags::FinishDestroyed) |
			static_cast<uint32_t>(SDK::EObjectFlags::MirroredGarbage);

		bool Holds(const SDK::UAudioComponent* comp);

		// Edge-logs the two pause signals, and once what the controller flag resolved to.
		void DevLogSignals(float deltaSeconds)
		{
			IPluginSelf* self = GetSelf();
			if (!self)
				return;
			const int loader = (self->hooks->GameMenu && self->hooks->GameMenu->IsPauseMenuOpen()) ? 1 : 0;
			const int controller = ControllerInPauseMenu(deltaSeconds) ? 1 : 0;

			SDK::ACrPlayerControllerBase* pc = g_controller.Get();
			if (pc && !g_devFieldReported)
			{
				g_devFieldReported = true;
				const bool* flag = Fields::CrPlayerControllerBase_bReplInPauseMenu.In(self->hooks->ObjectProperties, pc);
				LOG_INFO("Wind dev: controller resolved, bReplInPauseMenu field %s.", flag ? "found" : "NOT found");
			}
			if (loader != g_devLoader || controller != g_devController)
			{
				g_devLoader = loader;
				g_devController = controller;
				LOG_INFO("Wind dev: pause signals loader=%d controller=%d", loader, controller);
			}
		}

		// Logs each playing audio component's sound once per distinct asset while the menu
		// is up: owner class, the match flag, whether it is held, and the pause signals.
		void DevLogNewPlaying()
		{
			SDK::TUObjectArray* arr = SDK::UObject::GObjects.GetTypedPtr();
			SDK::UClass* audioClass = SDK::UAudioComponent::StaticClass();
			if (!arr || !audioClass)
				return;

			for (int i = 0; i < arr->Num(); ++i)
			{
				SDK::UObject* obj = arr->GetByIndex(i);
				if (!obj || obj->Class != audioClass)
					continue;
				if (static_cast<uint32_t>(obj->Flags) & kDevSkip)
					continue;

				auto* comp = static_cast<SDK::UAudioComponent*>(obj);
				if (!comp->IsPlaying())
					continue;

				SDK::USoundBase** sound = Fields::AudioComponent_Sound.In(comp);
				float* volume = Fields::AudioComponent_VolumeMultiplier.In(comp);
				const SDK::UObject* soundObj = sound ? *sound : nullptr;
				const std::string path = soundObj ? soundObj->GetFullName() : std::string("(none)");
				bool seen = false;
				for (const std::string& entry : g_devSeen)
				{
					if (entry == path)
					{
						seen = true;
						break;
					}
				}
				if (seen)
					continue;
				g_devSeen.push_back(path);

				const std::string owner = (comp->Outer && comp->Outer->Class) ? comp->Outer->Class->GetName() : std::string("(none)");
				LOG_INFO("Wind dev: playing sound=%s owner=%s volume=%.3f wind=%s held=%s loader=%d controller=%d",
					path.c_str(), owner.c_str(), volume ? *volume : -1.0f,
					IsWindSound(soundObj) ? "yes" : "no", Holds(comp) ? "yes" : "no", g_devLoader, g_devController);
			}
		}

		// Once per menu open: audio components whose class is a subclass of UAudioComponent,
		// which the exact-class walks would skip.
		void DevLogSubclasses()
		{
			SDK::TUObjectArray* arr = SDK::UObject::GObjects.GetTypedPtr();
			SDK::UClass* audioClass = SDK::UAudioComponent::StaticClass();
			if (!arr || !audioClass)
				return;

			int count = 0;
			for (int i = 0; i < arr->Num(); ++i)
			{
				SDK::UObject* obj = arr->GetByIndex(i);
				if (!obj || obj->Class == audioClass || !obj->Class)
					continue;
				if (static_cast<uint32_t>(obj->Flags) & kDevSkip)
					continue;
				if (!obj->IsA(audioClass))
					continue;
				LOG_INFO("Wind dev: UAudioComponent subclass instance class=%s", obj->Class->GetName().c_str());
				++count;
			}
			LOG_INFO("Wind dev: %d subclass audio components.", count);
		}

		bool Holds(const SDK::UAudioComponent* comp)
		{
			for (const Muted& entry : g_muted)
			{
				if (entry.comp.Get() == comp)
					return true;
			}
			return false;
		}

		// Matches on the cached UClass* rather than the class name: GetName() on every
		// object in GObjects is what made enemies.cpp's scan stall the frame.
		void RescanWind()
		{
			SDK::TUObjectArray* arr = SDK::UObject::GObjects.GetTypedPtr();
			SDK::UClass* audioClass = SDK::UAudioComponent::StaticClass();
			if (!arr || !audioClass)
				return;

			constexpr uint32_t kSkip =
				static_cast<uint32_t>(SDK::EObjectFlags::ClassDefaultObject) |
				static_cast<uint32_t>(SDK::EObjectFlags::ArchetypeObject) |
				static_cast<uint32_t>(SDK::EObjectFlags::BeginDestroyed) |
				static_cast<uint32_t>(SDK::EObjectFlags::FinishDestroyed) |
				static_cast<uint32_t>(SDK::EObjectFlags::MirroredGarbage);

			for (int i = 0; i < arr->Num(); ++i)
			{
				SDK::UObject* obj = arr->GetByIndex(i);
				if (!obj || obj->Class != audioClass)
					continue;
				if (static_cast<uint32_t>(obj->Flags) & kSkip)
					continue;

				auto* comp = static_cast<SDK::UAudioComponent*>(obj);
				if (Holds(comp))
					continue;

				SDK::USoundBase** sound = Fields::AudioComponent_Sound.In(comp);
				float* volume = Fields::AudioComponent_VolumeMultiplier.In(comp);
				if (!sound || !volume || !IsWindSound(*sound))
					continue;

				Muted entry;
				entry.comp.Set(comp);
				entry.gameVolume = *volume;
				g_muted.push_back(entry);
			}
		}

		// Keeps every wind component silent, and notes any volume the game wrote in
		// between so it can be given back.
		void HoldMuted()
		{
			for (size_t i = 0; i < g_muted.size();)
			{
				SDK::UAudioComponent* comp = g_muted[i].comp.Get();
				float* volume = comp ? Fields::AudioComponent_VolumeMultiplier.In(comp) : nullptr;
				if (!volume)
				{
					g_muted.erase(g_muted.begin() + i);
					continue;
				}

				if (std::fabs(*volume) > kSilent)
				{
					g_muted[i].gameVolume = *volume;
					comp->SetVolumeMultiplier(0.0f);
				}
				++i;
			}
		}

		void ReleaseMuted()
		{
			for (Muted& entry : g_muted)
			{
				SDK::UAudioComponent* comp = entry.comp.Get();
				float* volume = comp ? Fields::AudioComponent_VolumeMultiplier.In(comp) : nullptr;
				if (volume && std::fabs(*volume) <= kSilent && std::fabs(entry.gameVolume) > kSilent)
					comp->SetVolumeMultiplier(entry.gameVolume);
			}
			g_muted.clear();
			g_active = false;
		}

		// Counts the held components whose volume the game raised again since the last
		// tick, which says whether the hold fights a per-frame write.
		void DevCountRewrites()
		{
			++g_devTicks;
			if (static_cast<int>(g_muted.size()) > g_devHeldPeak)
				g_devHeldPeak = static_cast<int>(g_muted.size());
			for (const Muted& entry : g_muted)
			{
				SDK::UAudioComponent* comp = entry.comp.Get();
				float* volume = comp ? Fields::AudioComponent_VolumeMultiplier.In(comp) : nullptr;
				if (volume && std::fabs(*volume) > kSilent)
					++g_devRewrites;
			}
		}

		bool ControllerInPauseMenu(float deltaSeconds)
		{
			IPluginSelf* self = GetSelf();
			if (!self)
				return false;

			SDK::ACrPlayerControllerBase* pc = g_controller.Get();
			if (!pc)
			{
				g_controllerRetry -= deltaSeconds;
				if (g_controllerRetry > 0.0f)
					return false;
				g_controllerRetry = kRetryControllerSeconds;

				SDK::UWorld* world = PluginSdkRuntime::GetWorld();
				SDK::UClass* controllerClass = SDK::ACrPlayerControllerBase::StaticClass();
				if (!world || !controllerClass)
					return false;
				SDK::APlayerController* controller = SDK::UGameplayStatics::GetPlayerController(world, 0);
				if (!controller || !controller->IsA(controllerClass))
					return false;
				g_controller.Set(static_cast<SDK::ACrPlayerControllerBase*>(controller));
				pc = g_controller.Get();
			}

			const bool* inMenu = Fields::CrPlayerControllerBase_bReplInPauseMenu.In(self->hooks->ObjectProperties, pc);
			return inMenu && *inMenu;
		}

		// The pause menu or one of its screens is up. The loader's flag drops on some
		// screens the menu opens, so the controller's own flag counts too.
		bool IsPauseMenuUp(float deltaSeconds)
		{
			IPluginSelf* self = GetSelf();
			if (!self)
				return false;
			if (self->hooks->GameMenu && self->hooks->GameMenu->IsPauseMenuOpen())
				return true;
			return ControllerInPauseMenu(deltaSeconds);
		}

		void OnConfigChanged(const char* section, const char* key, const char* /*newValue*/)
		{
			if (section && key && !std::strcmp(section, "Audio") && !std::strcmp(key, "MuteWindInPauseMenu"))
				g_option = BetterCheatsConfig::Config::IsMuteWindInPauseMenuEnabled();
		}

		void OnWorldEndPlay(SDK::UWorld* /*world*/, const char* /*name*/)
		{
			ReleaseMuted();
			g_controller.Reset();
			g_controllerRetry = 0.0f;
		}
	}

	void Initialize()
	{
		IPluginSelf* self = GetSelf();
		if (!self)
			return;

		g_option = BetterCheatsConfig::Config::IsMuteWindInPauseMenuEnabled();
		self->hooks->UI->RegisterOnConfigChanged(self, &OnConfigChanged);
		self->hooks->World->RegisterOnAfterWorldEndPlay(&OnWorldEndPlay);
	}

	void Shutdown()
	{
		IPluginSelf* self = GetSelf();
		if (!self)
			return;

		self->hooks->UI->UnregisterOnConfigChanged(self, &OnConfigChanged);
		self->hooks->World->UnregisterOnAfterWorldEndPlay(&OnWorldEndPlay);

		// The loader can run shutdown off the game thread (process exit); a component
		// is only touched where the engine allows it.
		if (self->hooks->Engine->IsGameThread())
			ReleaseMuted();
		else
			g_muted.clear();
		g_controller.Reset();
	}

	void Tick(float deltaSeconds)
	{
		const bool inWorld = GameContext::IsInChimeraMain();
		const bool option = g_option.load();

		if (option && inWorld)
		{
			try { DevLogSignals(deltaSeconds); }
			catch (...) { LOG_WARN("Wind dev: exception reading pause signals."); }
		}

		if (option && inWorld && IsPauseMenuUp(deltaSeconds))
		{
			g_releaseTimer = kReleaseDelaySeconds;
			if (!g_active)
			{
				g_active = true;
				g_rescanTimer = 0.0f;
				g_devSeen.clear();
				g_devTicks = 0;
				g_devRewrites = 0;
				g_devHeldPeak = 0;
				g_devLogTimer = 0.0f;
				try { DevLogSubclasses(); }
				catch (...) { LOG_WARN("Wind dev: exception logging subclasses."); }
			}

			DevCountRewrites();
			g_rescanTimer -= deltaSeconds;
			if (g_rescanTimer <= 0.0f)
			{
				g_rescanTimer = kRescanSeconds;
				try { RescanWind(); }
				catch (...) { LOG_WARN("Wind: exception scanning GObjects."); }
			}
			g_devLogTimer -= deltaSeconds;
			if (g_devLogTimer <= 0.0f)
			{
				g_devLogTimer = kDevLogSeconds;
				try { DevLogNewPlaying(); }
				catch (...) { LOG_WARN("Wind dev: exception logging playing sounds."); }
			}
			HoldMuted();
			return;
		}

		if (!g_active)
			return;

		// Turning the option off, or leaving the world, gives the sound back at once;
		// a menu that merely blinks is held for a moment first.
		if (option && inWorld)
		{
			g_releaseTimer -= deltaSeconds;
			if (g_releaseTimer > 0.0f)
			{
				HoldMuted();
				return;
			}
		}
		LOG_INFO("Wind dev: menu closed; held up to %d components over %d ticks, game raised a held volume %d times.",
			g_devHeldPeak, g_devTicks, g_devRewrites);
		ReleaseMuted();
	}

	void RenderImGui(IModLoaderImGui* imgui)
	{
		bool enabled = g_option.load();
		if (imgui->Checkbox("Mute wind in the pause menu", &enabled))
		{
			BetterCheatsConfig::Config::SetMuteWindInPauseMenu(enabled);
			g_option = enabled;
		}
		imgui->SetItemTooltip("Silences the wind loops while the pause menu is open. "
			"Other sounds and the menu music are not touched.");
	}
}
