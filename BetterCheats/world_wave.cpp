#include "world_wave.h"
#include "plugin_helpers.h"
#include "session_config.h"
#include "aob_patterns.h"
#include "map_fields.h"
#include "object_ref.h"
#include "game_thread.h"

#include "Chimera_classes.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>

namespace BetterCheats::Panels::Wave
{
	namespace
	{
		// -------------------------------------------------------------------------
		// Subsystem cache — rescanned once per world change via GObjects iteration.
		// Matches by class name and excludes the CDO, same as the pattern used by
		// other plugins in this project.
		// -------------------------------------------------------------------------
		static ObjectRef<SDK::UCrEnviroWaveSubsystem>      g_waveSubsys;
		static ObjectRef<SDK::UCrEnviroWaveTimerSubsystem> g_timerSubsys;
		static bool                                        g_scanned     = false;

		// The configured calm length. The game assigns NextWaveTimer from its
		// WaitingDuration and the cheat only ever writes it back, so the largest
		// value seen between waves is that length. Zero until one is seen.
		static float                             g_calmLength  = 0.0f;

		// The subsystems don't necessarily exist on the first tick after world begin
		// play, and giving up after a single miss left the panel dead for the rest of
		// the session — retry on a slow cadence, then stop so a genuinely absent
		// subsystem doesn't cost a GObjects sweep every second forever.
		constexpr float kScanRetrySeconds = 1.0f;
		constexpr int   kMaxScanAttempts  = 10;

		static float g_scanRetryTimer = 0.0f;
		static int   g_scanAttempts   = 0;

		// Matches on cached UClass* pointers rather than obj->Class->GetName(). This
		// scan runs while the async loading thread is still registering UObjects, so
		// GObjects hands back entries whose Class pointer is garbage; comparing the
		// pointer never dereferences it, whereas GetName() walked it into the FName
		// pool and faulted. Same approach as enemies.cpp's RescanMassSubsystems.
		void FindWaveSubsystems()
		{
			SDK::TUObjectArray* arr = SDK::UObject::GObjects.GetTypedPtr();
			if (!arr)
			{
				LOG_WARN("Wave: GObjects array is null — scan aborted.");
				return;
			}

			SDK::UClass* waveClass  = SDK::UCrEnviroWaveSubsystem::StaticClass();
			SDK::UClass* timerClass = SDK::UCrEnviroWaveTimerSubsystem::StaticClass();
			if (!waveClass || !timerClass)
			{
				LOG_WARN("Wave: wave subsystem UClass unavailable — scan aborted.");
				return;
			}

			const SDK::UObject* waveCDO  = SDK::UCrEnviroWaveSubsystem::GetDefaultObj();
			const SDK::UObject* timerCDO = SDK::UCrEnviroWaveTimerSubsystem::GetDefaultObj();

			bool doneWave  = false;
			bool doneTimer = false;

			for (int i = 0; i < arr->Num() && (!doneWave || !doneTimer); ++i)
			{
				SDK::UObject* obj = arr->GetByIndex(i);
				if (!obj || !obj->Class) continue;

				if (!doneWave && obj != waveCDO && obj->Class == waveClass)
				{
					g_waveSubsys.Set(static_cast<SDK::UCrEnviroWaveSubsystem*>(obj));
					doneWave = true;
					continue;
				}
				if (!doneTimer && obj != timerCDO && obj->Class == timerClass)
				{
					g_timerSubsys.Set(static_cast<SDK::UCrEnviroWaveTimerSubsystem*>(obj));
					doneTimer = true;
					continue;
				}
			}
		}

