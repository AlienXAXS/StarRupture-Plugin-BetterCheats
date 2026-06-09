#include "world_wave.h"
#include "plugin_helpers.h"

#include "Chimera_classes.hpp"

#include <atomic>
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

		void RunObjectScan()
		{
			g_waveSubsys  = nullptr;
			g_timerSubsys = nullptr;
			g_scanned     = true;

			SDK::TUObjectArray* arr = SDK::UObject::GObjects.GetTypedPtr();
			if (!arr)
			{
				LOG_WARN("Wave: GObjects array is null — scan aborted.");
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

				if (!doneWave && obj != waveCDO && obj->Class->GetName() == "CrEnviroWaveSubsystem")
				{
					g_waveSubsys = static_cast<SDK::UCrEnviroWaveSubsystem*>(obj);
					doneWave = true;
					continue;
				}
				if (!doneTimer && obj != timerCDO && obj->Class->GetName() == "CrEnviroWaveTimerSubsystem")
				{
					g_timerSubsys = static_cast<SDK::UCrEnviroWaveTimerSubsystem*>(obj);
					doneTimer = true;
					continue;
				}
			}

			if (!g_waveSubsys)  LOG_WARN("Wave: UCrEnviroWaveSubsystem not found in GObjects.");
			if (!g_timerSubsys) LOG_WARN("Wave: UCrEnviroWaveTimerSubsystem not found in GObjects.");
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
		};

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
			std::lock_guard<std::mutex> lock(g_snapshotMutex);
			g_snapshot = snap;
		}

		// -------------------------------------------------------------------------
		// Pending action — queued from the ImGui render thread, applied on the
		// game thread in Tick(). Last write wins within a single frame.
		// -------------------------------------------------------------------------
		enum class PendingAction : int { None, Pause, Resume, Cancel, SkipSegment, WavesOn, WavesOff, StartHeat, StartCold };

		std::atomic<int> g_pendingAction{ static_cast<int>(PendingAction::None) };

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
					case PendingAction::Cancel:       if (g_waveSubsys)  g_waveSubsys->CancelCurrentWave();                      break;
					case PendingAction::SkipSegment:  if (g_waveSubsys)  g_waveSubsys->ForceWaveStageProgress(1.0f);             break;
					case PendingAction::WavesOn:      if (g_timerSubsys) g_timerSubsys->WavesActive(true);                       break;
					case PendingAction::WavesOff:     if (g_timerSubsys) g_timerSubsys->WavesActive(false);                      break;
					case PendingAction::StartHeat:    if (g_waveSubsys)  g_waveSubsys->StartWave(SDK::EEnviroWave::Heat);        break;
					case PendingAction::StartCold:    if (g_waveSubsys)  g_waveSubsys->StartWave(SDK::EEnviroWave::Cold);        break;
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
			case SDK::EEnviroWave::Cold: return "Cold Wave";
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
		void OnWorldBeginPlay(SDK::UWorld* /*world*/) { g_scanned = false; }
		void OnWorldEndPlay(SDK::UWorld* /*world*/, const char* /*name*/)
		{
			g_waveSubsys  = nullptr;
			g_timerSubsys = nullptr;
			g_scanned     = false;
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
		g_waveSubsys  = nullptr;
		g_timerSubsys = nullptr;
		g_scanned     = false;
	}

	void Tick(float /*deltaSeconds*/)
	{
		if (!g_scanned)
			RunObjectScan();

		ApplyPendingAction();
		RefreshSnapshot();
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
			imgui->SameLine(0.0f, 4.0f);
			if (imgui->SmallButton("Cold"))
				g_pendingAction.store(static_cast<int>(PendingAction::StartCold));
			imgui->TableSetColumnIndex(2);

			// Pause / Resume
			imgui->TableNextRow(0, 0.0f);
			imgui->TableSetColumnIndex(0); imgui->Text("Pause");
			imgui->TableSetColumnIndex(1);
			if (!snap.inProgress)   imgui->TextDisabled("No wave active");
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
			imgui->TableSetColumnIndex(1); imgui->Text(snap.inProgress ? "In Progress" : "No Wave Active");
			imgui->TableSetColumnIndex(2);
			if (snap.inProgress)
			{
				if (imgui->SmallButton("Cancel"))
					g_pendingAction.store(static_cast<int>(PendingAction::Cancel));
			}

			imgui->EndTable();
		}

		// ----- Wave Spawning Timer -----
		imgui->Spacing();
		imgui->SeparatorText("Wave Spawning Timer");

		if (!snap.subsysFound)
		{
			imgui->TextDisabled("Timer subsystem not found.");
		}
		else if (imgui->BeginTable("##wave_timer", 3, kTableFlags))
		{
			SetupWaveTableColumns(imgui);

			imgui->TableNextRow(0, 0.0f);
			imgui->TableSetColumnIndex(0); imgui->Text("Enable Waves");
			imgui->TableSetColumnIndex(1); imgui->TextDisabled("Activates the wave spawn timer");
			imgui->TableSetColumnIndex(2);
			if (imgui->SmallButton("Enable"))
				g_pendingAction.store(static_cast<int>(PendingAction::WavesOn));

			imgui->TableNextRow(0, 0.0f);
			imgui->TableSetColumnIndex(0); imgui->Text("Disable Waves");
			imgui->TableSetColumnIndex(1); imgui->TextDisabled("Stops new waves from spawning");
			imgui->TableSetColumnIndex(2);
			if (imgui->SmallButton("Disable"))
				g_pendingAction.store(static_cast<int>(PendingAction::WavesOff));

			imgui->EndTable();
		}
	}
}
