#include "world_wave.h"
#include "plugin_helpers.h"
#include "session_config.h"
#include "aob_patterns.h"

#include "Chimera_classes.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace BetterCheats::Panels::Wave
{
	namespace
	{
		// -------------------------------------------------------------------------
		// Subsystem cache — rescanned once per world change via GObjects iteration.
		// Matches by class name and excludes the CDO, same as the pattern used by
		// other plugins in this project.
		// -------------------------------------------------------------------------
		static SDK::UCrEnviroWaveSubsystem*      g_waveSubsys  = nullptr;
		static SDK::UCrEnviroWaveTimerSubsystem* g_timerSubsys = nullptr;
		static bool                              g_scanned     = false;

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
					g_waveSubsys = static_cast<SDK::UCrEnviroWaveSubsystem*>(obj);
					doneWave = true;
					continue;
				}
				if (!doneTimer && obj != timerCDO && obj->Class == timerClass)
				{
					g_timerSubsys = static_cast<SDK::UCrEnviroWaveTimerSubsystem*>(obj);
					doneTimer = true;
					continue;
				}
			}
		}

		void RunObjectScan()
		{
			g_waveSubsys  = nullptr;
			g_timerSubsys = nullptr;
			++g_scanAttempts;

			FindWaveSubsystems();

			if (g_waveSubsys && g_timerSubsys)
			{
				g_scanned = true;
				return;
			}

			if (g_scanAttempts >= kMaxScanAttempts)
			{
				g_scanned = true;
				if (!g_waveSubsys)  LOG_WARN("Wave: UCrEnviroWaveSubsystem not found in GObjects.");
				if (!g_timerSubsys) LOG_WARN("Wave: UCrEnviroWaveTimerSubsystem not found in GObjects.");
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
		// timer subsystem, so they are addressed by offset (see aob_patterns.h).
		template <typename T>
		T& TimerField(std::ptrdiff_t offset)
		{
			return *reinterpret_cast<T*>(reinterpret_cast<std::uint8_t*>(g_timerSubsys) + offset);
		}

		// The game pushes the subsystem's bPause to the actor via
		// NativeOnPauseChanged (not a UFunction); mirror it by hand so the HUD
		// view model, which reads the actor, agrees with the real state.
		void WriteTimerPause(bool paused)
		{
			TimerField<bool>(AOB::kWaveTimerPauseOffset) = paused;
			if (g_timerSubsys->TimerActor)
				g_timerSubsys->TimerActor->bPause = paused;
		}

		std::mutex   g_snapshotMutex;
		WaveSnapshot g_snapshot;

		void RefreshSnapshot()
		{
			WaveSnapshot snap;
			if (g_waveSubsys)
			{
				snap.subsysFound = true;
				try
				{
					snap.inProgress = g_waveSubsys->IsWaveInProgress();
					snap.isPaused   = g_waveSubsys->IsWavePaused();
					snap.waveType   = g_waveSubsys->GetCurrentType();
					snap.waveStage  = g_waveSubsys->GetCurrentStage();
					snap.progress   = g_waveSubsys->GetCurrentStageProgress();
				}
				catch (...)
				{
					LOG_WARN("Wave: exception reading wave subsystem state.");
				}
			}
			if (g_timerSubsys)
			{
				snap.timerFound = true;
				try
				{
					snap.waitingForNextWave = TimerField<bool>(AOB::kWaveTimerWaitingForNextWaveOffset);
					snap.timerPaused        = TimerField<bool>(AOB::kWaveTimerPauseOffset);
					snap.stopWaves          = TimerField<bool>(AOB::kWaveTimerStopWavesOffset);
					snap.secondsRemaining   = TimerField<float>(AOB::kWaveTimerNextWaveTimerOffset);
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

		void EnforceWavePause()
		{
			if (!g_timerSubsys)
				return;

			try
			{
				if (g_pauseWavesEnabled.load())
				{
					WriteTimerPause(true);
					g_pauseWritten = true;
				}
				else if (g_pauseWritten)
				{
					WriteTimerPause(false);
					g_pauseWritten = false;
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
			if (!g_timerSubsys)
				return;

			TimerField<float>(AOB::kWaveTimerNextWaveTimerOffset) = TimerField<float>(AOB::kWaveTimerWaitingDurationOffset);
			// Sets bWaitingForNextWave, clears bStopWaves and re-arms the HUD's NextTime.
			g_timerSubsys->WavesActive(true);
			const bool paused = g_pauseWavesEnabled.load();
			WriteTimerPause(paused);
			g_pauseWritten = paused;
		}

		void ApplyPendingAction()
		{
			const auto action = static_cast<PendingAction>(
				g_pendingAction.exchange(static_cast<int>(PendingAction::None)));

			if (action == PendingAction::None)
				return;

			try
			{
				switch (action)
				{
					case PendingAction::Pause:        if (g_waveSubsys)  g_waveSubsys->PauseCurrentWave();                       break;
					case PendingAction::Resume:       if (g_waveSubsys)  g_waveSubsys->ResumeCurrentWave();                      break;
					case PendingAction::Cancel:
						if (g_waveSubsys)
						{
							g_waveSubsys->CancelCurrentWave();
							RearmTimer();
						}
						break;
					case PendingAction::RestartTimer: RearmTimer();                                                         break;
					case PendingAction::SkipSegment:  if (g_waveSubsys)  g_waveSubsys->ForceWaveStageProgress(1.0f);             break;
					case PendingAction::WavesOn:      if (g_timerSubsys) g_timerSubsys->WavesActive(true);                       break;
					case PendingAction::WavesOff:     if (g_timerSubsys) g_timerSubsys->WavesActive(false);                      break;
					case PendingAction::StartHeat:    if (g_waveSubsys)  g_waveSubsys->StartWave(SDK::EEnviroWave::Heat);        break;
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
			g_waveSubsys     = nullptr;
			g_timerSubsys    = nullptr;
			g_pauseWritten   = false;
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
		g_waveSubsys     = nullptr;
		g_timerSubsys    = nullptr;
		g_pauseWritten   = false;
		g_scanned        = false;
		g_scanRetryTimer = 0.0f;
		g_scanAttempts   = 0;
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

	void RenderImGui(IModLoaderImGui* imgui)
	{
		WaveSnapshot snap;
		{
			std::lock_guard<std::mutex> lock(g_snapshotMutex);
			snap = g_snapshot;
		}

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
			imgui->TableSetColumnIndex(1); imgui->Text(WaveTypeName(snap.waveType));
			imgui->TableSetColumnIndex(2);

			// Stage
			imgui->TableNextRow(0, 0.0f);
			imgui->TableSetColumnIndex(0); imgui->Text("Stage");
			imgui->TableSetColumnIndex(1); imgui->Text(WaveStageName(snap.waveStage));
			imgui->TableSetColumnIndex(2);
			if (snap.inProgress)
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
			if (!snap.inProgress)   imgui->TextDisabled("Planet Stable");
			else if (snap.isPaused) imgui->Text("Paused");
			else                    imgui->Text("Running");
			imgui->TableSetColumnIndex(2);
			if (snap.inProgress)
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
			imgui->TableSetColumnIndex(1); imgui->Text(snap.inProgress ? "In Progress" : "Planet Stable");
			imgui->TableSetColumnIndex(2);
			if (snap.inProgress)
			{
				if (imgui->SmallButton("Cancel"))
					g_pendingAction.store(static_cast<int>(PendingAction::Cancel));
			}

			// Pause Waves Entirely
			{
				const bool pauseWaves = g_pauseWavesEnabled.load();
				imgui->TableNextRow(0, 0.0f);
				imgui->TableSetColumnIndex(0); imgui->Text("Pause Waves");
				imgui->TableSetColumnIndex(1); imgui->Text(pauseWaves ? "Paused" : "Running");
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
			if (snap.inProgress)
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