		void RunObjectScan()
		{
			g_waveSubsys.Reset();
			g_timerSubsys.Reset();
			g_calmLength = 0.0f;
			++g_scanAttempts;

			FindWaveSubsystems();

			const bool haveWave  = g_waveSubsys.Get() != nullptr;
			const bool haveTimer = g_timerSubsys.Get() != nullptr;
			if (haveWave && haveTimer)
			{
				g_scanned = true;
				return;
			}

			if (g_scanAttempts >= kMaxScanAttempts)
			{
				g_scanned = true;
				if (!haveWave)  LOG_INFO("Wave: UCrEnviroWaveSubsystem isn't available in this world.");
				if (!haveTimer) LOG_INFO("Wave: UCrEnviroWaveTimerSubsystem isn't available in this world.");
			}
		}

		// -------------------------------------------------------------------------
		// Snapshot — populated on game thread (Tick), read on ImGui render thread.
		// Never touch SDK objects from RenderImGui().
		// -------------------------------------------------------------------------
		struct WaveSnapshot
		{
			bool subsysFound = false;
			bool inProgress  = false;
			bool isPaused    = false;
			SDK::EEnviroWave      waveType  = SDK::EEnviroWave::None;
			SDK::EEnviroWaveStage waveStage = SDK::EEnviroWaveStage::None;
			float                 progress  = 0.0f;

			// Countdown state from UCrEnviroWaveTimerSubsystem (see aob_patterns.h).
			bool  timerFound         = false;
			bool  waitingForNextWave = false;
			bool  timerPaused        = false;
			bool  stopWaves          = false;
			float secondsRemaining   = 0.0f;
		};

		// The fields that drive the countdown sit inside the SDK's Pad_30 on the
		// timer subsystem, so they are map fields (see map_fields.h).
		template <typename T>
		T TimerRead(const PluginField<T>& field, T fallback)
		{
			const T* value = field.In(g_timerSubsys.Get());
			return value ? *value : fallback;
		}

		template <typename T>
		void TimerWrite(const PluginField<T>& field, T value)
		{
			if (T* slot = field.In(g_timerSubsys.Get()))
				*slot = value;
		}

		// The game pushes the subsystem's bPause to the actor via
		// NativeOnPauseChanged (not a UFunction); mirror it by hand so the HUD
		// view model, which reads the actor, agrees with the real state.
		void WriteTimerPause(bool paused)
		{
			TimerWrite(BetterCheats::Fields::CrEnviroWaveTimerSubsystem_bPause, paused);
			SDK::ACrWaveTimerActor* timerActor = BetterCheats::Fields::ValueOr(BetterCheats::Fields::CrEnviroWaveTimerSubsystem_TimerActor, g_timerSubsys.Get());
			BetterCheats::Fields::Store(BetterCheats::Fields::CrWaveTimerActor_bPause, timerActor, paused);
		}

		std::mutex   g_snapshotMutex;
		WaveSnapshot g_snapshot;

		void RefreshSnapshot()
		{
			WaveSnapshot snap;
			if (SDK::UCrEnviroWaveSubsystem* wave = g_waveSubsys.Get())
			{
				snap.subsysFound = true;
				try
				{
					snap.inProgress = wave->IsWaveInProgress();
					snap.isPaused   = wave->IsWavePaused();
					snap.waveType   = wave->GetCurrentType();
					snap.waveStage  = wave->GetCurrentStage();
					snap.progress   = wave->GetCurrentStageProgress();
				}
				catch (...)
				{
					LOG_WARN("Wave: exception reading wave subsystem state.");
				}
			}
			if (g_timerSubsys.Get())
			{
				snap.timerFound = true;
				try
				{
					snap.waitingForNextWave = TimerRead(BetterCheats::Fields::CrEnviroWaveTimerSubsystem_bWaitingForNextWave, false);
					snap.timerPaused        = TimerRead(BetterCheats::Fields::CrEnviroWaveTimerSubsystem_bPause, false);
					snap.stopWaves          = TimerRead(BetterCheats::Fields::CrEnviroWaveTimerSubsystem_bStopWaves, false);
					snap.secondsRemaining   = TimerRead(BetterCheats::Fields::CrEnviroWaveTimerSubsystem_NextWaveTimer, 0.0f);
					if (!snap.inProgress && snap.secondsRemaining > g_calmLength)
						g_calmLength = snap.secondsRemaining;
				}
				catch (...)
				{
					LOG_WARN("Wave: exception reading wave timer state.");
				}
			}
			std::lock_guard<std::mutex> lock(g_snapshotMutex);
			g_snapshot = snap;
		}

		// -------------------------------------------------------------------------
		// Pending action — queued from the ImGui render thread, applied on the
		// game thread in Tick(). Last write wins within a single frame.
		// -------------------------------------------------------------------------
		enum class PendingAction : int { None, Pause, Resume, Cancel, SkipSegment, WavesOn, WavesOff, StartHeat, RestartTimer };

		std::atomic<int> g_pendingAction{ static_cast<int>(PendingAction::None) };

		// -------------------------------------------------------------------------
		// "Pause Waves Entirely" — pins UCrEnviroWaveTimerSubsystem::bPause so the
		// countdown never advances, even with no wave currently active. The game
		// persists that flag into the save, so it's only written while the cheat
		// is on and cleared once on toggle-off — writing it every tick regardless
		// would silently override the game's own pause state.
		// -------------------------------------------------------------------------
		std::atomic<bool> g_pauseWavesEnabled{ false };
		static bool       g_pauseWritten = false;
		// What the two pause flags read before the first write of ours.
		static bool       g_earlierPause = false;
		static bool       g_earlierActorPause = false;
		// The session config remembers that our pause is in, because the game
		// saves it: a true read while this is set is ours, not an earlier value.
		constexpr const char* kPauseMark = "wave.pauseWritten";

		// The one-shot Pause button paused the current wave.
		static bool g_wavePaused = false;
		// Whether waves were running before the first Waves On/Off press, and what
		// the last press set.
		static std::optional<bool> g_wavesBefore;
		static bool g_wavesSet = false;

		SDK::ACrWaveTimerActor* TimerActor()
		{
			return BetterCheats::Fields::ValueOr(BetterCheats::Fields::CrEnviroWaveTimerSubsystem_TimerActor, g_timerSubsys.Get());
		}

		void MarkPauseWritten(bool written)
		{
			g_pauseWritten = written;
			SessionConfig::Set(kPauseMark, written);
		}

		void CaptureWavePause()
		{
			const bool ours = SessionConfig::Get(kPauseMark, false);
			const bool* actorPause = BetterCheats::Fields::CrWaveTimerActor_bPause.In(TimerActor());
			g_earlierPause      = !ours && TimerRead(BetterCheats::Fields::CrEnviroWaveTimerSubsystem_bPause, false);
			g_earlierActorPause = !ours && actorPause && *actorPause;
		}

		// Hands each flag its earlier value, and only where it still reads as paused.
		void ClearWavePause()
		{
			if (!g_pauseWritten)
				return;

			if (TimerRead(BetterCheats::Fields::CrEnviroWaveTimerSubsystem_bPause, false))
				TimerWrite(BetterCheats::Fields::CrEnviroWaveTimerSubsystem_bPause, g_earlierPause);
			SDK::ACrWaveTimerActor* timerActor = TimerActor();
			const bool* actorPause = BetterCheats::Fields::CrWaveTimerActor_bPause.In(timerActor);
			if (actorPause && *actorPause)
				BetterCheats::Fields::Store(BetterCheats::Fields::CrWaveTimerActor_bPause, timerActor, g_earlierActorPause);
			MarkPauseWritten(false);
		}

		void SetWavesActive(SDK::UCrEnviroWaveTimerSubsystem* timer, bool active)
		{
			const bool* stopped = BetterCheats::Fields::CrEnviroWaveTimerSubsystem_bStopWaves.In(timer);
			if (!g_wavesBefore && stopped)
				g_wavesBefore = !*stopped;
			g_wavesSet = active;
			timer->WavesActive(active);
		}

		void EnforceWavePause()
		{
			if (!g_timerSubsys.Get())
				return;

			try
			{
				if (g_pauseWavesEnabled.load())
				{
					const bool first = !g_pauseWritten;
					if (first)
						CaptureWavePause();
					WriteTimerPause(true);
					if (first)
						MarkPauseWritten(true);
				}
				else
				{
					ClearWavePause();
				}
			}
			catch (...)
			{
				LOG_WARN("Wave: exception enforcing wave pause.");
			}
		}

		// Puts the countdown back into its between-waves state. The game only
		// re-arms it from OnWaveFinished, which CancelCurrentWave never reaches,
		// and WavesActive(false) / a stale bPause are both written into the save —
		// so a broken cycle stays broken across reloads unless something does this.
		void RearmTimer()
		{
			SDK::UCrEnviroWaveTimerSubsystem* timer = g_timerSubsys.Get();
			if (!timer)
				return;

			if (g_calmLength > 0.0f)
				TimerWrite(BetterCheats::Fields::CrEnviroWaveTimerSubsystem_NextWaveTimer, g_calmLength);
			// Sets bWaitingForNextWave, clears bStopWaves and re-arms the HUD's NextTime.
			timer->WavesActive(true);
			// A restarted timer stays restarted: there is nothing earlier to go back to.
			g_wavesBefore.reset();
			g_earlierPause = g_earlierActorPause = false;
			const bool paused = g_pauseWavesEnabled.load();
			WriteTimerPause(paused);
			MarkPauseWritten(paused);
		}

		void ApplyPendingAction()
		{
			const auto action = static_cast<PendingAction>(
				g_pendingAction.exchange(static_cast<int>(PendingAction::None)));

			if (action == PendingAction::None)
				return;

			SDK::UCrEnviroWaveSubsystem*      wave  = g_waveSubsys.Get();
			SDK::UCrEnviroWaveTimerSubsystem* timer = g_timerSubsys.Get();
			try
			{
				switch (action)
				{
					case PendingAction::Pause:
						if (wave && !wave->IsWavePaused())
						{
							wave->PauseCurrentWave();
							g_wavePaused = true;
						}
						break;
					case PendingAction::Resume:
						if (wave)
						{
							wave->ResumeCurrentWave();
							g_wavePaused = false;
						}
						break;
					case PendingAction::Cancel:
						if (wave)
						{
							wave->CancelCurrentWave();
							g_wavePaused = false;
							RearmTimer();
						}
						break;
					case PendingAction::RestartTimer: RearmTimer();                                      break;
					case PendingAction::SkipSegment:  if (wave)  wave->ForceWaveStageProgress(1.0f);     break;
					case PendingAction::WavesOn:      if (timer) SetWavesActive(timer, true);            break;
					case PendingAction::WavesOff:     if (timer) SetWavesActive(timer, false);           break;
					case PendingAction::StartHeat:
						if (wave)
						{
							wave->StartWave(SDK::EEnviroWave::Heat);
							g_wavePaused = false;
						}
						break;
					default: break;
				}
			}
			catch (...)
			{
				LOG_ERROR("Wave: exception executing action %d.", static_cast<int>(action));
			}
		}

		// -------------------------------------------------------------------------
		// Table layout — matches player_attributes.cpp
		// ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp
		// -------------------------------------------------------------------------
		constexpr int kTableFlags  = (1 << 6) | (1 << 9) | (3 << 13);
		constexpr int kColumnFixed = 1 << 4; // ImGuiTableColumnFlags_WidthFixed

		void SetupWaveTableColumns(IModLoaderImGui* imgui)
		{
			imgui->TableSetupColumn("Property", kColumnFixed, 130.0f);
			imgui->TableSetupColumn("Value",    0,            0.0f);
			imgui->TableSetupColumn("Action",   kColumnFixed, 80.0f);
		}

		const char* WaveTypeName(SDK::EEnviroWave t)
		{
			switch (t)
			{
			case SDK::EEnviroWave::Heat: return "Heat Wave";
			default:                     return "None";
			}
		}

		const char* WaveStageName(SDK::EEnviroWaveStage s)
		{
			switch (s)
			{
			case SDK::EEnviroWaveStage::PreWave:  return "Pre-Wave";
			case SDK::EEnviroWaveStage::Moving:   return "Moving";
			case SDK::EEnviroWaveStage::Fadeout:  return "Fadeout";
			case SDK::EEnviroWaveStage::Growback: return "Growback";
			default:                              return "None";
			}
		}
	}

	namespace
	{
		void OnWorldBeginPlay(SDK::UWorld* /*world*/)
		{
			g_scanned        = false;
			g_scanRetryTimer = 0.0f;
			g_scanAttempts   = 0;
		}
		void OnWorldEndPlay(SDK::UWorld* /*world*/, const char* /*name*/)
		{
			g_waveSubsys.Reset();
			g_timerSubsys.Reset();
			g_calmLength     = 0.0f;
			g_pauseWritten   = false;
			g_wavePaused     = false;
			g_wavesBefore.reset();
			g_scanned        = false;
			g_scanRetryTimer = 0.0f;
			g_scanAttempts   = 0;
		}
	}

	void Initialize()
	{
		if (IPluginSelf* self = GetSelf())
		{
			self->hooks->World->RegisterOnWorldBeginPlay(&OnWorldBeginPlay);
			self->hooks->World->RegisterOnAfterWorldEndPlay(&OnWorldEndPlay);
		}
	}

	void Shutdown()
	{
		if (IPluginSelf* self = GetSelf())
		{
			self->hooks->World->UnregisterOnWorldBeginPlay(&OnWorldBeginPlay);
			self->hooks->World->UnregisterOnAfterWorldEndPlay(&OnWorldEndPlay);
		}
		// A pause this plugin wrote would outlive a reload.
		if (BetterCheats::IsGameThread())
			ReleaseValues();
		g_waveSubsys.Reset();
		g_timerSubsys.Reset();
		g_pauseWritten   = false;
		g_wavePaused     = false;
		g_wavesBefore.reset();
		g_scanned        = false;
		g_scanRetryTimer = 0.0f;
		g_scanAttempts   = 0;
	}

	void DropPendingAction()
	{
		g_pendingAction.store(static_cast<int>(PendingAction::None));
	}

	void ReleaseValues()
	{
		DropPendingAction();

		try
		{
			SDK::UCrEnviroWaveSubsystem* wave = g_waveSubsys.Get();
			if (g_wavePaused && wave && wave->IsWavePaused())
				wave->ResumeCurrentWave();
			g_wavePaused = false;

			// The Pause Waves Entirely choice stays; only the write into the timer is
			// undone, and Tick writes it again if the section comes back on.
			SDK::UCrEnviroWaveTimerSubsystem* timer = g_timerSubsys.Get();
			if (!timer)
				return;

			// Only where the waves are still as the last press left them.
			const bool* stopped = BetterCheats::Fields::CrEnviroWaveTimerSubsystem_bStopWaves.In(timer);
			if (g_wavesBefore && stopped && !*stopped == g_wavesSet && *g_wavesBefore != g_wavesSet)
				timer->WavesActive(*g_wavesBefore);
			g_wavesBefore.reset();

			ClearWavePause();
		}
		catch (...) { LOG_WARN("Wave: exception handing back wave state."); }
	}

	void Tick(float deltaSeconds)
	{
		if (!g_scanned)
		{
			g_scanRetryTimer -= deltaSeconds;
			if (g_scanRetryTimer <= 0.0f)
			{
				g_scanRetryTimer = kScanRetrySeconds;
				RunObjectScan();
			}
		}

		ApplyPendingAction();
		EnforceWavePause();
		RefreshSnapshot();
	}

	void ApplySavedConfig()
	{
		g_pauseWavesEnabled = SessionConfig::Get("wave.pauseWavesEnabled", false);
	}

	void RenderImGui(IModLoaderImGui* imgui, bool greyed)
	{
		// A greyed page draws the same rows and every button, with "-" where a
		// value would be. It reads no game state and no saved setting.
		WaveSnapshot snap;
		if (greyed)
		{
			snap.subsysFound = true;
			snap.timerFound  = true;
		}
		else
		{
			std::lock_guard<std::mutex> lock(g_snapshotMutex);
			snap = g_snapshot;
		}
		const bool showWaveButtons = greyed || snap.inProgress;

		// ----- Status -----
		imgui->SeparatorText("Wave Status");

		if (!snap.subsysFound)
		{
			imgui->TextDisabled("Wave subsystem not found.");
		}
		else if (imgui->BeginTable("##wave_status", 3, kTableFlags))
		{
			SetupWaveTableColumns(imgui);

			// Type
			imgui->TableNextRow(0, 0.0f);
			imgui->TableSetColumnIndex(0); imgui->Text("Type");
			imgui->TableSetColumnIndex(1); imgui->Text(greyed ? "-" : WaveTypeName(snap.waveType));
			imgui->TableSetColumnIndex(2);

			// Stage
			imgui->TableNextRow(0, 0.0f);
			imgui->TableSetColumnIndex(0); imgui->Text("Stage");
			imgui->TableSetColumnIndex(1); imgui->Text(greyed ? "-" : WaveStageName(snap.waveStage));
			imgui->TableSetColumnIndex(2);
			if (showWaveButtons)
			{
				if (imgui->SmallButton("Skip##stage"))
					g_pendingAction.store(static_cast<int>(PendingAction::SkipSegment));
				imgui->SetItemTooltip("Force-advance the current stage to 100%%, triggering the next stage.");
			}

			// Progress
			imgui->TableNextRow(0, 0.0f);
			imgui->TableSetColumnIndex(0); imgui->Text("Progress");
			imgui->TableSetColumnIndex(1);
			{
				imgui->SetNextItemWidth(-1.0f);
				float prog = snap.progress;
				char fmt[16];
				if (greyed)
					snprintf(fmt, sizeof(fmt), "-");
				else
					snprintf(fmt, sizeof(fmt), "%.0f%%", prog * 100.0f);
				imgui->SliderFloat("##wave_prog", &prog, 0.0f, 1.0f, fmt);
			}
			imgui->TableSetColumnIndex(2);

			imgui->EndTable();
		}

		// ----- Wave Controls -----
		imgui->Spacing();
		imgui->SeparatorText("Wave Controls");

		if (!snap.subsysFound)
		{
			imgui->TextDisabled("Wave subsystem not found.");
		}
		else if (imgui->BeginTable("##wave_ctrl", 3, kTableFlags))
		{
			SetupWaveTableColumns(imgui);

			// Start Wave
			imgui->TableNextRow(0, 0.0f);
			imgui->TableSetColumnIndex(0); imgui->Text("Start Wave");
			imgui->TableSetColumnIndex(1);
			if (imgui->SmallButton("Heat"))
				g_pendingAction.store(static_cast<int>(PendingAction::StartHeat));
			imgui->TableSetColumnIndex(2);

			// Pause / Resume
			imgui->TableNextRow(0, 0.0f);
			imgui->TableSetColumnIndex(0); imgui->Text("Pause");
			imgui->TableSetColumnIndex(1);
			if (greyed)             imgui->Text("-");
			else if (!snap.inProgress) imgui->TextDisabled("Planet Stable");
			else if (snap.isPaused) imgui->Text("Paused");
			else                    imgui->Text("Running");
			imgui->TableSetColumnIndex(2);
			if (showWaveButtons)
			{
				if (snap.isPaused)
				{
					if (imgui->SmallButton("Resume"))
						g_pendingAction.store(static_cast<int>(PendingAction::Resume));
				}
				else
				{
					if (imgui->SmallButton("Pause"))
						g_pendingAction.store(static_cast<int>(PendingAction::Pause));
				}
			}

			// Cancel
			imgui->TableNextRow(0, 0.0f);
			imgui->TableSetColumnIndex(0); imgui->Text("Cancel Wave");
			imgui->TableSetColumnIndex(1); imgui->Text(greyed ? "-" : snap.inProgress ? "In Progress" : "Planet Stable");
			imgui->TableSetColumnIndex(2);
			if (showWaveButtons)
			{
				if (imgui->SmallButton("Cancel"))
					g_pendingAction.store(static_cast<int>(PendingAction::Cancel));
			}

			// Pause Waves Entirely
			{
				const bool pauseWaves = !greyed && g_pauseWavesEnabled.load();
				imgui->TableNextRow(0, 0.0f);
				imgui->TableSetColumnIndex(0); imgui->Text("Pause Waves");
				imgui->TableSetColumnIndex(1); imgui->Text(greyed ? "-" : pauseWaves ? "Paused" : "Running");
				imgui->TableSetColumnIndex(2);
				if (imgui->SmallButton(pauseWaves ? "Unpause##wave_timer" : "Pause##wave_timer"))
				{
					g_pauseWavesEnabled = !pauseWaves;
					SessionConfig::Set("wave.pauseWavesEnabled", !pauseWaves);
				}
				imgui->SetItemTooltip("Pins the wave timer so the next wave never arrives, "
					"even while the menu is closed.");
			}

			imgui->EndTable();
		}

		// ----- Wave Spawning Timer -----
		imgui->Spacing();
		imgui->SeparatorText("Wave Spawning Timer");

		if (!snap.timerFound)
		{
			imgui->TextDisabled("Timer subsystem not found.");
		}
		else if (imgui->BeginTable("##wave_timer", 3, kTableFlags))
		{
			SetupWaveTableColumns(imgui);

			// A countdown that is neither running nor deliberately stopped is
			// the "rupture never comes" state — and it lands in the save file,
			// so surface it with the repair right next to it.
			const bool stalled = !snap.inProgress && !snap.waitingForNextWave && !snap.stopWaves;

			imgui->TableNextRow(0, 0.0f);
			imgui->TableSetColumnIndex(0); imgui->Text("Next Wave");
			imgui->TableSetColumnIndex(1);
			if (greyed)
				imgui->Text("-");
			else if (snap.inProgress)
				imgui->TextDisabled("Wave in progress");
			else if (snap.stopWaves)
				imgui->Text("Waves disabled");
			else if (stalled)
				imgui->Text("Not scheduled (timer stalled)");
			else
			{
				const int total = static_cast<int>(snap.secondsRemaining > 0.0f ? snap.secondsRemaining : 0.0f);
				char buf[32];
				snprintf(buf, sizeof(buf), "%d:%02d%s", total / 60, total % 60, snap.timerPaused ? " (paused)" : "");
				imgui->Text(buf);
			}
			imgui->TableSetColumnIndex(2);
			if (imgui->SmallButton("Restart##wave_timer_restart"))
				g_pendingAction.store(static_cast<int>(PendingAction::RestartTimer));
			imgui->SetItemTooltip("Re-arms the countdown to a full waiting period and re-enables waves. "
				"Use this if the rupture never arrives after loading a save.");

			imgui->TableNextRow(0, 0.0f);
			imgui->TableSetColumnIndex(0); imgui->Text("Enable Waves");
			imgui->TableSetColumnIndex(1); imgui->TextDisabled("Activates the wave spawn timer");
			imgui->TableSetColumnIndex(2);
			if (imgui->SmallButton("Enable"))
				g_pendingAction.store(static_cast<int>(PendingAction::WavesOn));

			imgui->TableNextRow(0, 0.0f);
			imgui->TableSetColumnIndex(0); imgui->Text("Disable Waves");
			imgui->TableSetColumnIndex(1); imgui->TextDisabled("Stops new waves from spawning (saved with the game)");
			imgui->TableSetColumnIndex(2);
			if (imgui->SmallButton("Disable"))
				g_pendingAction.store(static_cast<int>(PendingAction::WavesOff));

			imgui->EndTable();
		}
	}
}
